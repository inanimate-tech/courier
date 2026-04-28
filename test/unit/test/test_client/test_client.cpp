#include <unity.h>
#include <Courier.h>
#include <Transport.h>
#include <MqttTransport.h>
#include <mqtt_client.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cstring>
#include <vector>

using namespace Courier;

// Minimal mock transport for multi-transport tests
class MockTransport : public Transport {
public:
    bool _connected = false;
    bool _started = false;
    bool _suspended = false;
    int sendCount = 0;
    static constexpr int MAX_SENT = 8;
    std::string sentMessages[MAX_SENT];     // serialized JSON
    SendOptions lastOptions;                 // last options seen

    void begin(const char* host, uint16_t port, const char* path) override {
        _started = true;
    }
    void disconnect() override {
        _connected = false;
        _started = false;
    }
    bool isConnected() const override { return _connected; }
    bool send(JsonDocument& doc, const SendOptions& options = {}) override {
        if (!_connected) return false;
        if (sendCount < MAX_SENT) {
            char buf[256];
            size_t n = serializeJson(doc, buf, sizeof(buf));
            if (n > 0 && n < sizeof(buf)) sentMessages[sendCount] = buf;
        }
        sendCount++;
        lastOptions = options;
        return true;
    }
    bool sendBinary(const uint8_t* data, size_t len) override {
        return _connected;
    }
    const char* name() const override { return "MockTransport"; }
    void suspend() override { _suspended = true; _connected = false; }
    void resume() override { _suspended = false; }

    void simulateMessage(const char* payload) {
        queueIncomingMessage(payload, strlen(payload));
    }

    void simulateConnect() {
        _connected = true;
        queueConnectionChange(true);
    }

    void simulateDisconnect() {
        _connected = false;
        queueConnectionChange(false);
    }
};

static Client* courier = nullptr;

static void advanceToConnected() {
    courier->setup();
    courier->loop();  // WIFI_CONNECTING -> WIFI_CONNECTED
    courier->loop();  // WIFI_CONNECTED -> TRANSPORTS_CONNECTING
    courier->loop();  // TRANSPORTS_CONNECTING: calls begin(), creates mock WS client

    auto* mock = MockWebSocketClient::lastInstance();
    mock->simulateConnect();

    courier->loop();  // Transport loop drains connect -> CONNECTED
}

void setUp(void) {
    _mock_millis = 0;
    WiFi.resetMock();
    HTTPClient::setDefaultMockResponse(200, "{}");
    HTTPClient::setDefaultMockHeader("Tue, 18 Feb 2026 12:00:00 GMT");
    Serial.stopCapture();

    Config config;
    config.host = "test.example.com";
    config.port = 443;
    config.path = "/ws";
    courier = new Client(config);
}

void tearDown(void) {
    delete courier;
    courier = nullptr;
    HTTPClient::resetMockDefaults();
}

void test_initial_state() {
    TEST_ASSERT_TRUE(courier->getState() == State::Booting);
}

void test_setup_transitions_to_wifi_connecting() {
    courier->setup();
    TEST_ASSERT_TRUE(courier->getState() == State::WifiConnecting);
}

void test_builtin_ws_registered() {
    // transport<>() asserts on miss; surviving the call confirms registration.
    (void)courier->transport<WebSocketTransport>("ws");
}

void test_add_transport_returns_typed_ref() {
    auto& mt = courier->addTransport<MockTransport>("custom");
    TEST_ASSERT_EQUAL_PTR(&mt, &courier->transport<MockTransport>("custom"));
}

void test_remove_transport_frees_slot() {
    courier->addTransport<MockTransport>("custom");
    courier->removeTransport("custom");
    // Slot freed — re-adding the same name succeeds (would assert if still present).
    courier->addTransport<MockTransport>("custom");
}

void test_on_message_callback() {
    int callCount = 0;
    std::string receivedType;

    courier->onMessage([&](const char* tname, const char* type, JsonDocument& doc) {
        callCount++;
        receivedType = type;
    });

    advanceToConnected();

    auto* mock = MockWebSocketClient::lastInstance();
    mock->simulateTextMessage("{\"type\":\"test_msg\"}");
    courier->loop();

    TEST_ASSERT_EQUAL(1, callCount);
    TEST_ASSERT_EQUAL_STRING("test_msg", receivedType.c_str());
}

void test_on_message_single_slot() {
    int callCount1 = 0;
    int callCount2 = 0;

    courier->onMessage([&](const char* tname, const char* type, JsonDocument& doc) { callCount1++; });
    // Second registration replaces the first (single-slot, like ws.onmessage)
    courier->onMessage([&](const char* tname, const char* type, JsonDocument& doc) { callCount2++; });

    advanceToConnected();

    auto* mock = MockWebSocketClient::lastInstance();
    mock->simulateTextMessage("{\"type\":\"multi\"}");
    courier->loop();

    TEST_ASSERT_EQUAL(0, callCount1);  // replaced — never called
    TEST_ASSERT_EQUAL(1, callCount2);  // last registration wins
}

void test_onMessage_only_fires_for_json() {
    int jsonCount = 0;
    courier->onMessage([&](const char* tname, const char* type, JsonDocument& doc) { jsonCount++; });
    advanceToConnected();
    auto* mock = MockWebSocketClient::lastInstance();
    mock->simulateTextMessage("not json");
    courier->loop();
    TEST_ASSERT_EQUAL(0, jsonCount);
    mock->simulateTextMessage("{\"type\":\"x\"}");
    courier->loop();
    TEST_ASSERT_EQUAL(1, jsonCount);
}

void test_suspend_resume() {
    advanceToConnected();

    auto* mock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_TRUE(mock->started);

    courier->suspend();
    TEST_ASSERT_TRUE(mock->stopped);
    TEST_ASSERT_FALSE(mock->started);

    courier->resume();
    TEST_ASSERT_TRUE(mock->started);
}

void test_on_error_callback_registered() {
    int errorCount = 0;
    std::string lastCategory;
    std::string lastMessage;

    courier->onError([&](const char* category, const char* message) {
        errorCount++;
        lastCategory = category;
        lastMessage = message;
    });

    advanceToConnected();
    TEST_ASSERT_TRUE(courier->getState() == State::Connected);

    WiFi.setMockStatus(WL_DISCONNECTED);

    for (int i = 0; i < 3; i++) {
        _mock_millis += 5001;
        courier->loop();
    }

    TEST_ASSERT_GREATER_THAN(0, errorCount);
    TEST_ASSERT_EQUAL_STRING("WIFI", lastCategory.c_str());

    WiFi.resetMock();
}

void test_connection_change_fires_on_setup() {
    std::vector<State> states;
    courier->onConnectionChange([&](State s) {
        states.push_back(s);
    });
    courier->setup();
    TEST_ASSERT_EQUAL(1, states.size());
    TEST_ASSERT_TRUE(states[0] == State::WifiConnecting);
}

void test_connection_change_fires_through_to_connected() {
    std::vector<State> states;
    courier->onConnectionChange([&](State s) {
        states.push_back(s);
    });
    advanceToConnected();

    // Should see: WifiConnecting, WifiConnected, TransportsConnecting, Connected
    TEST_ASSERT_TRUE(states.size() >= 4);
    TEST_ASSERT_TRUE(states[0] == State::WifiConnecting);
    TEST_ASSERT_TRUE(states[1] == State::WifiConnected);
    TEST_ASSERT_TRUE(states[2] == State::TransportsConnecting);
    TEST_ASSERT_TRUE(states[3] == State::Connected);
}

void test_no_builtin_ws_when_host_null() {
    delete courier;
    Config cfg;
    // host is null by default
    courier = new Client(cfg);
    // Without a host, no built-in "ws" is registered. Looking it up
    // would assert. Instead, register a custom transport — succeeds
    // because slot 0 is free. (Pre-Phase-9, slot 0 was occupied by
    // the auto-registered WS, so this would have used slot 1.)
    courier->addTransport<MockTransport>("ws");
    auto& mt = courier->transport<MockTransport>("ws");
    TEST_ASSERT_EQUAL_STRING("MockTransport", mt.name());
}

void test_dns_config_defaults_to_zero() {
    Config config;
    config.host = "test.example.com";
    TEST_ASSERT_EQUAL_UINT32(0, config.dns1);
    TEST_ASSERT_EQUAL_UINT32(0, config.dns2);
}

void test_dns_config_custom_servers() {
    Config config;
    config.host = "test.example.com";
    config.dns1 = (uint32_t)IPAddress(8, 8, 8, 8);
    config.dns2 = (uint32_t)IPAddress(1, 1, 1, 1);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)IPAddress(8, 8, 8, 8), config.dns1);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)IPAddress(1, 1, 1, 1), config.dns2);
}

void test_message_callback_receives_transport_name() {
    std::string seenName;
    courier->onMessage([&](const char* tname, const char* type, JsonDocument& doc) {
        seenName = tname ? tname : "";
    });

    advanceToConnected();
    auto* mock = MockWebSocketClient::lastInstance();
    mock->simulateTextMessage("{\"type\":\"hi\"}");
    courier->loop();

    TEST_ASSERT_EQUAL_STRING("ws", seenName.c_str());
}

void test_client_send_routes_to_default_transport() {
    delete courier;
    Config cfg;
    cfg.host = "test.example.com";
    cfg.defaultTransport = "ws";
    courier = new Client(cfg);

    advanceToConnected();

    JsonDocument doc;
    doc["type"] = "hello";
    bool ok = courier->send(doc);
    TEST_ASSERT_TRUE(ok);

    auto* mock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL(1, mock->sendCount);
    // Mock records the serialized text; verify it parses back to expected.
    TEST_ASSERT_NOT_NULL(strstr(mock->sentMessages[0].c_str(), "hello"));
}

void test_client_send_returns_false_when_no_default_transport() {
    JsonDocument doc;
    doc["type"] = "x";
    // courier was constructed in setUp with no defaultTransport set
    bool ok = courier->send(doc);
    TEST_ASSERT_FALSE(ok);
}

void test_set_default_transport_overrides_config() {
    delete courier;
    Config cfg;
    cfg.host = "test.example.com";
    // No defaultTransport in config — set it via runtime API.
    courier = new Client(cfg);
    courier->setDefaultTransport("ws");

    advanceToConnected();

    JsonDocument doc;
    doc["type"] = "ping";
    bool ok = courier->send(doc);
    TEST_ASSERT_TRUE(ok);
    auto* mock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL(1, mock->sendCount);
}

void test_client_send_with_options_passes_topic() {
    // Use a MockTransport so we can inspect the SendOptions passed through.
    delete courier;
    Config cfg;
    // Leave host null so no built-in WS auto-registers; we register
    // our mock under "primary" and route Client::send through it.
    courier = new Client(cfg);
    auto& mt = courier->addTransport<MockTransport>("primary");
    courier->setDefaultTransport("primary");
    mt.simulateConnect();
    courier->loop();  // drain the connect signal

    JsonDocument doc;
    doc["msg"] = "telemetry";
    SendOptions opts;
    opts.topic = "sensors/me";
    opts.qos = 1;
    opts.retain = true;
    bool ok = courier->send(doc, opts);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL(1, mt.sendCount);
    TEST_ASSERT_EQUAL_STRING("sensors/me", mt.lastOptions.topic);
    TEST_ASSERT_EQUAL(1, mt.lastOptions.qos);
    TEST_ASSERT_TRUE(mt.lastOptions.retain);
    TEST_ASSERT_NOT_NULL(strstr(mt.sentMessages[0].c_str(), "telemetry"));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_initial_state);
    RUN_TEST(test_setup_transitions_to_wifi_connecting);
    RUN_TEST(test_builtin_ws_registered);
    RUN_TEST(test_add_transport_returns_typed_ref);
    RUN_TEST(test_remove_transport_frees_slot);
    RUN_TEST(test_on_message_callback);
    RUN_TEST(test_on_message_single_slot);
    RUN_TEST(test_onMessage_only_fires_for_json);
    RUN_TEST(test_suspend_resume);
    RUN_TEST(test_on_error_callback_registered);
    RUN_TEST(test_connection_change_fires_on_setup);
    RUN_TEST(test_connection_change_fires_through_to_connected);
    RUN_TEST(test_no_builtin_ws_when_host_null);
    RUN_TEST(test_dns_config_defaults_to_zero);
    RUN_TEST(test_dns_config_custom_servers);
    RUN_TEST(test_message_callback_receives_transport_name);
    RUN_TEST(test_client_send_routes_to_default_transport);
    RUN_TEST(test_client_send_returns_false_when_no_default_transport);
    RUN_TEST(test_set_default_transport_overrides_config);
    RUN_TEST(test_client_send_with_options_passes_topic);

    return UNITY_END();
}
