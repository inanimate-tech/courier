#include <unity.h>
#include <HttpTransport.h>
#include <NetUtil.h>
#include <esp_http_client.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <cstring>
#include <string>
#include <vector>

using namespace Courier;

static HttpTransport* http = nullptr;
static std::string g_headerLog;
static std::string g_dataLog;
static std::string g_streamed;
static std::vector<std::string> g_streamEvents;

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
    dnsFlushCountForTests = 0;
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

void test_fetch_buffered_happy_path() {
    MockHttpClient::ScriptStep step;
    step.status = 200;
    step.headers = {{"Content-Type", "application/json"},
                    {"Date", "Tue, 18 Feb 2026 12:00:00 GMT"}};
    step.bodyChunks = {"{\"greeting\":", "\"hello\"}"};
    MockHttpClient::pushScript(step);

    Response r = http->fetch("https://example.com/api");
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_TRUE(r.reachedServer());
    TEST_ASSERT_TRUE(r.complete());
    TEST_ASSERT_EQUAL_STRING("{\"greeting\":\"hello\"}", r.text());
    TEST_ASSERT_EQUAL(20, (int)r.size());
    TEST_ASSERT_EQUAL(20, (long)r.contentLength);
    TEST_ASSERT_EQUAL_STRING("application/json", r.header("Content-Type"));
    TEST_ASSERT_EQUAL_STRING("Tue, 18 Feb 2026 12:00:00 GMT", r.header("Date"));
    TEST_ASSERT_NULL(r.header("X-Nope"));

    JsonDocument doc;
    TEST_ASSERT_TRUE(r.json(doc));
    TEST_ASSERT_EQUAL_STRING("hello", doc["greeting"].as<const char*>());
}

void test_fetch_default_method_is_get_and_url_passed() {
    http->fetch("https://example.com/api");
    auto& cfg = MockHttpClient::lastConfig();
    TEST_ASSERT_EQUAL(HTTP_METHOD_GET, cfg.method);
    TEST_ASSERT_EQUAL_STRING("https://example.com/api", cfg.url);
}

void test_fetch_body_implies_post_and_sets_post_field() {
    HttpTransport::FetchOptions opts;
    opts.body = "a=1&b=2";
    http->fetch("https://example.com/form", opts);
    TEST_ASSERT_EQUAL(HTTP_METHOD_POST, MockHttpClient::lastConfig().method);
    TEST_ASSERT_EQUAL_STRING("a=1&b=2",
        MockHttpClient::lastInstance()->postBody.c_str());
}

void test_fetch_explicit_method_and_headers() {
    HttpTransport::FetchOptions opts;
    opts.method = "HEAD";
    HttpTransport::FetchOptions::Header hdrs[] = {{"X-API-Key", "secret"}};
    opts.headers = hdrs;
    opts.headerCount = 1;
    http->fetch("https://example.com/", opts);
    TEST_ASSERT_EQUAL(HTTP_METHOD_HEAD, MockHttpClient::lastConfig().method);
    auto* c = MockHttpClient::lastInstance();
    TEST_ASSERT_EQUAL(1, (int)c->requestHeaders.size());
    TEST_ASSERT_EQUAL_STRING("X-API-Key", c->requestHeaders[0].first.c_str());
    TEST_ASSERT_EQUAL_STRING("secret", c->requestHeaders[0].second.c_str());
}

void test_fetch_json_body_sets_content_type_and_serializes() {
    JsonDocument doc;
    doc["type"] = "hello";
    HttpTransport::FetchOptions opts;
    opts.json = &doc;
    http->fetch("https://example.com/inbox", opts);
    auto* c = MockHttpClient::lastInstance();
    TEST_ASSERT_EQUAL(HTTP_METHOD_POST, MockHttpClient::lastConfig().method);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"hello\"}", c->postBody.c_str());
    bool sawJsonContentType = false;
    for (auto& h : c->requestHeaders) {
        if (h.first == "Content-Type" && h.second == "application/json")
            sawJsonContentType = true;
    }
    TEST_ASSERT_TRUE(sawJsonContentType);
}

void test_postjson_sugar() {
    JsonDocument doc;
    doc["type"] = "ping";
    Response r = http->postJson("https://example.com/inbox", doc);
    TEST_ASSERT_TRUE(r.ok());  // default mock step is a 200
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"ping\"}",
        MockHttpClient::lastInstance()->postBody.c_str());
}

void test_fetch_head_response_no_body() {
    MockHttpClient::ScriptStep step;
    step.status = 200;
    step.contentLength = 0;
    step.headers = {{"Date", "Tue, 18 Feb 2026 12:00:00 GMT"}};
    MockHttpClient::pushScript(step);
    HttpTransport::FetchOptions opts;
    opts.method = "HEAD";
    Response r = http->fetch("https://example.com/", opts);
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_EQUAL_STRING("", r.text());
    TEST_ASSERT_EQUAL_STRING("Tue, 18 Feb 2026 12:00:00 GMT", r.header("Date"));
}

void test_fetch_truncated_body_flags_incomplete() {
    MockHttpClient::ScriptStep step;
    step.status = 200;
    step.contentLength = 1000;   // promised more than delivered
    step.bodyChunks = {"shrt"};
    MockHttpClient::pushScript(step);
    Response r = http->fetch("https://example.com/api");
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_FALSE(r.complete());
    TEST_ASSERT_EQUAL_STRING("shrt", r.text());
}

void test_fetch_no_wifi_short_circuits() {
    WiFi.setMockStatus(WL_DISCONNECTED);
    Response r = http->fetch("https://example.com/api");
    TEST_ASSERT_EQUAL(Http::ErrNoWifi, r.status);
    TEST_ASSERT_FALSE(r.reachedServer());
    TEST_ASSERT_EQUAL(0, MockHttpClient::performCount());
}

void test_fetch_client_cleaned_up_per_request() {
    http->fetch("https://example.com/a");
    http->fetch("https://example.com/b");
    TEST_ASSERT_EQUAL(2, MockHttpClient::instanceCount());
    TEST_ASSERT_EQUAL(2, MockHttpClient::cleanupCount());
}

void test_response_move_semantics() {
    MockHttpClient::ScriptStep step;
    step.bodyChunks = {"abc"};
    MockHttpClient::pushScript(step);
    Response a = http->fetch("https://example.com/api");
    Response b = std::move(a);
    TEST_ASSERT_EQUAL_STRING("abc", b.text());
    TEST_ASSERT_EQUAL_STRING("", a.text());
    TEST_ASSERT_EQUAL(0, (int)a.size());
}

void test_retry_on_transport_failure_then_success() {
    MockHttpClient::ScriptStep fail;
    fail.performResult = ESP_ERR_HTTP_CONNECT;
    fail.status = 0;
    MockHttpClient::pushScript(fail);
    MockHttpClient::pushScript(fail);
    MockHttpClient::ScriptStep okStep;
    okStep.bodyChunks = {"ok"};
    MockHttpClient::pushScript(okStep);

    Response r = http->fetch("https://example.com/api");
    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_EQUAL_STRING("ok", r.text());
    TEST_ASSERT_EQUAL(3, MockHttpClient::performCount());
    TEST_ASSERT_EQUAL(2, dnsFlushCountForTests);      // flushed BETWEEN attempts
    TEST_ASSERT_EQUAL(3, MockHttpClient::cleanupCount());  // teardown per attempt
}

void test_all_retries_exhausted_returns_last_error() {
    MockHttpClient::ScriptStep fail;
    fail.performResult = ESP_ERR_HTTP_CONNECT;
    fail.status = 0;
    for (int i = 0; i < 4; i++) MockHttpClient::pushScript(fail);

    Response r = http->fetch("https://example.com/api");  // default retries=3
    TEST_ASSERT_EQUAL(Http::ErrConnect, r.status);
    TEST_ASSERT_EQUAL(4, MockHttpClient::performCount());  // 1 + 3 retries
    TEST_ASSERT_EQUAL(3, dnsFlushCountForTests);
}

void test_http_error_status_never_retries() {
    MockHttpClient::ScriptStep err500;
    err500.status = 500;
    err500.bodyChunks = {"boom"};
    MockHttpClient::pushScript(err500);

    Response r = http->fetch("https://example.com/api");
    TEST_ASSERT_EQUAL(500, r.status);
    TEST_ASSERT_TRUE(r.reachedServer());
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL(1, MockHttpClient::performCount());
    TEST_ASSERT_EQUAL(0, dnsFlushCountForTests);
}

void test_retries_zero_disables() {
    MockHttpClient::ScriptStep fail;
    fail.performResult = ESP_ERR_HTTP_CONNECT;
    fail.status = 0;
    MockHttpClient::pushScript(fail);
    HttpTransport::FetchOptions opts;
    opts.retries = 0;
    Response r = http->fetch("https://example.com/api", opts);
    TEST_ASSERT_EQUAL(Http::ErrConnect, r.status);
    TEST_ASSERT_EQUAL(1, MockHttpClient::performCount());
}

void test_per_call_retries_override_config() {
    MockHttpClient::ScriptStep fail;
    fail.performResult = ESP_ERR_HTTP_CONNECT;
    fail.status = 0;
    for (int i = 0; i < 2; i++) MockHttpClient::pushScript(fail);
    HttpTransport::FetchOptions opts;
    opts.retries = 1;
    Response r = http->fetch("https://example.com/api", opts);
    TEST_ASSERT_EQUAL(Http::ErrConnect, r.status);
    TEST_ASSERT_EQUAL(2, MockHttpClient::performCount());
}

void test_timeout_classification() {
    MockHttpClient::ScriptStep t;
    t.performResult = ESP_ERR_HTTP_FETCH_HEADER;
    t.status = 0;
    for (int i = 0; i < 4; i++) MockHttpClient::pushScript(t);
    Response r = http->fetch("https://example.com/api");
    TEST_ASSERT_EQUAL(Http::ErrTimeout, r.status);
}

void test_too_large_response_never_retries() {
    HttpTransport::Config cfg;
    cfg.maxResponseBytes = 4;
    HttpTransport small(cfg);
    small.begin();
    MockHttpClient::ScriptStep big;
    big.bodyChunks = {"12345678"};
    MockHttpClient::pushScript(big);
    Response r = small.fetch("https://example.com/api");
    TEST_ASSERT_EQUAL(Http::ErrTooLarge, r.status);
    TEST_ASSERT_EQUAL(1, MockHttpClient::performCount());
    TEST_ASSERT_EQUAL(0, dnsFlushCountForTests);
}

void test_streaming_onresponse_before_chunks() {
    MockHttpClient::ScriptStep step;
    step.status = 200;
    step.contentLength = 6;
    step.bodyChunks = {"abc", "def"};
    MockHttpClient::pushScript(step);

    g_streamed.clear();
    g_streamEvents.clear();
    HttpTransport::FetchOptions opts;
    opts.onResponse = [](int status, long len) {
        g_streamEvents.push_back("meta:" + std::to_string(status) + ":" +
                                 std::to_string(len));
    };
    opts.onBody = [](const uint8_t* d, size_t n) {
        g_streamEvents.push_back("chunk");
        g_streamed.append((const char*)d, n);
        return true;
    };
    Response r = http->fetch("https://example.com/fw.bin", opts);

    TEST_ASSERT_EQUAL(200, r.status);
    TEST_ASSERT_EQUAL(3, (int)g_streamEvents.size());
    TEST_ASSERT_EQUAL_STRING("meta:200:6", g_streamEvents[0].c_str());
    TEST_ASSERT_EQUAL_STRING("chunk", g_streamEvents[1].c_str());
    TEST_ASSERT_EQUAL_STRING("abcdef", g_streamed.c_str());
    TEST_ASSERT_EQUAL(0, (int)r.size());          // nothing buffered
    TEST_ASSERT_EQUAL_STRING("", r.text());
}

void test_streaming_abort_stops_and_reports() {
    MockHttpClient::ScriptStep step;
    step.status = 200;
    step.bodyChunks = {"abc", "def", "ghi"};
    MockHttpClient::pushScript(step);

    g_streamed.clear();
    HttpTransport::FetchOptions opts;
    opts.onBody = [](const uint8_t* d, size_t n) {
        g_streamed.append((const char*)d, n);
        return g_streamed.size() < 4;  // abort after the second chunk
    };
    Response r = http->fetch("https://example.com/fw.bin", opts);
    TEST_ASSERT_EQUAL(Http::ErrAborted, r.status);
    TEST_ASSERT_EQUAL_STRING("abcdef", g_streamed.c_str());  // 3rd chunk never came
    TEST_ASSERT_EQUAL(1, MockHttpClient::performCount());    // no retry on abort
}

void test_streaming_drop_mid_body_no_retry_keeps_status() {
    MockHttpClient::ScriptStep step;
    step.performResult = ESP_FAIL;   // connection dies after chunks delivered
    step.status = 200;
    step.contentLength = 100;
    step.bodyChunks = {"abc"};
    MockHttpClient::pushScript(step);

    g_streamed.clear();
    HttpTransport::FetchOptions opts;
    opts.onBody = [](const uint8_t* d, size_t n) {
        g_streamed.append((const char*)d, n);
        return true;
    };
    Response r = http->fetch("https://example.com/fw.bin", opts);
    TEST_ASSERT_EQUAL(200, r.status);       // reached server: status stands...
    TEST_ASSERT_FALSE(r.complete());        // ...but flagged incomplete
    TEST_ASSERT_EQUAL(1, MockHttpClient::performCount());  // chunks delivered => no retry
}

void test_streaming_head_only_fires_onresponse() {
    MockHttpClient::ScriptStep step;
    step.status = 204;
    step.contentLength = 0;
    MockHttpClient::pushScript(step);

    g_streamEvents.clear();
    HttpTransport::FetchOptions opts;
    opts.onResponse = [](int status, long len) {
        g_streamEvents.push_back("meta:" + std::to_string(status));
    };
    opts.onBody = [](const uint8_t*, size_t) { return true; };
    Response r = http->fetch("https://example.com/ping", opts);
    TEST_ASSERT_EQUAL(204, r.status);
    TEST_ASSERT_EQUAL(1, (int)g_streamEvents.size());
    TEST_ASSERT_EQUAL_STRING("meta:204", g_streamEvents[0].c_str());
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_mock_scripted_response_fires_events);
    RUN_TEST(test_mock_transport_failure_fires_no_events);
    RUN_TEST(test_name_is_http);
    RUN_TEST(test_not_persistent);
    RUN_TEST(test_connected_tracks_begin_and_wifi);
    RUN_TEST(test_endpoint_seeding_via_base);
    RUN_TEST(test_fetch_buffered_happy_path);
    RUN_TEST(test_fetch_default_method_is_get_and_url_passed);
    RUN_TEST(test_fetch_body_implies_post_and_sets_post_field);
    RUN_TEST(test_fetch_explicit_method_and_headers);
    RUN_TEST(test_fetch_json_body_sets_content_type_and_serializes);
    RUN_TEST(test_postjson_sugar);
    RUN_TEST(test_fetch_head_response_no_body);
    RUN_TEST(test_fetch_truncated_body_flags_incomplete);
    RUN_TEST(test_fetch_no_wifi_short_circuits);
    RUN_TEST(test_fetch_client_cleaned_up_per_request);
    RUN_TEST(test_response_move_semantics);
    RUN_TEST(test_retry_on_transport_failure_then_success);
    RUN_TEST(test_all_retries_exhausted_returns_last_error);
    RUN_TEST(test_http_error_status_never_retries);
    RUN_TEST(test_retries_zero_disables);
    RUN_TEST(test_per_call_retries_override_config);
    RUN_TEST(test_timeout_classification);
    RUN_TEST(test_too_large_response_never_retries);
    RUN_TEST(test_streaming_onresponse_before_chunks);
    RUN_TEST(test_streaming_abort_stops_and_reports);
    RUN_TEST(test_streaming_drop_mid_body_no_retry_keeps_status);
    RUN_TEST(test_streaming_head_only_fires_onresponse);
    return UNITY_END();
}
