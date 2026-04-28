#include <unity.h>
#include <MqttTransport.h>
#include <mqtt_client.h>
#include <cstring>
#include <string>

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

static MqttTransport* mqtt = nullptr;

void setUp(void) {
    MockMqttClient::resetInstanceCount();
    deliveredMessageCount = 0;
    lastDeliveredPayload[0] = '\0';
    lastDeliveredLength = 0;
    connectionEventCount = 0;
    lastConnectionState = false;
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
    TEST_ASSERT_TRUE(client->cert_pem.empty());
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

void test_send_always_returns_false() {
    // send() is a no-op: MQTT requires an explicit topic via publish().
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    bool result = mqtt->send("{\"type\":\"test\"}");
    TEST_ASSERT_FALSE(result);
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
}

void test_mqtt_no_cert_by_default() {
    mqtt = new MqttTransport();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->cert_pem.empty());
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
    RUN_TEST(test_send_always_returns_false);
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
    RUN_TEST(test_mqtt_no_cert_by_default);
    RUN_TEST(test_mqtt_on_configure_called_before_init);
    RUN_TEST(test_mqtt_on_configure_can_override_config_cert);
    RUN_TEST(test_mqtt_on_configure_not_set_works);
    RUN_TEST(test_onMessage_receives_topic_and_payload);
    return UNITY_END();
}
