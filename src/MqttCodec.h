#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace Courier {
namespace MqttCodec {

// Minimal MQTT 3.1.1 packet codec for the QoS-0 subset used by
// TunnelMqttTransport: CONNECT/CONNACK, SUBSCRIBE/SUBACK,
// UNSUBSCRIBE/UNSUBACK, PUBLISH (QoS 0), PINGREQ/PINGRESP, DISCONNECT.
//
// Encoders write into a caller-provided buffer and return the number of
// bytes written, or 0 when the buffer is too small.

// Longest topic the decoder accepts in an inbound PUBLISH.
constexpr size_t kMaxTopicLen = 256;

size_t encodeVarint(uint8_t* out, size_t cap, uint32_t value);
size_t encodeConnect(uint8_t* out, size_t cap, const char* clientId,
                     uint16_t keepAliveSec);
size_t encodeSubscribe(uint8_t* out, size_t cap, uint16_t packetId,
                       const char* topic);
size_t encodeUnsubscribe(uint8_t* out, size_t cap, uint16_t packetId,
                         const char* topic);
size_t encodePublish(uint8_t* out, size_t cap, const char* topic,
                     const uint8_t* payload, size_t payloadLen);
size_t encodePingReq(uint8_t* out, size_t cap);
size_t encodeDisconnect(uint8_t* out, size_t cap);

// Exact size encodePublish() will produce for this topic/payload — use it
// to size the buffer for large payloads.
size_t publishSize(const char* topic, size_t payloadLen);

// A decoded inbound packet. Pointer fields are views into decoder-owned
// storage, valid only for the duration of the callback. `payload` carries a
// NUL at payload[payloadLen] so JSON consumers can treat it as a C string.
struct Packet {
    enum class Type : uint8_t { ConnAck, SubAck, UnsubAck, Publish, PingResp };
    Type type = Type::ConnAck;
    uint8_t connackReturnCode = 0;   // ConnAck only
    const char* topic = nullptr;     // Publish only, NUL-terminated
    const uint8_t* payload = nullptr;  // Publish only
    size_t payloadLen = 0;           // Publish only
};

// Incremental byte-stream decoder. Bytes may arrive split or coalesced
// arbitrarily; each complete packet in the QoS-0 subset is delivered to the
// callback (other packet types are skipped silently to keep the stream in
// sync). Returns false on a protocol error or an oversized packet — the
// decoder then discards input until reset().
class Decoder {
public:
    using PacketCallback = std::function<void(const Packet&)>;

    explicit Decoder(size_t maxPacketLen = 64 * 1024);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool feed(const uint8_t* data, size_t len, const PacketCallback& cb);
    void reset();

private:
    enum class State : uint8_t { Header, Length, Body, Error };

    bool parseBody(const PacketCallback& cb);
    void freeBody();

    size_t _maxPacketLen;
    State _state = State::Header;
    uint8_t _fixedHeader = 0;
    uint32_t _remainingLen = 0;
    uint32_t _lenMultiplier = 1;
    uint8_t _lenBytes = 0;
    uint8_t* _body = nullptr;
    size_t _bodyPos = 0;
    char _topic[kMaxTopicLen + 1] = {0};
};

}  // namespace MqttCodec
}  // namespace Courier
