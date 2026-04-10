#pragma once

#include <cstdint>
#include "Arduino.h"

class WiFiManager {
public:
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
    bool startConfigPortal(const char* apName) { return true; }
    bool startConfigPortal(const char* apName, const char* apPassword) { return true; }
    void process() {}
    String getConfigPortalSSID() { return String("MockAP"); }
    String getWiFiSSID() { return String("MockNetwork"); }
    bool getConfigPortalActive() { return false; }
};
