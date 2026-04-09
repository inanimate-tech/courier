#pragma once

#include "Arduino.h"
#include <cstdint>

typedef enum {
    WL_IDLE_STATUS = 0,
    WL_NO_SSID_AVAIL = 1,
    WL_SCAN_COMPLETED = 2,
    WL_CONNECTED = 3,
    WL_CONNECT_FAILED = 4,
    WL_CONNECTION_LOST = 5,
    WL_DISCONNECTED = 6
} wl_status_t;

class IPAddress {
public:
    IPAddress() : _addr(0) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
        : _addr((uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(c) << 8) | d) {}
    IPAddress(uint32_t addr) : _addr(addr) {}

    String toString() const {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                 (int)((_addr >> 24) & 0xFF), (int)((_addr >> 16) & 0xFF),
                 (int)((_addr >> 8) & 0xFF), (int)(_addr & 0xFF));
        return String(buf);
    }

    operator uint32_t() const { return _addr; }

private:
    uint32_t _addr;
};

#define WIFI_STA 1
#define WIFI_AP 2
#define WIFI_AP_STA 3

class WiFiClass {
public:
    wl_status_t begin(const char*, const char*) { return WL_CONNECTED; }
    void disconnect(bool = false) {}
    wl_status_t status() { return _mockStatus; }

    void setMockStatus(wl_status_t s) { _mockStatus = s; }
    void resetMock() { _mockStatus = WL_CONNECTED; }

    IPAddress localIP() { return IPAddress(192, 168, 1, 100); }
    IPAddress gatewayIP() { return IPAddress(192, 168, 1, 1); }
    IPAddress subnetMask() { return IPAddress(255, 255, 255, 0); }
    IPAddress dnsIP(int n = 0) { return IPAddress(8, 8, 8, 8); }
    String macAddress() { return String("AA:BB:CC:DD:EE:FF"); }
    int32_t RSSI() { return -50; }
    String SSID() { return String("MockNetwork"); }
    void setAutoReconnect(bool) {}
    void mode(int) {}
    void config(IPAddress ip, IPAddress gateway, IPAddress subnet,
                IPAddress dns1 = IPAddress(), IPAddress dns2 = IPAddress()) {}
    int hostByName(const char* hostname, IPAddress& result) {
        result = IPAddress(1, 2, 3, 4); return 1;
    }

private:
    wl_status_t _mockStatus = WL_CONNECTED;
};

extern WiFiClass WiFi;

class WiFiClient {
public:
    int connect(const char*, uint16_t) { return 1; }
    int connect(IPAddress, uint16_t) { return 1; }
    void stop() {}
    bool connected() { return true; }
    int available() { return 0; }
    int read() { return -1; }
    size_t write(const uint8_t*, size_t len) { return len; }
    size_t write(uint8_t c) { return 1; }
    void flush() {}
    operator bool() const { return true; }
};

class WiFiClientSecure : public WiFiClient {
public:
    void setInsecure() {}
    void setCACert(const char*) {}
    void setCertificate(const char*) {}
    void setPrivateKey(const char*) {}
};
