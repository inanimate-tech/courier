#pragma once

#include <cstdint>
#include "Arduino.h"
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class WiFiManager {
public:
    // Tests set these to drive the no-saved-network path and see what ran.
    static inline String mockSavedSSID = "MockNetwork";
    static inline int mockPortalStarts = 0;

    bool autoConnect() { return true; }
    bool autoConnect(const char* apName) { return true; }
    bool autoConnect(const char* apName, const char* apPassword) { return true; }
    void resetSettings() {}
    void setConfigPortalBlocking(bool shouldBlock) {}
    void setConfigPortalTimeout(unsigned long seconds) {}
    void setConnectTimeout(unsigned long seconds) {}
    void setSaveConfigCallback(void (*func)(void)) {}
    void setAPCallback(void (*func)(WiFiManager*)) {}
    bool startConfigPortal() { return true; }
    bool startConfigPortal(const char* apName) { mockPortalStarts++; return true; }
    bool startConfigPortal(const char* apName, const char* apPassword) { return true; }
    void process() {}
    String getConfigPortalSSID() { return String("MockAP"); }
    String getWiFiSSID() { return mockSavedSSID; }
    bool getConfigPortalActive() { return false; }

    void setMenu(const char* menu[], uint8_t size) {
        mockMenu.clear();
        for (uint8_t i = 0; i < size; i++) mockMenu.push_back(menu[i]);
    }
    void setShowInfoUpdate(bool enabled) { mockShowInfoUpdate = enabled; }
    void setShowInfoErase(bool enabled) { mockShowInfoErase = enabled; }
    void setWebServerCallback(std::function<void()> fn) { _webservercallback = fn; }

    // What WiFiManager does when it starts the portal's web server: the
    // web-server callback first, then its own routes.
    void startWebServerForTests() {
        if (_webservercallback) _webservercallback();
        for (const char* p : {"/", "/wifi", "/info", "/update", "/u", "/erase", "/restart"})
            server->on(p, [] {});
    }

    // Records routes in order; the first registered for a path answers it,
    // as in the Arduino WebServer.
    struct MockWebServer {
        std::vector<std::pair<std::string, std::function<void()>>> routes;
        void on(const char* path, std::function<void()> fn) { routes.push_back({path, fn}); }
        int request(const char* path) {
            lastCode = 0;
            for (auto& r : routes) if (r.first == path) { r.second(); return lastCode; }
            return -1;
        }
        void sendHeader(const char*, const char*) {}
        void send(int code, const char*, const char*) { lastCode = code; }
        int lastCode = 0;
    };
    std::unique_ptr<MockWebServer> server{new MockWebServer};

    std::vector<std::string> mockMenu;
    bool mockShowInfoUpdate = true;
    bool mockShowInfoErase = true;

protected:
    std::function<void()> _webservercallback;
};
