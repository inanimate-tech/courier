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
    struct Config {
        const char* cert_pem = nullptr;      // Specific CA cert in PEM format
        bool use_default_certs = true;       // Use Courier's built-in root CA certs (GTS Root R4)
    };

    WebSocketTransport();
    explicit WebSocketTransport(const Config& config);
    ~WebSocketTransport();

    // Raw IDF config access — called after Courier fills its fields, before init.
    // Use for custom headers, subprotocol, ping settings, cert, etc.
    using ConfigureCallback = std::function<void(esp_websocket_client_config_t&)>;
    void onConfigure(ConfigureCallback cb);
    void useDefaultCerts();  // Use Courier's built-in root CA certs (GTS Root R4)

    void begin(const char* host, uint16_t port, const char* path) override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(const char* payload) override;
    bool sendBinary(const uint8_t* data, size_t len) override;
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
    bool _useDefaultCerts = true;
    ConfigureCallback _configureCallback;

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
