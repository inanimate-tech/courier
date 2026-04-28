#ifndef COURIER_TRANSPORT_H
#define COURIER_TRANSPORT_H

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <atomic>

#include <ArduinoJson.h>

#include "SpscQueue.h"

namespace Courier {

// Per-call options for transport-specific send parameters. Most fields
// are no-ops on transports that don't use them.
struct SendOptions {
    const char* topic = nullptr;   // required for MqttTransport::send; ignored for WS/UDP
    int qos = 0;                    // MQTT QoS (0/1/2)
    bool retain = false;            // MQTT retain flag
};

class Transport {
public:
    using MessageCallback = std::function<void(const char* payload, size_t length)>;
    using BinaryMessageCallback = std::function<void(const uint8_t* data, size_t length)>;
    using ConnectionCallback = std::function<void(Transport* transport, bool connected)>;

    virtual ~Transport() {
        PendingMessage msg;
        while (_pending.pop(msg)) free(msg.payload);
    }

    virtual void begin(const char* host, uint16_t port, const char* path) = 0;
    virtual void disconnect() = 0;
    virtual void loop() { drainPending(); }
    virtual bool isConnected() const = 0;
    virtual bool send(JsonDocument& doc, const SendOptions& options = {}) = 0;
    virtual bool sendBinary(const uint8_t* data, size_t len) { (void)data; (void)len; return false; }
    virtual const char* name() const = 0;

    virtual void suspend() {}
    virtual void resume() {}
    virtual bool isPersistent() const { return true; }

    using FailureCallback = std::function<void()>;
    void setFailureCallback(FailureCallback cb) { _onFailure = cb; }
    void setMessageCallback(MessageCallback cb) { _onMessage = cb; }
    void setBinaryMessageCallback(BinaryMessageCallback cb) { _onBinaryMessage = cb; }
    void setConnectionCallback(ConnectionCallback cb) { _onConnection = cb; }

    // Set by Client when transport is registered. Fires on every text
    // payload regardless of whether the user has registered _onMessage.
    // Used for JSON dispatch via Client::onMessage(type, doc).
    void setClientHook(MessageCallback cb) { _clientHook = cb; }

protected:
    MessageCallback _onMessage;
    BinaryMessageCallback _onBinaryMessage;
    ConnectionCallback _onConnection;
    FailureCallback _onFailure;
    MessageCallback _clientHook;

    struct PendingMessage {
        void*  payload;
        size_t length;
        bool   isBinary;
    };

    static constexpr size_t MESSAGE_QUEUE_DEPTH = 8;
    SpscQueue<PendingMessage, MESSAGE_QUEUE_DEPTH> _pending;

    std::atomic<bool> _connChangePending{false};
    std::atomic<bool> _connChangeState{false};
    std::atomic<bool> _failurePending{false};

    void queueIncomingMessage(const char* payload, size_t len) {
        char* buf = (char*)malloc(len + 1);
        if (!buf) return;
        memcpy(buf, payload, len);
        buf[len] = '\0';
        if (!_pending.push(PendingMessage{buf, len, false})) free(buf);
    }

    void queueIncomingBinary(const uint8_t* data, size_t len) {
        uint8_t* buf = (uint8_t*)malloc(len);
        if (!buf) return;
        memcpy(buf, data, len);
        if (!_pending.push(PendingMessage{buf, len, true})) free(buf);
    }

    void queueConnectionChange(bool connected) {
        _connChangeState.store(connected, std::memory_order_relaxed);
        _connChangePending.store(true, std::memory_order_release);
    }

    void queueTransportFailed() {
        _failurePending.store(true, std::memory_order_release);
    }

    // Drain pending connection-state changes and failure flags. Subclasses
    // that override drainPending() should call this at the end so they
    // don't have to copy the dispatch logic.
    void drainSignals() {
        if (_connChangePending.load(std::memory_order_acquire)) {
            bool state = _connChangeState.load(std::memory_order_relaxed);
            _connChangePending.store(false, std::memory_order_release);
            if (_onConnection) _onConnection(this, state);
        }
        if (_failurePending.load(std::memory_order_acquire)) {
            _failurePending.store(false, std::memory_order_release);
            if (_onFailure) _onFailure();
        }
    }

    // Default drain — pops messages and dispatches via _onMessage /
    // _onBinaryMessage / _clientHook, then drains signals. Subclasses
    // that need different per-message dispatch (e.g. MqttTransport with
    // topic-aware delivery) override this and call drainSignals().
    void drainPending() {
        PendingMessage msg;
        while (_pending.pop(msg)) {
            if (msg.isBinary) {
                if (_onBinaryMessage) _onBinaryMessage((const uint8_t*)msg.payload, msg.length);
            } else {
                if (_onMessage) _onMessage((const char*)msg.payload, msg.length);
                if (_clientHook) _clientHook((const char*)msg.payload, msg.length);
            }
            free(msg.payload);
        }
        drainSignals();
    }
};

}  // namespace Courier

#endif // COURIER_TRANSPORT_H
