#include "HttpTransport.h"
#include "NetUtil.h"

#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_crt_bundle.h"
#include "esp32-hal.h"  // delay() — Arduino.h conflicts with IDF lwip headers
// Arduino-only builds (prebuilt arduino-esp32 core): WiFiClientSecure ships a
// same-named esp_crt_bundle.h that shadows the IDF one and declares only
// arduino_esp_crt_bundle_attach. The IDF symbol (and the default bundle data)
// is still present in the prebuilt libmbedtls.a — re-declare it so both
// include orders compile. Harmless redeclaration in hybrid/IDF builds.
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);
static const char* TAG = "HttpTransport";
#else
#include <Arduino.h>
#include <WiFi.h>
#include <cstdio>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "HttpTransport";
#endif

namespace Courier {

static bool wifiUp()
{
#ifdef ESP_PLATFORM
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
#else
    return WiFi.status() == WL_CONNECTED;
#endif
}

// PSRAM-preferred body allocation, internal-RAM fallback (same policy as the
// WS/MQTT reassembly buffers). free()/realloc work across both heaps.
static void* bodyRealloc(void* ptr, size_t n)
{
#ifdef ESP_PLATFORM
    void* p = heap_caps_realloc(ptr, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return realloc(ptr, n);
}

static esp_http_client_method_t methodFromString(const char* m)
{
    if (!m) return HTTP_METHOD_GET;
    if (strcasecmp(m, "GET") == 0) return HTTP_METHOD_GET;
    if (strcasecmp(m, "POST") == 0) return HTTP_METHOD_POST;
    if (strcasecmp(m, "PUT") == 0) return HTTP_METHOD_PUT;
    if (strcasecmp(m, "PATCH") == 0) return HTTP_METHOD_PATCH;
    if (strcasecmp(m, "DELETE") == 0) return HTTP_METHOD_DELETE;
    if (strcasecmp(m, "HEAD") == 0) return HTTP_METHOD_HEAD;
    return HTTP_METHOD_GET;
}

bool HttpTransport::isConnected() const
{
    return _begun && wifiUp();
}

// --- Response ---

void Response::moveFrom(Response& other)
{
    status = other.status;
    contentLength = other.contentLength;
    _body = other._body;
    _size = other._size;
    _alloc = other._alloc;
    _complete = other._complete;
    _contentType = std::move(other._contentType);
    _contentLengthStr = std::move(other._contentLengthStr);
    _date = std::move(other._date);
    other._body = nullptr;
    other._size = 0;
    other._alloc = 0;
}

const char* Response::header(const char* name) const
{
    if (!name) return nullptr;
    if (strcasecmp(name, "Content-Type") == 0)
        return _contentType.empty() ? nullptr : _contentType.c_str();
    if (strcasecmp(name, "Content-Length") == 0)
        return _contentLengthStr.empty() ? nullptr : _contentLengthStr.c_str();
    if (strcasecmp(name, "Date") == 0)
        return _date.empty() ? nullptr : _date.c_str();
    return nullptr;
}

void Response::captureHeader(const char* key, const char* value)
{
    if (!key || !value) return;
    if (strcasecmp(key, "Content-Type") == 0) _contentType = value;
    else if (strcasecmp(key, "Content-Length") == 0) _contentLengthStr = value;
    else if (strcasecmp(key, "Date") == 0) _date = value;
}

void Response::resetForNewHop()
{
    if (_body) free(_body);
    _body = nullptr;
    _size = 0;
    _alloc = 0;
    _complete = true;
    _contentType.clear();
    _contentLengthStr.clear();
    _date.clear();
}

char* Response::releaseBody(size_t* outLen)
{
    char* b = _body;
    if (outLen) *outLen = _size;
    _body = nullptr;
    _size = 0;
    _alloc = 0;
    return b;
}

bool Response::appendBody(const char* data, size_t len, size_t cap)
{
    if (_size + len > cap) return false;
    if (_size + len + 1 > _alloc) {
        size_t want = _alloc ? _alloc * 2 : 1024;
        while (want < _size + len + 1) want *= 2;
        if (want > cap + 1) want = cap + 1;
        void* p = bodyRealloc(_body, want);
        if (!p) return false;  // alloc failure: treated as over-cap upstream
        _body = (char*)p;
        _alloc = want;
    }
    memcpy(_body + _size, data, len);
    _size += len;
    _body[_size] = '\0';
    return true;
}

// --- HttpTransport ---

struct HttpTransport::FetchCtx {
    HttpTransport* self = nullptr;
    const FetchOptions* opts = nullptr;
    Response* resp = nullptr;
    esp_http_client_handle_t client = nullptr;
    size_t cap = 0;
    bool streaming = false;
    bool responseFired = false;
    bool sawBody = false;
    bool tooLarge = false;
    bool aborted = false;
};

esp_err_t HttpTransport::eventHandler(esp_http_client_event_t* evt)
{
    auto* ctx = (FetchCtx*)evt->user_data;
    if (!ctx) return ESP_OK;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (ctx->responseFired || ctx->sawBody) {
            // Headers arriving after we'd already latched a response for a
            // previous hop means esp_http_client just followed a redirect
            // (the prior hop's body — e.g. a redirect page — had data of its
            // own). Discard that hop's accumulated body/headers so they
            // don't bleed into (or concatenate with) the new hop's response.
            ctx->resp->resetForNewHop();
            ctx->responseFired = false;
            ctx->sawBody = false;
        }
        ctx->resp->captureHeader(evt->header_key, evt->header_value);
        break;

    case HTTP_EVENT_ON_DATA: {
        int status = esp_http_client_get_status_code(ctx->client);
        bool isRedirectHop = status >= 300 && status < 400;
        if (!ctx->responseFired) {
            ctx->responseFired = true;
            ctx->resp->status = status;
            ctx->resp->contentLength =
                (long)esp_http_client_get_content_length(ctx->client);
            if (ctx->streaming && !isRedirectHop && ctx->opts->onResponse) {
                ctx->opts->onResponse(ctx->resp->status, ctx->resp->contentLength);
            }
        }
        if (isRedirectHop) {
            // A 3xx response's body (if any) isn't payload — status/headers
            // are already captured (above and in ON_HEADER); never accumulate
            // or stream it. This applies equally to an intermediate redirect
            // hop and to a *final* 3xx (disable_auto_redirect) — either way
            // the caller sees status + headers + an empty body.
            break;
        }
        ctx->sawBody = true;
        if (ctx->streaming) {
            if (ctx->opts->onBody &&
                !ctx->opts->onBody((const uint8_t*)evt->data,
                                   (size_t)evt->data_len)) {
                ctx->aborted = true;
                esp_http_client_close(ctx->client);
            }
        } else {
            if (!ctx->resp->appendBody((const char*)evt->data,
                                       (size_t)evt->data_len, ctx->cap)) {
                ESP_LOGW(TAG, "response exceeds %u-byte cap, dropping",
                         (unsigned)ctx->cap);
                ctx->tooLarge = true;
                esp_http_client_close(ctx->client);
            }
        }
        break;
    }

    default:
        break;
    }
    return ESP_OK;
}

Response HttpTransport::performOnce(const char* url, const FetchOptions& opts,
                                    const char* bodyBuf, size_t bodyLen,
                                    const char* contentType)
{
    Response resp;
    FetchCtx ctx;
    ctx.self = this;
    ctx.opts = &opts;
    ctx.resp = &resp;
    ctx.cap = _cfg.maxResponseBytes;
    ctx.streaming = (bool)opts.onBody;

    const char* methodStr =
        opts.method ? opts.method : (bodyBuf ? "POST" : "GET");

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = methodFromString(methodStr);
    cfg.timeout_ms = opts.timeoutMs >= 0 ? (int)opts.timeoutMs
                                         : (int)_cfg.timeoutMs;
    if (_cfg.cert_pem) {
        cfg.cert_pem = _cfg.cert_pem;
    } else if (_cfg.use_cert_bundle) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    if (_configureCallback) _configureCallback(cfg);
    if (opts.configure) opts.configure(cfg);
    // Reserved fields — the response plumbing rides on these; re-assert
    // after both trapdoors so a hook can't sever it.
    cfg.event_handler = &HttpTransport::eventHandler;
    cfg.user_data = &ctx;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        resp.status = Http::ErrConnect;
        return resp;
    }
    ctx.client = client;

    for (size_t i = 0; i < opts.headerCount; i++) {
        esp_http_client_set_header(client, opts.headers[i].name,
                                   opts.headers[i].value);
    }
    if (contentType) {
        esp_http_client_set_header(client, "Content-Type", contentType);
    }
    if (bodyBuf) {
        esp_http_client_set_post_field(client, bodyBuf, (int)bodyLen);
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    long clen = (long)esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);

    if (ctx.aborted) {
        resp.status = Http::ErrAborted;
        return resp;
    }
    if (ctx.tooLarge) {
        resp.status = Http::ErrTooLarge;
        return resp;
    }

    if (err == ESP_OK) {
        if (!ctx.responseFired) {
            // Header-only responses (HEAD, 204, empty body).
            resp.status = status;
            resp.contentLength = clen;
            if (ctx.streaming && opts.onResponse) opts.onResponse(status, clen);
        }
        if (!ctx.streaming && resp.contentLength >= 0 &&
            (long)resp.size() != resp.contentLength) {
            resp._complete = false;
        }
        return resp;
    }

    if (status >= 100 && ctx.sawBody) {
        // Connection dropped mid-body after a real status arrived.
        resp.status = status;
        resp._complete = false;
        return resp;
    }
    resp.status = (err == ESP_ERR_HTTP_FETCH_HEADER) ? Http::ErrTimeout
                                                     : Http::ErrConnect;
    return resp;
}

Response HttpTransport::fetch(const char* url)
{
    return fetch(url, FetchOptions());
}

Response HttpTransport::fetch(const char* url, const FetchOptions& opts)
{
    std::string jsonBody;
    const char* bodyBuf = opts.body;
    size_t bodyLen = opts.bodyLength;
    const char* contentType = nullptr;
    if (opts.json) {
        serializeJson(*opts.json, jsonBody);
        bodyBuf = jsonBody.c_str();
        bodyLen = jsonBody.size();
        contentType = "application/json";
    } else if (bodyBuf && bodyLen == 0) {
        bodyLen = strlen(bodyBuf);
    }

    if (!wifiUp()) {
        Response resp;
        resp.status = Http::ErrNoWifi;
        return resp;
    }

    int attempts = 1 + (opts.retries >= 0 ? opts.retries : (int)_cfg.retries);
    Response resp;
    for (int attempt = 1; attempt <= attempts; attempt++) {
        resp = performOnce(url, opts, bodyBuf, bodyLen, contentType);
        // Any real HTTP status means the server was reached — done, even on
        // 5xx. Abort and over-cap are caller/client-side finals too.
        if (resp.status >= 100 || resp.status == Http::ErrAborted ||
            resp.status == Http::ErrTooLarge) {
            return resp;
        }
        if (attempt < attempts) {
            ESP_LOGW(TAG,
                     "fetch attempt %d/%d failed (%d) - flushing DNS, retrying",
                     attempt, attempts, resp.status);
            // Anycast DNS shuffles record order per query: a fresh resolve
            // is a coin-flip failover away from a broken IP.
            flushDnsCache();
            delay(RETRY_DELAY_MS);
        }
    }
    return resp;
}

bool HttpTransport::send(JsonDocument& doc, const SendOptions& options)
{
    (void)options;  // topic/qos/retain are MQTT concepts — ignored (convention)
    if (!isConnected()) return false;
    if (_host.empty()) {
        ESP_LOGW(TAG, "send: no endpoint host configured");
        return false;
    }

    char url[288];
    int n;
    if (_port == 443 || _port == 0) {
        n = snprintf(url, sizeof(url), "https://%s%s", _host.c_str(), _path.c_str());
    } else {
        n = snprintf(url, sizeof(url), "https://%s:%u%s", _host.c_str(),
                     (unsigned)_port, _path.c_str());
    }
    if (n < 0 || (size_t)n >= sizeof(url)) {
        ESP_LOGW(TAG, "send: url truncated (host/path too long), aborting");
        return false;
    }

    FetchOptions opts;
    opts.json = &doc;
    Response r = fetch(url, opts);

    // Truncated JSON must never reach the raw hook — only dispatch a
    // complete body, even though a truncated-but-2xx response still counts
    // as ok() (and is still returned to the caller as such).
    if (r.ok() && r.complete() && r.size() > 0) {
        const char* ct = r.header("Content-Type");
        if (ct && strstr(ct, "json") != nullptr) {
            // Hand the body buffer (heap-owned, NUL at [len]) to the rx
            // queue zero-copy; drainPending dispatches and frees it.
            size_t len = 0;
            char* buf = r.releaseBody(&len);
            if (buf) queueIncomingMessageOwned(buf, len);
        }
    }
    return r.ok();
}

}  // namespace Courier
