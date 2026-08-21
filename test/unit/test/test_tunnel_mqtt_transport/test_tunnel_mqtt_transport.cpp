#include <unity.h>
#include <Arduino.h>
#include <TunnelMqttTransport.h>
#include <MqttCodec.h>
#include <ArduinoJson.h>
#include <cstring>
#include <string>
#include <vector>

using namespace Courier;

// ---------------------------------------------------------------------------
// Harness: a fake byte pipe that records outbound frames and decodes them.
// ---------------------------------------------------------------------------

namespace {

TunnelMqttTransport* tunnel = nullptr;
std::vector<std::vector<uint8_t>> pipeFrames;
bool pipeSendResult = true;

int connectionEventCount = 0;
bool lastConnectionState = false;

int messageCount = 0;
std::string lastTopic;
std::string lastPayload;

// Decode every recorded outbound frame into a flat packet list.
struct OutPacket {
    MqttCodec::Packet::Type type;  // only used for inbound; outbound uses raw
    uint8_t fixedHeader;
    std::vector<uint8_t> bytes;
};

std::vector<OutPacket> sentPackets() {
    std::vector<OutPacket> out;
    for (const auto& frame : pipeFrames) {
        size_t i = 0;
        while (i < frame.size()) {
            OutPacket p;
            p.fixedHeader = frame[i];
            // parse varint remaining length
            size_t rl = 0, mult = 1, j = i + 1;
            while (true) {
                TEST_ASSERT_TRUE(j < frame.size());
                uint8_t b = frame[j++];
                rl += (b & 0x7F) * mult;
                mult *= 128;
                if (!(b & 0x80)) break;
            }
            p.bytes.assign(frame.begin() + i, frame.begin() + j + rl);
            i = j + rl;
            out.push_back(p);
        }
    }
    return out;
}

int countPackets(uint8_t fixedHeader) {
    int n = 0;
    for (const auto& p : sentPackets()) {
        if (p.fixedHeader == fixedHeader) n++;
    }
    return n;
}

TunnelMqttTransport* createTunnel(uint16_t keepAliveSec = 60) {
    pipeFrames.clear();
    pipeSendResult = true;
    connectionEventCount = 0;
    lastConnectionState = false;
    messageCount = 0;
    lastTopic.clear();
    lastPayload.clear();
    _mock_millis = 1000;

    TunnelMqttTransport::Config cfg;
    cfg.keepAliveSec = keepAliveSec;
    auto* t = new TunnelMqttTransport(cfg);
    t->setClientId("stick-dev1");
    t->setPipeSend([](const uint8_t* data, size_t len) {
        if (!pipeSendResult) return false;
        pipeFrames.emplace_back(data, data + len);
        return true;
    });
    t->setConnectionCallback([](Transport*, bool connected) {
        connectionEventCount++;
        lastConnectionState = connected;
    });
    t->onMessage([](const char* topic, const char* payload, size_t len) {
        messageCount++;
        lastTopic = topic;
        lastPayload.assign(payload, len);
    });
    return t;
}

void injectConnack(TunnelMqttTransport* t, uint8_t rc = 0) {
    const uint8_t connack[] = {0x20, 0x02, 0x00, rc};
    t->injectBytes(connack, sizeof(connack));
}

void bringUp(TunnelMqttTransport* t) {
    t->begin();
    t->notifyPipeUp(true);
    injectConnack(t);
    t->loop();
}

}  // namespace

void setUp() {}
void tearDown() {
    delete tunnel;
    tunnel = nullptr;
}

// ---------------------------------------------------------------------------
// Connect handshake
// ---------------------------------------------------------------------------

void test_no_connect_before_pipe_up() {
    tunnel = createTunnel();
    tunnel->begin();
    TEST_ASSERT_EQUAL(0, pipeFrames.size());
}

void test_connect_sent_when_pipe_up() {
    tunnel = createTunnel();
    tunnel->begin();
    tunnel->notifyPipeUp(true);
    TEST_ASSERT_EQUAL(1, countPackets(0x10));
    // CONNECT carries the configured client id
    auto pkts = sentPackets();
    std::string asStr(pkts[0].bytes.begin(), pkts[0].bytes.end());
    TEST_ASSERT_TRUE(asStr.find("stick-dev1") != std::string::npos);
}

void test_connect_sent_when_begin_follows_pipe_up() {
    tunnel = createTunnel();
    tunnel->notifyPipeUp(true);
    TEST_ASSERT_EQUAL(0, pipeFrames.size());
    tunnel->begin();
    TEST_ASSERT_EQUAL(1, countPackets(0x10));
}

void test_not_connected_until_connack() {
    tunnel = createTunnel();
    tunnel->begin();
    tunnel->notifyPipeUp(true);
    TEST_ASSERT_FALSE(tunnel->isConnected());
    injectConnack(tunnel);
    tunnel->loop();
    TEST_ASSERT_TRUE(tunnel->isConnected());
    TEST_ASSERT_EQUAL(1, connectionEventCount);
    TEST_ASSERT_TRUE(lastConnectionState);
}

void test_connack_error_code_stays_disconnected() {
    tunnel = createTunnel();
    tunnel->begin();
    tunnel->notifyPipeUp(true);
    injectConnack(tunnel, 0x05);  // not authorized
    tunnel->loop();
    TEST_ASSERT_FALSE(tunnel->isConnected());
    TEST_ASSERT_EQUAL(0, connectionEventCount);
}

// ---------------------------------------------------------------------------
// Subscriptions
// ---------------------------------------------------------------------------

void test_subscriptions_flushed_after_connack() {
    tunnel = createTunnel();
    tunnel->subscribe("devices/dev1/command");
    tunnel->subscribe("devices/+/event");
    bringUp(tunnel);
    TEST_ASSERT_EQUAL(2, countPackets(0x82));
}

void test_subscribe_while_connected_sends_immediately() {
    tunnel = createTunnel();
    bringUp(tunnel);
    TEST_ASSERT_EQUAL(0, countPackets(0x82));
    tunnel->subscribe("devices/dev1/system");
    TEST_ASSERT_EQUAL(1, countPackets(0x82));
}

void test_resubscribe_after_reconnect() {
    tunnel = createTunnel(10);
    tunnel->subscribe("devices/dev1/command");
    bringUp(tunnel);
    TEST_ASSERT_EQUAL(1, countPackets(0x82));

    // Kill the session via keepalive timeout, then reconnect.
    _mock_millis += 10 * 1500 + 1;
    tunnel->loop();                  // death + fresh CONNECT
    TEST_ASSERT_FALSE(tunnel->isConnected());
    injectConnack(tunnel);
    tunnel->loop();
    TEST_ASSERT_TRUE(tunnel->isConnected());
    TEST_ASSERT_EQUAL(2, countPackets(0x82));  // resubscribed
}

void test_unsubscribe_sends_packet_and_forgets() {
    tunnel = createTunnel();
    tunnel->subscribe("devices/dev1/command");
    bringUp(tunnel);
    tunnel->unsubscribe("devices/dev1/command");
    TEST_ASSERT_EQUAL(1, countPackets(0xA2));

    // A later reconnect must not resubscribe the forgotten topic.
    tunnel->notifyPipeUp(false);
    tunnel->loop();
    tunnel->notifyPipeUp(true);
    injectConnack(tunnel);
    tunnel->loop();
    TEST_ASSERT_EQUAL(1, countPackets(0x82));  // only the original subscribe
}

// ---------------------------------------------------------------------------
// Publish
// ---------------------------------------------------------------------------

void test_publish_sends_decodable_packet() {
    tunnel = createTunnel();
    bringUp(tunnel);
    size_t before = pipeFrames.size();
    TEST_ASSERT_TRUE(tunnel->publish("devices/dev1/event", "{\"name\":\"x\"}"));
    TEST_ASSERT_EQUAL(before + 1, pipeFrames.size());

    MqttCodec::Decoder d;
    bool saw = false;
    d.feed(pipeFrames.back().data(), pipeFrames.back().size(),
           [&](const MqttCodec::Packet& p) {
               TEST_ASSERT_TRUE(p.type == MqttCodec::Packet::Type::Publish);
               TEST_ASSERT_EQUAL_STRING("devices/dev1/event", p.topic);
               TEST_ASSERT_EQUAL_STRING("{\"name\":\"x\"}",
                                        reinterpret_cast<const char*>(p.payload));
               saw = true;
           });
    TEST_ASSERT_TRUE(saw);
}

void test_publish_while_disconnected_fails() {
    tunnel = createTunnel();
    tunnel->begin();
    TEST_ASSERT_FALSE(tunnel->publish("t", "x"));
    TEST_ASSERT_EQUAL(0, pipeFrames.size());
}

void test_publish_qos_downgraded_to_zero() {
    tunnel = createTunnel();
    bringUp(tunnel);
    TEST_ASSERT_TRUE(tunnel->publish("t", "x", 1, false));
    // QoS-0 PUBLISH fixed header, no packet id
    TEST_ASSERT_EQUAL(1, countPackets(0x30));
}

void test_publish_pipe_failure_returns_false() {
    tunnel = createTunnel();
    bringUp(tunnel);
    pipeSendResult = false;
    TEST_ASSERT_FALSE(tunnel->publish("t", "x"));
}

void test_send_with_topic_publishes_json() {
    tunnel = createTunnel();
    bringUp(tunnel);
    JsonDocument doc;
    doc["type"] = "hello";
    SendOptions opts;
    opts.topic = "devices/dev1/system";
    TEST_ASSERT_TRUE(tunnel->send(doc, opts));
    TEST_ASSERT_EQUAL(1, countPackets(0x30));
}

void test_send_without_topic_fails() {
    tunnel = createTunnel();
    bringUp(tunnel);
    JsonDocument doc;
    doc["type"] = "hello";
    SendOptions opts;
    TEST_ASSERT_FALSE(tunnel->send(doc, opts));
}

// ---------------------------------------------------------------------------
// Inbound delivery
// ---------------------------------------------------------------------------

void test_inbound_publish_delivered_to_callback() {
    tunnel = createTunnel();
    bringUp(tunnel);
    uint8_t buf[64];
    size_t n = MqttCodec::encodePublish(
        buf, sizeof(buf), "devices/dev1/command",
        reinterpret_cast<const uint8_t*>("{\"type\":\"app\"}"), 14);
    tunnel->injectBytes(buf, n);
    TEST_ASSERT_EQUAL(1, messageCount);
    TEST_ASSERT_EQUAL_STRING("devices/dev1/command", lastTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"app\"}", lastPayload.c_str());
}

void test_inbound_split_across_injects() {
    tunnel = createTunnel();
    bringUp(tunnel);
    uint8_t buf[64];
    size_t n = MqttCodec::encodePublish(buf, sizeof(buf), "t",
                                        reinterpret_cast<const uint8_t*>("hi"), 2);
    tunnel->injectBytes(buf, 3);
    TEST_ASSERT_EQUAL(0, messageCount);
    tunnel->injectBytes(buf + 3, n - 3);
    TEST_ASSERT_EQUAL(1, messageCount);
}

// ---------------------------------------------------------------------------
// Keepalive
// ---------------------------------------------------------------------------

void test_pingreq_sent_when_idle() {
    tunnel = createTunnel(60);
    bringUp(tunnel);
    TEST_ASSERT_EQUAL(0, countPackets(0xC0));
    _mock_millis += 30 * 1000 + 1;  // half the keepalive
    tunnel->loop();
    TEST_ASSERT_EQUAL(1, countPackets(0xC0));
}

void test_keepalive_timeout_disconnects_and_reconnects() {
    tunnel = createTunnel(60);
    bringUp(tunnel);
    TEST_ASSERT_EQUAL(1, countPackets(0x10));
    _mock_millis += 90 * 1000 + 1;  // past 1.5x keepalive with no inbound
    tunnel->loop();
    TEST_ASSERT_FALSE(tunnel->isConnected());
    TEST_ASSERT_EQUAL(2, connectionEventCount);
    TEST_ASSERT_FALSE(lastConnectionState);
    TEST_ASSERT_EQUAL(2, countPackets(0x10));  // fresh CONNECT attempt
}

void test_pingresp_keeps_session_alive() {
    tunnel = createTunnel(60);
    bringUp(tunnel);
    _mock_millis += 30 * 1000 + 1;
    tunnel->loop();  // PINGREQ out
    const uint8_t pingresp[] = {0xD0, 0x00};
    tunnel->injectBytes(pingresp, sizeof(pingresp));
    _mock_millis += 60 * 1000;  // 1.5x keepalive from the PINGRESP has not elapsed
    tunnel->loop();
    TEST_ASSERT_TRUE(tunnel->isConnected());
}

// ---------------------------------------------------------------------------
// Pipe / lifecycle
// ---------------------------------------------------------------------------

void test_pipe_down_disconnects() {
    tunnel = createTunnel();
    bringUp(tunnel);
    tunnel->notifyPipeUp(false);
    tunnel->loop();
    TEST_ASSERT_FALSE(tunnel->isConnected());
    TEST_ASSERT_EQUAL(2, connectionEventCount);
    TEST_ASSERT_FALSE(lastConnectionState);
}

void test_pipe_up_again_reconnects() {
    tunnel = createTunnel();
    bringUp(tunnel);
    tunnel->notifyPipeUp(false);
    tunnel->loop();
    tunnel->notifyPipeUp(true);
    TEST_ASSERT_EQUAL(2, countPackets(0x10));
    injectConnack(tunnel);
    tunnel->loop();
    TEST_ASSERT_TRUE(tunnel->isConnected());
}

void test_disconnect_sends_disconnect_packet() {
    tunnel = createTunnel();
    bringUp(tunnel);
    tunnel->disconnect();
    TEST_ASSERT_EQUAL(1, countPackets(0xE0));
    TEST_ASSERT_FALSE(tunnel->isConnected());
}

void test_is_persistent() {
    tunnel = createTunnel();
    TEST_ASSERT_TRUE(tunnel->isPersistent());
}

void test_name() {
    tunnel = createTunnel();
    TEST_ASSERT_EQUAL_STRING("tunnel-mqtt", tunnel->name());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_no_connect_before_pipe_up);
    RUN_TEST(test_connect_sent_when_pipe_up);
    RUN_TEST(test_connect_sent_when_begin_follows_pipe_up);
    RUN_TEST(test_not_connected_until_connack);
    RUN_TEST(test_connack_error_code_stays_disconnected);
    RUN_TEST(test_subscriptions_flushed_after_connack);
    RUN_TEST(test_subscribe_while_connected_sends_immediately);
    RUN_TEST(test_resubscribe_after_reconnect);
    RUN_TEST(test_unsubscribe_sends_packet_and_forgets);
    RUN_TEST(test_publish_sends_decodable_packet);
    RUN_TEST(test_publish_while_disconnected_fails);
    RUN_TEST(test_publish_qos_downgraded_to_zero);
    RUN_TEST(test_publish_pipe_failure_returns_false);
    RUN_TEST(test_send_with_topic_publishes_json);
    RUN_TEST(test_send_without_topic_fails);
    RUN_TEST(test_inbound_publish_delivered_to_callback);
    RUN_TEST(test_inbound_split_across_injects);
    RUN_TEST(test_pingreq_sent_when_idle);
    RUN_TEST(test_keepalive_timeout_disconnects_and_reconnects);
    RUN_TEST(test_pingresp_keeps_session_alive);
    RUN_TEST(test_pipe_down_disconnects);
    RUN_TEST(test_disconnect_sends_disconnect_packet);
    RUN_TEST(test_pipe_up_again_reconnects);
    RUN_TEST(test_is_persistent);
    RUN_TEST(test_name);
    return UNITY_END();
}
