#pragma once

#include "Transport.h"
#include "MqttCodec.h"

#include <functional>
#include <string>

namespace Courier {

// MQTT (3.1.1, QoS 0 only) spoken over a caller-supplied byte pipe instead
// of an owned network connection — the single-TLS-session alternative to
// MqttTransport. The host wires the pipe to whatever carries the bytes
// (typically WebSocketTransport tagged binary frames): outbound packets go
// through the function set with setPipeSend(); inbound bytes are handed to
// injectBytes(); pipe availability is signalled via notifyPipeUp().
//
// Exposes the MqttTransport API subset consumers use: setClientId,
// subscribe/unsubscribe, publish, topic-aware onMessage, and
// send(doc, options) with options.topic. QoS is accepted for signature
// compatibility and always sent as QoS 0.
//
// Threading: all state-changing calls (begin/loop/injectBytes/subscribe/
// notifyPipeUp) must come from the task running Client::loop(). publish()
// may additionally be called from other tasks — it only reads connection
// state and writes to the pipe, whose implementation must be thread-safe
// (esp_websocket_client send is).
class TunnelMqttTransport : public Transport {
public:
    struct Config {
        const char* clientId = nullptr;
        uint16_t keepAliveSec = 60;
        // Largest inbound packet accepted before the session is reset.
        size_t maxInboundPacket = 64 * 1024;
    };

    using Transport::begin;  // unhide 3-arg sugar
    using PipeSendFn = std::function<bool(const uint8_t* data, size_t len)>;
    using TopicMessageCallback =
        std::function<void(const char* topic, const char* payload, size_t len)>;

    TunnelMqttTransport();
    explicit TunnelMqttTransport(const Config& config);
    ~TunnelMqttTransport() override;

    // --- Pipe wiring (host side) ---
    void setPipeSend(PipeSendFn fn) { _pipeSend = std::move(fn); }
    void notifyPipeUp(bool up);
    void injectBytes(const uint8_t* data, size_t len);

    // --- MqttTransport-compatible surface ---
    void setClientId(const char* id) { _clientId = id ? id : ""; }
    bool subscribe(const char* topic, int qos = 0);
    void unsubscribe(const char* topic);
    bool publish(const char* topic, const char* payload, int qos = 0,
                 bool retain = false);
    bool publish(const char* topic, JsonDocument& doc, int qos = 0,
                 bool retain = false);
    void onMessage(TopicMessageCallback cb) { _onTopicMessage = std::move(cb); }

    // --- Transport interface ---
    void begin() override;
    void disconnect() override;
    void loop() override;
    bool isConnected() const override { return _state == State::Connected; }
    bool send(JsonDocument& doc, const SendOptions& options = {}) override;
    const char* name() const override { return "tunnel-mqtt"; }
    void suspend() override;
    void resume() override;

private:
    enum class State : uint8_t { Down, Connecting, Connected };

    static constexpr int kMaxSubscriptions = 8;
    static constexpr unsigned long kConnackTimeoutMs = 10000;

    bool pipeWrite(const uint8_t* data, size_t len);
    void maybeConnect();
    void markDisconnected();
    void sendSubscribe(const char* topic);
    void handlePacket(const MqttCodec::Packet& pkt);

    Config _config;
    std::string _clientId;
    PipeSendFn _pipeSend;
    TopicMessageCallback _onTopicMessage;
    MqttCodec::Decoder _decoder;

    State _state = State::Down;
    bool _began = false;
    bool _pipeUp = false;
    unsigned long _lastTxMs = 0;
    unsigned long _lastRxMs = 0;
    unsigned long _connectSentMs = 0;
    uint16_t _packetId = 0;

    std::string _subscriptions[kMaxSubscriptions];
    int _subscriptionCount = 0;
};

}  // namespace Courier
