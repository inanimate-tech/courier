#ifndef COURIER_MQTT_TRANSPORT_H
#define COURIER_MQTT_TRANSPORT_H

#include "Transport.h"
#include <mqtt_client.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace Courier {

class MqttTransport : public Transport {
public:
    // Configuration struct for MqttTransport.
    // topics: auto-subscribed on (re)connect.
    // clientId: if set, used as MQTT client ID; otherwise IDF generates one.
    // cert_pem: TLS certificate (nullptr = no cert set by Courier).
    struct Config {
        std::vector<std::string> topics;
        const char* clientId = nullptr;
        const char* cert_pem = nullptr;
        int task_stack = 8192;
    };

    MqttTransport();
    MqttTransport(const Config& config);
    ~MqttTransport();

    void begin(const char* host, uint16_t port, const char* path) override;
    void disconnect() override;
    bool isConnected() const override;
    bool topicRequired() const override { return true; }
    const char* name() const override { return "MQTT"; }
    void suspend() override;
    void resume() override;

    // Raw IDF config access — called after Courier fills its fields, before init.
    // Use for custom TLS settings, timeouts, etc.
    using ConfigureCallback = std::function<void(esp_mqtt_client_config_t&)>;
    void onConfigure(ConfigureCallback cb);

    // Set the MQTT client ID (must be called before begin()).
    void setClientId(const char* clientId) { _configClientId = clientId ? clientId : ""; }

    // MQTT requires a topic — use publish(topic, payload) instead.
    // This override exists only to satisfy Transport's pure-virtual send().
    bool send(const char* payload) override {
        (void)payload;
        return false;
    }

    // Dynamic topic management.
    // subscribe() adds the topic to the managed list and subscribes immediately
    // if connected; on (re)connect the full list is re-subscribed.
    void subscribe(const char* topic, int qos = 0);
    void unsubscribe(const char* topic);

    // Publish to an explicit topic (virtual override — uses QoS 0, no retain).
    bool publish(const char* topic, const char* payload) override;

    // Publish with explicit QoS and retain control.
    bool publish(const char* topic, const char* payload, int qos, bool retain);

    // Per-MQTT topic-aware receive hook. Fires for every incoming message,
    // alongside Client::onMessage (which JSON-parses the payload only — no
    // topic). For text-only / non-JSON payloads, this is the only path.
    using TopicMessageCallback =
        std::function<void(const char* topic, const char* payload, size_t length)>;
    void onMessage(TopicMessageCallback cb) { _onTopicMessage = cb; }

    void loop() override;

private:
    // Self-healing: track disconnect time for failure escalation
    static constexpr unsigned long SELF_HEAL_TIMEOUT = 60000;  // 60 seconds
    unsigned long _disconnectedSinceMillis = 0;
    bool _selfHealActive = false;

    const char* _certPem = nullptr;
    ConfigureCallback _configureCallback;

    esp_mqtt_client_handle_t _client = nullptr;
    std::atomic<bool> _connected{false};

    std::string _configClientId;   // optional override from Config

    // Managed topic list — single source of truth for subscriptions.
    std::vector<std::string> _topics;

    void destroyClient();
    void subscribeAll();  // Subscribe every topic in _topics on the live client

    // PSRAM reassembly buffer for fragmented messages
    char* _reassemblyBuf = nullptr;
    size_t _reassemblyLen = 0;
    size_t _reassemblyPos = 0;
    char* _reassemblyTopic = nullptr;  // topic captured on first chunk
    void freeReassemblyBuf();

    TopicMessageCallback _onTopicMessage;

    // Parallel topic queue, in lockstep with the base class's _pending FIFO.
    // Stores topic strings (heap-allocated, freed on drain).
    static constexpr size_t TOPIC_QUEUE_DEPTH = 8;
    SpscQueue<char*, TOPIC_QUEUE_DEPTH> _topicQueue;

    void queueIncomingMqttMessage(const char* topic, const char* payload, size_t len);

    static void mqttEventHandler(void* handler_arg,
                                  esp_event_base_t base,
                                  int32_t event_id,
                                  void* event_data);
};

}  // namespace Courier

#endif // COURIER_MQTT_TRANSPORT_H
