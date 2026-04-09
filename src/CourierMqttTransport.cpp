#include "CourierMqttTransport.h"
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
// ESP-IDF v5.x restructured esp_mqtt_client_config_t into nested sub-structs.
// Arduino framework (PlatformIO) bundles ESP-IDF v4.4.x with flat fields.
#define MQTT_CONFIG_V5 (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
static const char* TAG = "MqttTransport";
#else
#include <cstdio>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "MqttTransport";
#endif

CourierMqttTransport::CourierMqttTransport()
{
}

CourierMqttTransport::CourierMqttTransport(const CourierMqttTransportConfig& config)
    : _certPem(config.cert_pem),
      _topics(config.topics.begin(), config.topics.end())
{
    if (config.clientId) {
        _configClientId = config.clientId;
    }
    if (config.defaultPublishTopic) {
        _defaultPublishTopic = config.defaultPublishTopic;
    }
}

void CourierMqttTransport::onConfigure(ConfigureCallback cb)
{
    _configureCallback = cb;
}

CourierMqttTransport::~CourierMqttTransport()
{
    destroyClient();
    freeReassemblyBuf();
}

void CourierMqttTransport::freeReassemblyBuf()
{
    free(_reassemblyBuf);
    _reassemblyBuf = nullptr;
    _reassemblyLen = 0;
    _reassemblyPos = 0;
}

void CourierMqttTransport::destroyClient()
{
    if (_client) {
        esp_mqtt_client_stop(_client);
        esp_mqtt_client_destroy(_client);
        _client = nullptr;
    }
    freeReassemblyBuf();
    _connected.store(false, std::memory_order_release);
}

void CourierMqttTransport::subscribeAll()
{
    if (!_client) return;
    for (const auto& topic : _topics) {
        esp_mqtt_client_subscribe(_client, topic.c_str(), 0);
    }
}

void CourierMqttTransport::subscribe(const char* topic, int qos)
{
    // Add to list if not already present.
    for (const auto& t : _topics) {
        if (t == topic) return;  // Already tracked — idempotent, nothing to do.
    }
    _topics.push_back(topic);
    if (_client && _connected.load(std::memory_order_acquire)) {
        esp_mqtt_client_subscribe(_client, topic, qos);
    }
}

void CourierMqttTransport::unsubscribe(const char* topic)
{
    for (auto it = _topics.begin(); it != _topics.end(); ++it) {
        if (*it == topic) {
            _topics.erase(it);
            break;
        }
    }
    if (_client && _connected.load(std::memory_order_acquire)) {
        esp_mqtt_client_unsubscribe(_client, topic);
    }
}

bool CourierMqttTransport::publishTo(const char* topic, const char* payload,
                                      int qos, bool retain)
{
    if (!_client || !_connected.load(std::memory_order_acquire)) return false;
    int result = esp_mqtt_client_publish(_client, topic, payload, 0,
                                         qos, retain ? 1 : 0);
    return result >= 0;
}

void CourierMqttTransport::begin(const char* host, uint16_t port, const char* path)
{
    // Tear down previous client cleanly
    destroyClient();

    // Build wss:// URI
    std::string uri = "wss://";
    uri += host;
    uri += ":";
    uri += std::to_string(port);
    uri += path;

    ESP_LOGI(TAG, "Connecting to %s", uri.c_str());

    esp_mqtt_client_config_t config = {};
    // Buffer stays at default 1024. Large messages are fragmented by the
    // library and reassembled in PSRAM by our event handler, keeping
    // internal SRAM free for OTA TLS handshakes.
#if defined(MQTT_CONFIG_V5) && MQTT_CONFIG_V5
    config.broker.address.uri = uri.c_str();
    if (_certPem) {
        config.broker.verification.certificate = _certPem;
    }
    config.credentials.client_id = _configClientId.empty() ? nullptr : _configClientId.c_str();
    config.network.disable_auto_reconnect = true;
    config.task.stack_size = 8192;
#else
    config.uri = uri.c_str();
    if (_certPem) {
        config.cert_pem = _certPem;
    }
    config.client_id = _configClientId.empty() ? nullptr : _configClientId.c_str();
    config.disable_auto_reconnect = true;
    config.task_stack = 8192;
    config.user_context = this;
#endif

    // Allow caller to modify the raw IDF config before init
    if (_configureCallback) _configureCallback(config);

    _client = esp_mqtt_client_init(&config);
    esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY,
                                    mqttEventHandler, this);
    esp_mqtt_client_start(_client);
}

void CourierMqttTransport::disconnect()
{
    if (_client) {
        esp_mqtt_client_stop(_client);
    }
    _connected.store(false, std::memory_order_release);
}

bool CourierMqttTransport::isConnected() const
{
    return _client && _connected.load(std::memory_order_acquire);
}

bool CourierMqttTransport::sendMessage(const char* payload)
{
    if (!_client || !_connected.load(std::memory_order_acquire) ||
        _defaultPublishTopic.empty()) return false;
    int result = esp_mqtt_client_publish(_client, _defaultPublishTopic.c_str(),
                                          payload, 0, 0, 0);
    return result >= 0;
}

void CourierMqttTransport::suspend()
{
    if (_client) {
        ESP_LOGI(TAG, "Suspending (freeing task stack)");
        esp_mqtt_client_stop(_client);
        _connected.store(false, std::memory_order_release);
    }
}

void CourierMqttTransport::resume()
{
    if (_client) {
        ESP_LOGI(TAG, "Resuming");
        esp_mqtt_client_start(_client);
    }
}

void CourierMqttTransport::mqttEventHandler(void* handler_arg,
                                              esp_event_base_t base,
                                              int32_t event_id,
                                              void* event_data)
{
    (void)base;
    auto* self = (CourierMqttTransport*)handler_arg;
    auto* event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected");
        self->_connected.store(true, std::memory_order_release);

        // Re-subscribe to all managed topics.
        self->subscribeAll();

        self->queueConnectionChange(true);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected");
        self->_connected.store(false, std::memory_order_release);
        self->queueConnectionChange(false);
        break;

    case MQTT_EVENT_DATA: {
        if (!event->data || event->data_len <= 0) break;

        // Single-chunk message (fits in library's 1KB buffer)
        if (event->total_data_len == event->data_len && event->current_data_offset == 0) {
            self->freeReassemblyBuf();
            self->queueIncomingMessage(event->data, event->data_len);
            break;
        }

        // Multi-chunk: reassemble into PSRAM to keep internal SRAM free
        if (event->current_data_offset == 0) {
            self->freeReassemblyBuf();
#ifdef ESP_PLATFORM
            self->_reassemblyBuf = (char*)heap_caps_malloc(event->total_data_len + 1, MALLOC_CAP_SPIRAM);
#else
            self->_reassemblyBuf = (char*)malloc(event->total_data_len + 1);
#endif
            if (!self->_reassemblyBuf) break;
            self->_reassemblyLen = event->total_data_len;
            self->_reassemblyPos = 0;
        }

        if (self->_reassemblyBuf &&
            self->_reassemblyPos + event->data_len <= self->_reassemblyLen) {
            memcpy(self->_reassemblyBuf + self->_reassemblyPos,
                   event->data, event->data_len);
            self->_reassemblyPos += event->data_len;

            if (self->_reassemblyPos == self->_reassemblyLen) {
                self->_reassemblyBuf[self->_reassemblyLen] = '\0';
                self->queueIncomingMessage(self->_reassemblyBuf, self->_reassemblyLen);
                self->freeReassemblyBuf();
            }
        } else {
            ESP_LOGW(TAG, "MQTT reassembly overflow, dropping message");
            self->freeReassemblyBuf();
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}
