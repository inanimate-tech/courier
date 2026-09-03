#ifndef COURIER_MQTT_TRANSPORT_H
#define COURIER_MQTT_TRANSPORT_H

#include "Lock.h"
#include "Transport.h"
#include <mqtt_client.h>
#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Courier {

class MqttTransport : public Transport {
public:
    // Configuration struct for MqttTransport.
    // topics: auto-subscribed on (re)connect.
    // clientId: if set, used as MQTT client ID; otherwise IDF generates one.
    // TLS precedence: cert_pem (pin) > use_cert_bundle (IDF certificate
    // bundle, the default; requires MBEDTLS_CERTIFICATE_BUNDLE) > nothing.
    struct Config {
        std::vector<std::string> topics;
        // Topics whose payloads are opaque bytes: delivered through onBinary()
        // and never through the text / JSON lane. See subscribeBinary().
        std::vector<std::string> binaryTopics;
        const char* clientId = nullptr;
        const char* cert_pem = nullptr;
        bool use_cert_bundle = true;
        int task_stack = 8192;
        // 0 = leave the IDF default (1024). Raising the outbound buffer past
        // the largest payload you publish saves a write syscall and a TLS
        // record per fragment; costs internal RAM.
        int out_buffer_size = 0;
        // 0 = leave the IDF default (10000). Bounds every network operation:
        // the TLS handshake and CONNACK wait as well as a publish's socket
        // write. Hitting it aborts the connection, so this is "how long before
        // we give up on this TCP session", not a per-send timeout.
        int network_timeout_ms = 0;
    };

    MqttTransport();
    MqttTransport(const Config& config);
    ~MqttTransport();

    using Transport::begin;  // unhide 3-arg sugar
    void begin() override;
    void disconnect() override;
    bool isConnected() const override;
    const char* name() const override { return "MQTT"; }
    void suspend() override;
    void resume() override;

    // Raw IDF config access — called after Courier fills its fields, before init.
    // Use for custom TLS settings, timeouts, etc.
    using ConfigureCallback = std::function<void(esp_mqtt_client_config_t&)>;
    void onConfigure(ConfigureCallback cb);

    // Set the MQTT client ID (must be called before begin()).
    void setClientId(const char* clientId) { _configClientId = clientId ? clientId : ""; }

    // MQTT requires a topic — set options.topic. Serializes the JSON
    // document and routes through publish() with options.qos / .retain.
    bool send(JsonDocument& doc, const SendOptions& options = {}) override;

    // Dynamic topic management.
    // subscribe() adds the topic to the managed list and subscribes immediately
    // if connected; on (re)connect the full list is re-subscribed.
    void subscribe(const char* topic, int qos = 0);

    // As subscribe(), but marks the filter as carrying opaque bytes. MQTT
    // 3.1.1 has no content-type on the wire, so the subscriber declares it.
    // Payloads arriving on a matching topic carry no NUL-termination contract,
    // are dispatched to onBinary(), and never enter the text / JSON lane —
    // so Client::onMessage does not parse them. Calling subscribe() and
    // subscribeBinary() for the same filter: last call wins.
    void subscribeBinary(const char* topic, int qos = 0);

    void unsubscribe(const char* topic);

    // MQTT 3.1.1 topic-filter matching ('+' single level, '#' multi level).
    // Exposed because callers doing their own dispatch inside onMessage()
    // need the same rule the transport uses.
    static bool topicMatches(const char* filter, const char* topic);

    // Publish a raw payload to an explicit topic with optional QoS and retain.
    // Length is taken from strlen(payload) — text only. For bytes that may
    // contain NULs, use publishBinary().
    bool publish(const char* topic, const char* payload, int qos = 0, bool retain = false);

    // How long publish() / publishBinary() wait for the client handle before
    // reporting "busy". This is a lifecycle guard, not serialisation —
    // esp-mqtt takes its own API lock on every entry point, but
    // esp_mqtt_client_destroy takes none and frees the handle. It cannot bound
    // a socket write already in progress inside ESP-IDF (see
    // Config::network_timeout_ms for that).
    static constexpr uint32_t PUBLISH_LOCK_TIMEOUT_MS = 250;

    // Publish an opaque byte payload with an explicit length. Use this for
    // anything that is not a NUL-terminated string (audio frames, images).
    // Rejects data == nullptr and len == 0: esp_mqtt_client_publish treats
    // len <= 0 as "call strlen(data)", which would read past the end of a
    // buffer that carries no terminator. A genuinely empty payload is a text
    // publish — publish(topic, "", qos, retain).
    bool publishBinary(const char* topic, const uint8_t* data, size_t len,
                       int qos = 0, bool retain = false);

    // Publish a JSON document to an explicit topic.
    bool publish(const char* topic, JsonDocument& doc, int qos = 0, bool retain = false);

    // Per-MQTT topic-aware receive hook. Fires for every incoming message,
    // alongside Client::onMessage (which JSON-parses the payload only — no
    // topic). For text-only / non-JSON payloads, this is the only path.
    using TopicMessageCallback =
        std::function<void(const char* topic, const char* payload, size_t length)>;
    void onMessage(TopicMessageCallback cb) { _onTopicMessage = cb; }

    // Per-MQTT binary receive hook. Fires only for topics declared via
    // subscribeBinary() / Config::binaryTopics. Mirrors
    // WebSocketTransport::onBinary, plus the topic MQTT addressing needs.
    //
    // Sized for control-rate traffic: the inbound path is a depth-8 queue
    // drained on the app task at loop() cadence. A sustained high-rate stream
    // (e.g. a 16.7 fps audio downlink) will overflow it — see docs/api.md.
    using TopicBinaryCallback =
        std::function<void(const char* topic, const uint8_t* data, size_t length)>;
    void onBinary(TopicBinaryCallback cb) { _onTopicBinary = cb; }

    // Structured detail for an MQTT_EVENT_ERROR, as reported by ESP-IDF.
    // Which fields are meaningful depends on `type`:
    //   MQTT_ERROR_TYPE_CONNECTION_REFUSED -> connectReturnCode
    //   MQTT_ERROR_TYPE_TCP_TRANSPORT      -> tls* and sockErrno
    // Available identically on ESP-IDF 4.4 and 5.x.
    //
    // gnu++11: the default member initialisers below stop this being an
    // aggregate, so never brace-initialise it with member values. Default
    // construction and value-initialisation (what SpscQueue does) are fine.
    struct ErrorInfo {
        esp_mqtt_error_type_t          type = MQTT_ERROR_TYPE_NONE;
        esp_mqtt_connect_return_code_t connectReturnCode = MQTT_CONNECTION_ACCEPTED;
        esp_err_t tlsLastEspErr      = 0;
        int       tlsStackErr        = 0;
        int       tlsCertVerifyFlags = 0;
        int       sockErrno          = 0;

        // The broker sent a CONNACK with a non-zero return code.
        bool isConnectionRefused() const {
            return type == MQTT_ERROR_TYPE_CONNECTION_REFUSED;
        }

        // CONNACK return code 5. The broker accepted the packet and rejected
        // this client's authorization — distinct from bad credentials (4) and
        // a rejected client ID (2). Retrying unchanged will not help; the
        // application must change its identity. See docs/api.md.
        bool isNotAuthorized() const {
            return type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
                   connectReturnCode == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED;
        }

        // Static, human-readable summary for logs. Never null.
        const char* describe() const;
    };

    // Per-MQTT error hook. Fires for every MQTT_EVENT_ERROR — a CONNACK
    // refusal, a TLS failure, a socket error.
    //
    // Runs on the app task at loop() cadence, with no transport lock held.
    // The callback MAY call disconnect() / begin() re-entrantly; the transport
    // is in a consistent state when it returns. It MUST NOT block — it runs
    // inside Client::loop(), so a blocking HTTPS re-registration here stalls
    // the whole state machine. Record intent and act outside the callback;
    // see docs/api.md.
    //
    // Do not call onError() again from inside the callback — that assigns to
    // this std::function while its target is executing, which is undefined
    // behaviour.
    //
    // Delivery happens while Client is in TransportsConnecting or Connected —
    // the only states from which Client calls loop(). An error queued outside
    // those states is retained, not lost, and delivered on the next loop().
    //
    // disconnect() does not drain the queue: an error queued before a
    // teardown is still delivered on the next loop(), potentially after a
    // subsequent begin() — it reports the error that genuinely happened, not
    // one from the new session.
    //
    // Reporting only: Courier keeps retrying regardless. Recovery policy is
    // the application's.
    using ErrorCallback = std::function<void(const ErrorInfo&)>;
    void onError(ErrorCallback cb) { _onError = cb; }

    void loop() override;

private:
    // Self-healing: track disconnect time for failure escalation
    static constexpr unsigned long SELF_HEAL_TIMEOUT = 60000;  // 60 seconds
    unsigned long _disconnectedSinceMillis = 0;
    bool _selfHealActive = false;

    const char* _certPem = nullptr;
    bool _useCertBundle = true;
    int _taskStack = 8192;
    int _outBufferSize = 0;
    int _networkTimeoutMs = 0;
    ConfigureCallback _configureCallback;

    esp_mqtt_client_handle_t _client = nullptr;
    std::atomic<bool> _connected{false};

    // Two locks, never nested (see Lock.h for why nesting would deadlock
    // against ESP-IDF's own API lock):
    //   _clientLock — guards the client handle's lifetime, not its internal
    //     state: esp-mqtt serialises its own API calls, but
    //     esp_mqtt_client_destroy frees the handle with no lock held. Taken
    //     with a timeout by the publish path, unbounded by
    //     begin()/disconnect()/suspend()/resume().
    //   _topicsLock — held around _topics only, never across an IDF call.
    //     Read by the IDF event task once per connect (subscribeAll).
    TimedMutex _clientLock;
    mutable Mutex _topicsLock;

    std::string _configClientId;   // optional override from Config

    // Managed topic list — single source of truth for subscriptions.
    // Explicit constructor rather than default member initialisers: the
    // Arduino core builds at gnu++11, where NSDMIs would stop this being an
    // aggregate and break brace-initialisation.
    struct Subscription {
        std::string filter;
        int qos;
        bool binary;
        Subscription(std::string f, int q, bool b)
            : filter(std::move(f)), qos(q), binary(b) {}
    };
    std::vector<Subscription> _topics;

    void addSubscription(const char* topic, int qos, bool binary);
    bool isBinaryTopic(const char* topic) const;

    void destroyClient();        // takes _clientLock
    void destroyClientLocked();  // caller already holds _clientLock
    void subscribeAll();  // Subscribe every topic in _topics on the live client

    // PSRAM reassembly buffer for fragmented messages
    char* _reassemblyBuf = nullptr;
    size_t _reassemblyLen = 0;
    size_t _reassemblyPos = 0;
    char* _reassemblyTopic = nullptr;  // topic captured on first chunk
    void freeReassemblyBuf();

    TopicMessageCallback _onTopicMessage;
    TopicBinaryCallback _onTopicBinary;

    // Parallel topic queue, in lockstep with the base class's _pending FIFO.
    // Stores topic strings (heap-allocated, freed on drain).
    static constexpr size_t TOPIC_QUEUE_DEPTH = 8;
    SpscQueue<char*, TOPIC_QUEUE_DEPTH> _topicQueue;

    // Error reports from the IDF event task. Single-producer (that task only)
    // / single-consumer (loop()). Synchronous app-task failures are NOT routed
    // here — they are already visible through return values — which is what
    // keeps the SPSC contract intact.
    static constexpr size_t ERROR_QUEUE_DEPTH = 4;
    SpscQueue<ErrorInfo, ERROR_QUEUE_DEPTH> _errorQueue;
    ErrorCallback _onError;

    void queueIncomingMqttMessage(const char* topic, const char* payload, size_t len);

    static void mqttEventHandler(void* handler_arg,
                                  esp_event_base_t base,
                                  int32_t event_id,
                                  void* event_data);
};

}  // namespace Courier

#endif // COURIER_MQTT_TRANSPORT_H
