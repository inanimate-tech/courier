#ifndef COURIER_WS_TRANSPORT_H
#define COURIER_WS_TRANSPORT_H

#include "CourierTransport.h"
#include <esp_websocket_client.h>
#include <atomic>
#include <functional>
#include <string>

struct CourierWSTransportConfig {
    const char* cert_pem = nullptr;  // TLS cert (nullptr = no cert set by Courier)
};

class CourierWSTransport : public CourierTransport {
public:
    CourierWSTransport();
    explicit CourierWSTransport(const CourierWSTransportConfig& config);
    ~CourierWSTransport();

    // Raw IDF config access — called after Courier fills its fields, before init.
    // Use for custom headers, subprotocol, ping settings, cert, etc.
#ifdef ESP_PLATFORM
    using ConfigureCallback = std::function<void(esp_websocket_client_config_t&)>;
#else
    using ConfigureCallback = std::function<void(esp_websocket_client_config_t&)>;
#endif
    void onConfigure(ConfigureCallback cb);

    void begin(const char* host, uint16_t port, const char* path) override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(const char* payload) override;
    bool sendBinary(const uint8_t* data, size_t len) override;
    const char* name() const override { return "WebSocket"; }
    void suspend() override;
    void resume() override;

    // loop() inherited from base — just calls drainPending()

private:
    const char* _certPem = nullptr;
    ConfigureCallback _configureCallback;

    esp_websocket_client_handle_t _client = nullptr;
    std::atomic<bool> _connected{false};

    void destroyClient();

    // PSRAM reassembly buffer for fragmented messages
    char* _reassemblyBuf = nullptr;
    size_t _reassemblyLen = 0;
    size_t _reassemblyPos = 0;
    void freeReassemblyBuf();

    static void wsEventHandler(void* handler_arg,
                                esp_event_base_t base,
                                int32_t event_id,
                                void* event_data);
};

#endif // COURIER_WS_TRANSPORT_H
