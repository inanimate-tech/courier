#pragma once
// Mock esp_http_client for native unit tests, in the style of the
// esp_websocket_client / mqtt_client mocks. Script responses with
// MockHttpClient::pushScript(); each esp_http_client_perform() consumes one
// step. An empty script serves s_defaultStep (a benign 200-with-Date JSON
// response) so Client-level tests that fetch incidentally keep working.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif

#define ESP_ERR_HTTP_BASE         0x7000
#define ESP_ERR_HTTP_CONNECT      (ESP_ERR_HTTP_BASE + 2)
#define ESP_ERR_HTTP_FETCH_HEADER (ESP_ERR_HTTP_BASE + 4)

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
} esp_http_client_event_id_t;

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD,
} esp_http_client_method_t;

class MockHttpClient;
typedef MockHttpClient* esp_http_client_handle_t;

typedef struct esp_http_client_event {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void* data;
    int data_len;
    void* user_data;
    char* header_key;
    char* header_value;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t* evt);

typedef struct {
    const char* url;
    esp_http_client_method_t method;
    int timeout_ms;
    const char* cert_pem;
    esp_err_t (*crt_bundle_attach)(void* conf);
    http_event_handle_cb event_handler;
    void* user_data;
    int buffer_size;
    int buffer_size_tx;
    bool disable_auto_redirect;
} esp_http_client_config_t;

// Native stand-in for the real symbol from esp_crt_bundle.h.
inline esp_err_t esp_crt_bundle_attach(void*) { return ESP_OK; }

class MockHttpClient {
public:
    struct ScriptStep {
        esp_err_t performResult;
        int status;
        long contentLength;  // -2 = derive from total chunk bytes
        std::vector<std::pair<std::string, std::string>> headers;
        std::vector<std::string> bodyChunks;

        ScriptStep() : performResult(ESP_OK), status(200), contentLength(-2) {}
    };

    esp_http_client_config_t config;
    std::string url;
    std::vector<std::pair<std::string, std::string>> requestHeaders;
    std::string postBody;
    bool closed = false;
    int currentStatus = 0;
    long currentContentLength = -1;

    explicit MockHttpClient(const esp_http_client_config_t* cfg) {
        config = *cfg;
        if (cfg->url) url = cfg->url;
        s_lastInstance = this;
        s_lastConfig = *cfg;
        s_instanceCount++;
    }

    esp_err_t perform() {
        s_performCount++;
        ScriptStep step = s_defaultStep;
        if (s_scriptIndex < (int)s_script.size()) step = s_script[s_scriptIndex++];

        if (step.performResult != ESP_OK && step.status < 100) {
            // Transport-class failure: server never reached, no events fire.
            currentStatus = 0;
            currentContentLength = -1;
            return step.performResult;
        }

        currentStatus = step.status;
        size_t total = 0;
        for (auto& c : step.bodyChunks) total += c.size();
        currentContentLength =
            (step.contentLength == -2) ? (long)total : step.contentLength;

        for (auto& h : step.headers) fireHeader(h.first.c_str(), h.second.c_str());
        for (auto& c : step.bodyChunks) {
            if (closed) break;
            fireData(c.data(), c.size());
        }
        if (closed) return ESP_FAIL;
        return step.performResult;
    }

    static void pushScript(const ScriptStep& step) { s_script.push_back(step); }
    static void resetMock() {
        s_script.clear();
        s_scriptIndex = 0;
        s_performCount = 0;
        s_cleanupCount = 0;
        s_instanceCount = 0;
        s_lastInstance = nullptr;
        s_lastConfig = {};
        s_defaultStep = ScriptStep{};
        s_defaultStep.headers = {{"Content-Type", "application/json"},
                                 {"Date", "Tue, 18 Feb 2026 12:00:00 GMT"}};
        s_defaultStep.bodyChunks = {"{}"};
    }

    static MockHttpClient* lastInstance() { return s_lastInstance; }
    static const esp_http_client_config_t& lastConfig() { return s_lastConfig; }
    static int performCount() { return s_performCount; }
    static int cleanupCount() { return s_cleanupCount; }
    static int instanceCount() { return s_instanceCount; }

    inline static int s_cleanupCount = 0;

private:
    void fireHeader(const char* k, const char* v) {
        if (!config.event_handler) return;
        esp_http_client_event_t evt = {};
        evt.event_id = HTTP_EVENT_ON_HEADER;
        evt.client = this;
        evt.user_data = config.user_data;
        evt.header_key = const_cast<char*>(k);
        evt.header_value = const_cast<char*>(v);
        config.event_handler(&evt);
    }
    void fireData(const char* d, size_t n) {
        if (!config.event_handler) return;
        esp_http_client_event_t evt = {};
        evt.event_id = HTTP_EVENT_ON_DATA;
        evt.client = this;
        evt.user_data = config.user_data;
        evt.data = const_cast<char*>(d);
        evt.data_len = (int)n;
        config.event_handler(&evt);
    }

    inline static std::vector<ScriptStep> s_script;
    inline static int s_scriptIndex = 0;
    inline static int s_performCount = 0;
    inline static int s_instanceCount = 0;
    inline static MockHttpClient* s_lastInstance = nullptr;
    inline static esp_http_client_config_t s_lastConfig = {};
    inline static ScriptStep s_defaultStep;
};

inline esp_http_client_handle_t esp_http_client_init(
    const esp_http_client_config_t* config)
{
    return new MockHttpClient(config);
}
inline esp_err_t esp_http_client_set_header(esp_http_client_handle_t c,
                                            const char* key, const char* value)
{
    c->requestHeaders.push_back({key, value});
    return ESP_OK;
}
inline esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c,
                                                const char* data, int len)
{
    c->postBody.assign(data, (size_t)len);
    return ESP_OK;
}
inline esp_err_t esp_http_client_perform(esp_http_client_handle_t c)
{
    return c->perform();
}
inline int esp_http_client_get_status_code(esp_http_client_handle_t c)
{
    return c->currentStatus;
}
inline int64_t esp_http_client_get_content_length(esp_http_client_handle_t c)
{
    return c->currentContentLength;
}
inline esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    c->closed = true;
    return ESP_OK;
}
inline esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    MockHttpClient::s_cleanupCount++;
    // Note: we intentionally DON'T delete c here, to allow tests to access
    // instance data (postBody, requestHeaders) after cleanup. This is a
    // memory leak in tests, but safe for unit testing.
    (void)c;
    return ESP_OK;
}
