#include <unity.h>
#include <CourierWSTransport.h>
#include <esp_websocket_client.h>
#include <cstring>

static int deliveredMessageCount = 0;
static char lastDeliveredPayload[512] = "";

static void onMessageCallback(const char* payload, size_t length) {
    deliveredMessageCount++;
    size_t copyLen = length < sizeof(lastDeliveredPayload) - 1 ? length : sizeof(lastDeliveredPayload) - 1;
    memcpy(lastDeliveredPayload, payload, copyLen);
    lastDeliveredPayload[copyLen] = '\0';
}

static int connectionEventCount = 0;
static bool lastConnectionState = false;

static void onConnectionCallback(CourierTransport* transport, bool connected) {
    connectionEventCount++;
    lastConnectionState = connected;
}

static CourierWSTransport* ws = nullptr;

void setUp(void) {
    MockWebSocketClient::resetInstanceCount();
    ws = new CourierWSTransport();
    ws->setMessageCallback(onMessageCallback);
    ws->setConnectionCallback(onConnectionCallback);
    deliveredMessageCount = 0;
    lastDeliveredPayload[0] = '\0';
    connectionEventCount = 0;
    lastConnectionState = false;
}

void tearDown(void) {
    delete ws;
    ws = nullptr;
}

void test_name_is_ws() {
    TEST_ASSERT_EQUAL_STRING("WebSocket", ws->name());
}

void test_begin_creates_client_with_wss_uri() {
    ws->begin("example.com", 443, "/ws/abc123");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL_STRING("wss://example.com:443/ws/abc123", client->uri.c_str());
}

void test_begin_sets_config_no_cert_by_default() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_TRUE(client->cert_pem.empty());
    TEST_ASSERT_TRUE(client->disable_auto_reconnect);
    TEST_ASSERT_EQUAL(20, client->pingpong_timeout_sec);
}

void test_begin_starts_client() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_TRUE(client->started);
}

void test_connected_after_connect_event() {
    ws->begin("host", 443, "/path");
    TEST_ASSERT_FALSE(ws->isConnected());
    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();
    TEST_ASSERT_TRUE(ws->isConnected());
}

void test_disconnected_after_disconnect_event() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();
    TEST_ASSERT_TRUE(ws->isConnected());
    client->simulateDisconnect();
    ws->loop();
    TEST_ASSERT_FALSE(ws->isConnected());
}

void test_message_delivered_to_callback() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    client->simulateTextMessage("{\"type\":\"test\",\"value\":42}");
    ws->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"test\",\"value\":42}", lastDeliveredPayload);
}

void test_connection_callback_on_connect() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();
    TEST_ASSERT_EQUAL(1, connectionEventCount);
    TEST_ASSERT_TRUE(lastConnectionState);
}

void test_connection_callback_on_disconnect() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();
    client->simulateDisconnect();
    ws->loop();
    TEST_ASSERT_EQUAL(2, connectionEventCount);
    TEST_ASSERT_FALSE(lastConnectionState);
}

void test_send_when_connected() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();
    bool result = ws->send("{\"type\":\"test\"}");
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(1, client->sendCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"test\"}", client->sentMessages[0].c_str());
}

void test_send_fails_when_disconnected() {
    ws->begin("host", 443, "/path");
    bool result = ws->send("{\"type\":\"test\"}");
    TEST_ASSERT_FALSE(result);
}

void test_disconnect_sets_not_connected() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();
    TEST_ASSERT_TRUE(ws->isConnected());
    ws->disconnect();
    TEST_ASSERT_FALSE(ws->isConnected());
}

void test_reconnect_creates_new_client() {
    ws->begin("host", 443, "/path1");
    TEST_ASSERT_EQUAL(1, MockWebSocketClient::instanceCount());
    ws->begin("host", 443, "/path2");
    TEST_ASSERT_EQUAL(2, MockWebSocketClient::instanceCount());
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("wss://host:443/path2", client->uri.c_str());
}

void test_config_cert_pem_passed_to_client() {
    delete ws;
    static const char* MY_CERT = "-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n";
    CourierWSTransportConfig cfg;
    cfg.cert_pem = MY_CERT;
    ws = new CourierWSTransport(cfg);
    ws->setMessageCallback(onMessageCallback);
    ws->setConnectionCallback(onConnectionCallback);
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING(MY_CERT, client->cert_pem.c_str());
}

void test_config_no_cert_by_default_nullptr() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_TRUE(client->cert_pem.empty());
}

void test_on_configure_called_before_init() {
    bool called = false;
    ws->onConfigure([&](esp_websocket_client_config_t& config) {
        called = true;
        config.cert_pem = "HOOK_CERT";
    });
    ws->begin("host", 443, "/path");
    TEST_ASSERT_TRUE(called);
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("HOOK_CERT", client->cert_pem.c_str());
}

void test_on_configure_can_override_config_cert() {
    delete ws;
    static const char* ORIGINAL_CERT = "ORIGINAL";
    static const char* OVERRIDE_CERT = "OVERRIDE";
    CourierWSTransportConfig cfg;
    cfg.cert_pem = ORIGINAL_CERT;
    ws = new CourierWSTransport(cfg);
    ws->setMessageCallback(onMessageCallback);
    ws->onConfigure([](esp_websocket_client_config_t& config) {
        config.cert_pem = OVERRIDE_CERT;
    });
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("OVERRIDE", client->cert_pem.c_str());
}

void test_on_configure_not_set_works() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_TRUE(client->started);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_name_is_ws);
    RUN_TEST(test_begin_creates_client_with_wss_uri);
    RUN_TEST(test_begin_sets_config_no_cert_by_default);
    RUN_TEST(test_begin_starts_client);
    RUN_TEST(test_connected_after_connect_event);
    RUN_TEST(test_disconnected_after_disconnect_event);
    RUN_TEST(test_message_delivered_to_callback);
    RUN_TEST(test_connection_callback_on_connect);
    RUN_TEST(test_connection_callback_on_disconnect);
    RUN_TEST(test_send_when_connected);
    RUN_TEST(test_send_fails_when_disconnected);
    RUN_TEST(test_disconnect_sets_not_connected);
    RUN_TEST(test_reconnect_creates_new_client);
    RUN_TEST(test_config_cert_pem_passed_to_client);
    RUN_TEST(test_config_no_cert_by_default_nullptr);
    RUN_TEST(test_on_configure_called_before_init);
    RUN_TEST(test_on_configure_can_override_config_cert);
    RUN_TEST(test_on_configure_not_set_works);
    return UNITY_END();
}
