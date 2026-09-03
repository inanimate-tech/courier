#include <unity.h>
#include <MqttTransport.h>
#include <mqtt_client.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using namespace Courier;

static int deliveredMessageCount = 0;
static constexpr size_t DELIVERED_BUF_SIZE = 12288;
static char lastDeliveredPayload[DELIVERED_BUF_SIZE] = "";
static size_t lastDeliveredLength = 0;

static void onMessageCallback(const char* payload, size_t length) {
    deliveredMessageCount++;
    lastDeliveredLength = length;
    size_t copyLen = length < DELIVERED_BUF_SIZE - 1 ? length : DELIVERED_BUF_SIZE - 1;
    memcpy(lastDeliveredPayload, payload, copyLen);
    lastDeliveredPayload[copyLen] = '\0';
}

static int connectionEventCount = 0;
static bool lastConnectionState = false;

static void onConnectionCallback(Transport* transport, bool connected) {
    connectionEventCount++;
    lastConnectionState = connected;
}

static int errorCount = 0;
static MqttTransport::ErrorInfo lastError;
static MqttTransport::ErrorInfo errorSequence[8];

static void onErrorCallback(const MqttTransport::ErrorInfo& err) {
    if (errorCount < 8) errorSequence[errorCount] = err;
    errorCount++;
    lastError = err;
}

static MqttTransport* mqtt = nullptr;

void setUp(void) {
    MockMqttClient::resetInstanceCount();
    deliveredMessageCount = 0;
    lastDeliveredPayload[0] = '\0';
    lastDeliveredLength = 0;
    connectionEventCount = 0;
    lastConnectionState = false;
    errorCount = 0;
    lastError = MqttTransport::ErrorInfo();
    for (auto& e : errorSequence) e = MqttTransport::ErrorInfo();
}

void tearDown(void) {
    delete mqtt;
    mqtt = nullptr;
}

static MqttTransport* createWithTopics(const char* deviceId = "dev123",
                                                const char* deviceType = "sensor")
{
    std::string commandTopic = std::string("devices/") + deviceId + "/command";
    std::string statusTopic  = std::string("devices/") + deviceId + "/status";
    std::string eventTopic   = std::string("devices/") + deviceId + "/event";
    std::string allEvents    = "devices/+/event";

    MqttTransport::Config cfg;
    cfg.topics = {commandTopic, statusTopic, allEvents};
    std::string clientId = std::string(deviceType) + "-" + deviceId;
    cfg.clientId = clientId.c_str();

    auto* t = new MqttTransport(cfg);
    t->setMessageCallback(onMessageCallback);
    t->setConnectionCallback(onConnectionCallback);
    return t;
}

void test_name_is_mqtt() {
    mqtt = new MqttTransport();
    TEST_ASSERT_EQUAL_STRING("MQTT", mqtt->name());
}

void test_begin_constructs_wss_uri() {
    mqtt = createWithTopics();
    mqtt->begin("example.com", 443, "/agents/broker/room456");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL_STRING("wss://example.com:443/agents/broker/room456", client->uri.c_str());
}

void test_begin_sets_client_id() {
    mqtt = createWithTopics("dev123", "sensor");
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("sensor-dev123", client->clientId.c_str());
}

void test_begin_without_client_id_uses_empty() {
    mqtt = new MqttTransport();
    mqtt->setMessageCallback(onMessageCallback);
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->clientId.empty());
}

void test_begin_starts_client() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->started);
}

void test_begin_no_cert_by_default_and_disables_auto_reconnect() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->cert_pem.empty());           // bundle, not a pinned PEM
    TEST_ASSERT_NOT_NULL(client->crt_bundle_attach);      // IDF cert bundle by default
    TEST_ASSERT_FALSE(client->disable_auto_reconnect);  // auto-reconnect enabled for self-healing
}

void test_subscribes_to_configured_topics() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    TEST_ASSERT_EQUAL(3, client->subscriptionCount);
    TEST_ASSERT_EQUAL_STRING("devices/dev123/command", client->subscribedTopics[0].c_str());
    TEST_ASSERT_EQUAL_STRING("devices/dev123/status", client->subscribedTopics[1].c_str());
    TEST_ASSERT_EQUAL_STRING("devices/+/event", client->subscribedTopics[2].c_str());
}

void test_connected_after_connect_event() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    TEST_ASSERT_FALSE(mqtt->isConnected());
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    TEST_ASSERT_TRUE(mqtt->isConnected());
    TEST_ASSERT_EQUAL(1, connectionEventCount);
    TEST_ASSERT_TRUE(lastConnectionState);
}

void test_disconnected_after_disconnect_event() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    client->simulateDisconnect();
    mqtt->loop();
    TEST_ASSERT_FALSE(mqtt->isConnected());
    TEST_ASSERT_EQUAL(2, connectionEventCount);
    TEST_ASSERT_FALSE(lastConnectionState);
}

void test_command_message_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/command", "{\"type\":\"test\",\"value\":42}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"test\",\"value\":42}", lastDeliveredPayload);
}

void test_status_message_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/status", "{\"type\":\"ota\",\"action\":\"check\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
}

void test_own_event_topic_message_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/event", "{\"type\":\"app_event\",\"name\":\"test\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
}

void test_other_device_event_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/other789/event", "{\"type\":\"app_event\",\"name\":\"test\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
}

void test_send_requires_topic() {
    // send() returns false unless options.topic is set.
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    JsonDocument doc;
    doc["type"] = "test";

    bool result = mqtt->send(doc);  // no topic
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_EQUAL(0, client->publishCount);
}

void test_send_with_topic_publishes() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    JsonDocument doc;
    doc["msg"] = "telemetry";
    SendOptions opts;
    opts.topic = "sensors/me";
    opts.qos = 1;
    opts.retain = true;
    bool result = mqtt->send(doc, opts);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(1, client->publishCount);
    TEST_ASSERT_EQUAL_STRING("sensors/me", client->lastPublishTopic.c_str());
    TEST_ASSERT_NOT_NULL(strstr(client->lastPublishPayload.c_str(), "telemetry"));
}

void test_publish_json_overload_serializes() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    JsonDocument doc;
    doc["temp"] = 22.5;
    bool result = mqtt->publish("sensors/temp", doc);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(1, client->publishCount);
    TEST_ASSERT_EQUAL_STRING("sensors/temp", client->lastPublishTopic.c_str());
    TEST_ASSERT_NOT_NULL(strstr(client->lastPublishPayload.c_str(), "22.5"));
}

void test_publish_raw_payload() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    bool result = mqtt->publish("foo/bar", "hello", 0, false);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(1, client->publishCount);
    TEST_ASSERT_EQUAL_STRING("foo/bar", client->lastPublishTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("hello", client->lastPublishPayload.c_str());
}

void test_publish_sends_to_explicit_topic() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    bool ok = mqtt->publish("my/topic", R"({"hello":1})");
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("my/topic", client->lastPublishTopic.c_str());
    TEST_ASSERT_EQUAL_STRING(R"({"hello":1})", client->lastPublishPayload.c_str());
    TEST_ASSERT_EQUAL(1, client->publishCount);
}

void test_publish_fails_when_disconnected() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    // No simulateConnect() — transport is not connected
    bool ok = mqtt->publish("my/topic", R"({"x":1})");
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL(0, client->publishCount);
}

void test_publish_with_qos_and_retain() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    bool ok = mqtt->publish("my/topic", "{\"x\":1}", 1, true);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, client->lastPublishQos);
    TEST_ASSERT_TRUE(client->lastPublishRetain);
}

void test_disconnect_sets_not_connected() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    mqtt->disconnect();
    TEST_ASSERT_FALSE(mqtt->isConnected());
}

void test_reconnect_after_disconnect() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    TEST_ASSERT_TRUE(mqtt->isConnected());
    client->simulateDisconnect();
    mqtt->loop();
    TEST_ASSERT_FALSE(mqtt->isConnected());
    mqtt->begin("host", 443, "/path");
    client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->started);
    client->simulateConnect();
    mqtt->loop();
    TEST_ASSERT_TRUE(mqtt->isConnected());
    TEST_ASSERT_EQUAL(3, client->subscriptionCount);
    TEST_ASSERT_EQUAL_STRING("devices/dev123/command", client->subscribedTopics[0].c_str());
    TEST_ASSERT_EQUAL_STRING("devices/dev123/status", client->subscribedTopics[1].c_str());
    TEST_ASSERT_EQUAL_STRING("devices/+/event", client->subscribedTopics[2].c_str());
}

void test_reconnect_with_new_path() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/agents/broker/room456");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    mqtt->begin("host", 443, "/agents/broker/room789");
    client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("wss://host:443/agents/broker/room789", client->uri.c_str());
    client->simulateConnect();
    TEST_ASSERT_EQUAL(3, client->subscriptionCount);
}

void test_multiple_connect_disconnect_cycles() {
    mqtt = createWithTopics();
    for (int cycle = 0; cycle < 3; cycle++) {
        mqtt->begin("host", 443, "/path");
        auto* client = MockMqttClient::lastInstance();
        client->simulateConnect();
        mqtt->loop();
        TEST_ASSERT_TRUE(mqtt->isConnected());
        TEST_ASSERT_EQUAL(3, client->subscriptionCount);
        client->simulateDisconnect();
        mqtt->loop();
        TEST_ASSERT_FALSE(mqtt->isConnected());
        mqtt->disconnect();
    }
    TEST_ASSERT_EQUAL(6, connectionEventCount);
    TEST_ASSERT_EQUAL(3, MockMqttClient::instanceCount());
}

void test_reconnect_creates_fresh_client() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    TEST_ASSERT_EQUAL(1, MockMqttClient::instanceCount());
    mqtt->begin("host", 443, "/path");
    TEST_ASSERT_EQUAL(2, MockMqttClient::instanceCount());
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(0, client->subscriptionCount);
    TEST_ASSERT_TRUE(client->started);
}

void test_reconnect_exact_subscription_count() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/agents/broker/room456");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    TEST_ASSERT_EQUAL(3, client->subscriptionCount);
    mqtt->disconnect();
    mqtt->begin("host", 443, "/agents/broker/room789");
    client = MockMqttClient::lastInstance();
    client->simulateConnect();
    TEST_ASSERT_EQUAL(3, client->subscriptionCount);
}

void test_reconnect_message_delivered_exactly_once() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    mqtt->disconnect();
    mqtt->begin("host", 443, "/path");
    client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    client->simulateMessage("devices/dev123/command", "{\"type\":\"test\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
}

void test_reconnect_connect_event_fires_once() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, connectionEventCount);
    mqtt->disconnect();
    connectionEventCount = 0;
    mqtt->begin("host", 443, "/path");
    client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, connectionEventCount);
}

void test_multiple_room_changes_no_accumulation() {
    const char* rooms[] = {"roomA", "roomB", "roomC"};
    mqtt = createWithTopics();
    for (int i = 0; i < 3; i++) {
        if (i > 0) mqtt->disconnect();
        std::string path = "/agents/broker/";
        path += rooms[i];
        mqtt->begin("host", 443, path.c_str());
        auto* client = MockMqttClient::lastInstance();
        client->simulateConnect();
        mqtt->loop();
        TEST_ASSERT_EQUAL(3, client->subscriptionCount);
    }
    TEST_ASSERT_EQUAL(3, MockMqttClient::instanceCount());
    deliveredMessageCount = 0;
    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/command", "{\"type\":\"test\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
}

void test_large_command_message_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    std::string largePayload = "{\"type\":\"app\",\"code\":\"";
    largePayload.append(8000, 'x');
    largePayload += "\"}";
    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/command", largePayload.c_str());
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
    TEST_ASSERT_EQUAL(largePayload.size(), lastDeliveredLength);
    TEST_ASSERT_EQUAL_STRING_LEN("{\"type\":\"app\"", lastDeliveredPayload, 13);
}

void test_message_at_buffer_limit_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    std::string payload(10239, 'A');
    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/command", payload.c_str());
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
    TEST_ASSERT_EQUAL(payload.size(), lastDeliveredLength);
}

// Bursts arriving before a single drain are now absorbed by the FIFO
// rather than dropped after the first.
void test_burst_messages_before_drain_all_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/command", "{\"type\":\"first\"}");
    client->simulateMessage("devices/dev123/command", "{\"type\":\"second\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(2, deliveredMessageCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"second\"}", lastDeliveredPayload);
}

void test_second_message_after_drain() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/command", "{\"type\":\"first\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
    client->simulateMessage("devices/dev123/command", "{\"type\":\"second\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(2, deliveredMessageCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"second\"}", lastDeliveredPayload);
}

void test_set_client_id_before_begin() {
    mqtt = new MqttTransport();
    mqtt->setMessageCallback(onMessageCallback);
    mqtt->setClientId("my-custom-id");
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("my-custom-id", client->clientId.c_str());
}

void test_config_cert_pem_passed_to_mqtt_client() {
    static const char* MY_CERT = "-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n";
    MqttTransport::Config cfg;
    cfg.cert_pem = MY_CERT;
    mqtt = new MqttTransport(cfg);
    mqtt->setMessageCallback(onMessageCallback);
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING(MY_CERT, client->cert_pem.c_str());
    TEST_ASSERT_NULL(client->crt_bundle_attach);          // a pin overrides the bundle
}

void test_mqtt_bundle_by_default_no_cert_when_disabled() {
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->cert_pem.empty());
    TEST_ASSERT_NOT_NULL(client->crt_bundle_attach);

    delete mqtt;
    MqttTransport::Config cfg;
    cfg.use_cert_bundle = false;
    mqtt = new MqttTransport(cfg);
    mqtt->begin("host", 443, "/path");
    client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->cert_pem.empty());
    TEST_ASSERT_NULL(client->crt_bundle_attach);
}

void test_mqtt_on_configure_called_before_init() {
    mqtt = new MqttTransport();
    mqtt->setMessageCallback(onMessageCallback);
    bool called = false;
    mqtt->onConfigure([&](esp_mqtt_client_config_t& config) {
        called = true;
        config.cert_pem = "HOOK_CERT";
    });
    mqtt->begin("host", 443, "/path");
    TEST_ASSERT_TRUE(called);
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("HOOK_CERT", client->cert_pem.c_str());
}

void test_mqtt_on_configure_can_override_config_cert() {
    static const char* ORIGINAL_CERT = "ORIGINAL";
    static const char* OVERRIDE_CERT = "OVERRIDE";
    MqttTransport::Config cfg;
    cfg.cert_pem = ORIGINAL_CERT;
    mqtt = new MqttTransport(cfg);
    mqtt->setMessageCallback(onMessageCallback);
    mqtt->onConfigure([](esp_mqtt_client_config_t& config) {
        config.cert_pem = OVERRIDE_CERT;
    });
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("OVERRIDE", client->cert_pem.c_str());
}

void test_mqtt_on_configure_not_set_works() {
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_TRUE(client->started);
}

// Phase 8: topic-aware receive hook — onMessage(topic, payload, len) fires
// alongside the existing payload-only callbacks, threading topic through the
// FIFO via the parallel topic queue.
static int onMessageCount = 0;
static char lastTopicBuf[256] = "";
static char lastPayloadBuf[512] = "";

void test_onMessage_receives_topic_and_payload() {
    onMessageCount = 0;
    lastTopicBuf[0] = '\0';
    lastPayloadBuf[0] = '\0';

    mqtt = createWithTopics();
    mqtt->onMessage([](const char* topic, const char* payload, size_t len) {
        onMessageCount++;
        strncpy(lastTopicBuf, topic, sizeof(lastTopicBuf) - 1);
        lastTopicBuf[sizeof(lastTopicBuf) - 1] = '\0';
        size_t copyLen = len < sizeof(lastPayloadBuf) - 1 ? len : sizeof(lastPayloadBuf) - 1;
        memcpy(lastPayloadBuf, payload, copyLen);
        lastPayloadBuf[copyLen] = '\0';
    });
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/foo/temp", "{\"v\":42}");
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, onMessageCount);
    TEST_ASSERT_EQUAL_STRING("devices/foo/temp", lastTopicBuf);
    TEST_ASSERT_EQUAL_STRING("{\"v\":42}", lastPayloadBuf);
}


// ---------------------------------------------------------------------------
// Binary lane: length-carrying publish + topic-scoped binary receive.
// MQTT 3.1.1 has no content-type on the wire, so the subscriber declares
// which topics carry opaque bytes.
// ---------------------------------------------------------------------------

static int binaryCount = 0;
static char lastBinaryTopic[128] = "";
static uint8_t lastBinaryData[64];
static size_t lastBinaryLength = 0;

static int textCount = 0;
static int clientHookCount = 0;

static void resetBinaryCounters() {
    binaryCount = 0;
    lastBinaryTopic[0] = '\0';
    lastBinaryLength = 0;
    memset(lastBinaryData, 0, sizeof(lastBinaryData));
    textCount = 0;
    clientHookCount = 0;
}

static MqttTransport* createBinaryTransport() {
    auto* t = new MqttTransport();
    t->onBinary([](const char* topic, const uint8_t* data, size_t len) {
        binaryCount++;
        strncpy(lastBinaryTopic, topic, sizeof(lastBinaryTopic) - 1);
        lastBinaryTopic[sizeof(lastBinaryTopic) - 1] = '\0';
        lastBinaryLength = len;
        size_t copyLen = len < sizeof(lastBinaryData) ? len : sizeof(lastBinaryData);
        memcpy(lastBinaryData, data, copyLen);
    });
    t->onMessage([](const char* topic, const char* payload, size_t len) {
        (void)topic; (void)payload; (void)len;
        textCount++;
    });
    t->setClientHook([](const char* payload, size_t len) {
        (void)payload; (void)len;
        clientHookCount++;
    });
    return t;
}

// --- publishBinary ---------------------------------------------------------

void test_publish_binary_preserves_embedded_nuls() {
    resetBinaryCounters();
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    const uint8_t frame[] = {0x00, 0x01, 0x00, 0xFF};
    TEST_ASSERT_TRUE(mqtt->publishBinary("devices/d1/voice/audio", frame, sizeof(frame)));

    TEST_ASSERT_EQUAL_STRING("devices/d1/voice/audio", client->lastPublishTopic.c_str());
    TEST_ASSERT_EQUAL(4, client->lastPublishLength);
    TEST_ASSERT_EQUAL(4, client->lastPublishPayload.size());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, client->lastPublishPayload.data(), 4);
}

void test_publish_binary_rejects_zero_length() {
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    client->publishCount = 0;

    const uint8_t frame[] = {0x01};
    // Zero length would hit IDF's `len <= 0 -> strlen(data)` branch and read
    // past the end of a buffer that is not NUL-terminated.
    TEST_ASSERT_FALSE(mqtt->publishBinary("t", frame, 0));
    TEST_ASSERT_EQUAL(0, client->publishCount);
}

void test_publish_binary_rejects_null_data() {
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    client->publishCount = 0;

    TEST_ASSERT_FALSE(mqtt->publishBinary("t", nullptr, 4));
    TEST_ASSERT_FALSE(mqtt->publishBinary(nullptr, (const uint8_t*)"ab", 2));
    TEST_ASSERT_EQUAL(0, client->publishCount);
}

void test_publish_binary_fails_when_disconnected() {
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/mqtt");
    const uint8_t frame[] = {0x01, 0x02};
    TEST_ASSERT_FALSE(mqtt->publishBinary("t", frame, sizeof(frame)));
}

void test_publish_binary_qos_and_retain() {
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    const uint8_t frame[] = {0xAA, 0x00, 0xBB};
    TEST_ASSERT_TRUE(mqtt->publishBinary("t", frame, sizeof(frame), 1, true));
    TEST_ASSERT_EQUAL(1, client->lastPublishQos);
    TEST_ASSERT_TRUE(client->lastPublishRetain);
    TEST_ASSERT_EQUAL(3, client->lastPublishLength);
}

// --- topicMatches ----------------------------------------------------------

void test_topic_matches_exact_and_wildcards() {
    TEST_ASSERT_TRUE(MqttTransport::topicMatches("a/b/c", "a/b/c"));
    TEST_ASSERT_FALSE(MqttTransport::topicMatches("a/b/c", "a/b/d"));
    TEST_ASSERT_FALSE(MqttTransport::topicMatches("a/b", "a/b/c"));
    TEST_ASSERT_FALSE(MqttTransport::topicMatches("a/b/c", "a/b"));

    TEST_ASSERT_TRUE(MqttTransport::topicMatches("devices/+/voice/audio",
                                                 "devices/abc/voice/audio"));
    TEST_ASSERT_FALSE(MqttTransport::topicMatches("devices/+/voice/audio",
                                                  "devices/abc/def/voice/audio"));
    TEST_ASSERT_TRUE(MqttTransport::topicMatches("devices/+", "devices/abc"));
    TEST_ASSERT_FALSE(MqttTransport::topicMatches("devices/+", "devices/abc/x"));

    TEST_ASSERT_TRUE(MqttTransport::topicMatches("devices/#", "devices/abc/voice/audio"));
    TEST_ASSERT_TRUE(MqttTransport::topicMatches("devices/#", "devices/abc"));
    TEST_ASSERT_TRUE(MqttTransport::topicMatches("#", "anything/at/all"));
    TEST_ASSERT_FALSE(MqttTransport::topicMatches("devices/#", "rooms/abc"));

    // A trailing "#" also matches the parent level itself (MQTT 3.1.1 4.7.1.2).
    TEST_ASSERT_TRUE(MqttTransport::topicMatches("devices/#", "devices"));
}

// --- binary receive --------------------------------------------------------

void test_binary_topic_delivered_to_onBinary() {
    resetBinaryCounters();
    mqtt = createBinaryTransport();
    mqtt->subscribeBinary("devices/d1/voice/audio");
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    const uint8_t frame[] = {0x00, 0x01, 0x00, 0xFF};
    client->simulateBinaryMessage("devices/d1/voice/audio", frame, sizeof(frame));
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, binaryCount);
    TEST_ASSERT_EQUAL_STRING("devices/d1/voice/audio", lastBinaryTopic);
    TEST_ASSERT_EQUAL(4, lastBinaryLength);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, lastBinaryData, 4);
}

void test_binary_topic_bypasses_text_and_client_hook() {
    resetBinaryCounters();
    mqtt = createBinaryTransport();
    mqtt->subscribeBinary("devices/d1/voice/audio");
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    const uint8_t frame[] = {0x00, 0x01};
    client->simulateBinaryMessage("devices/d1/voice/audio", frame, sizeof(frame));
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, binaryCount);
    TEST_ASSERT_EQUAL(0, textCount);
    TEST_ASSERT_EQUAL(0, clientHookCount);
}

void test_binary_wildcard_filter_matches_concrete_topic() {
    resetBinaryCounters();
    mqtt = createBinaryTransport();
    mqtt->subscribeBinary("devices/+/voice/audio");
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    const uint8_t frame[] = {0x00, 0x7F, 0x00};
    client->simulateBinaryMessage("devices/xyz/voice/audio", frame, sizeof(frame));
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, binaryCount);
    TEST_ASSERT_EQUAL_STRING("devices/xyz/voice/audio", lastBinaryTopic);
    TEST_ASSERT_EQUAL(3, lastBinaryLength);
    TEST_ASSERT_EQUAL(0, clientHookCount);
}

void test_text_topic_unaffected_by_binary_subscription() {
    resetBinaryCounters();
    mqtt = createBinaryTransport();
    mqtt->subscribeBinary("devices/d1/voice/audio");
    mqtt->subscribe("devices/d1/command");
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    client->simulateMessage("devices/d1/command", "{\"type\":\"ping\"}");
    mqtt->loop();

    TEST_ASSERT_EQUAL(0, binaryCount);
    TEST_ASSERT_EQUAL(1, textCount);
    TEST_ASSERT_EQUAL(1, clientHookCount);
}

void test_config_binary_topics_subscribed_and_routed() {
    resetBinaryCounters();
    MqttTransport::Config cfg;
    cfg.topics = {"devices/d1/command"};
    cfg.binaryTopics = {"devices/d1/voice/audio"};
    mqtt = new MqttTransport(cfg);
    mqtt->onBinary([](const char* topic, const uint8_t* data, size_t len) {
        (void)topic; (void)data;
        binaryCount++;
        lastBinaryLength = len;
    });
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    TEST_ASSERT_EQUAL(2, client->subscriptionCount);

    const uint8_t frame[] = {0x00, 0x00};
    client->simulateBinaryMessage("devices/d1/voice/audio", frame, sizeof(frame));
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, binaryCount);
    TEST_ASSERT_EQUAL(2, lastBinaryLength);
}

void test_binary_subscription_survives_reconnect() {
    resetBinaryCounters();
    mqtt = createBinaryTransport();
    mqtt->subscribeBinary("devices/d1/voice/audio", 1);
    mqtt->begin("host", 443, "/mqtt");
    MockMqttClient::lastInstance()->simulateConnect();
    MockMqttClient::lastInstance()->simulateDisconnect();

    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    TEST_ASSERT_EQUAL(1, client->subscriptionCount);
    TEST_ASSERT_EQUAL_STRING("devices/d1/voice/audio",
                             client->subscribedTopics[0].c_str());
    TEST_ASSERT_EQUAL(1, client->lastSubscribeQos);

    const uint8_t frame[] = {0x00, 0x01};
    client->simulateBinaryMessage("devices/d1/voice/audio", frame, sizeof(frame));
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, binaryCount);
}

void test_binary_multichunk_reassembly() {
    resetBinaryCounters();
    mqtt = createBinaryTransport();
    mqtt->subscribeBinary("devices/d1/voice/audio");
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    const uint8_t first[]  = {0x00, 0x01};
    const uint8_t second[] = {0x00, 0xFF};
    client->simulateBinaryChunk("devices/d1/voice/audio", first, 2, 4, 0);
    client->simulateBinaryChunk("devices/d1/voice/audio", second, 2, 4, 2);
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, binaryCount);
    TEST_ASSERT_EQUAL(4, lastBinaryLength);
    const uint8_t expected[] = {0x00, 0x01, 0x00, 0xFF};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, lastBinaryData, 4);
    TEST_ASSERT_EQUAL(0, clientHookCount);
}


// ---------------------------------------------------------------------------
// Client lock: a publish reports "busy" rather than blocking indefinitely
// behind another task inside the ESP-IDF client. Bounds contention only.
// ---------------------------------------------------------------------------

void test_publish_reports_busy_while_another_task_holds_the_client() {
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    client->blockPublish.store(true);
    std::atomic<bool> inFlight{false};
    std::thread holder([&]() {
        inFlight.store(true);
        mqtt->publish("t", "held");
    });
    while (!inFlight.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    auto start = std::chrono::steady_clock::now();
    bool ok = mqtt->publish("t", "second");
    auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    client->blockPublish.store(false);
    holder.join();

    TEST_ASSERT_FALSE(ok);   // refused, not blocked
    TEST_ASSERT_TRUE(waited >= (long)MqttTransport::PUBLISH_LOCK_TIMEOUT_MS);
    TEST_ASSERT_TRUE(waited < (long)MqttTransport::PUBLISH_LOCK_TIMEOUT_MS * 4);
}

void test_publish_binary_reports_busy_while_client_held() {
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    client->blockPublish.store(true);
    std::atomic<bool> inFlight{false};
    std::thread holder([&]() {
        inFlight.store(true);
        mqtt->publish("t", "held");
    });
    while (!inFlight.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const uint8_t frame[] = {0x00, 0x01};
    bool ok = mqtt->publishBinary("t", frame, sizeof(frame));

    client->blockPublish.store(false);
    holder.join();

    TEST_ASSERT_FALSE(ok);
}

void test_publish_succeeds_once_the_client_is_free_again() {
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    client->blockPublish.store(true);
    std::atomic<bool> inFlight{false};
    std::thread holder([&]() {
        inFlight.store(true);
        mqtt->publish("t", "held");
    });
    while (!inFlight.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    client->blockPublish.store(false);
    holder.join();

    TEST_ASSERT_TRUE(mqtt->publish("t", "after"));
    TEST_ASSERT_EQUAL_STRING("after", client->lastPublishPayload.c_str());
}

// ---------------------------------------------------------------------------
// Buffer and timeout knobs — both default to "leave the IDF default alone".
// ---------------------------------------------------------------------------

void test_buffer_and_timeout_defaults_are_not_set() {
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL(0, client->out_buffer_size);
    TEST_ASSERT_EQUAL(0, client->network_timeout_ms);
}

void test_config_out_buffer_and_network_timeout_passed_through() {
    MqttTransport::Config cfg;
    cfg.out_buffer_size = 2048;
    cfg.network_timeout_ms = 3000;
    mqtt = new MqttTransport(cfg);
    mqtt->begin("host", 443, "/mqtt");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL(2048, client->out_buffer_size);
    TEST_ASSERT_EQUAL(3000, client->network_timeout_ms);
}

// ---------------------------------------------------------------------------
// ErrorInfo value type
// ---------------------------------------------------------------------------

void test_error_info_defaults_are_benign() {
    MqttTransport::ErrorInfo err;
    TEST_ASSERT_FALSE(err.isConnectionRefused());
    TEST_ASSERT_FALSE(err.isNotAuthorized());
    TEST_ASSERT_NOT_NULL(err.describe());
}

void test_error_info_not_authorized_only_for_connack_5() {
    MqttTransport::ErrorInfo err;
    err.type = MQTT_ERROR_TYPE_CONNECTION_REFUSED;

    err.connectReturnCode = MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED;
    TEST_ASSERT_TRUE(err.isConnectionRefused());
    TEST_ASSERT_TRUE(err.isNotAuthorized());

    err.connectReturnCode = MQTT_CONNECTION_REFUSE_BAD_USERNAME;   // code 4
    TEST_ASSERT_TRUE(err.isConnectionRefused());
    TEST_ASSERT_FALSE(err.isNotAuthorized());

    err.connectReturnCode = MQTT_CONNECTION_REFUSE_ID_REJECTED;    // code 2
    TEST_ASSERT_FALSE(err.isNotAuthorized());
}

void test_error_info_stale_connack_does_not_leak_through_tcp_error() {
    // IDF leaves connect_return_code untouched on a transport error. A stale
    // value from a previous refusal must not read as an authorization failure.
    MqttTransport::ErrorInfo err;
    err.type = MQTT_ERROR_TYPE_TCP_TRANSPORT;
    err.connectReturnCode = MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED;
    TEST_ASSERT_FALSE(err.isConnectionRefused());
    TEST_ASSERT_FALSE(err.isNotAuthorized());
}

void test_error_info_describe_distinguishes_causes() {
    MqttTransport::ErrorInfo refused;
    refused.type = MQTT_ERROR_TYPE_CONNECTION_REFUSED;
    refused.connectReturnCode = MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED;

    MqttTransport::ErrorInfo badUser;
    badUser.type = MQTT_ERROR_TYPE_CONNECTION_REFUSED;
    badUser.connectReturnCode = MQTT_CONNECTION_REFUSE_BAD_USERNAME;

    MqttTransport::ErrorInfo tcp;
    tcp.type = MQTT_ERROR_TYPE_TCP_TRANSPORT;

    TEST_ASSERT_NOT_NULL(refused.describe());
    TEST_ASSERT_NOT_NULL(badUser.describe());
    TEST_ASSERT_NOT_NULL(tcp.describe());
    // The whole point: these three are no longer the same string.
    TEST_ASSERT_TRUE(strcmp(refused.describe(), tcp.describe())     != 0);
    TEST_ASSERT_TRUE(strcmp(refused.describe(), badUser.describe()) != 0);
    TEST_ASSERT_NOT_NULL(strstr(refused.describe(), "not authorized"));
}

void test_error_info_describe_handles_unknown_type() {
    // MQTT_ERROR_TYPE_SUBSCRIBE_FAILED exists only on IDF 5; on IDF 4.4 it
    // falls to the default arm. Either way describe() returns a valid string.
    MqttTransport::ErrorInfo err;
    err.type = MQTT_ERROR_TYPE_SUBSCRIBE_FAILED;
    TEST_ASSERT_NOT_NULL(err.describe());
}

// ---------------------------------------------------------------------------
// onError delivery — capture, queue, drain
// ---------------------------------------------------------------------------

void test_error_is_queued_not_dispatched_on_the_idf_task() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    mqtt->onError(onErrorCallback);
    MockMqttClient::lastInstance()->simulateError(
        MQTT_ERROR_TYPE_CONNECTION_REFUSED,
        MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED);

    // The event handler runs on the IDF task; user code must not run there.
    TEST_ASSERT_EQUAL(0, errorCount);

    mqtt->loop();
    TEST_ASSERT_EQUAL(1, errorCount);
}

void test_connack_not_authorized_reaches_the_application() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    mqtt->onError(onErrorCallback);
    MockMqttClient::lastInstance()->simulateError(
        MQTT_ERROR_TYPE_CONNECTION_REFUSED,
        MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED);
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, errorCount);
    TEST_ASSERT_TRUE(lastError.isConnectionRefused());
    TEST_ASSERT_TRUE(lastError.isNotAuthorized());
}

void test_connack_bad_username_is_not_reported_as_unauthorized() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    mqtt->onError(onErrorCallback);
    MockMqttClient::lastInstance()->simulateError(
        MQTT_ERROR_TYPE_CONNECTION_REFUSED,
        MQTT_CONNECTION_REFUSE_BAD_USERNAME);
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, errorCount);
    TEST_ASSERT_TRUE(lastError.isConnectionRefused());
    TEST_ASSERT_FALSE(lastError.isNotAuthorized());
}

void test_transport_error_carries_tls_and_socket_detail() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    mqtt->onError(onErrorCallback);
    MockMqttClient::lastInstance()->simulateTransportError(
        -0x2700, 0x7280, 4, 113);
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, errorCount);
    TEST_ASSERT_FALSE(lastError.isNotAuthorized());
    TEST_ASSERT_EQUAL(-0x2700, lastError.tlsLastEspErr);
    TEST_ASSERT_EQUAL(0x7280, lastError.tlsStackErr);
    TEST_ASSERT_EQUAL(4, lastError.tlsCertVerifyFlags);
    TEST_ASSERT_EQUAL(113, lastError.sockErrno);
}

void test_multiple_errors_delivered_in_order_on_one_loop() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    mqtt->onError(onErrorCallback);
    MockMqttClient* client = MockMqttClient::lastInstance();

    client->simulateError(MQTT_ERROR_TYPE_CONNECTION_REFUSED,
                          MQTT_CONNECTION_REFUSE_ID_REJECTED);
    client->simulateError(MQTT_ERROR_TYPE_CONNECTION_REFUSED,
                          MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED);
    mqtt->loop();

    TEST_ASSERT_EQUAL(2, errorCount);
    TEST_ASSERT_EQUAL(MQTT_CONNECTION_REFUSE_ID_REJECTED,
                      errorSequence[0].connectReturnCode);
    TEST_ASSERT_EQUAL(MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED,
                      errorSequence[1].connectReturnCode);
}

void test_error_without_registered_callback_is_safe() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    // deliberately no onError()
    MockMqttClient::lastInstance()->simulateError(
        MQTT_ERROR_TYPE_CONNECTION_REFUSED,
        MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED);
    mqtt->loop();
    TEST_ASSERT_EQUAL(0, errorCount);

    // The queue must actually have drained above, not merely skipped
    // delivery — otherwise it silently fills to ERROR_QUEUE_DEPTH and every
    // later error hits the "queue full" drop path. Register a callback now
    // and fire exactly one more error: if the earlier one had been left
    // sitting in the queue, this would deliver it too and errorCount would
    // be 2.
    mqtt->onError(onErrorCallback);
    MockMqttClient::lastInstance()->simulateError(
        MQTT_ERROR_TYPE_TCP_TRANSPORT, MQTT_CONNECTION_ACCEPTED);
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, errorCount);
    TEST_ASSERT_FALSE(lastError.isConnectionRefused());
}

void test_error_with_null_handle_does_not_crash() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    mqtt->onError(onErrorCallback);
    MockMqttClient::lastInstance()->simulateErrorWithNullHandle();
    mqtt->loop();

    // Still reported, with benign defaults — an error happened, detail unknown.
    TEST_ASSERT_EQUAL(1, errorCount);
    TEST_ASSERT_FALSE(lastError.isNotAuthorized());
}

void test_error_reporting_does_not_disturb_message_delivery() {
    mqtt = createWithTopics();       // already wires onMessageCallback
    mqtt->begin("host", 443, "/path");
    mqtt->onError(onErrorCallback);
    MockMqttClient* client = MockMqttClient::lastInstance();

    client->simulateConnect();
    client->simulateMessage("devices/dev123/command", "{\"type\":\"ping\"}");
    client->simulateError(MQTT_ERROR_TYPE_TCP_TRANSPORT,
                          MQTT_CONNECTION_ACCEPTED);
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
    TEST_ASSERT_EQUAL(1, errorCount);
}

// ---------------------------------------------------------------------------
// Contract guards — queue overflow and callback re-entrancy
// ---------------------------------------------------------------------------

void test_error_queue_overflow_drops_without_corrupting_earlier_reports() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    mqtt->onError(onErrorCallback);
    MockMqttClient* client = MockMqttClient::lastInstance();

    // Depth is 4; push 6 without draining.
    client->simulateError(MQTT_ERROR_TYPE_CONNECTION_REFUSED,
                          MQTT_CONNECTION_REFUSE_PROTOCOL);            // 1
    client->simulateError(MQTT_ERROR_TYPE_CONNECTION_REFUSED,
                          MQTT_CONNECTION_REFUSE_ID_REJECTED);         // 2
    client->simulateError(MQTT_ERROR_TYPE_CONNECTION_REFUSED,
                          MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE);  // 3
    client->simulateError(MQTT_ERROR_TYPE_CONNECTION_REFUSED,
                          MQTT_CONNECTION_REFUSE_BAD_USERNAME);        // 4
    client->simulateError(MQTT_ERROR_TYPE_CONNECTION_REFUSED,
                          MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED);      // 5 dropped
    client->simulateError(MQTT_ERROR_TYPE_TCP_TRANSPORT,
                          MQTT_CONNECTION_ACCEPTED);                   // 6 dropped

    mqtt->loop();

    // Oldest four survive intact; the newest are dropped, not the earliest.
    TEST_ASSERT_EQUAL(4, errorCount);
    TEST_ASSERT_EQUAL(MQTT_CONNECTION_REFUSE_PROTOCOL,
                      errorSequence[0].connectReturnCode);
    TEST_ASSERT_EQUAL(MQTT_CONNECTION_REFUSE_BAD_USERNAME,
                      errorSequence[3].connectReturnCode);
}

void test_queue_recovers_after_overflow() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    mqtt->onError(onErrorCallback);
    MockMqttClient* client = MockMqttClient::lastInstance();

    for (int i = 0; i < 6; i++) {
        client->simulateError(MQTT_ERROR_TYPE_TCP_TRANSPORT,
                              MQTT_CONNECTION_ACCEPTED);
    }
    mqtt->loop();
    TEST_ASSERT_EQUAL(4, errorCount);

    errorCount = 0;
    client->simulateError(MQTT_ERROR_TYPE_CONNECTION_REFUSED,
                          MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED);
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, errorCount);
    TEST_ASSERT_TRUE(lastError.isNotAuthorized());
}

static int reentrantDisconnectCount = 0;
static int reentrantBeginCount = 0;

static void onErrorCallsDisconnect(const MqttTransport::ErrorInfo& err) {
    (void)err;
    reentrantDisconnectCount++;
    mqtt->disconnect();       // takes _clientLock — must not be held by the drain
}

static void onErrorCallsBegin(const MqttTransport::ErrorInfo& err) {
    (void)err;
    reentrantBeginCount++;
    if (reentrantBeginCount > 1) return;   // guard against a rescue loop
    mqtt->setClientId("rescued-client");
    mqtt->begin();            // destroys + rebuilds; takes _clientLock
}

void test_callback_may_call_disconnect_reentrantly() {
    reentrantDisconnectCount = 0;
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    mqtt->onError(onErrorCallsDisconnect);
    MockMqttClient::lastInstance()->simulateConnect();
    MockMqttClient::lastInstance()->simulateError(
        MQTT_ERROR_TYPE_CONNECTION_REFUSED,
        MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED);

    mqtt->loop();   // hangs here if the drain holds _clientLock

    TEST_ASSERT_EQUAL(1, reentrantDisconnectCount);
    TEST_ASSERT_FALSE(mqtt->isConnected());
}

void test_callback_may_call_begin_reentrantly() {
    // This is the executable guard on the no-lock-held rule (spec 3.4).
    // begin() acquires _clientLock; if the error drain still held it, this
    // test would deadlock rather than fail.
    reentrantBeginCount = 0;
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    mqtt->onError(onErrorCallsBegin);
    int before = MockMqttClient::instanceCount();   // after the initial begin()

    MockMqttClient::lastInstance()->simulateError(
        MQTT_ERROR_TYPE_CONNECTION_REFUSED,
        MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED);
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, reentrantBeginCount);
    // A fresh client was built with the rescued identity.
    TEST_ASSERT_EQUAL(before + 1, MockMqttClient::instanceCount());
    TEST_ASSERT_EQUAL_STRING(
        "rescued-client",
        MockMqttClient::lastInstance()->clientId.c_str());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_name_is_mqtt);
    RUN_TEST(test_begin_constructs_wss_uri);
    RUN_TEST(test_begin_sets_client_id);
    RUN_TEST(test_begin_without_client_id_uses_empty);
    RUN_TEST(test_begin_starts_client);
    RUN_TEST(test_begin_no_cert_by_default_and_disables_auto_reconnect);
    RUN_TEST(test_subscribes_to_configured_topics);
    RUN_TEST(test_connected_after_connect_event);
    RUN_TEST(test_disconnected_after_disconnect_event);
    RUN_TEST(test_command_message_delivered);
    RUN_TEST(test_status_message_delivered);
    RUN_TEST(test_own_event_topic_message_delivered);
    RUN_TEST(test_other_device_event_delivered);
    RUN_TEST(test_send_requires_topic);
    RUN_TEST(test_send_with_topic_publishes);
    RUN_TEST(test_publish_json_overload_serializes);
    RUN_TEST(test_publish_raw_payload);
    RUN_TEST(test_publish_sends_to_explicit_topic);
    RUN_TEST(test_publish_fails_when_disconnected);
    RUN_TEST(test_publish_with_qos_and_retain);
    RUN_TEST(test_disconnect_sets_not_connected);
    RUN_TEST(test_reconnect_after_disconnect);
    RUN_TEST(test_reconnect_with_new_path);
    RUN_TEST(test_multiple_connect_disconnect_cycles);
    RUN_TEST(test_reconnect_creates_fresh_client);
    RUN_TEST(test_reconnect_exact_subscription_count);
    RUN_TEST(test_reconnect_message_delivered_exactly_once);
    RUN_TEST(test_reconnect_connect_event_fires_once);
    RUN_TEST(test_multiple_room_changes_no_accumulation);
    RUN_TEST(test_large_command_message_delivered);
    RUN_TEST(test_message_at_buffer_limit_delivered);
    RUN_TEST(test_burst_messages_before_drain_all_delivered);
    RUN_TEST(test_second_message_after_drain);
    RUN_TEST(test_set_client_id_before_begin);
    RUN_TEST(test_config_cert_pem_passed_to_mqtt_client);
    RUN_TEST(test_mqtt_bundle_by_default_no_cert_when_disabled);
    RUN_TEST(test_mqtt_on_configure_called_before_init);
    RUN_TEST(test_mqtt_on_configure_can_override_config_cert);
    RUN_TEST(test_mqtt_on_configure_not_set_works);
    RUN_TEST(test_onMessage_receives_topic_and_payload);
    RUN_TEST(test_publish_binary_preserves_embedded_nuls);
    RUN_TEST(test_publish_binary_rejects_zero_length);
    RUN_TEST(test_publish_binary_rejects_null_data);
    RUN_TEST(test_publish_binary_fails_when_disconnected);
    RUN_TEST(test_publish_binary_qos_and_retain);
    RUN_TEST(test_topic_matches_exact_and_wildcards);
    RUN_TEST(test_binary_topic_delivered_to_onBinary);
    RUN_TEST(test_binary_topic_bypasses_text_and_client_hook);
    RUN_TEST(test_binary_wildcard_filter_matches_concrete_topic);
    RUN_TEST(test_text_topic_unaffected_by_binary_subscription);
    RUN_TEST(test_config_binary_topics_subscribed_and_routed);
    RUN_TEST(test_binary_subscription_survives_reconnect);
    RUN_TEST(test_binary_multichunk_reassembly);
    RUN_TEST(test_publish_reports_busy_while_another_task_holds_the_client);
    RUN_TEST(test_publish_binary_reports_busy_while_client_held);
    RUN_TEST(test_publish_succeeds_once_the_client_is_free_again);
    RUN_TEST(test_buffer_and_timeout_defaults_are_not_set);
    RUN_TEST(test_config_out_buffer_and_network_timeout_passed_through);
    RUN_TEST(test_error_info_defaults_are_benign);
    RUN_TEST(test_error_info_not_authorized_only_for_connack_5);
    RUN_TEST(test_error_info_stale_connack_does_not_leak_through_tcp_error);
    RUN_TEST(test_error_info_describe_distinguishes_causes);
    RUN_TEST(test_error_info_describe_handles_unknown_type);
    RUN_TEST(test_error_is_queued_not_dispatched_on_the_idf_task);
    RUN_TEST(test_connack_not_authorized_reaches_the_application);
    RUN_TEST(test_connack_bad_username_is_not_reported_as_unauthorized);
    RUN_TEST(test_transport_error_carries_tls_and_socket_detail);
    RUN_TEST(test_multiple_errors_delivered_in_order_on_one_loop);
    RUN_TEST(test_error_without_registered_callback_is_safe);
    RUN_TEST(test_error_with_null_handle_does_not_crash);
    RUN_TEST(test_error_reporting_does_not_disturb_message_delivery);
    RUN_TEST(test_error_queue_overflow_drops_without_corrupting_earlier_reports);
    RUN_TEST(test_queue_recovers_after_overflow);
    RUN_TEST(test_callback_may_call_disconnect_reentrantly);
    RUN_TEST(test_callback_may_call_begin_reentrantly);
    return UNITY_END();
}
