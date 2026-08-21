#ifndef COURIER_WS_TRANSPORT_H
#define COURIER_WS_TRANSPORT_H

#include "Transport.h"
#include <esp_websocket_client.h>
#include <atomic>
#include <functional>
#include <string>

namespace Courier {

class WebSocketTransport : public Transport {
public:
    // TLS precedence: cert_pem (pin) > use_cert_bundle (IDF certificate
    // bundle, the default) > use_default_certs (embedded GTS Root R4 —
    // legacy fallback for builds without MBEDTLS_CERTIFICATE_BUNDLE).
    struct Config {
        const char* cert_pem = nullptr;      // Specific CA cert in PEM format (pin)
        bool use_cert_bundle = true;         // IDF cert bundle (esp_crt_bundle_attach)
        bool use_default_certs = true;       // Embedded GTS Root R4 when bundle disabled
    };

    WebSocketTransport();
    explicit WebSocketTransport(const Config& config);
    ~WebSocketTransport();

    // Raw IDF config access — called after Courier fills its fields, before init.
    // Use for custom headers, subprotocol, ping settings, cert, etc.
    using ConfigureCallback = std::function<void(esp_websocket_client_config_t&)>;
    void onConfigure(ConfigureCallback cb);
    void useDefaultCerts();  // Use Courier's built-in root CA certs (GTS Root R4)

    // Per-frame-type receive hooks.
    using TextCallback = std::function<void(const char* payload, size_t length)>;
    using BinaryCallback = std::function<void(const uint8_t* data, size_t length)>;

    void onText(TextCallback cb)   { setMessageCallback(cb); }
    void onBinary(BinaryCallback cb) { setBinaryMessageCallback(cb); }

    // Tagged binary frames: a 1-byte application-defined channel tag,
    // prefixed on send and stripped on receive. Mechanism only — tag values
    // are the application's contract. Registering onBinaryTagged claims the
    // single binary receive slot (last registration wins, as everywhere in
    // Courier), so it replaces any onBinary handler: every binary frame is
    // then treated as tagged, and empty frames are dropped.
    using TaggedBinaryCallback =
        std::function<void(uint8_t tag, const uint8_t* data, size_t length)>;
    void onBinaryTagged(TaggedBinaryCallback cb) {
        _onTaggedBinary = std::move(cb);
        setBinaryMessageCallback([this](const uint8_t* data, size_t len) {
            if (!_onTaggedBinary || len < 1) return;
            _onTaggedBinary(data[0], data + 1, len - 1);
        });
    }
    bool sendBinaryTagged(uint8_t tag, const uint8_t* data, size_t len);

    using Transport::begin;  // unhide 3-arg sugar
    void begin() override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(JsonDocument& doc, const SendOptions& options = {}) override;
    bool sendText(const char* payload);
    // WS-specific binary frame send (not on Transport base — binary is a
    // WebSocket protocol concept).
    bool sendBinary(const uint8_t* data, size_t len);
    const char* name() const override { return "WebSocket"; }
    void suspend() override;
    void resume() override;

    void loop() override;

private:
    // Self-healing: track disconnect time for failure escalation
    static constexpr unsigned long SELF_HEAL_TIMEOUT = 60000;  // 60 seconds
    unsigned long _disconnectedSinceMillis = 0;
    bool _selfHealActive = false;

    const char* _certPem = nullptr;
    bool _useCertBundle = true;
    bool _useDefaultCerts = true;
    ConfigureCallback _configureCallback;
    TaggedBinaryCallback _onTaggedBinary;

    esp_websocket_client_handle_t _client = nullptr;
    std::atomic<bool> _connected{false};

    void destroyClient();

    // PSRAM reassembly buffer for chunked frames. Shared between text
    // and binary paths — only one frame is in flight per transport at a time.
    char* _reassemblyBuf = nullptr;
    size_t _reassemblyLen = 0;
    size_t _reassemblyPos = 0;
    bool _reassemblyIsBinary = false;
    void freeReassemblyBuf();

    static void wsEventHandler(void* handler_arg,
                                esp_event_base_t base,
                                int32_t event_id,
                                void* event_data);
};

}  // namespace Courier

#endif // COURIER_WS_TRANSPORT_H
