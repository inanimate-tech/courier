#include "CourierWSTransport.h"
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
static const char* TAG = "WSTransport";
#else
#include <cstdio>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "WSTransport";
#endif

CourierWSTransport::CourierWSTransport()
{
}

CourierWSTransport::CourierWSTransport(const CourierWSTransportConfig& config)
    : _certPem(config.cert_pem)
{
}

void CourierWSTransport::onConfigure(ConfigureCallback cb)
{
    _configureCallback = cb;
}

CourierWSTransport::~CourierWSTransport()
{
    destroyClient();
    freeReassemblyBuf();
}

void CourierWSTransport::freeReassemblyBuf()
{
    free(_reassemblyBuf);
    _reassemblyBuf = nullptr;
    _reassemblyLen = 0;
    _reassemblyPos = 0;
}

void CourierWSTransport::destroyClient()
{
    if (_client) {
        esp_websocket_client_stop(_client);
        esp_websocket_client_destroy(_client);
        _client = nullptr;
    }
    freeReassemblyBuf();
    _connected.store(false, std::memory_order_release);
}

void CourierWSTransport::begin(const char* host, uint16_t port, const char* path)
{
    // Tear down previous client if reconnecting
    destroyClient();

    // Build wss:// URI
    std::string uri = "wss://";
    uri += host;
    uri += ":";
    uri += std::to_string(port);
    uri += path;

    ESP_LOGI(TAG, "Connecting to %s", uri.c_str());

    esp_websocket_client_config_t config = {};
    config.uri = uri.c_str();
    if (_certPem) {
        config.cert_pem = _certPem;
    }
    // Buffer stays at default 1024. Large messages are fragmented by the
    // library and reassembled in PSRAM by our event handler, keeping
    // internal SRAM free for OTA TLS handshakes.
    config.disable_auto_reconnect = true;
    config.pingpong_timeout_sec = 20;
    config.task_stack = 8192;
    config.user_context = this;

    // Allow caller to modify the raw IDF config before init
    if (_configureCallback) _configureCallback(config);

    _client = esp_websocket_client_init(&config);
    esp_websocket_register_events(_client, WEBSOCKET_EVENT_ANY,
                                   wsEventHandler, this);
    esp_websocket_client_start(_client);
}

void CourierWSTransport::disconnect()
{
    destroyClient();
}

bool CourierWSTransport::isConnected() const
{
    return _connected.load(std::memory_order_acquire);
}

bool CourierWSTransport::sendMessage(const char* payload)
{
    if (!_connected.load(std::memory_order_acquire) || !_client) return false;
    int result = esp_websocket_client_send_text(_client, payload,
                                                 strlen(payload), portMAX_DELAY);
    return result >= 0;
}

bool CourierWSTransport::sendBinary(const uint8_t* data, size_t len)
{
    if (!_connected.load(std::memory_order_acquire) || !_client) return false;
    int result = esp_websocket_client_send_bin(_client, (const char*)data,
                                                len, portMAX_DELAY);
    return result >= 0;
}

void CourierWSTransport::suspend()
{
    if (_client) {
        ESP_LOGI(TAG, "Suspending (freeing task stack)");
        esp_websocket_client_stop(_client);
        _connected.store(false, std::memory_order_release);
    }
}

void CourierWSTransport::resume()
{
    if (_client) {
        ESP_LOGI(TAG, "Resuming");
        esp_websocket_client_start(_client);
    }
}

void CourierWSTransport::wsEventHandler(void* handler_arg,
                                          esp_event_base_t base,
                                          int32_t event_id,
                                          void* event_data)
{
    (void)base;
    auto* self = (CourierWSTransport*)handler_arg;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected");
        self->_connected.store(true, std::memory_order_release);
        self->queueConnectionChange(true);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected");
        self->_connected.store(false, std::memory_order_release);
        self->queueConnectionChange(false);
        break;

    case WEBSOCKET_EVENT_DATA: {
        auto* data = (esp_websocket_event_data_t*)event_data;
        if (data->op_code != 0x01 || !data->data_ptr || data->data_len <= 0) break;

        // Single-chunk message (fits in library's 1KB buffer)
        if (data->payload_len == data->data_len && data->payload_offset == 0) {
            self->freeReassemblyBuf();
            self->queueIncomingMessage(data->data_ptr, data->data_len);
            break;
        }

        // Multi-chunk: reassemble into PSRAM to keep internal SRAM free
        if (data->payload_offset == 0) {
            self->freeReassemblyBuf();
#ifdef ESP_PLATFORM
            self->_reassemblyBuf = (char*)heap_caps_malloc(data->payload_len + 1, MALLOC_CAP_SPIRAM);
#else
            self->_reassemblyBuf = (char*)malloc(data->payload_len + 1);
#endif
            if (!self->_reassemblyBuf) break;
            self->_reassemblyLen = data->payload_len;
            self->_reassemblyPos = 0;
        }

        if (self->_reassemblyBuf &&
            self->_reassemblyPos + data->data_len <= self->_reassemblyLen) {
            memcpy(self->_reassemblyBuf + self->_reassemblyPos,
                   data->data_ptr, data->data_len);
            self->_reassemblyPos += data->data_len;

            if (self->_reassemblyPos == self->_reassemblyLen) {
                self->_reassemblyBuf[self->_reassemblyLen] = '\0';
                self->queueIncomingMessage(self->_reassemblyBuf, self->_reassemblyLen);
                self->freeReassemblyBuf();
            }
        } else {
            ESP_LOGW(TAG, "WS reassembly overflow, dropping message");
            self->freeReassemblyBuf();
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}
