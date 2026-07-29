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

    using Transport::begin;  // unhide 3-arg sugar
    void begin() override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(JsonDocument& doc, const SendOptions& options = {}) override;
    const char* name() const override { return "UDP"; }

    bool isPersistent() const override { return false; }

    // Raw per-packet receive hook — fires for every multicast packet with the
    // payload bytes (NUL-terminated scratch buffer, valid for the callback
    // duration only). Same idiom as WebSocketTransport::onText. UDP is
    // usually a non-default transport, so this is its receive path.
    using TextCallback = MessageCallback;
    void onText(TextCallback cb) { setMessageCallback(cb); }

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
