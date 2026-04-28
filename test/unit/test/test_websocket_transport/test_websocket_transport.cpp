#include <unity.h>
#include <WebSocketTransport.h>
#include <esp_websocket_client.h>
#include <cstring>

using namespace Courier;

static int deliveredMessageCount = 0;
static char lastDeliveredPayload[512] = "";

static int deliveredBinaryCount = 0;
static uint8_t lastDeliveredBinary[512] = {0};
static size_t lastDeliveredBinaryLen = 0;

static void onBinaryCallback(const uint8_t* data, size_t length) {
    deliveredBinaryCount++;
    size_t copyLen = length < sizeof(lastDeliveredBinary)
                        ? length : sizeof(lastDeliveredBinary);
    memcpy(lastDeliveredBinary, data, copyLen);
    lastDeliveredBinaryLen = length;
}

static void onMessageCallback(const char* payload, size_t length) {
    deliveredMessageCount++;
    size_t copyLen = length < sizeof(lastDeliveredPayload) - 1 ? length : sizeof(lastDeliveredPayload) - 1;
    memcpy(lastDeliveredPayload, payload, copyLen);
    lastDeliveredPayload[copyLen] = '\0';
}

static int connectionEventCount = 0;
static bool lastConnectionState = false;

static void onConnectionCallback(Transport* transport, bool connected) {
    connectionEventCount++;
    lastConnectionState = connected;
}

static WebSocketTransport* ws = nullptr;

void setUp(void) {
    MockWebSocketClient::resetInstanceCount();
    ws = new WebSocketTransport();
    ws->setMessageCallback(onMessageCallback);
    ws->setBinaryMessageCallback(onBinaryCallback);
    ws->setConnectionCallback(onConnectionCallback);
    deliveredMessageCount = 0;
    lastDeliveredPayload[0] = '\0';
    connectionEventCount = 0;
    lastConnectionState = false;
    deliveredBinaryCount = 0;
    lastDeliveredBinaryLen = 0;
    memset(lastDeliveredBinary, 0, sizeof(lastDeliveredBinary));
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

void test_begin_sets_config_defaults() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_FALSE(client->cert_pem.empty());
    TEST_ASSERT_FALSE(client->disable_auto_reconnect);  // auto-reconnect enabled for self-healing
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

void test_send_text_when_connected() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();
    bool result = ws->sendText("{\"type\":\"test\"}");
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(1, client->sendCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"test\"}", client->sentMessages[0].c_str());
}

void test_send_text_fails_when_disconnected() {
    ws->begin("host", 443, "/path");
    bool result = ws->sendText("{\"type\":\"test\"}");
    TEST_ASSERT_FALSE(result);
}

void test_send_json_serializes_and_calls_send_text() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();
    JsonDocument doc;
    doc["type"] = "hello";
    doc["value"] = 42;
    bool result = ws->send(doc);
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(1, client->sendCount);
    TEST_ASSERT_NOT_NULL(strstr(client->sentMessages[0].c_str(), "\"type\":\"hello\""));
    TEST_ASSERT_NOT_NULL(strstr(client->sentMessages[0].c_str(), "\"value\":42"));
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
    WebSocketTransport::Config cfg;
    cfg.cert_pem = MY_CERT;
    ws = new WebSocketTransport(cfg);
    ws->setMessageCallback(onMessageCallback);
    ws->setConnectionCallback(onConnectionCallback);
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING(MY_CERT, client->cert_pem.c_str());
}

void test_default_certs_used_by_default() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_FALSE(client->cert_pem.empty());
    TEST_ASSERT_NOT_NULL(strstr(client->cert_pem.c_str(), "BEGIN CERTIFICATE"));
}

void test_no_cert_when_default_certs_disabled() {
    delete ws;
    WebSocketTransport::Config cfg;
    cfg.use_default_certs = false;
    ws = new WebSocketTransport(cfg);
    ws->setMessageCallback(onMessageCallback);
    ws->setConnectionCallback(onConnectionCallback);
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
    WebSocketTransport::Config cfg;
    cfg.cert_pem = ORIGINAL_CERT;
    ws = new WebSocketTransport(cfg);
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

void test_fifo_absorbs_burst_before_drain() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    client->simulateTextMessage("{\"type\":\"session\"}");
    client->simulateTextMessage("{\"type\":\"ready\"}");
    client->simulateTextMessage("{\"type\":\"settings\"}");
    ws->loop();
    TEST_ASSERT_EQUAL(3, deliveredMessageCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"settings\"}", lastDeliveredPayload);
}

// Regression: the old handler filtered op_code != 0x01, so 0x02 (binary)
// frames never reached any callback.
void test_binary_message_delivered_to_callback() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    const uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF};
    client->simulateBinaryMessage(payload, sizeof(payload));
    ws->loop();
    TEST_ASSERT_EQUAL(1, deliveredBinaryCount);
    TEST_ASSERT_EQUAL(sizeof(payload), lastDeliveredBinaryLen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, lastDeliveredBinary, sizeof(payload));
    TEST_ASSERT_EQUAL(0, deliveredMessageCount);  // not routed to text callback
}

// Verifies the multi-chunk reassembly path: a 4KB binary payload
// arrives as four 1KB chunks; the first chunk's op_code (0x02) is
// captured into _reassemblyIsBinary, continuation chunks (op_code 0x00)
// inherit it. Reassembled bytes must match the original.
void test_binary_chunked_message_reassembled() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();

    // Build a 4KB pattern (each byte = its index modulo 256).
    constexpr size_t total = 4096;
    static uint8_t payload[total];
    for (size_t i = 0; i < total; ++i) payload[i] = (uint8_t)(i & 0xFF);

    client->simulateChunkedMessage(0x02, payload, total, 1024);
    ws->loop();

    TEST_ASSERT_EQUAL(1, deliveredBinaryCount);
    TEST_ASSERT_EQUAL(total, lastDeliveredBinaryLen);
    // Spot-check the bytes — `lastDeliveredBinary` is sized 512, so we
    // only verify the first 512 bytes of the reassembly. The length
    // assertion above already proves the rest arrived.
    for (size_t i = 0; i < 512; ++i) {
        TEST_ASSERT_EQUAL((uint8_t)(i & 0xFF), lastDeliveredBinary[i]);
    }
    TEST_ASSERT_EQUAL(0, deliveredMessageCount);
}

// Phase 8: typed wrappers — onText / onBinary route to the same slots as
// setMessageCallback / setBinaryMessageCallback, but with frame-type-named API.
void test_onText_receives_text_frames() {
    int textCount = 0;
    std::string lastPayload;
    ws->onText([&](const char* payload, size_t len) {
        textCount++;
        lastPayload = std::string(payload, len);
    });
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    client->simulateTextMessage("{\"hello\":1}");
    ws->loop();
    TEST_ASSERT_EQUAL(1, textCount);
    TEST_ASSERT_EQUAL_STRING("{\"hello\":1}", lastPayload.c_str());
}

void test_onBinary_receives_binary_frames() {
    int binaryCount = 0;
    ws->onBinary([&](const uint8_t* data, size_t len) {
        (void)data; (void)len;
        binaryCount++;
    });
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    const uint8_t payload[] = {0xAA, 0xBB};
    client->simulateBinaryMessage(payload, sizeof(payload));
    ws->loop();
    TEST_ASSERT_EQUAL(1, binaryCount);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_name_is_ws);
    RUN_TEST(test_begin_creates_client_with_wss_uri);
    RUN_TEST(test_begin_sets_config_defaults);
    RUN_TEST(test_begin_starts_client);
    RUN_TEST(test_connected_after_connect_event);
    RUN_TEST(test_disconnected_after_disconnect_event);
    RUN_TEST(test_message_delivered_to_callback);
    RUN_TEST(test_connection_callback_on_connect);
    RUN_TEST(test_connection_callback_on_disconnect);
    RUN_TEST(test_send_text_when_connected);
    RUN_TEST(test_send_text_fails_when_disconnected);
    RUN_TEST(test_send_json_serializes_and_calls_send_text);
    RUN_TEST(test_disconnect_sets_not_connected);
    RUN_TEST(test_reconnect_creates_new_client);
    RUN_TEST(test_config_cert_pem_passed_to_client);
    RUN_TEST(test_default_certs_used_by_default);
    RUN_TEST(test_no_cert_when_default_certs_disabled);
    RUN_TEST(test_on_configure_called_before_init);
    RUN_TEST(test_on_configure_can_override_config_cert);
    RUN_TEST(test_on_configure_not_set_works);
    RUN_TEST(test_fifo_absorbs_burst_before_drain);
    RUN_TEST(test_binary_message_delivered_to_callback);
    RUN_TEST(test_binary_chunked_message_reassembled);
    RUN_TEST(test_onText_receives_text_frames);
    RUN_TEST(test_onBinary_receives_binary_frames);
    return UNITY_END();
}
