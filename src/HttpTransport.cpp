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
    // Implemented in Task 4.
    (void)data; (void)len; (void)cap;
    return false;
}

// --- HttpTransport ---

esp_err_t HttpTransport::eventHandler(esp_http_client_event_t* evt)
{
    // Implemented in Task 4.
    (void)evt;
    return ESP_OK;
}

Response HttpTransport::performOnce(const char* url, const FetchOptions& opts,
                                    const char* bodyBuf, size_t bodyLen,
                                    const char* contentType)
{
    // Implemented in Task 4.
    (void)url; (void)opts; (void)bodyBuf; (void)bodyLen; (void)contentType;
    Response resp;
    resp.status = Http::ErrConnect;
    return resp;
}

Response HttpTransport::fetch(const char* url, const FetchOptions& opts)
{
    // Single attempt for now; retry loop lands in Task 5.
    (void)url; (void)opts;
    Response resp;
    resp.status = Http::ErrConnect;
    return resp;
}

bool HttpTransport::send(JsonDocument& doc, const SendOptions& options)
{
    // Implemented in Task 8.
    (void)doc; (void)options;
    return false;
}

}  // namespace Courier
