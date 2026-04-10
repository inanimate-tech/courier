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
    time_t now() { return 0; }
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
};

extern Timezone UTC;

enum timeStatus_t { timeNotSet, timeNeedsSync, timeSet };

inline void setDebug(int level) {}
inline void setServer(const String& server) {}
inline void setServer(const char* server) {}
inline void setInterval(uint16_t interval = 0) {}
inline timeStatus_t timeStatus() { return timeSet; }
inline void waitForSync(uint16_t timeout = 0) {}
inline bool updateNTP() { return true; }
inline String dateTime(const String& format = "") { return String("2025-01-20 12:00:00"); }
inline time_t now() { return 0; }
inline time_t makeTime(int hr, int min, int sec, int day, int month, int yr) { return 0; }
inline void events() {}
inline void setTime(int hr, int min, int sec, int day, int month, int yr) {}
