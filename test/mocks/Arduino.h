#pragma once

// Mock Arduino.h for native testing

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <string>

using byte = uint8_t;

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

class String {
public:
    String() : _str() {}
    String(const char* s) : _str(s ? s : "") {}
    String(const String& s) : _str(s._str) {}
    String(int value) : _str(std::to_string(value)) {}
    String(unsigned int value) : _str(std::to_string(value)) {}
    String(long value) : _str(std::to_string(value)) {}
    String(unsigned long value) : _str(std::to_string(value)) {}
    String(unsigned long value, int base) {
        if (base == 16) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lx", value);
            _str = buf;
        } else {
            _str = std::to_string(value);
        }
    }
#ifdef __APPLE__
    String(uint64_t value, int base = 10) {
        if (base == 16) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%llx", value);
            _str = buf;
        } else {
            _str = std::to_string(value);
        }
    }
#endif
    String(double value, int decimalPlaces = 2) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimalPlaces, value);
        _str = buf;
    }

    const char* c_str() const { return _str.c_str(); }
    unsigned int length() const { return _str.length(); }
    bool isEmpty() const { return _str.empty(); }

    String& operator=(const char* s) { _str = s ? s : ""; return *this; }
    String& operator=(const String& s) { _str = s._str; return *this; }

    String operator+(const String& s) const { return String((_str + s._str).c_str()); }
    String operator+(const char* s) const { return String((_str + (s ? s : "")).c_str()); }
    String& operator+=(const String& s) { _str += s._str; return *this; }
    String& operator+=(const char* s) { if (s) _str += s; return *this; }
    String& operator+=(char c) { _str += c; return *this; }

    bool operator==(const String& s) const { return _str == s._str; }
    bool operator==(const char* s) const { return _str == (s ? s : ""); }
    bool operator!=(const String& s) const { return _str != s._str; }
    bool operator!=(const char* s) const { return _str != (s ? s : ""); }

    char operator[](unsigned int index) const { return _str[index]; }
    char& operator[](unsigned int index) { return _str[index]; }

    int indexOf(char c) const {
        auto pos = _str.find(c);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }
    int indexOf(const char* s) const {
        auto pos = _str.find(s);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }

    String substring(unsigned int from) const {
        return String(_str.substr(from).c_str());
    }
    String substring(unsigned int from, unsigned int to) const {
        return String(_str.substr(from, to - from).c_str());
    }

    int toInt() const { return std::stoi(_str); }
    float toFloat() const { return std::stof(_str); }
    double toDouble() const { return std::stod(_str); }

    int read() {
        if (_readPos >= _str.length()) return -1;
        return _str[_readPos++];
    }
    int available() const { return _str.length() - _readPos; }
    int peek() const {
        if (_readPos >= _str.length()) return -1;
        return _str[_readPos];
    }

    void trim() {
        auto start = _str.find_first_not_of(" \t\r\n");
        auto end = _str.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) {
            _str.clear();
        } else {
            _str = _str.substr(start, end - start + 1);
        }
    }

    void toLowerCase() {
        for (char& c : _str) c = std::tolower(c);
    }

    void toUpperCase() {
        for (char& c : _str) c = std::toupper(c);
    }

    bool operator<(const String& other) const { return _str < other._str; }

private:
    std::string _str;
    mutable size_t _readPos = 0;
};

inline String operator+(const char* lhs, const String& rhs) {
    return String(lhs) + rhs;
}

inline unsigned long _mock_millis = 0;
inline unsigned long millis() { return _mock_millis; }
inline unsigned long micros() { return _mock_millis * 1000; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}

inline long random(long max) { return 0; }
inline long random(long min, long max) { return min; }
inline void randomSeed(unsigned long seed) {}

class MockSerial {
public:
    void begin(unsigned long) {}
    void end() {}
    void print(const char* s) { capture(s); }
    void print(const String& s) { capture(s.c_str()); }
    void print(int) {}
    void print(unsigned int) {}
    void print(long) {}
    void print(unsigned long) {}
    void print(double, int = 2) {}
    void println() { capture("\n"); }
    void println(const char* s) { capture(s); capture("\n"); }
    void println(const String& s) { capture(s.c_str()); capture("\n"); }
    void println(int) {}
    void println(unsigned int) {}
    void println(long) {}
    void println(unsigned long) {}
    void println(double, int = 2) {}
    void printf(const char* fmt, ...) {
        char buf[256];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        capture(buf);
    }
    int available() { return 0; }
    int read() { return -1; }
    void flush() {}
    operator bool() const { return true; }

    void startCapture() { _capturing = true; _captured.clear(); }
    void stopCapture() { _capturing = false; }
    const std::string& getCaptured() const { return _captured; }
    bool capturedContains(const char* needle) const {
        return _captured.find(needle) != std::string::npos;
    }

private:
    void capture(const char* s) {
        if (_capturing && s) _captured += s;
    }
    bool _capturing = false;
    std::string _captured;
};

extern MockSerial Serial;

#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define LOW 0
#define HIGH 1

inline void pinMode(uint8_t, uint8_t) {}
inline void digitalWrite(uint8_t, uint8_t) {}
inline int digitalRead(uint8_t) { return 0; }
inline int analogRead(uint8_t) { return 0; }
inline void analogWrite(uint8_t, int) {}

template<typename T> inline T min(T a, T b) { return a < b ? a : b; }
template<typename T> inline T max(T a, T b) { return a > b ? a : b; }
template<typename T> inline T constrain(T x, T low, T high) {
    return x < low ? low : (x > high ? high : x);
}
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

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
