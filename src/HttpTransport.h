#ifndef COURIER_HTTP_TRANSPORT_H
#define COURIER_HTTP_TRANSPORT_H

#include "Transport.h"
#include <esp_http_client.h>
#include <functional>
#include <string>

namespace Courier {

// Transport-class failure sentinels for Response::status. Any status >= 100
// is a real HTTP status (the server was reached); anything below is a
// transport failure. Retries apply only below 100.
namespace Http {
constexpr int ErrNoWifi   = -1000;
constexpr int ErrDns      = -1001;  // reserved (device maps DNS fail to ErrConnect)
constexpr int ErrConnect  = -1002;
constexpr int ErrTimeout  = -1003;
constexpr int ErrTooLarge = -1004;
constexpr int ErrAborted  = -1005;
}  // namespace Http

// HTTP response value type. Owns the buffered body (PSRAM-preferred heap);
// move-only. In streaming mode (FetchOptions::onBody set) it carries status
// and headers but no body.
class Response {
public:
    Response() = default;
    ~Response() { if (_body) free(_body); }
    Response(Response&& other) noexcept { moveFrom(other); }
    Response& operator=(Response&& other) noexcept {
        if (this != &other) {
            if (_body) free(_body);
            moveFrom(other);
        }
        return *this;
    }
    Response(const Response&) = delete;
    Response& operator=(const Response&) = delete;

    int status = 0;
    long contentLength = -1;  // -1 if the server didn't say

    bool ok() const { return status >= 200 && status <= 299; }
    bool reachedServer() const { return status >= 100; }
    // False when the server promised a Content-Length and the connection
    // dropped or truncated before the full body arrived.
    bool complete() const { return _complete; }
    const char* text() const { return _body ? _body : ""; }
    size_t size() const { return _size; }
    bool json(JsonDocument& doc) const {
        if (!_body) return false;
        // const char* source => ArduinoJson copies; doc outlives Response.
        return deserializeJson(doc, (const char*)_body)
               == DeserializationError::Ok;
    }
    // Collected response headers: Content-Type, Content-Length, Date.
    // Returns nullptr when absent. (Other headers: use FetchOptions::configure
    // or onResponse.)
    const char* header(const char* name) const;

private:
    friend class HttpTransport;
    char* _body = nullptr;  // NUL-terminated at _size; malloc/heap_caps heap
    size_t _size = 0;
    size_t _alloc = 0;
    bool _complete = true;
    std::string _contentType;
    std::string _contentLengthStr;
    std::string _date;

    void moveFrom(Response& other);
    bool appendBody(const char* data, size_t len, size_t cap);
    void captureHeader(const char* key, const char* value);
    char* releaseBody(size_t* outLen);  // hand buffer ownership to the caller
};

// HTTPS transport wrapping esp_http_client. Blocking, JS-shaped fetch();
// full transport citizen: send(doc) POSTs JSON to the Config-seeded endpoint
// and JSON responses dispatch through Client::onMessage. Appliance posture:
// a fresh client per request, full teardown after — no connection state
// survives between fetches.
class HttpTransport : public Transport {
public:
    struct Config {
        const char* cert_pem = nullptr;       // pin a CA (overrides the bundle)
        bool use_cert_bundle = true;          // IDF cert bundle (esp_crt_bundle_attach)
        size_t maxResponseBytes = 16 * 1024;  // buffered-body cap -> ErrTooLarge
        uint32_t timeoutMs = 10000;           // per-request default
        uint8_t retries = 3;                  // transport-failure retries default
    };

    using ResponseCallback  = std::function<void(int status, long contentLength)>;
    using BodyCallback      = std::function<bool(const uint8_t* data, size_t length)>;
    using ConfigureCallback = std::function<void(esp_http_client_config_t&)>;

    struct FetchOptions {
        const char* method = nullptr;  // default "GET"; "POST" when body/json set
        struct Header { const char* name; const char* value; };
        const Header* headers = nullptr;  // caller-owned array
        size_t headerCount = 0;
        const char* body = nullptr;
        size_t bodyLength = 0;         // 0 with body set -> strlen(body)
        JsonDocument* json = nullptr;  // serialize as body + JSON content-type
        int32_t timeoutMs = -1;        // -1 = inherit Config::timeoutMs
        int16_t retries = -1;          // -1 = inherit Config::retries; 0 disables
        ResponseCallback onResponse;   // streaming: fires once, before chunks
        BodyCallback onBody;           // streaming: return false to abort
        ConfigureCallback configure;   // per-call raw-config trapdoor
    };

    HttpTransport() {}
    explicit HttpTransport(const Config& config) : _cfg(config) {}

    // Standing raw-config trapdoor — fires on every fetch after Courier's
    // fields, before the per-call FetchOptions::configure. Reserved fields
    // (overwritten after both hooks): event_handler, user_data.
    void onConfigure(ConfigureCallback cb) { _configureCallback = cb; }

    // Per-transport receive hook for send() responses (like WS onText).
    void onMessage(MessageCallback cb) { setMessageCallback(cb); }

    // Two overloads instead of `opts = {}`: Apple Clang rejects a defaulted
    // FetchOptions argument inside the enclosing class definition ("default
    // member initializer needed within definition of enclosing class").
    Response fetch(const char* url);
    Response fetch(const char* url, const FetchOptions& opts);
    Response get(const char* url) { return fetch(url); }
    Response postJson(const char* url, JsonDocument& doc) {
        FetchOptions opts;
        opts.json = &doc;
        return fetch(url, opts);
    }

    using Transport::begin;  // unhide 3-arg sugar
    void begin() override { _begun = true; }
    void disconnect() override { _begun = false; }
    bool isConnected() const override;
    bool send(JsonDocument& doc, const SendOptions& options = {}) override;
    const char* name() const override { return "HTTP"; }
    bool isPersistent() const override { return false; }

private:
    struct FetchCtx;

    Config _cfg;
    ConfigureCallback _configureCallback;
    bool _begun = false;

    static constexpr uint32_t RETRY_DELAY_MS = 250;

    Response performOnce(const char* url, const FetchOptions& opts,
                         const char* bodyBuf, size_t bodyLen,
                         const char* contentType);
    static esp_err_t eventHandler(esp_http_client_event_t* evt);
};

}  // namespace Courier

#endif  // COURIER_HTTP_TRANSPORT_H
