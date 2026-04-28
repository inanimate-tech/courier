#include "MqttTransport.h"
#include <cstring>
#include <cstdlib>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp32-hal.h"  // millis() — Arduino.h conflicts with IDF mqtt/lwip headers
// ESP-IDF v5.x restructured esp_mqtt_client_config_t into nested sub-structs.
// Arduino framework (PlatformIO) bundles ESP-IDF v4.4.x with flat fields.
#define MQTT_CONFIG_V5 (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0))
static const char* TAG = "MqttTransport";
#else
#include <Arduino.h>
#include <cstdio>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "MqttTransport";
#endif

namespace Courier {

MqttTransport::MqttTransport()
{
}

MqttTransport::MqttTransport(const Config& config)
    : _certPem(config.cert_pem),
      _taskStack(config.task_stack),
      _topics(config.topics.begin(), config.topics.end())
{
    if (config.clientId) {
        _configClientId = config.clientId;
    }
}

void MqttTransport::onConfigure(ConfigureCallback cb)
{
    _configureCallback = cb;
}

MqttTransport::~MqttTransport()
{
    destroyClient();
    freeReassemblyBuf();
    char* topic = nullptr;
    while (_topicQueue.pop(topic)) free(topic);
    // Note: base class destructor drains _pending and frees its payloads.
}

void MqttTransport::freeReassemblyBuf()
{
    if (_reassemblyBuf) {
        free(_reassemblyBuf);
        _reassemblyBuf = nullptr;
    }
    if (_reassemblyTopic) {
        free(_reassemblyTopic);
        _reassemblyTopic = nullptr;
    }
    _reassemblyLen = 0;
    _reassemblyPos = 0;
}

void MqttTransport::destroyClient()
{
    if (_client) {
        esp_mqtt_client_stop(_client);
        esp_mqtt_client_destroy(_client);
        _client = nullptr;
    }
    freeReassemblyBuf();
    _connected.store(false, std::memory_order_release);
    _selfHealActive = false;
}

void MqttTransport::subscribeAll()
{
    if (!_client) return;
    for (const auto& topic : _topics) {
        esp_mqtt_client_subscribe(_client, topic.c_str(), 0);
    }
}

void MqttTransport::subscribe(const char* topic, int qos)
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

void MqttTransport::unsubscribe(const char* topic)
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

bool MqttTransport::publish(const char* topic, const char* payload,
                                    int qos, bool retain)
{
    if (!_client || !_connected.load(std::memory_order_acquire)) return false;
    int result = esp_mqtt_client_publish(_client, topic, payload, 0,
                                         qos, retain ? 1 : 0);
    return result >= 0;
}

bool MqttTransport::publish(const char* topic, JsonDocument& doc,
                            int qos, bool retain)
{
    char buf[1024];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    return publish(topic, buf, qos, retain);
}

bool MqttTransport::send(JsonDocument& doc, const SendOptions& options)
{
    if (!options.topic) return false;  // MQTT requires a topic
    char buf[1024];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    return publish(options.topic, buf, options.qos, options.retain);
}

void MqttTransport::begin(const char* host, uint16_t port, const char* path)
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
    config.task.stack_size = _taskStack;
#else
    config.uri = uri.c_str();
    if (_certPem) {
        config.cert_pem = _certPem;
    }
    config.client_id = _configClientId.empty() ? nullptr : _configClientId.c_str();
    config.task_stack = _taskStack;
    config.user_context = this;
#endif

    // Allow caller to modify the raw IDF config before init
    if (_configureCallback) _configureCallback(config);

    _client = esp_mqtt_client_init(&config);
    esp_mqtt_client_register_event(_client, MQTT_EVENT_ANY,
                                    mqttEventHandler, this);
    esp_mqtt_client_start(_client);
}

void MqttTransport::disconnect()
{
    destroyClient();
    _selfHealActive = false;
}

bool MqttTransport::isConnected() const
{
    return _client && _connected.load(std::memory_order_acquire);
}

void MqttTransport::queueIncomingMqttMessage(const char* topic, const char* payload, size_t len)
{
    // Push payload first. If the topic push fails after, that one message
    // gets _onMessage / _clientHook but no _onTopicMessage — bounded loss.
    // Pushing topic first risks a permanent index-shift if payload then
    // fails, which is much worse.
    queueIncomingMessage(payload, len);  // base; silent drop on failure

    char* topicCopy = strdup(topic);
    if (!topicCopy) return;
    if (!_topicQueue.push(topicCopy)) {
        free(topicCopy);
    }
}

void MqttTransport::loop()
{
    PendingMessage pmsg;
    char* topic = nullptr;
    while (_pending.pop(pmsg)) {
        bool gotTopic = _topicQueue.pop(topic);
        if (!pmsg.isBinary) {
            if (_onTopicMessage && gotTopic) {
                _onTopicMessage(topic, (const char*)pmsg.payload, pmsg.length);
            }
            if (_onMessage) _onMessage((const char*)pmsg.payload, pmsg.length);
            if (_clientHook) _clientHook((const char*)pmsg.payload, pmsg.length);
        } else {
            if (_onBinaryMessage) _onBinaryMessage((const uint8_t*)pmsg.payload, pmsg.length);
        }
        if (gotTopic) free(topic);
        free(pmsg.payload);
    }

    if (_selfHealActive) {
        if (_connected.load(std::memory_order_acquire)) {
            _selfHealActive = false;
        } else if (millis() - _disconnectedSinceMillis >= SELF_HEAL_TIMEOUT) {
            _selfHealActive = false;
            queueTransportFailed();
        }
    }

    drainSignals();
}

void MqttTransport::suspend()
{
    if (_client) {
        ESP_LOGI(TAG, "Suspending (freeing task stack)");
        esp_mqtt_client_stop(_client);
        _connected.store(false, std::memory_order_release);
    }
}

void MqttTransport::resume()
{
    if (_client) {
        ESP_LOGI(TAG, "Resuming");
        esp_mqtt_client_start(_client);
    }
}

void MqttTransport::mqttEventHandler(void* handler_arg,
                                              esp_event_base_t base,
                                              int32_t event_id,
                                              void* event_data)
{
    (void)base;
    auto* self = (MqttTransport*)handler_arg;
    auto* event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected");
        self->_connected.store(true, std::memory_order_release);

        // Re-subscribe to all managed topics.
        self->subscribeAll();

        self->queueConnectionChange(true);
        self->_selfHealActive = false;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected");
        self->_connected.store(false, std::memory_order_release);
        self->queueConnectionChange(false);
        self->_disconnectedSinceMillis = millis();
        self->_selfHealActive = true;
        break;

    case MQTT_EVENT_DATA: {
        if (!event->data || event->data_len <= 0) break;

        // Single-chunk message (fits in library's 1KB buffer)
        if (event->total_data_len == event->data_len && event->current_data_offset == 0) {
            self->freeReassemblyBuf();
            // Heap-allocate the topic to avoid silent truncation of topics
            // longer than a fixed stack buffer. Topic is not NUL-terminated
            // in the IDF event. queueIncomingMqttMessage strdups internally,
            // so the local copy can be freed immediately after.
            char* topicCopy = (char*)malloc(event->topic_len + 1);
            if (!topicCopy) break;
            memcpy(topicCopy, event->topic, event->topic_len);
            topicCopy[event->topic_len] = '\0';
            self->queueIncomingMqttMessage(topicCopy, event->data, event->data_len);
            free(topicCopy);
            break;
        }

        // Multi-chunk: reassemble into PSRAM to keep internal SRAM free.
        // First chunk allocates buffer + captures topic (only first chunk
        // has event->topic per IDF docs).
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
            // Capture the topic for use when reassembly completes.
            self->_reassemblyTopic = (char*)malloc(event->topic_len + 1);
            if (self->_reassemblyTopic) {
                memcpy(self->_reassemblyTopic, event->topic, event->topic_len);
                self->_reassemblyTopic[event->topic_len] = '\0';
            }
        }

        if (self->_reassemblyBuf &&
            self->_reassemblyPos + event->data_len <= self->_reassemblyLen) {
            memcpy(self->_reassemblyBuf + self->_reassemblyPos,
                   event->data, event->data_len);
            self->_reassemblyPos += event->data_len;

            if (self->_reassemblyPos == self->_reassemblyLen) {
                self->_reassemblyBuf[self->_reassemblyLen] = '\0';
                const char* topic = self->_reassemblyTopic ? self->_reassemblyTopic : "";
                self->queueIncomingMqttMessage(topic, self->_reassemblyBuf, self->_reassemblyLen);
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

}  // namespace Courier
