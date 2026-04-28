#include "WebSocketTransport.h"
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp32-hal.h"  // millis() — Arduino.h conflicts with IDF websocket/lwip headers
static const char* TAG = "WSTransport";
#else
#include <Arduino.h>
#include <cstdio>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "WSTransport";
#endif

namespace Courier {

WebSocketTransport::WebSocketTransport()
{
}

// GTS Root R4 — root CA for Cloudflare, Google Cloud, and other major providers.
// Used when use_default_certs is true and no explicit cert_pem is provided.
static const char* GTS_ROOT_R4_PEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

WebSocketTransport::WebSocketTransport(const Config& config)
    : _certPem(config.cert_pem),
      _useDefaultCerts(config.use_default_certs)
{
}

void WebSocketTransport::onConfigure(ConfigureCallback cb)
{
    _configureCallback = cb;
}

void WebSocketTransport::useDefaultCerts()
{
    _useDefaultCerts = true;
}

WebSocketTransport::~WebSocketTransport()
{
    destroyClient();
    freeReassemblyBuf();
}

void WebSocketTransport::freeReassemblyBuf()
{
    free(_reassemblyBuf);
    _reassemblyBuf = nullptr;
    _reassemblyLen = 0;
    _reassemblyPos = 0;
}

void WebSocketTransport::destroyClient()
{
    _selfHealActive = false;
    if (_client) {
        esp_websocket_client_stop(_client);
        esp_websocket_client_destroy(_client);
        _client = nullptr;
    }
    freeReassemblyBuf();
    _connected.store(false, std::memory_order_release);
}

void WebSocketTransport::begin()
{
    // Tear down previous client if reconnecting
    destroyClient();

    // Build wss:// URI
    std::string uri = "wss://";
    uri += _host.c_str();
    uri += ":";
    uri += std::to_string(_port);
    uri += _path.c_str();

    ESP_LOGI(TAG, "Connecting to %s", uri.c_str());

    esp_websocket_client_config_t config = {};
    config.uri = uri.c_str();
    if (_certPem) {
        config.cert_pem = _certPem;
    } else if (_useDefaultCerts) {
        config.cert_pem = GTS_ROOT_R4_PEM;
    }
    // Buffer stays at default 1024. Large messages are fragmented by the
    // library and reassembled in PSRAM by our event handler, keeping
    // internal SRAM free for OTA TLS handshakes.
    config.disable_auto_reconnect = false;
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

void WebSocketTransport::disconnect()
{
    destroyClient();
}

void WebSocketTransport::loop()
{
    drainPending();

    if (_selfHealActive) {
        if (_connected.load(std::memory_order_acquire)) {
            _selfHealActive = false;
        } else if (millis() - _disconnectedSinceMillis >= SELF_HEAL_TIMEOUT) {
            _selfHealActive = false;
            queueTransportFailed();
            drainPending();  // Deliver failure callback immediately
        }
    }
}

bool WebSocketTransport::isConnected() const
{
    return _connected.load(std::memory_order_acquire);
}

bool WebSocketTransport::send(JsonDocument& doc, const SendOptions&)
{
    char buf[1024];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    return sendText(buf);
}

bool WebSocketTransport::sendText(const char* payload)
{
    if (!_connected.load(std::memory_order_acquire) || !_client) return false;
    int result = esp_websocket_client_send_text(_client, payload,
                                                 strlen(payload), portMAX_DELAY);
    return result >= 0;
}

bool WebSocketTransport::sendBinary(const uint8_t* data, size_t len)
{
    if (!_connected.load(std::memory_order_acquire) || !_client) return false;
    int result = esp_websocket_client_send_bin(_client, (const char*)data,
                                                len, portMAX_DELAY);
    return result >= 0;
}

void WebSocketTransport::suspend()
{
    if (_client) {
        ESP_LOGI(TAG, "Suspending (freeing task stack)");
        esp_websocket_client_stop(_client);
        _connected.store(false, std::memory_order_release);
    }
}

void WebSocketTransport::resume()
{
    if (_client) {
        ESP_LOGI(TAG, "Resuming");
        esp_websocket_client_start(_client);
    }
}

void WebSocketTransport::wsEventHandler(void* handler_arg,
                                          esp_event_base_t base,
                                          int32_t event_id,
                                          void* event_data)
{
    (void)base;
    auto* self = (WebSocketTransport*)handler_arg;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected");
        self->_connected.store(true, std::memory_order_release);
        self->queueConnectionChange(true);
        self->_selfHealActive = false;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected");
        self->_connected.store(false, std::memory_order_release);
        self->queueConnectionChange(false);
        self->_disconnectedSinceMillis = millis();
        self->_selfHealActive = true;
        break;

    case WEBSOCKET_EVENT_DATA: {
        auto* data = (esp_websocket_event_data_t*)event_data;
        if (!data->data_ptr || data->data_len <= 0) break;

        // Dispatch text (0x01) and binary (0x02) frames. Control frames
        // (ping/pong/close) are handled by the IDF client; other op_codes
        // are ignored. Continuation chunks (op_code 0x00) inherit the
        // type captured from the first chunk.
        const bool isFirstChunk = (data->payload_offset == 0);
        const bool isSingleChunk =
            (data->payload_len == data->data_len && isFirstChunk);

        if (isFirstChunk) {
            if (data->op_code == 0x01) {
                self->_reassemblyIsBinary = false;
            } else if (data->op_code == 0x02) {
                self->_reassemblyIsBinary = true;
            } else {
                break;  // unsupported op_code on first chunk — drop
            }
        }

        if (isSingleChunk) {
            self->freeReassemblyBuf();
            if (self->_reassemblyIsBinary) {
                self->queueIncomingBinary((const uint8_t*)data->data_ptr,
                                           data->data_len);
            } else {
                self->queueIncomingMessage(data->data_ptr, data->data_len);
            }
            break;
        }

        // Multi-chunk: reassemble into PSRAM. +1 lets us NUL-terminate
        // text payloads; unused for binary.
        if (isFirstChunk) {
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
                if (self->_reassemblyIsBinary) {
                    self->queueIncomingBinary((const uint8_t*)self->_reassemblyBuf,
                                               self->_reassemblyLen);
                } else {
                    self->_reassemblyBuf[self->_reassemblyLen] = '\0';
                    self->queueIncomingMessage(self->_reassemblyBuf,
                                                self->_reassemblyLen);
                }
                self->freeReassemblyBuf();
            }
        } else {
            ESP_LOGW(TAG, "WS reassembly overflow, dropping frame");
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

}  // namespace Courier
