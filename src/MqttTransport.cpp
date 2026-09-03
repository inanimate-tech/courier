#include "MqttTransport.h"
#include <climits>
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
// See WebSocketTransport.cpp: Arduino cores ship a same-named esp_crt_bundle.h
// shadowing the IDF one — declare the IDF symbol directly.
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);
static const char* TAG = "MqttTransport";
#else
#include <Arduino.h>
#include <cstdio>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "MqttTransport";
// Native tests: stand-in with the same shape; the mock config records the
// pointer so tests can assert bundle selection.
static esp_err_t esp_crt_bundle_attach(void* conf) { (void)conf; return 0; }
#endif

namespace Courier {

MqttTransport::MqttTransport()
{
}

MqttTransport::MqttTransport(const Config& config)
    : _certPem(config.cert_pem),
      _useCertBundle(config.use_cert_bundle),
      _taskStack(config.task_stack),
      _outBufferSize(config.out_buffer_size),
      _networkTimeoutMs(config.network_timeout_ms)
{
    for (const auto& t : config.topics) {
        _topics.push_back(Subscription{t, 0, false});
    }
    for (const auto& t : config.binaryTopics) {
        _topics.push_back(Subscription{t, 0, true});
    }
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
    LockGuard<TimedMutex> guard(_clientLock);
    destroyClientLocked();
}

void MqttTransport::destroyClientLocked()
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

// Runs on the ESP-IDF event task, which already holds the IDF API lock. It
// must not take _clientLock: an app task blocked inside esp_mqtt_client_publish
// holds _clientLock while waiting for that same IDF lock. Snapshot the list
// under _topicsLock, then subscribe with no Courier lock held.
void MqttTransport::subscribeAll()
{
    if (!_client) return;
    std::vector<Subscription> snapshot;
    {
        LockGuard<Mutex> guard(_topicsLock);
        snapshot = _topics;
    }
    for (const auto& sub : snapshot) {
        esp_mqtt_client_subscribe(_client, sub.filter.c_str(), sub.qos);
    }
}

void MqttTransport::addSubscription(const char* topic, int qos, bool binary)
{
    if (!topic) return;
    bool isNew = true;
    {
        LockGuard<Mutex> guard(_topicsLock);
        // Already tracked — idempotent on the wire, but the lane can be restated.
        for (auto& sub : _topics) {
            if (sub.filter == topic) {
                sub.binary = binary;
                isNew = false;
                break;
            }
        }
        if (isNew) _topics.push_back(Subscription{topic, qos, binary});
    }
    if (!isNew) return;

    // Outside _topicsLock: this calls into ESP-IDF.
    LockGuard<TimedMutex> guard(_clientLock);
    if (_client && _connected.load(std::memory_order_acquire)) {
        esp_mqtt_client_subscribe(_client, topic, qos);
    }
}

void MqttTransport::subscribe(const char* topic, int qos)
{
    addSubscription(topic, qos, false);
}

void MqttTransport::subscribeBinary(const char* topic, int qos)
{
    addSubscription(topic, qos, true);
}

void MqttTransport::unsubscribe(const char* topic)
{
    if (!topic) return;
    {
        LockGuard<Mutex> guard(_topicsLock);
        for (auto it = _topics.begin(); it != _topics.end(); ++it) {
            if (it->filter == topic) {
                _topics.erase(it);
                break;
            }
        }
    }
    LockGuard<TimedMutex> guard(_clientLock);
    if (_client && _connected.load(std::memory_order_acquire)) {
        esp_mqtt_client_unsubscribe(_client, topic);
    }
}

// MQTT 3.1.1 section 4.7: '+' matches exactly one level, '#' matches the
// remainder including the parent level ("a/#" matches "a").
bool MqttTransport::topicMatches(const char* filter, const char* topic)
{
    if (!filter || !topic) return false;

    const char* f = filter;
    const char* t = topic;

    while (*f) {
        // '#' is only valid as the final level; it matches the remainder.
        if (*f == '#') return true;
        // ...and "a/#" matches the parent "a" as well (4.7.1.2).
        if (*f == '/' && f[1] == '#' && *t == '\0') return true;

        if (*f == '+') {
            ++f;
            while (*t && *t != '/') ++t;   // consume exactly one level
        } else if (*f == *t) {
            ++f;
            ++t;
            continue;
        } else {
            return false;
        }
        // After a '+', both sides must sit on the same boundary.
        if (*f != *t) return false;        // both '/' or both end-of-string
        if (*f == '/') { ++f; ++t; }
    }
    return *t == '\0';
}

// Called from loop() on the app task, which is also the only task that mutates
// _topics — so the lock is uncontended here. It is taken anyway so the rule
// stays simple: _topics is never touched without _topicsLock.
bool MqttTransport::isBinaryTopic(const char* topic) const
{
    LockGuard<Mutex> guard(_topicsLock);
    for (const auto& sub : _topics) {
        if (sub.binary && topicMatches(sub.filter.c_str(), topic)) return true;
    }
    return false;
}

bool MqttTransport::publish(const char* topic, const char* payload,
                                    int qos, bool retain)
{
    if (!_client || !_connected.load(std::memory_order_acquire)) return false;

    // Not serialisation — esp-mqtt already takes its own API lock on every
    // entry point. This guards the handle itself: esp_mqtt_client_destroy
    // takes no lock and frees the client, so a teardown racing this call is a
    // use-after-free. Bounded, because teardown can be slow (stop() waits for
    // the IDF task to leave a connect that is bounded by network_timeout_ms).
    if (!_clientLock.tryLockFor(PUBLISH_LOCK_TIMEOUT_MS)) return false;
    int result = _client
        ? esp_mqtt_client_publish(_client, topic, payload, 0, qos, retain ? 1 : 0)
        : -1;
    _clientLock.unlock();
    return result >= 0;
}

bool MqttTransport::publishBinary(const char* topic, const uint8_t* data,
                                  size_t len, int qos, bool retain)
{
    if (!topic || !_client || !_connected.load(std::memory_order_acquire)) return false;

    // esp_mqtt_client_publish does `if (len <= 0 && data != NULL) len =
    // strlen(data)`. `data` here carries no NUL terminator, so letting that
    // branch fire would read past the end of the buffer — a heap overread that
    // publishes adjacent memory at best. Reject rather than massage arguments.
    if (!data) return false;
    if (len == 0) return false;
    if (len > (size_t)INT_MAX) return false;

    if (!_clientLock.tryLockFor(PUBLISH_LOCK_TIMEOUT_MS)) return false;
    int result = _client
        ? esp_mqtt_client_publish(_client, topic, (const char*)data,
                                  (int)len, qos, retain ? 1 : 0)
        : -1;
    _clientLock.unlock();
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

void MqttTransport::begin()
{
    LockGuard<TimedMutex> guard(_clientLock);

    // Tear down previous client cleanly
    destroyClientLocked();

    // Build wss:// URI
    std::string uri = "wss://";
    uri += _host.c_str();
    uri += ":";
    uri += std::to_string(_port);
    uri += _path.c_str();

    ESP_LOGI(TAG, "Connecting to %s", uri.c_str());

    esp_mqtt_client_config_t config = {};
    // Receive buffer stays at the IDF default (1024). Large messages are
    // fragmented by the library and reassembled in PSRAM by our event handler,
    // keeping internal SRAM free for OTA TLS handshakes. The outbound buffer
    // is separately tunable via Config::out_buffer_size.
#if defined(MQTT_CONFIG_V5) && MQTT_CONFIG_V5
    config.broker.address.uri = uri.c_str();
    if (_certPem) {
        config.broker.verification.certificate = _certPem;
    } else if (_useCertBundle) {
        config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    config.credentials.client_id = _configClientId.empty() ? nullptr : _configClientId.c_str();
    config.task.stack_size = _taskStack;
    if (_outBufferSize > 0)     config.buffer.out_size = _outBufferSize;
    if (_networkTimeoutMs > 0)  config.network.timeout_ms = _networkTimeoutMs;
#else
    config.uri = uri.c_str();
    if (_certPem) {
        config.cert_pem = _certPem;
    } else if (_useCertBundle) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    config.client_id = _configClientId.empty() ? nullptr : _configClientId.c_str();
    config.task_stack = _taskStack;
    if (_outBufferSize > 0)     config.out_buffer_size = _outBufferSize;
    if (_networkTimeoutMs > 0)  config.network_timeout_ms = _networkTimeoutMs;
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

void MqttTransport::queueIncomingMqttMessage(const char* topic, const char* payload,
                                             size_t len)
{
    // Queued uniformly; the text/binary lane is chosen at drain time, on the
    // task that owns _topics. The extra NUL costs one byte and is ignored by
    // binary consumers, which read `len`.
    //
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

const char* MqttTransport::ErrorInfo::describe() const
{
    switch (type) {
    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
        switch (connectReturnCode) {
        case MQTT_CONNECTION_REFUSE_PROTOCOL:
            return "connection refused: unacceptable protocol version";
        case MQTT_CONNECTION_REFUSE_ID_REJECTED:
            return "connection refused: identifier rejected";
        case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
            return "connection refused: server unavailable";
        case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
            return "connection refused: bad username or password";
        case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
            return "connection refused: not authorized";
        default:
            return "connection refused: unknown reason";
        }
    case MQTT_ERROR_TYPE_TCP_TRANSPORT:
        return "transport error (TLS or socket)";
    case MQTT_ERROR_TYPE_NONE:
        return "no error";
    default:
        // MQTT_ERROR_TYPE_SUBSCRIBE_FAILED on IDF 5; unreachable on IDF 4.4.
        // Named via default: rather than the constant, which 4.4 lacks.
        return "other MQTT error (e.g. broker-side subscribe failure on IDF >= 5.0)";
    }
}

void MqttTransport::loop()
{
    PendingMessage pmsg;
    char* topic = nullptr;
    while (_pending.pop(pmsg)) {
        bool gotTopic = _topicQueue.pop(topic);
        if (gotTopic && isBinaryTopic(topic)) {
            // Binary never enters the JSON lane: _clientHook is not called.
            if (_onTopicBinary) {
                _onTopicBinary(topic, (const uint8_t*)pmsg.payload, pmsg.length);
            }
            if (_onBinaryMessage) _onBinaryMessage((const uint8_t*)pmsg.payload, pmsg.length);
        } else {
            if (_onTopicMessage && gotTopic) {
                _onTopicMessage(topic, (const char*)pmsg.payload, pmsg.length);
            }
            if (_onMessage) _onMessage((const char*)pmsg.payload, pmsg.length);
            if (_clientHook) _clientHook((const char*)pmsg.payload, pmsg.length);
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

    // Errors drain LAST and with no lock held. Both matter:
    //  - last, because the callback may disconnect()/begin(), so nothing of
    //    ours may run after it in this iteration;
    //  - unlocked, because begin()/disconnect() take _clientLock unbounded and
    //    would self-deadlock the app task if we held it here (see Lock.h).
    //
    // Copy the callback out: the contract lets the callback call begin(),
    // and a consumer that also re-registers onError() from in there would
    // otherwise assign to the std::function while its target is running.
    // Bounded by the queue depth: begin() from inside the callback starts a
    // client whose task can push new errors into this same drain.
    ErrorCallback cb = _onError;
    if (cb) {
        ErrorInfo err;
        for (size_t i = 0; i < ERROR_QUEUE_DEPTH && _errorQueue.pop(err); ++i) {
            cb(err);
        }
    }
}

void MqttTransport::suspend()
{
    LockGuard<TimedMutex> guard(_clientLock);
    if (_client) {
        ESP_LOGI(TAG, "Suspending (freeing task stack)");
        esp_mqtt_client_stop(_client);
        _connected.store(false, std::memory_order_release);
    }
}

void MqttTransport::resume()
{
    LockGuard<TimedMutex> guard(_clientLock);
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

        // Multi-chunk: reassemble into PSRAM to keep internal SRAM free, but
        // fall back to internal SRAM on boards without PSRAM (e.g. the M5Dial) —
        // otherwise the alloc returns NULL and the whole message is dropped.
        // First chunk allocates buffer + captures topic (only first chunk
        // has event->topic per IDF docs).
        if (event->current_data_offset == 0) {
            self->freeReassemblyBuf();
#ifdef ESP_PLATFORM
            self->_reassemblyBuf = (char*)heap_caps_malloc(event->total_data_len + 1, MALLOC_CAP_SPIRAM);
            if (!self->_reassemblyBuf)
                self->_reassemblyBuf = (char*)heap_caps_malloc(event->total_data_len + 1, MALLOC_CAP_8BIT);
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
                self->queueIncomingMqttMessage(topic, self->_reassemblyBuf,
                                               self->_reassemblyLen);
                self->freeReassemblyBuf();
            }
        } else {
            ESP_LOGW(TAG, "MQTT reassembly overflow, dropping message");
            self->freeReassemblyBuf();
        }
        break;
    }

    case MQTT_EVENT_ERROR: {
        ErrorInfo info;
        // error_handle is a pointer on both IDF 4.4 and 5.x. Reach the fields
        // through auto* — the type is named differently by the host mock.
        if (event->error_handle) {
            auto* h = event->error_handle;
            info.type               = (esp_mqtt_error_type_t)h->error_type;
            info.connectReturnCode  = (esp_mqtt_connect_return_code_t)h->connect_return_code;
            info.tlsLastEspErr      = h->esp_tls_last_esp_err;
            info.tlsStackErr        = h->esp_tls_stack_err;
            info.tlsCertVerifyFlags = h->esp_tls_cert_verify_flags;
            info.sockErrno          = h->esp_transport_sock_errno;
        }

        ESP_LOGE(TAG, "MQTT error: %s", info.describe());

        if (!self->_errorQueue.push(info)) {
            ESP_LOGW(TAG, "error queue full, dropping error report");
        }
        break;
    }

    default:
        break;
    }
}

}  // namespace Courier
