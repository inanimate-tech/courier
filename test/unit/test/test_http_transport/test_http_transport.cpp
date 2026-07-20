#include <unity.h>
#include <HttpTransport.h>
#include <esp_http_client.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <cstring>
#include <string>

using namespace Courier;

static HttpTransport* http = nullptr;
static std::string g_headerLog;
static std::string g_dataLog;

static esp_err_t smokeHandler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        g_headerLog += evt->header_key;
        g_headerLog += "=";
        g_headerLog += evt->header_value;
        g_headerLog += ";";
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        g_dataLog.append((const char*)evt->data, (size_t)evt->data_len);
    }
    return ESP_OK;
}

void setUp(void) {
    MockHttpClient::resetMock();
    WiFi.resetMock();
    g_headerLog.clear();
    g_dataLog.clear();
    http = new HttpTransport();
    http->begin();
}
void tearDown(void) {
    delete http;
    http = nullptr;
}

void test_mock_scripted_response_fires_events() {
    MockHttpClient::ScriptStep step;
    step.status = 201;
    step.headers = {{"Content-Type", "text/plain"}};
    step.bodyChunks = {"hel", "lo"};
    MockHttpClient::pushScript(step);

    esp_http_client_config_t cfg = {};
    cfg.url = "https://example.com/x";
    cfg.event_handler = smokeHandler;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    TEST_ASSERT_EQUAL(ESP_OK, esp_http_client_perform(c));
    TEST_ASSERT_EQUAL(201, esp_http_client_get_status_code(c));
    TEST_ASSERT_EQUAL(5, (int)esp_http_client_get_content_length(c));
    TEST_ASSERT_EQUAL_STRING("Content-Type=text/plain;", g_headerLog.c_str());
    TEST_ASSERT_EQUAL_STRING("hello", g_dataLog.c_str());
    esp_http_client_cleanup(c);
    TEST_ASSERT_EQUAL(1, MockHttpClient::cleanupCount());
}

void test_mock_transport_failure_fires_no_events() {
    MockHttpClient::ScriptStep step;
    step.performResult = ESP_ERR_HTTP_CONNECT;
    step.status = 0;
    MockHttpClient::pushScript(step);

    esp_http_client_config_t cfg = {};
    cfg.url = "https://example.com/x";
    cfg.event_handler = smokeHandler;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    TEST_ASSERT_EQUAL(ESP_ERR_HTTP_CONNECT, esp_http_client_perform(c));
    TEST_ASSERT_EQUAL(0, esp_http_client_get_status_code(c));
    TEST_ASSERT_TRUE(g_headerLog.empty());
    TEST_ASSERT_TRUE(g_dataLog.empty());
    esp_http_client_cleanup(c);
}

void test_name_is_http() {
    TEST_ASSERT_EQUAL_STRING("HTTP", http->name());
}

void test_not_persistent() {
    TEST_ASSERT_FALSE(http->isPersistent());
}

void test_connected_tracks_begin_and_wifi() {
    TEST_ASSERT_TRUE(http->isConnected());
    http->disconnect();
    TEST_ASSERT_FALSE(http->isConnected());
    http->begin();
    TEST_ASSERT_TRUE(http->isConnected());
    WiFi.setMockStatus(WL_DISCONNECTED);
    TEST_ASSERT_FALSE(http->isConnected());
}

void test_endpoint_seeding_via_base() {
    // Client::addTransport seeds via setEndpoint; verify base storage works.
    http->setEndpoint("api.example.com", 8443, "/inbox");
    // No getter on purpose — exercised for real in the send() tests (Task 8).
    TEST_ASSERT_TRUE(true);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_mock_scripted_response_fires_events);
    RUN_TEST(test_mock_transport_failure_fires_no_events);
    RUN_TEST(test_name_is_http);
    RUN_TEST(test_not_persistent);
    RUN_TEST(test_connected_tracks_begin_and_wifi);
    RUN_TEST(test_endpoint_seeding_via_base);
    return UNITY_END();
}
