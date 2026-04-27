#include "UdpTransport.h"
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char* TAG = "UDPTransport";
#else
#include <cstdio>
#define ESP_LOGI(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[%s] WARN: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[%s] ERROR: " fmt "\n", tag, ##__VA_ARGS__)
static const char* TAG = "UDPTransport";
#endif

namespace Courier {

UdpTransport::UdpTransport() {}

UdpTransport::~UdpTransport() {
    leaveMulticast();
}

void UdpTransport::begin(const char* host, uint16_t port, const char* path) {
    (void)path;
    if (!host || !host[0]) {
        ESP_LOGW(TAG, "No multicast group — skipping begin");
        return;
    }
    leaveMulticast();
    _multicastHost = host;
    _multicastPort = port;
    joinMulticast();
}

void UdpTransport::joinMulticast() {
    if (_joined.load(std::memory_order_acquire)) return;

    IPAddress group;
    group.fromString(_multicastHost.c_str());

    _udp.onPacket([this](AsyncUDPPacket& packet) {
        const char* data = (const char*)packet.data();
        size_t len = packet.length();
        if (!data || len == 0) return;
        queueIncomingMessage(data, len);
    });

    if (_udp.listenMulticast(group, _multicastPort)) {
        _joined.store(true, std::memory_order_release);
        queueConnectionChange(true);
        ESP_LOGI(TAG, "Joined multicast group %s:%d", _multicastHost.c_str(), _multicastPort);
    } else {
        ESP_LOGE(TAG, "Failed to join multicast group");
    }
}

void UdpTransport::leaveMulticast() {
    if (!_joined.load(std::memory_order_acquire)) return;
    _udp.close();
    _joined.store(false, std::memory_order_release);
    queueConnectionChange(false);
    ESP_LOGI(TAG, "Left multicast group");
}

void UdpTransport::disconnect() {
    leaveMulticast();
}

bool UdpTransport::isConnected() const {
    return _joined.load(std::memory_order_acquire);
}

bool UdpTransport::send(const char* payload) {
    if (!_joined.load(std::memory_order_acquire)) return false;
    IPAddress group;
    group.fromString(_multicastHost.c_str());
    _udp.writeTo((const uint8_t*)payload, strlen(payload), group, _multicastPort);
    return true;
}

}  // namespace Courier
