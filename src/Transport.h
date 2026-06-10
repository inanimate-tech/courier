#ifndef COURIER_TRANSPORT_H
#define COURIER_TRANSPORT_H

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <atomic>
#include <string>

#include <ArduinoJson.h>

#include "SpscQueue.h"
#include "Endpoint.h"

// Incoming-path failures (allocation, queue overflow) drop the message; log
// them so oversized or bursty traffic doesn't vanish without a trace.
#ifdef ESP_PLATFORM
#include "esp_log.h"
#define COURIER_TRANSPORT_LOGW(fmt, ...) ESP_LOGW("courier", fmt, ##__VA_ARGS__)
#else
#define COURIER_TRANSPORT_LOGW(fmt, ...) \
    fprintf(stderr, "[courier] " fmt "\n", ##__VA_ARGS__)
#endif

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

    // Pure virtual — subclasses read host/port/path from the protected base
    // members _host / _port / _path. setEndpoint() writes those members.
    virtual void begin() = 0;

    // Sugar: setEndpoint then begin. Backwards-compatible call-site signature.
    void begin(const char* host, uint16_t port, const char* path) {
        setEndpoint(host, port, path);
        begin();
    }

    // Stores the endpoint into the protected base members. Strings are copied
    // — caller can pass a String::c_str() temporary safely.
    virtual void setEndpoint(const char* host, uint16_t port, const char* path) {
        _host = host ? host : "";
        _port = port;
        _path = path ? path : "";
    }
    void setEndpoint(const Endpoint& ep) {
        setEndpoint(ep.host, ep.port, ep.path);
    }

    virtual void disconnect() = 0;
    virtual void loop() { drainPending(); }
    virtual bool isConnected() const = 0;
    virtual bool send(JsonDocument& doc, const SendOptions& options = {}) = 0;
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
    // Endpoint storage. Seeded by Client::addTransport<T> from Config; user
    // can override via setEndpoint() before begin() is called.
    std::string _host;
    uint16_t _port = 0;
    std::string _path;

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
        if (!buf) {
            COURIER_TRANSPORT_LOGW("rx alloc failed (%u bytes), message dropped",
                                   (unsigned)len);
            return;
        }
        memcpy(buf, payload, len);
        buf[len] = '\0';
        queueIncomingMessageOwned(buf, len);
    }

    // Zero-copy variant: takes ownership of a heap buffer (malloc /
    // heap_caps_malloc) that already holds the payload with a NUL at
    // buf[len]. Freed by drainPending after dispatch, or here on overflow.
    void queueIncomingMessageOwned(char* buf, size_t len) {
        if (!_pending.push(PendingMessage{buf, len, false})) {
            COURIER_TRANSPORT_LOGW("rx queue full, message dropped (%u bytes)",
                                   (unsigned)len);
            free(buf);
        }
    }

    void queueIncomingBinary(const uint8_t* data, size_t len) {
        uint8_t* buf = (uint8_t*)malloc(len);
        if (!buf) {
            COURIER_TRANSPORT_LOGW("rx alloc failed (%u bytes), binary dropped",
                                   (unsigned)len);
            return;
        }
        memcpy(buf, data, len);
        queueIncomingBinaryOwned(buf, len);
    }

    void queueIncomingBinaryOwned(uint8_t* buf, size_t len) {
        if (!_pending.push(PendingMessage{buf, len, true})) {
            COURIER_TRANSPORT_LOGW("rx queue full, binary dropped (%u bytes)",
                                   (unsigned)len);
            free(buf);
        }
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
    //
    // Contract: every payload handed to the hooks is a heap-owned,
    // NUL-terminated scratch buffer freed immediately after dispatch.
    // _clientHook runs LAST and is allowed to mutate the buffer in place
    // (Client::dispatchJSON parses it zero-copy); _onMessage always sees
    // the untouched bytes. Overriding drains must preserve this order.
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
