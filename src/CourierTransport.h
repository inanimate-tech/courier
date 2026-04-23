#ifndef COURIER_TRANSPORT_H
#define COURIER_TRANSPORT_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <atomic>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#endif

class CourierTransport {
public:
    using MessageCallback = std::function<void(const char* payload, size_t length)>;
    using BinaryMessageCallback = std::function<void(const uint8_t* data, size_t length)>;
    using ConnectionCallback = std::function<void(CourierTransport* transport, bool connected)>;

    virtual ~CourierTransport() {
#ifdef ESP_PLATFORM
        if (_messageQueue) {
            PendingMessage msg;
            while (xQueueReceive(_messageQueue, &msg, 0) == pdTRUE) {
                free(msg.payload);
            }
            vQueueDelete(_messageQueue);
            _messageQueue = nullptr;
        }
#endif
    }

    virtual void begin(const char* host, uint16_t port, const char* path) = 0;
    virtual void disconnect() = 0;
    virtual void loop() { drainPending(); }
    virtual bool isConnected() const = 0;
    virtual bool send(const char* payload) = 0;
    virtual bool sendBinary(const uint8_t* data, size_t len) { (void)data; (void)len; return false; }
    virtual bool publish(const char* topic, const char* payload) {
        (void)topic;
        return send(payload);  // default: ignore topic, just send
    }
    virtual bool topicRequired() const { return false; }
    virtual const char* name() const = 0;

    // Suspend/resume: stops the transport task to free its stack memory
    // (e.g. before OTA TLS handshake), then restarts without full teardown.
    virtual void suspend() {}
    virtual void resume() {}

    virtual bool isPersistent() const { return true; }

    using FailureCallback = std::function<void()>;
    void setFailureCallback(FailureCallback cb) { _onFailure = cb; }

    void setMessageCallback(MessageCallback cb) { _onMessage = cb; }
    void setBinaryMessageCallback(BinaryMessageCallback cb) { _onBinaryMessage = cb; }
    void setConnectionCallback(ConnectionCallback cb) { _onConnection = cb; }

protected:
    MessageCallback _onMessage;
    BinaryMessageCallback _onBinaryMessage;
    ConnectionCallback _onConnection;
    FailureCallback _onFailure;

    // --- Cross-task pending message FIFO ---
    // Transport event handler pushes via queueIncoming{Message,Binary}.
    // Main loop drains via drainPending() (called from loop()).
    //
    // Servers commonly send a burst of frames on connect (e.g. session +
    // ready + settings in one TCP segment). A single-slot buffer dropped
    // all but the first; a small FIFO absorbs the burst.
    //
    // Still lossy under sustained overload — by design. The main loop is
    // expected to keep up. The FIFO only needs to cover the jitter between
    // the WS task delivering a burst and the main loop draining.

    struct PendingMessage {
        char* payload;
        size_t length;
        bool isBinary;
    };

    static constexpr int MESSAGE_QUEUE_DEPTH = 8;

#ifdef ESP_PLATFORM
    QueueHandle_t _messageQueue = nullptr;

    void ensureMessageQueue() {
        if (!_messageQueue) {
            _messageQueue = xQueueCreate(MESSAGE_QUEUE_DEPTH, sizeof(PendingMessage));
        }
    }
#else
    // Host build fallback: single-slot, single-threaded. No real queue.
    PendingMessage _pendingSlot = {nullptr, 0, false};
    bool _pendingSlotFilled = false;
    void ensureMessageQueue() {}
#endif

    std::atomic<bool> _connChangePending{false};
    std::atomic<bool> _connChangeState{false};
    std::atomic<bool> _failurePending{false};

    // Called from transport event handler (may be on a different task).
    // Copies payload into a malloc'd buffer and enqueues it. Drops if the
    // queue is full or allocation fails.
    void queueIncomingMessage(const char* payload, size_t len) {
        char* buf = (char*)malloc(len + 1);
        if (!buf) return;
        memcpy(buf, payload, len);
        buf[len] = '\0';
        PendingMessage msg{buf, len, false};
#ifdef ESP_PLATFORM
        ensureMessageQueue();
        if (!_messageQueue || xQueueSend(_messageQueue, &msg, 0) != pdTRUE) {
            free(buf);
        }
#else
        if (_pendingSlotFilled) { free(buf); return; }
        _pendingSlot = msg;
        _pendingSlotFilled = true;
#endif
    }

    void queueIncomingBinary(const uint8_t* data, size_t len) {
        uint8_t* buf = (uint8_t*)malloc(len);
        if (!buf) return;
        memcpy(buf, data, len);
        PendingMessage msg{(char*)buf, len, true};
#ifdef ESP_PLATFORM
        ensureMessageQueue();
        if (!_messageQueue || xQueueSend(_messageQueue, &msg, 0) != pdTRUE) {
            free(buf);
        }
#else
        if (_pendingSlotFilled) { free(buf); return; }
        _pendingSlot = msg;
        _pendingSlotFilled = true;
#endif
    }

    // Called from transport event handler.
    void queueConnectionChange(bool connected) {
        _connChangeState.store(connected, std::memory_order_relaxed);
        _connChangePending.store(true, std::memory_order_release);
    }

    void queueTransportFailed() {
        _failurePending.store(true, std::memory_order_release);
    }

    // Called from loop() on the main task. Fires callbacks for every
    // message that accumulated since the last drain.
    void drainPending() {
#ifdef ESP_PLATFORM
        if (_messageQueue) {
            PendingMessage msg;
            while (xQueueReceive(_messageQueue, &msg, 0) == pdTRUE) {
                if (msg.isBinary) {
                    if (_onBinaryMessage) _onBinaryMessage((const uint8_t*)msg.payload, msg.length);
                } else {
                    if (_onMessage) _onMessage(msg.payload, msg.length);
                }
                free(msg.payload);
            }
        }
#else
        if (_pendingSlotFilled) {
            PendingMessage msg = _pendingSlot;
            _pendingSlotFilled = false;
            if (msg.isBinary) {
                if (_onBinaryMessage) _onBinaryMessage((const uint8_t*)msg.payload, msg.length);
            } else {
                if (_onMessage) _onMessage(msg.payload, msg.length);
            }
            free(msg.payload);
        }
#endif
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
};

#endif // COURIER_TRANSPORT_H
