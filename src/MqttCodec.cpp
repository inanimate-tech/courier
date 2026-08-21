#include "MqttCodec.h"

#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace Courier {
namespace MqttCodec {

namespace {

// Sequential writer with overflow tracking: any write past `cap` marks the
// writer failed and the encoder returns 0.
struct Writer {
    // Explicit constructor: aggregate init with default member initializers
    // is C++14, and the Arduino example builds compile at gnu++11.
    Writer(uint8_t* out_, size_t cap_) : out(out_), cap(cap_) {}
    uint8_t* out;
    size_t cap;
    size_t pos = 0;
    bool ok = true;

    void byte(uint8_t b) {
        if (pos >= cap) { ok = false; return; }
        out[pos++] = b;
    }
    void be16(uint16_t v) {
        byte((uint8_t)(v >> 8));
        byte((uint8_t)(v & 0xFF));
    }
    void bytes(const uint8_t* data, size_t len) {
        if (pos + len > cap) { ok = false; return; }
        memcpy(out + pos, data, len);
        pos += len;
    }
    void lenPrefixed(const char* s) {
        size_t len = strlen(s);
        if (len > 0xFFFF) { ok = false; return; }
        be16((uint16_t)len);
        bytes((const uint8_t*)s, len);
    }
    void varint(uint32_t value) {
        do {
            uint8_t b = value % 128;
            value /= 128;
            if (value > 0) b |= 0x80;
            byte(b);
        } while (value > 0 && ok);
    }
    size_t result() const { return ok ? pos : 0; }
};

size_t varintSize(uint32_t value) {
    size_t n = 0;
    do { n++; value /= 128; } while (value > 0);
    return n;
}

uint8_t* allocBody(size_t len) {
#ifdef ESP_PLATFORM
    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_8BIT);
    return buf;
#else
    return (uint8_t*)malloc(len);
#endif
}

}  // namespace

size_t encodeVarint(uint8_t* out, size_t cap, uint32_t value) {
    Writer w{out, cap};
    w.varint(value);
    return w.result();
}

size_t encodeConnect(uint8_t* out, size_t cap, const char* clientId,
                     uint16_t keepAliveSec) {
    size_t idLen = strlen(clientId);
    uint32_t remaining = 10 + 2 + idLen;  // variable header + clientId
    Writer w{out, cap};
    w.byte(0x10);
    w.varint(remaining);
    w.lenPrefixed("MQTT");
    w.byte(0x04);            // protocol level 3.1.1
    w.byte(0x02);            // clean session
    w.be16(keepAliveSec);
    w.lenPrefixed(clientId);
    return w.result();
}

size_t encodeSubscribe(uint8_t* out, size_t cap, uint16_t packetId,
                       const char* topic) {
    uint32_t remaining = 2 + 2 + strlen(topic) + 1;
    Writer w{out, cap};
    w.byte(0x82);
    w.varint(remaining);
    w.be16(packetId);
    w.lenPrefixed(topic);
    w.byte(0x00);            // requested QoS 0
    return w.result();
}

size_t encodeUnsubscribe(uint8_t* out, size_t cap, uint16_t packetId,
                         const char* topic) {
    uint32_t remaining = 2 + 2 + strlen(topic);
    Writer w{out, cap};
    w.byte(0xA2);
    w.varint(remaining);
    w.be16(packetId);
    w.lenPrefixed(topic);
    return w.result();
}

size_t encodePublish(uint8_t* out, size_t cap, const char* topic,
                     const uint8_t* payload, size_t payloadLen) {
    uint32_t remaining = 2 + strlen(topic) + payloadLen;
    Writer w{out, cap};
    w.byte(0x30);            // QoS 0, no retain, no dup
    w.varint(remaining);
    w.lenPrefixed(topic);
    w.bytes(payload, payloadLen);
    return w.result();
}

size_t publishSize(const char* topic, size_t payloadLen) {
    uint32_t remaining = 2 + strlen(topic) + payloadLen;
    return 1 + varintSize(remaining) + remaining;
}

size_t encodePingReq(uint8_t* out, size_t cap) {
    Writer w{out, cap};
    w.byte(0xC0);
    w.byte(0x00);
    return w.result();
}

size_t encodeDisconnect(uint8_t* out, size_t cap) {
    Writer w{out, cap};
    w.byte(0xE0);
    w.byte(0x00);
    return w.result();
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

Decoder::Decoder(size_t maxPacketLen) : _maxPacketLen(maxPacketLen) {}

Decoder::~Decoder() { freeBody(); }

void Decoder::freeBody() {
    free(_body);
    _body = nullptr;
}

void Decoder::reset() {
    freeBody();
    _state = State::Header;
    _remainingLen = 0;
    _lenMultiplier = 1;
    _lenBytes = 0;
    _bodyPos = 0;
}

bool Decoder::feed(const uint8_t* data, size_t len, const PacketCallback& cb) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        switch (_state) {
        case State::Error:
            return false;

        case State::Header:
            _fixedHeader = b;
            _remainingLen = 0;
            _lenMultiplier = 1;
            _lenBytes = 0;
            _state = State::Length;
            break;

        case State::Length:
            _remainingLen += (uint32_t)(b & 0x7F) * _lenMultiplier;
            _lenMultiplier *= 128;
            _lenBytes++;
            if (b & 0x80) {
                if (_lenBytes >= 4) { _state = State::Error; return false; }
                break;
            }
            if (_remainingLen > _maxPacketLen) { _state = State::Error; return false; }
            if (_remainingLen == 0) {
                if (!parseBody(cb)) { _state = State::Error; return false; }
                _state = State::Header;
                break;
            }
            _body = allocBody(_remainingLen + 1);
            if (!_body) { _state = State::Error; return false; }
            _bodyPos = 0;
            _state = State::Body;
            break;

        case State::Body:
            _body[_bodyPos++] = b;
            if (_bodyPos == _remainingLen) {
                _body[_remainingLen] = 0;
                bool ok = parseBody(cb);
                freeBody();
                if (!ok) { _state = State::Error; return false; }
                _state = State::Header;
            }
            break;
        }
    }
    return true;
}

bool Decoder::parseBody(const PacketCallback& cb) {
    Packet pkt;
    uint8_t type = _fixedHeader >> 4;
    switch (type) {
    case 2:  // CONNACK
        if (_remainingLen < 2) return false;
        pkt.type = Packet::Type::ConnAck;
        pkt.connackReturnCode = _body[1];
        break;

    case 3: {  // PUBLISH
        if (_remainingLen < 2) return false;
        size_t topicLen = ((size_t)_body[0] << 8) | _body[1];
        if (topicLen > kMaxTopicLen) return false;
        size_t offset = 2 + topicLen;
        uint8_t qos = (_fixedHeader >> 1) & 0x03;
        if (qos > 0) offset += 2;  // tolerate a QoS>0 publish: skip packet id
        if (offset > _remainingLen) return false;
        memcpy(_topic, _body + 2, topicLen);
        _topic[topicLen] = 0;
        pkt.type = Packet::Type::Publish;
        pkt.topic = _topic;
        pkt.payload = _body + offset;
        pkt.payloadLen = _remainingLen - offset;
        // _body[_remainingLen] is already NUL — payload stays C-string safe.
        break;
    }

    case 9:   // SUBACK
        pkt.type = Packet::Type::SubAck;
        break;
    case 11:  // UNSUBACK
        pkt.type = Packet::Type::UnsubAck;
        break;
    case 13:  // PINGRESP
        pkt.type = Packet::Type::PingResp;
        break;

    default:
        // Outside the QoS-0 subset (e.g. PUBACK) — skip, stream stays in sync.
        return true;
    }
    if (cb) cb(pkt);
    return true;
}

}  // namespace MqttCodec
}  // namespace Courier
