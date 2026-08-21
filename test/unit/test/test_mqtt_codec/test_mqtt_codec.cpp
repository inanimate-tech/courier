#include <unity.h>
#include <MqttCodec.h>
#include <cstring>
#include <vector>

using namespace Courier::MqttCodec;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Varint (remaining length) encoding
// ---------------------------------------------------------------------------

static void test_varint_single_byte() {
    uint8_t out[4];
    TEST_ASSERT_EQUAL(1, encodeVarint(out, sizeof(out), 0));
    TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
    TEST_ASSERT_EQUAL(1, encodeVarint(out, sizeof(out), 127));
    TEST_ASSERT_EQUAL_HEX8(0x7F, out[0]);
}

static void test_varint_two_bytes() {
    uint8_t out[4];
    TEST_ASSERT_EQUAL(2, encodeVarint(out, sizeof(out), 128));
    TEST_ASSERT_EQUAL_HEX8(0x80, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[1]);
    TEST_ASSERT_EQUAL(2, encodeVarint(out, sizeof(out), 16383));
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x7F, out[1]);
}

static void test_varint_three_bytes() {
    uint8_t out[4];
    TEST_ASSERT_EQUAL(3, encodeVarint(out, sizeof(out), 16384));
    TEST_ASSERT_EQUAL_HEX8(0x80, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[2]);
}

static void test_varint_overflow_returns_zero() {
    uint8_t out[1];
    TEST_ASSERT_EQUAL(0, encodeVarint(out, sizeof(out), 128));
}

// ---------------------------------------------------------------------------
// Packet encoders — golden bytes from MQTT 3.1.1 (OASIS spec §3)
// ---------------------------------------------------------------------------

static void test_encode_connect_golden() {
    // clientId "abc", keepalive 60, clean session
    const uint8_t expected[] = {0x10, 0x0F, 0x00, 0x04, 'M', 'Q', 'T', 'T',
                                0x04, 0x02, 0x00, 0x3C, 0x00, 0x03, 'a', 'b', 'c'};
    uint8_t out[32];
    size_t n = encodeConnect(out, sizeof(out), "abc", 60);
    TEST_ASSERT_EQUAL(sizeof(expected), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(expected));
}

static void test_encode_connect_buffer_too_small() {
    uint8_t out[8];
    TEST_ASSERT_EQUAL(0, encodeConnect(out, sizeof(out), "abc", 60));
}

static void test_encode_subscribe_golden() {
    // packetId 1, topic "a/b", qos 0
    const uint8_t expected[] = {0x82, 0x08, 0x00, 0x01, 0x00, 0x03, 'a', '/', 'b', 0x00};
    uint8_t out[32];
    size_t n = encodeSubscribe(out, sizeof(out), 1, "a/b");
    TEST_ASSERT_EQUAL(sizeof(expected), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(expected));
}

static void test_encode_unsubscribe_golden() {
    // packetId 2, topic "a/b"
    const uint8_t expected[] = {0xA2, 0x07, 0x00, 0x02, 0x00, 0x03, 'a', '/', 'b'};
    uint8_t out[32];
    size_t n = encodeUnsubscribe(out, sizeof(out), 2, "a/b");
    TEST_ASSERT_EQUAL(sizeof(expected), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(expected));
}

static void test_encode_publish_golden() {
    // topic "t", payload "hi", qos 0, no retain
    const uint8_t expected[] = {0x30, 0x05, 0x00, 0x01, 't', 'h', 'i'};
    uint8_t out[32];
    size_t n = encodePublish(out, sizeof(out), "t",
                             reinterpret_cast<const uint8_t*>("hi"), 2);
    TEST_ASSERT_EQUAL(sizeof(expected), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, sizeof(expected));
}

static void test_publish_size_matches_encode() {
    uint8_t out[64];
    const char* topic = "devices/abc/event";
    const uint8_t payload[] = {1, 2, 3, 4, 5};
    size_t predicted = publishSize(topic, sizeof(payload));
    size_t actual = encodePublish(out, sizeof(out), topic, payload, sizeof(payload));
    TEST_ASSERT_TRUE(actual > 0);
    TEST_ASSERT_EQUAL(predicted, actual);
}

static void test_encode_pingreq_and_disconnect() {
    uint8_t out[4];
    TEST_ASSERT_EQUAL(2, encodePingReq(out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0xC0, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[1]);
    TEST_ASSERT_EQUAL(2, encodeDisconnect(out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0xE0, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[1]);
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

struct Collected {
    std::vector<Packet::Type> types;
    uint8_t lastConnackCode = 0xFF;
    std::string lastTopic;
    std::string lastPayload;
};

static bool feedAll(Decoder& d, const uint8_t* data, size_t len, Collected& c) {
    return d.feed(data, len, [&](const Packet& p) {
        c.types.push_back(p.type);
        if (p.type == Packet::Type::ConnAck) c.lastConnackCode = p.connackReturnCode;
        if (p.type == Packet::Type::Publish) {
            c.lastTopic = p.topic;
            c.lastPayload.assign(reinterpret_cast<const char*>(p.payload), p.payloadLen);
        }
    });
}

static void test_decode_connack() {
    const uint8_t bytes[] = {0x20, 0x02, 0x00, 0x00};
    Decoder d;
    Collected c;
    TEST_ASSERT_TRUE(feedAll(d, bytes, sizeof(bytes), c));
    TEST_ASSERT_EQUAL(1, c.types.size());
    TEST_ASSERT_TRUE(c.types[0] == Packet::Type::ConnAck);
    TEST_ASSERT_EQUAL_HEX8(0x00, c.lastConnackCode);
}

static void test_decode_connack_nonzero_return_code() {
    const uint8_t bytes[] = {0x20, 0x02, 0x00, 0x05};  // not authorized
    Decoder d;
    Collected c;
    TEST_ASSERT_TRUE(feedAll(d, bytes, sizeof(bytes), c));
    TEST_ASSERT_EQUAL_HEX8(0x05, c.lastConnackCode);
}

static void test_decode_publish() {
    const uint8_t bytes[] = {0x30, 0x05, 0x00, 0x01, 't', 'h', 'i'};
    Decoder d;
    Collected c;
    TEST_ASSERT_TRUE(feedAll(d, bytes, sizeof(bytes), c));
    TEST_ASSERT_EQUAL(1, c.types.size());
    TEST_ASSERT_TRUE(c.types[0] == Packet::Type::Publish);
    TEST_ASSERT_EQUAL_STRING("t", c.lastTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("hi", c.lastPayload.c_str());
}

static void test_decode_publish_payload_nul_terminated() {
    // The payload view handed to the callback must have a NUL just past the
    // end so JSON consumers can treat it as a C string.
    const uint8_t bytes[] = {0x30, 0x05, 0x00, 0x01, 't', 'h', 'i'};
    Decoder d;
    bool checked = false;
    TEST_ASSERT_TRUE(d.feed(bytes, sizeof(bytes), [&](const Packet& p) {
        TEST_ASSERT_EQUAL_HEX8(0x00, p.payload[p.payloadLen]);
        checked = true;
    }));
    TEST_ASSERT_TRUE(checked);
}

static void test_decode_byte_at_a_time() {
    const uint8_t bytes[] = {0x30, 0x05, 0x00, 0x01, 't', 'h', 'i'};
    Decoder d;
    Collected c;
    for (size_t i = 0; i < sizeof(bytes); i++) {
        TEST_ASSERT_TRUE(feedAll(d, &bytes[i], 1, c));
    }
    TEST_ASSERT_EQUAL(1, c.types.size());
    TEST_ASSERT_EQUAL_STRING("t", c.lastTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("hi", c.lastPayload.c_str());
}

static void test_decode_two_packets_in_one_feed() {
    const uint8_t bytes[] = {0x20, 0x02, 0x00, 0x00,             // CONNACK
                             0x90, 0x03, 0x00, 0x01, 0x00};      // SUBACK
    Decoder d;
    Collected c;
    TEST_ASSERT_TRUE(feedAll(d, bytes, sizeof(bytes), c));
    TEST_ASSERT_EQUAL(2, c.types.size());
    TEST_ASSERT_TRUE(c.types[0] == Packet::Type::ConnAck);
    TEST_ASSERT_TRUE(c.types[1] == Packet::Type::SubAck);
}

static void test_decode_pingresp() {
    const uint8_t bytes[] = {0xD0, 0x00};
    Decoder d;
    Collected c;
    TEST_ASSERT_TRUE(feedAll(d, bytes, sizeof(bytes), c));
    TEST_ASSERT_EQUAL(1, c.types.size());
    TEST_ASSERT_TRUE(c.types[0] == Packet::Type::PingResp);
}

static void test_decode_unknown_packet_type_ignored() {
    // PUBACK (0x40) is valid MQTT but outside the QoS-0 subset — the decoder
    // must skip it and keep the stream in sync for what follows.
    const uint8_t bytes[] = {0x40, 0x02, 0x00, 0x01,   // PUBACK, skipped
                             0xD0, 0x00};              // PINGRESP
    Decoder d;
    Collected c;
    TEST_ASSERT_TRUE(feedAll(d, bytes, sizeof(bytes), c));
    // Only the PINGRESP is reported (Ignored packets are not surfaced).
    TEST_ASSERT_EQUAL(1, c.types.size());
    TEST_ASSERT_TRUE(c.types[0] == Packet::Type::PingResp);
}

static void test_decode_oversized_packet_rejected() {
    Decoder d(16);  // 16-byte cap
    // PUBLISH with remaining length 100 (> cap)
    const uint8_t bytes[] = {0x30, 0x64};
    Collected c;
    TEST_ASSERT_FALSE(feedAll(d, bytes, sizeof(bytes), c));
}

static void test_decode_reset_recovers() {
    Decoder d(16);
    const uint8_t oversized[] = {0x30, 0x64};
    Collected c;
    TEST_ASSERT_FALSE(feedAll(d, oversized, sizeof(oversized), c));
    d.reset();
    const uint8_t ok[] = {0xD0, 0x00};
    TEST_ASSERT_TRUE(feedAll(d, ok, sizeof(ok), c));
    TEST_ASSERT_EQUAL(1, c.types.size());
}

static void test_decode_topic_too_long_rejected() {
    // Topic length 300 exceeds the decoder's topic bound.
    uint8_t bytes[2 + 2 + 300 + 4];
    bytes[0] = 0x30;
    // remaining length 302 -> varint 0xAE 0x02
    bytes[1] = 0xAE;
    bytes[2] = 0x02;
    bytes[3] = 0x01;  // topic len hi (256)
    bytes[4] = 0x2C;  // topic len lo (44) -> 300
    memset(&bytes[5], 'x', 300);
    Decoder d;
    Collected c;
    TEST_ASSERT_FALSE(feedAll(d, bytes, 5 + 300, c));
}

static void test_roundtrip_publish_larger_payload() {
    std::vector<uint8_t> payload(1000);
    for (size_t i = 0; i < payload.size(); i++) payload[i] = (uint8_t)(i & 0xFF);
    std::vector<uint8_t> buf(publishSize("devices/d1/session-telemetry", payload.size()));
    size_t n = encodePublish(buf.data(), buf.size(), "devices/d1/session-telemetry",
                             payload.data(), payload.size());
    TEST_ASSERT_EQUAL(buf.size(), n);

    Decoder d;
    Collected c;
    TEST_ASSERT_TRUE(feedAll(d, buf.data(), n, c));
    TEST_ASSERT_EQUAL(1, c.types.size());
    TEST_ASSERT_EQUAL_STRING("devices/d1/session-telemetry", c.lastTopic.c_str());
    TEST_ASSERT_EQUAL(payload.size(), c.lastPayload.size());
    TEST_ASSERT_EQUAL_MEMORY(payload.data(), c.lastPayload.data(), payload.size());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_varint_single_byte);
    RUN_TEST(test_varint_two_bytes);
    RUN_TEST(test_varint_three_bytes);
    RUN_TEST(test_varint_overflow_returns_zero);
    RUN_TEST(test_encode_connect_golden);
    RUN_TEST(test_encode_connect_buffer_too_small);
    RUN_TEST(test_encode_subscribe_golden);
    RUN_TEST(test_encode_unsubscribe_golden);
    RUN_TEST(test_encode_publish_golden);
    RUN_TEST(test_publish_size_matches_encode);
    RUN_TEST(test_encode_pingreq_and_disconnect);
    RUN_TEST(test_decode_connack);
    RUN_TEST(test_decode_connack_nonzero_return_code);
    RUN_TEST(test_decode_publish);
    RUN_TEST(test_decode_publish_payload_nul_terminated);
    RUN_TEST(test_decode_byte_at_a_time);
    RUN_TEST(test_decode_two_packets_in_one_feed);
    RUN_TEST(test_decode_pingresp);
    RUN_TEST(test_decode_unknown_packet_type_ignored);
    RUN_TEST(test_decode_oversized_packet_rejected);
    RUN_TEST(test_decode_reset_recovers);
    RUN_TEST(test_decode_topic_too_long_rejected);
    RUN_TEST(test_roundtrip_publish_larger_payload);
    return UNITY_END();
}
