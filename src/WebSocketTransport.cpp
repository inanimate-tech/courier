#include "WebSocketTransport.h"
#include <cstring>
#include <utility>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp32-hal.h"  // millis() — Arduino.h conflicts with IDF websocket/lwip headers
// Arduino-only builds (prebuilt arduino-esp32 core): WiFiClientSecure ships a
// same-named esp_crt_bundle.h that shadows the IDF one and declares only
// arduino_esp_crt_bundle_attach. The IDF symbol (and the default bundle data)
// is still present in the prebuilt libmbedtls.a — declare it directly so both
// include orders compile. Harmless redeclaration in hybrid/IDF builds.
extern "C" esp_err_t esp_crt_bundle_attach(void* conf);
static const char* TAG = "WSTransport";
#else
#include <Arduino.h>
#include <cstdio>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "WSTransport";
// Native tests: stand-in with the same shape; the mock config records the
// pointer so tests can assert bundle selection.
static esp_err_t esp_crt_bundle_attach(void* conf) { (void)conf; return 0; }
#endif

namespace Courier {

// Old bundled esp_websocket_client copies (the IDF 4.x SDKs inside Arduino
// 2.x cores) predate the crt_bundle_attach hook on the client config.
// Detect the member at compile time: when absent, return false so begin()
// falls back to the embedded root CA instead of failing to compile.
template <typename C>
static auto tryAttachCertBundle(C& cfg, int)
    -> decltype((void)std::declval<C&>().crt_bundle_attach, bool())
{
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    return true;
}
template <typename C>
static bool tryAttachCertBundle(C&, long) { return false; }

WebSocketTransport::WebSocketTransport()
{
}

// GTS Root R4 — root CA for Cloudflare, Google Cloud, and other major providers.
// Used when use_default_certs is true and no explicit cert_pem is provided.
static const char* GTS_ROOT_R4_PEM =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

WebSocketTransport::WebSocketTransport(const Config& config)
    : _certPem(config.cert_pem),
      _useCertBundle(config.use_cert_bundle),
      _useDefaultCerts(config.use_default_certs)
{
}

void WebSocketTransport::onConfigure(ConfigureCallback cb)
{
    _configureCallback = cb;
}

void WebSocketTransport::useDefaultCerts()
{
    // Explicit call = explicit intent: pin to the embedded GTS Root R4
    // instead of the certificate bundle.
    _useCertBundle = false;
    _useDefaultCerts = true;
}

WebSocketTransport::~WebSocketTransport()
{
    destroyClient();
    freeReassemblyBuf();
}

void WebSocketTransport::freeReassemblyBuf()
{
    free(_reassemblyBuf);
    _reassemblyBuf = nullptr;
    _reassemblyLen = 0;
    _reassemblyPos = 0;
}

void WebSocketTransport::destroyClient()
{
    _selfHealActive = false;
    if (_client) {
        esp_websocket_client_stop(_client);
        esp_websocket_client_destroy(_client);
        _client = nullptr;
    }
    freeReassemblyBuf();
    _connected.store(false, std::memory_order_release);
}

void WebSocketTransport::begin()
{
    // Tear down previous client if reconnecting
    destroyClient();

    // Build wss:// URI
    std::string uri = "wss://";
    uri += _host.c_str();
    uri += ":";
    uri += std::to_string(_port);
    uri += _path.c_str();

    ESP_LOGI(TAG, "Connecting to %s", uri.c_str());

    esp_websocket_client_config_t config = {};
    config.uri = uri.c_str();
    if (_certPem) {
        config.cert_pem = _certPem;
    } else if (_useCertBundle && tryAttachCertBundle(config, 0)) {
        // IDF certificate bundle attached (esp_crt_bundle_attach).
    } else if (_useDefaultCerts) {
        config.cert_pem = GTS_ROOT_R4_PEM;
    }
    // Buffer stays at default 1024. Large messages are fragmented by the
    // library and reassembled in PSRAM by our event handler, keeping
    // internal SRAM free for OTA TLS handshakes.
    config.disable_auto_reconnect = false;
    config.pingpong_timeout_sec = 20;
    config.task_stack = 8192;
    config.user_context = this;

    // Allow caller to modify the raw IDF config before init
    if (_configureCallback) _configureCallback(config);

    _client = esp_websocket_client_init(&config);
    esp_websocket_register_events(_client, WEBSOCKET_EVENT_ANY,
                                   wsEventHandler, this);
    esp_websocket_client_start(_client);
}

void WebSocketTransport::disconnect()
{
    destroyClient();
}

void WebSocketTransport::loop()
{
    drainPending();

    if (_selfHealActive) {
        if (_connected.load(std::memory_order_acquire)) {
            _selfHealActive = false;
        } else if (millis() - _disconnectedSinceMillis >= SELF_HEAL_TIMEOUT) {
            _selfHealActive = false;
            queueTransportFailed();
            drainPending();  // Deliver failure callback immediately
        }
    }
}

bool WebSocketTransport::isConnected() const
{
    return _connected.load(std::memory_order_acquire);
}

bool WebSocketTransport::send(JsonDocument& doc, const SendOptions&)
{
    char buf[1024];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return false;
    return sendText(buf);
}

// Bounded send timeout. A realtime send must never block its caller
// indefinitely: under memory pressure the ESP-IDF WS client can stall mid-send
// (e.g. holding its lock through a failing TLS reconnect), and portMAX_DELAY
// would wedge the caller forever — killing a dedicated audio-send task. A
// healthy send completes in a few ms, well under this bound; on timeout we
// report failure and let the caller defer/retry (or the transport self-heal).
#ifdef ESP_PLATFORM
static constexpr TickType_t SEND_TIMEOUT = pdMS_TO_TICKS(250);
#else
static constexpr uint32_t SEND_TIMEOUT = portMAX_DELAY;  // host mock ignores timeout
#endif

bool WebSocketTransport::sendText(const char* payload)
{
    if (!_connected.load(std::memory_order_acquire) || !_client) return false;
    int result = esp_websocket_client_send_text(_client, payload,
                                                 strlen(payload), SEND_TIMEOUT);
    return result >= 0;  // on timeout/error the caller (Fix B) defers/retries
}

bool WebSocketTransport::sendBinary(const uint8_t* data, size_t len)
{
    if (!_connected.load(std::memory_order_acquire) || !_client) return false;
    int result = esp_websocket_client_send_bin(_client, (const char*)data,
                                                len, SEND_TIMEOUT);
    return result >= 0;  // on timeout/error the caller (Fix B) defers/retries
}

void WebSocketTransport::suspend()
{
    if (_client) {
        ESP_LOGI(TAG, "Suspending (freeing task stack)");
        esp_websocket_client_stop(_client);
        _connected.store(false, std::memory_order_release);
    }
}

void WebSocketTransport::resume()
{
    if (_client) {
        ESP_LOGI(TAG, "Resuming");
        esp_websocket_client_start(_client);
    }
}

void WebSocketTransport::wsEventHandler(void* handler_arg,
                                          esp_event_base_t base,
                                          int32_t event_id,
                                          void* event_data)
{
    (void)base;
    auto* self = (WebSocketTransport*)handler_arg;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected");
        self->_connected.store(true, std::memory_order_release);
        self->queueConnectionChange(true);
        self->_selfHealActive = false;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected");
        self->_connected.store(false, std::memory_order_release);
        self->queueConnectionChange(false);
        self->_disconnectedSinceMillis = millis();
        self->_selfHealActive = true;
        break;

    case WEBSOCKET_EVENT_DATA: {
        auto* data = (esp_websocket_event_data_t*)event_data;
        if (!data->data_ptr || data->data_len <= 0) break;

        // Dispatch text (0x01) and binary (0x02) frames. Control frames
        // (ping/pong/close) are handled by the IDF client; other op_codes
        // are ignored. Continuation chunks (op_code 0x00) inherit the
        // type captured from the first chunk.
        const bool isFirstChunk = (data->payload_offset == 0);
        const bool isSingleChunk =
            (data->payload_len == data->data_len && isFirstChunk);

        if (isFirstChunk) {
            if (data->op_code == 0x01) {
                self->_reassemblyIsBinary = false;
            } else if (data->op_code == 0x02) {
                self->_reassemblyIsBinary = true;
            } else {
                break;  // unsupported op_code on first chunk — drop
            }
        }

        if (isSingleChunk) {
            self->freeReassemblyBuf();
            if (self->_reassemblyIsBinary) {
                self->queueIncomingBinary((const uint8_t*)data->data_ptr,
                                           data->data_len);
            } else {
                self->queueIncomingMessage(data->data_ptr, data->data_len);
            }
            break;
        }

        // Multi-chunk: reassemble into PSRAM. +1 lets us NUL-terminate
        // text payloads; unused for binary. On boards without PSRAM (e.g.
        // M5Dial / ESP32-S3FN8) the SPIRAM alloc returns NULL, which would
        // silently drop every fragmented frame — i.e. any pushed app/shader
        // over ~1KB. Fall back to internal RAM so those still arrive.
        if (isFirstChunk) {
            self->freeReassemblyBuf();
#ifdef ESP_PLATFORM
            self->_reassemblyBuf = (char*)heap_caps_malloc(data->payload_len + 1, MALLOC_CAP_SPIRAM);
            if (!self->_reassemblyBuf)
                self->_reassemblyBuf = (char*)heap_caps_malloc(data->payload_len + 1, MALLOC_CAP_8BIT);
#else
            self->_reassemblyBuf = (char*)malloc(data->payload_len + 1);
#endif
            if (!self->_reassemblyBuf) {
                ESP_LOGW(TAG, "reassembly alloc failed (%d bytes), frame dropped",
                         data->payload_len + 1);
                break;
            }
            self->_reassemblyLen = data->payload_len;
            self->_reassemblyPos = 0;
        }

        if (self->_reassemblyBuf &&
            self->_reassemblyPos + data->data_len <= self->_reassemblyLen) {
            memcpy(self->_reassemblyBuf + self->_reassemblyPos,
                   data->data_ptr, data->data_len);
            self->_reassemblyPos += data->data_len;

            if (self->_reassemblyPos == self->_reassemblyLen) {
                // Hand the buffer to the queue instead of copying — the
                // queue's malloc+memcpy briefly doubled the footprint, which
                // is what capped pushable app size on no-PSRAM boards.
                char* buf = self->_reassemblyBuf;
                size_t len = self->_reassemblyLen;
                self->_reassemblyBuf = nullptr;
                self->_reassemblyLen = 0;
                self->_reassemblyPos = 0;
                if (self->_reassemblyIsBinary) {
                    self->queueIncomingBinaryOwned((uint8_t*)buf, len);
                } else {
                    buf[len] = '\0';
                    self->queueIncomingMessageOwned(buf, len);
                }
            }
        } else {
            ESP_LOGW(TAG, "WS reassembly overflow, dropping frame");
            self->freeReassemblyBuf();
        }
        break;
    }

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

}  // namespace Courier
