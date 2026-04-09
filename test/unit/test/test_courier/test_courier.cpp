#include <unity.h>
#include <Courier.h>
#include <CourierTransport.h>
#include <CourierMqttTransport.h>
#include <mqtt_client.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cstring>

// Minimal mock transport for multi-transport tests
class MockTransport : public CourierTransport {
public:
    bool _connected = false;
    bool _started = false;
    bool _suspended = false;
    int sendCount = 0;
    static constexpr int MAX_SENT = 8;
    std::string sentMessages[MAX_SENT];

    void begin(const char* host, uint16_t port, const char* path) override {
        _started = true;
    }
    void disconnect() override {
        _connected = false;
        _started = false;
    }
    bool isConnected() const override { return _connected; }
    bool send(const char* payload) override {
        if (!_connected) return false;
        if (sendCount < MAX_SENT) sentMessages[sendCount] = payload;
        sendCount++;
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

static Courier* courier = nullptr;

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

    CourierConfig config = {
        .host = "test.example.com",
        .port = 443,
        .path = "/ws"
    };
    courier = new Courier(config);
}

void tearDown(void) {
    delete courier;
    courier = nullptr;
    HTTPClient::resetMockDefaults();
}

void test_initial_state() {
    TEST_ASSERT_EQUAL(COURIER_BOOTING, courier->getState());
}

void test_setup_transitions_to_wifi_connecting() {
    courier->setup();
    TEST_ASSERT_EQUAL(COURIER_WIFI_CONNECTING, courier->getState());
}

void test_builtin_ws_registered() {
    CourierTransport* ws = courier->getTransport("ws");
    TEST_ASSERT_NOT_NULL(ws);
}

void test_add_transport() {
    MockTransport mt;
    courier->addTransport("custom", &mt);
    TEST_ASSERT_EQUAL_PTR(&mt, courier->getTransport("custom"));
}

void test_remove_transport() {
    MockTransport mt;
    courier->addTransport("custom", &mt);
    TEST_ASSERT_NOT_NULL(courier->getTransport("custom"));
    courier->removeTransport("custom");
    TEST_ASSERT_NULL(courier->getTransport("custom"));
}

void test_get_transport_unknown() {
    TEST_ASSERT_NULL(courier->getTransport("nonexistent"));
}

void test_on_message_callback() {
    int callCount = 0;
    std::string receivedType;

    courier->onMessage([&](const char* type, JsonDocument& doc) {
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

void test_on_message_multiple_callbacks() {
    int callCount1 = 0;
    int callCount2 = 0;

    courier->onMessage([&](const char* type, JsonDocument& doc) { callCount1++; });
    courier->onMessage([&](const char* type, JsonDocument& doc) { callCount2++; });

    advanceToConnected();

    auto* mock = MockWebSocketClient::lastInstance();
    mock->simulateTextMessage("{\"type\":\"multi\"}");
    courier->loop();

    TEST_ASSERT_EQUAL(1, callCount1);
    TEST_ASSERT_EQUAL(1, callCount2);
}

void test_on_raw_message_callback() {
    int callCount = 0;
    std::string receivedPayload;
    size_t receivedLength = 0;

    courier->onRawMessage([&](const char* payload, size_t length) {
        callCount++;
        receivedPayload = std::string(payload, length);
        receivedLength = length;
    });

    advanceToConnected();

    const char* msg = "{\"type\":\"raw_test\",\"data\":123}";
    auto* mock = MockWebSocketClient::lastInstance();
    mock->simulateTextMessage(msg);
    courier->loop();

    TEST_ASSERT_EQUAL(1, callCount);
    TEST_ASSERT_EQUAL_STRING(msg, receivedPayload.c_str());
    TEST_ASSERT_EQUAL(strlen(msg), receivedLength);
}

void test_send_text_when_connected() {
    advanceToConnected();

    bool result = courier->send("{\"hello\":true}");
    TEST_ASSERT_TRUE(result);

    auto* mock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL(1, mock->sendCount);
    TEST_ASSERT_EQUAL_STRING("{\"hello\":true}", mock->sentMessages[0].c_str());
}

void test_send_text_when_disconnected() {
    courier->setup();
    bool result = courier->send("{\"hello\":true}");
    TEST_ASSERT_FALSE(result);
}

void test_send_targets_default_transport() {
    MockTransport mt;
    courier->addTransport("extra", &mt);

    advanceToConnected();

    mt.simulateConnect();
    mt.loop();

    bool result = courier->send("{\"targeted\":true}");
    TEST_ASSERT_TRUE(result);

    auto* wsMock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL(1, wsMock->sendCount);
    TEST_ASSERT_EQUAL_STRING("{\"targeted\":true}", wsMock->sentMessages[0].c_str());

    TEST_ASSERT_EQUAL(0, mt.sendCount);
}

void test_suspend_resume() {
    advanceToConnected();

    auto* mock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_TRUE(mock->started);

    courier->suspendTransports();
    TEST_ASSERT_TRUE(mock->stopped);
    TEST_ASSERT_FALSE(mock->started);

    courier->resumeTransports();
    TEST_ASSERT_TRUE(mock->started);
}

void test_send_to_named_transport() {
    advanceToConnected();

    bool result = courier->sendTo("ws", "{\"targeted\":true}");
    TEST_ASSERT_TRUE(result);

    auto* mock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL(1, mock->sendCount);
    TEST_ASSERT_EQUAL_STRING("{\"targeted\":true}", mock->sentMessages[0].c_str());
}

void test_send_to_unknown_transport() {
    advanceToConnected();

    bool result = courier->sendTo("nonexistent", "{\"x\":1}");
    TEST_ASSERT_FALSE(result);
}

void test_send_to_disconnected() {
    MockTransport mt;
    courier->addTransport("extra", &mt);

    advanceToConnected();

    bool result = courier->sendTo("extra", "{\"x\":1}");
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_EQUAL(0, mt.sendCount);
}

void test_publish_to_mqtt() {
    MockMqttClient::resetInstanceCount();

    CourierMqttTransport mqttTransport;
    courier->addTransport("mqtt", &mqttTransport);

    advanceToConnected();

    mqttTransport.begin("test.example.com", 443, "/mqtt");
    auto* mqttMock = MockMqttClient::lastInstance();
    TEST_ASSERT_NOT_NULL(mqttMock);
    mqttMock->simulateConnect();

    TEST_ASSERT_TRUE(mqttTransport.isConnected());

    bool result = courier->publishTo("mqtt", "my/topic", "{\"msg\":1}");
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("my/topic", mqttMock->lastPublishTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"msg\":1}", mqttMock->lastPublishPayload.c_str());
    TEST_ASSERT_EQUAL(1, mqttMock->publishCount);
}

void test_publish_to_non_mqtt() {
    advanceToConnected();

    bool result = courier->publishTo("ws", "ignored/topic", "{\"fallback\":true}");
    TEST_ASSERT_TRUE(result);

    auto* mock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL(1, mock->sendCount);
    TEST_ASSERT_EQUAL_STRING("{\"fallback\":true}", mock->sentMessages[0].c_str());
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
    TEST_ASSERT_EQUAL(COURIER_CONNECTED, courier->getState());

    WiFi.setMockStatus(WL_DISCONNECTED);

    for (int i = 0; i < 3; i++) {
        _mock_millis += 5001;
        courier->loop();
    }

    TEST_ASSERT_GREATER_THAN(0, errorCount);
    TEST_ASSERT_EQUAL_STRING("WIFI", lastCategory.c_str());

    WiFi.resetMock();
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_initial_state);
    RUN_TEST(test_setup_transitions_to_wifi_connecting);
    RUN_TEST(test_builtin_ws_registered);
    RUN_TEST(test_add_transport);
    RUN_TEST(test_remove_transport);
    RUN_TEST(test_get_transport_unknown);
    RUN_TEST(test_on_message_callback);
    RUN_TEST(test_on_message_multiple_callbacks);
    RUN_TEST(test_on_raw_message_callback);
    RUN_TEST(test_send_text_when_connected);
    RUN_TEST(test_send_text_when_disconnected);
    RUN_TEST(test_send_targets_default_transport);
    RUN_TEST(test_suspend_resume);
    RUN_TEST(test_send_to_named_transport);
    RUN_TEST(test_send_to_unknown_transport);
    RUN_TEST(test_send_to_disconnected);
    RUN_TEST(test_publish_to_mqtt);
    RUN_TEST(test_publish_to_non_mqtt);
    RUN_TEST(test_on_error_callback_registered);

    return UNITY_END();
}
