#pragma once

#include <cstdint>
#include "Arduino.h"

class Timezone {
public:
    Timezone() {}
    bool setCache(int8_t index) { return true; }
    bool setLocation(const String& location = "") { return true; }
    bool setLocation(const char* location) { return true; }
    String dateTime(const String& format = "") { return String("2025-01-20 12:00:00"); }
    time_t now() { return _mockNow; }
    void setMockNow(time_t t) { _mockNow = t; }
    int hour(time_t t = 0) { return 12; }
    int minute(time_t t = 0) { return 0; }
    int second(time_t t = 0) { return 0; }
    int day(time_t t = 0) { return 20; }
    int month(time_t t = 0) { return 1; }
    int year(time_t t = 0) { return 2025; }
    int weekday(time_t t = 0) { return 1; }
    bool isDST(time_t t = 0) { return false; }
    String getTimezoneName() { return String("UTC"); }
    int getOffset() { return 0; }
    void setTime(time_t t) {}
    void setTime(int hr, int min, int sec, int day, int month, int yr) {}

private:
    time_t _mockNow = 0;
};

extern Timezone UTC;

enum timeStatus_t { timeNotSet, timeNeedsSync, timeSet };

inline void setDebug(int level) {}
inline void setServer(const String& server) {}
inline void setServer(const char* server) {}
inline void setInterval(uint16_t interval = 0) {}
inline timeStatus_t g_mockTimeStatus = timeSet;
inline timeStatus_t timeStatus() { return g_mockTimeStatus; }
// Real ezTime's waitForSync returns bool (sync achieved within timeout).
// Default false: NTP "times out", so existing tests exercise the HTTP Date
// fallback path unchanged. g_lastWaitForSyncTimeout records the bound passed
// — waitForSync(0) blocks forever in real ezTime, so tests assert it's set.
inline bool g_mockWaitForSyncResult = false;
inline uint16_t g_lastWaitForSyncTimeout = 0;
inline bool waitForSync(uint16_t timeout = 0) {
    g_lastWaitForSyncTimeout = timeout;
    return g_mockWaitForSyncResult;
}
inline bool updateNTP() { return true; }
inline String dateTime(const String& format = "") { return String("2025-01-20 12:00:00"); }
inline time_t now() { return 0; }
inline time_t makeTime(int hr, int min, int sec, int day, int month, int yr) { return 0; }
inline int g_mockEventsCount = 0;
inline void events() { g_mockEventsCount++; }
inline void setTime(int hr, int min, int sec, int day, int month, int yr) {}
