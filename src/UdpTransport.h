#ifndef COURIER_UDP_TRANSPORT_H
#define COURIER_UDP_TRANSPORT_H

#include "Transport.h"
#include <atomic>
#include <string>

#ifdef ESP_PLATFORM
#include <AsyncUDP.h>
#else
#include <AsyncUDP.h>  // Mock for native tests
#endif

namespace Courier {

class UdpTransport : public Transport {
public:
    UdpTransport();
    ~UdpTransport();

    void begin(const char* host, uint16_t port, const char* path) override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(JsonDocument& doc, const SendOptions& options = {}) override;
    const char* name() const override { return "UDP"; }

    bool isPersistent() const override { return false; }

private:
    AsyncUDP _udp;
    std::atomic<bool> _joined{false};
    std::string _multicastHost;
    uint16_t _multicastPort = 0;

    void joinMulticast();
    void leaveMulticast();
};

}  // namespace Courier

#endif // COURIER_UDP_TRANSPORT_H
