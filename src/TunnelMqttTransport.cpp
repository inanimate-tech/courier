#include "TunnelMqttTransport.h"

#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp32-hal.h"  // millis()
static const char* TAG = "TunnelMqtt";
#else
#include <Arduino.h>
#include <cstdio>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "TunnelMqtt";
#endif

namespace Courier {

namespace {

// publish() encodes into the stack up to this size; larger packets take a
// one-off heap allocation (PSRAM-preferring on device).
constexpr size_t kStackEncodeBuf = 512;

uint8_t* allocEncodeBuf(size_t len) {
#ifdef ESP_PLATFORM
    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_8BIT);
    return buf;
#else
    return (uint8_t*)malloc(len);
#endif
}

}  // namespace

TunnelMqttTransport::TunnelMqttTransport() : TunnelMqttTransport(Config{}) {}

TunnelMqttTransport::TunnelMqttTransport(const Config& config)
    : _config(config),
      _clientId(config.clientId ? config.clientId : ""),
      _decoder(config.maxInboundPacket) {}

TunnelMqttTransport::~TunnelMqttTransport() = default;

bool TunnelMqttTransport::pipeWrite(const uint8_t* data, size_t len) {
    if (!_pipeSend) return false;
    if (!_pipeSend(data, len)) return false;
    _lastTxMs = millis();
    return true;
}

void TunnelMqttTransport::maybeConnect() {
    if (!_began || !_pipeUp || _state != State::Down) return;
    unsigned long now = millis();
    if (_connectBlockedUntilMs && now < _connectBlockedUntilMs) return;
    uint8_t buf[160];
    const char* id = _clientId.empty() ? "courier-tunnel" : _clientId.c_str();
    size_t n = MqttCodec::encodeConnect(buf, sizeof(buf), id,
                                        _config.keepAliveSec);
    if (n == 0) {
        ESP_LOGE(TAG, "client id too long for CONNECT");
        return;
    }
    if (!pipeWrite(buf, n)) {
        ESP_LOGW(TAG, "CONNECT send failed (pipe down?)");
        _connectBlockedUntilMs = now + kConnectRetryMs;
        return;
    }
    _connectBlockedUntilMs = 0;
    _decoder.reset();
    _state = State::Connecting;
    _connectSentMs = millis();
    _lastRxMs = millis();
    ESP_LOGI(TAG, "CONNECT sent (client id %s)", id);
}

void TunnelMqttTransport::markDisconnected() {
    if (_state == State::Connected) queueConnectionChange(false);
    _state = State::Down;
}

void TunnelMqttTransport::begin() {
    _began = true;
    maybeConnect();
}

void TunnelMqttTransport::disconnect() {
    if (_state == State::Connected) {
        uint8_t buf[4];
        size_t n = MqttCodec::encodeDisconnect(buf, sizeof(buf));
        pipeWrite(buf, n);
    }
    markDisconnected();
    _began = false;
}

void TunnelMqttTransport::suspend() {
    disconnect();
}

void TunnelMqttTransport::resume() {
    _began = true;
    maybeConnect();
}

void TunnelMqttTransport::notifyPipeUp(bool up) {
    _pipeUp = up;
    if (up) {
        maybeConnect();
    } else if (_state != State::Down) {
        ESP_LOGI(TAG, "pipe down — session lost");
        markDisconnected();
    }
}

void TunnelMqttTransport::injectBytes(const uint8_t* data, size_t len) {
    _lastRxMs = millis();
    bool ok = _decoder.feed(data, len, [this](const MqttCodec::Packet& pkt) {
        handlePacket(pkt);
    });
    if (!ok) {
        ESP_LOGW(TAG, "protocol error on tunnel stream — resetting session");
        _decoder.reset();
        markDisconnected();
        maybeConnect();
    }
}

void TunnelMqttTransport::handlePacket(const MqttCodec::Packet& pkt) {
    switch (pkt.type) {
    case MqttCodec::Packet::Type::ConnAck:
        if (pkt.connackReturnCode != 0) {
            ESP_LOGW(TAG, "CONNACK refused (rc=%u)",
                     (unsigned)pkt.connackReturnCode);
            _state = State::Down;
            _connectBlockedUntilMs = millis() + kConnectRetryMs;
            return;
        }
        _state = State::Connected;
        queueConnectionChange(true);
        ESP_LOGI(TAG, "Connected");
        for (int i = 0; i < _subscriptionCount; i++) {
            sendSubscribe(_subscriptions[i].c_str());
        }
        break;

    case MqttCodec::Packet::Type::Publish:
        if (_onTopicMessage) {
            _onTopicMessage(pkt.topic, (const char*)pkt.payload, pkt.payloadLen);
        }
        break;

    case MqttCodec::Packet::Type::SubAck:
    case MqttCodec::Packet::Type::UnsubAck:
    case MqttCodec::Packet::Type::PingResp:
        break;  // rx timestamp already refreshed in injectBytes
    }
}

void TunnelMqttTransport::sendSubscribe(const char* topic) {
    uint8_t buf[kStackEncodeBuf];
    size_t n = MqttCodec::encodeSubscribe(buf, sizeof(buf), ++_packetId, topic);
    if (n == 0 || !pipeWrite(buf, n)) {
        ESP_LOGW(TAG, "SUBSCRIBE failed: %s", topic);
    }
}

bool TunnelMqttTransport::subscribe(const char* topic, int qos) {
    (void)qos;
    for (int i = 0; i < _subscriptionCount; i++) {
        if (_subscriptions[i] == topic) return true;
    }
    if (_subscriptionCount >= kMaxSubscriptions) {
        ESP_LOGW(TAG, "subscription table full, dropping %s", topic);
        return false;
    }
    _subscriptions[_subscriptionCount++] = topic;
    if (_state == State::Connected) sendSubscribe(topic);
    return true;
}

void TunnelMqttTransport::unsubscribe(const char* topic) {
    for (int i = 0; i < _subscriptionCount; i++) {
        if (_subscriptions[i] == topic) {
            _subscriptions[i] = _subscriptions[--_subscriptionCount];
            break;
        }
    }
    if (_state != State::Connected) return;
    uint8_t buf[kStackEncodeBuf];
    size_t n = MqttCodec::encodeUnsubscribe(buf, sizeof(buf), ++_packetId, topic);
    if (n == 0 || !pipeWrite(buf, n)) {
        ESP_LOGW(TAG, "UNSUBSCRIBE failed: %s", topic);
    }
}

bool TunnelMqttTransport::publish(const char* topic, const char* payload,
                                  int qos, bool retain) {
    if (qos != 0 || retain) {
        // QoS-0 subset: higher levels are accepted but sent as QoS 0.
        ESP_LOGW(TAG, "publish qos=%d retain=%d downgraded to qos0", qos,
                 (int)retain);
    }
    if (_state != State::Connected) return false;
    size_t payloadLen = strlen(payload);
    size_t size = MqttCodec::publishSize(topic, payloadLen);

    uint8_t stackBuf[kStackEncodeBuf];
    uint8_t* buf = stackBuf;
    bool heap = size > sizeof(stackBuf);
    if (heap) {
        buf = allocEncodeBuf(size);
        if (!buf) {
            ESP_LOGE(TAG, "publish alloc failed (%u bytes)", (unsigned)size);
            return false;
        }
    }
    size_t n = MqttCodec::encodePublish(buf, size, topic,
                                        (const uint8_t*)payload, payloadLen);
    bool ok = (n != 0) && pipeWrite(buf, n);
    if (heap) free(buf);
    return ok;
}

bool TunnelMqttTransport::publish(const char* topic, JsonDocument& doc,
                                  int qos, bool retain) {
    std::string json;
    serializeJson(doc, json);
    return publish(topic, json.c_str(), qos, retain);
}

bool TunnelMqttTransport::send(JsonDocument& doc, const SendOptions& options) {
    if (!options.topic) {
        ESP_LOGW(TAG, "send() requires options.topic");
        return false;
    }
    return publish(options.topic, doc, options.qos, options.retain);
}

void TunnelMqttTransport::loop() {
    unsigned long now = millis();

    if (_state == State::Connected) {
        unsigned long keepAliveMs = (unsigned long)_config.keepAliveSec * 1000;
        if (now - _lastRxMs > keepAliveMs + keepAliveMs / 2) {
            ESP_LOGW(TAG, "keepalive timeout — reconnecting");
            markDisconnected();
            maybeConnect();
        } else if (now - _lastTxMs > keepAliveMs / 2) {
            uint8_t buf[4];
            size_t n = MqttCodec::encodePingReq(buf, sizeof(buf));
            pipeWrite(buf, n);
        }
    } else if (_state == State::Connecting) {
        if (now - _connectSentMs > kConnackTimeoutMs) {
            ESP_LOGW(TAG, "CONNACK timeout — retrying CONNECT");
            _state = State::Down;
            maybeConnect();
        }
    } else {
        // Down with the pipe up (failed write, refused CONNACK): retry,
        // paced by _connectBlockedUntilMs.
        maybeConnect();
    }

    drainPending();
}

}  // namespace Courier
