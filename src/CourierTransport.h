#ifndef COURIER_TRANSPORT_H
#define COURIER_TRANSPORT_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <atomic>

class CourierTransport {
public:
    using MessageCallback = std::function<void(const char* payload, size_t length)>;
    using ConnectionCallback = std::function<void(CourierTransport* transport, bool connected)>;

    virtual ~CourierTransport() {
        free(_pendingPayload);
    }

    virtual void begin(const char* host, uint16_t port, const char* path) = 0;
    virtual void disconnect() = 0;
    virtual void loop() { drainPending(); }
    virtual bool isConnected() const = 0;
    virtual bool sendMessage(const char* payload) = 0;
    virtual bool sendBinary(const uint8_t* data, size_t len) { (void)data; (void)len; return false; }
    virtual bool publishTo(const char* topic, const char* payload, int qos = 0, bool retain = false) {
        (void)topic; (void)qos; (void)retain;
        return sendMessage(payload);  // default: ignore topic, just send
    }
    virtual const char* name() const = 0;

    // Suspend/resume: stops the transport task to free its stack memory
    // (e.g. before OTA TLS handshake), then restarts without full teardown.
    virtual void suspend() {}
    virtual void resume() {}

    void setMessageCallback(MessageCallback cb) { _onMessage = cb; }
    void setConnectionCallback(ConnectionCallback cb) { _onConnection = cb; }

protected:
    MessageCallback _onMessage;
    ConnectionCallback _onConnection;

    // --- Cross-task pending message buffer ---
    // Transport task writes via queueIncomingMessage/queueConnectionChange.
    // Main loop drains via drainPending() (called from loop()).

    char* _pendingPayload = nullptr;
    size_t _pendingLength = 0;
    std::atomic<bool> _msgPending{false};

    std::atomic<bool> _connChangePending{false};
    std::atomic<bool> _connChangeState{false};

    // NOTE: Single-slot pending buffer. If the main loop doesn't call loop()
    // fast enough, messages arriving while a previous message is pending will
    // be silently dropped. This is a deliberate trade-off for minimal RAM
    // usage on ESP32. For high-throughput use cases, consider replacing with
    // a FreeRTOS queue or ring buffer.

    // Called from transport event handler (may be on a different task).
    // Copies payload into a malloc'd buffer and sets the pending flag.
    void queueIncomingMessage(const char* payload, size_t len) {
        if (_msgPending.load(std::memory_order_acquire)) {
            // Previous message still pending — drop this one
            return;
        }
        char* buf = (char*)malloc(len + 1);
        if (!buf) return;
        memcpy(buf, payload, len);
        buf[len] = '\0';
        _pendingPayload = buf;
        _pendingLength = len;
        _msgPending.store(true, std::memory_order_release);
    }

    // Called from transport event handler.
    void queueConnectionChange(bool connected) {
        _connChangeState.store(connected, std::memory_order_relaxed);
        _connChangePending.store(true, std::memory_order_release);
    }

    // Called from loop() on the main task. Fires callbacks if pending.
    void drainPending() {
        if (_msgPending.load(std::memory_order_acquire)) {
            if (_onMessage) _onMessage(_pendingPayload, _pendingLength);
            free(_pendingPayload);
            _pendingPayload = nullptr;
            _msgPending.store(false, std::memory_order_release);
        }
        if (_connChangePending.load(std::memory_order_acquire)) {
            bool state = _connChangeState.load(std::memory_order_relaxed);
            _connChangePending.store(false, std::memory_order_release);
            if (_onConnection) _onConnection(this, state);
        }
    }
};

#endif // COURIER_TRANSPORT_H
