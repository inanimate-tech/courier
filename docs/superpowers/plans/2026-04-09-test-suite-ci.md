# Test Suite & CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add unit tests, build verification, static analysis, CI pipeline, and branch protection to the Courier library.

**Architecture:** Mocks for all ESP-IDF/Arduino dependencies allow unit tests to run natively. PlatformIO build tests compile examples against the real ESP32 toolchain. An ESP-IDF build test verifies the component registration. A Python test runner orchestrates everything locally and in CI.

**Tech Stack:** PlatformIO (native + espressif32), ESP-IDF v5.3, Unity test framework, cppcheck, GitHub Actions, Python/Click/uv

---

## File Structure

```
test/
  mocks/
    Arduino.h          — String, millis, Serial, pin stubs
    Arduino.cpp        — MockSerial global
    WiFi.h             — WiFiClass, IPAddress, WiFiClient, WiFiClientSecure
    WiFi.cpp           — WiFiClass global
    WiFiManager.h      — WiFiManager mock (autoConnect returns true)
    WiFiClientSecure.h — redirect include to WiFi.h
    HTTPClient.h       — HTTP mock with static defaults
    esp_websocket_client.h — MockWebSocketClient + IDF API stubs
    mqtt_client.h      — MockMqttClient + IDF API stubs
    ezTime.h           — Timezone, NTP stubs
    ezTime.cpp         — Timezone UTC global
  unit/
    platformio.ini
    test/
      test_courier/test_courier.cpp
      test_ws_transport/test_ws_transport.cpp
      test_mqtt_transport/test_mqtt_transport.cpp
  build-platformio/
    basic-websocket/platformio.ini
    mqtt-pubsub/platformio.ini
  build-espidf/
    sample-project/
      CMakeLists.txt
      sdkconfig.defaults
examples/
  espidf-basic/
    main/
      main.cpp
      CMakeLists.txt
tools/
  run-tests.py
.github/
  workflows/
    ci.yml
```

---

### Task 1: Mock — Arduino.h and Arduino.cpp

**Files:**
- Create: `test/mocks/Arduino.h`
- Create: `test/mocks/Arduino.cpp`

- [ ] **Step 1: Create Arduino.h mock**

```cpp
#pragma once

// Mock Arduino.h for native testing

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <string>

// Arduino types
using byte = uint8_t;

// Number bases for String constructor
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

// Arduino String class (minimal implementation)
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

// Time functions — settable for testing
inline unsigned long _mock_millis = 0;
inline unsigned long millis() { return _mock_millis; }
inline unsigned long micros() { return _mock_millis * 1000; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}

// Random functions
inline long random(long max) { return 0; }
inline long random(long min, long max) { return min; }
inline void randomSeed(unsigned long seed) {}

// Mock Serial
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

// Pin modes and values
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

// Math helpers
template<typename T> inline T min(T a, T b) { return a < b ? a : b; }
template<typename T> inline T max(T a, T b) { return a > b ? a : b; }
template<typename T> inline T constrain(T x, T low, T high) {
    return x < low ? low : (x > high ? high : x);
}
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
```

- [ ] **Step 2: Create Arduino.cpp**

```cpp
#include "Arduino.h"

MockSerial Serial;
```

- [ ] **Step 3: Commit**

```bash
git add test/mocks/Arduino.h test/mocks/Arduino.cpp
git commit -m "test: add Arduino mock for native testing"
```

---

### Task 2: Mock — WiFi, WiFiManager, WiFiClientSecure, HTTPClient

**Files:**
- Create: `test/mocks/WiFi.h`
- Create: `test/mocks/WiFi.cpp`
- Create: `test/mocks/WiFiManager.h`
- Create: `test/mocks/WiFiClientSecure.h`
- Create: `test/mocks/HTTPClient.h`

- [ ] **Step 1: Create WiFi.h mock**

```cpp
#pragma once

// Mock WiFi.h for native testing

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
```

- [ ] **Step 2: Create WiFi.cpp**

```cpp
#include "WiFi.h"

WiFiClass WiFi;
```

- [ ] **Step 3: Create WiFiManager.h mock**

```cpp
#pragma once
// Mock WiFiManager for native testing

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
```

- [ ] **Step 4: Create WiFiClientSecure.h redirect**

```cpp
#pragma once
// Redirect to WiFi.h which defines WiFiClientSecure
#include "WiFi.h"
```

- [ ] **Step 5: Create HTTPClient.h mock**

```cpp
#pragma once

// Mock HTTPClient.h for native testing

#include "Arduino.h"
#include "WiFi.h"

#define HTTP_CODE_OK 200
#define HTTP_CODE_MOVED_PERMANENTLY 301
#define HTTP_CODE_FOUND 302
#define HTTP_CODE_NOT_FOUND 404
#define HTTP_CODE_INTERNAL_SERVER_ERROR 500

class HTTPClient {
public:
    HTTPClient()
        : _responseCode(s_defaultResponseCode),
          _responseBody(s_defaultResponseBody),
          _responseHeader(s_defaultResponseHeader) {}

    void begin(WiFiClient&, const String&) {}
    void begin(WiFiClientSecure&, const String&) {}
    void begin(const String&) {}
    void end() {}

    void addHeader(const String&, const String&) {}
    void collectHeaders(const char* headerKeys[], size_t count) {
        (void)headerKeys; (void)count;
    }

    int GET() { return _responseCode; }
    int POST(const String& body) { s_lastPostBody = body; return _responseCode; }
    int PUT(const String&) { return _responseCode; }
    int sendRequest(const char*, const uint8_t* = nullptr, size_t = 0) {
        return _responseCode;
    }

    int getSize() { return static_cast<int>(_responseBody.length()); }
    String getString() { return _responseBody; }
    String header(const char*) { return _responseHeader; }
    bool hasHeader(const char*) { return !_responseHeader.isEmpty(); }

    WiFiClient* getStreamPtr() { return nullptr; }

    void setMockResponse(int code, const String& body) {
        _responseCode = code; _responseBody = body;
    }
    void setMockHeader(const String& value) { _responseHeader = value; }

    static void setDefaultMockResponse(int code, const String& body) {
        s_defaultResponseCode = code; s_defaultResponseBody = body;
    }
    static void setDefaultMockHeader(const String& value) {
        s_defaultResponseHeader = value;
    }
    static String getLastPostBody() { return s_lastPostBody; }

    static void resetMockDefaults() {
        s_defaultResponseCode = 200;
        s_defaultResponseBody = "";
        s_defaultResponseHeader = "";
        s_lastPostBody = "";
    }

private:
    int _responseCode;
    String _responseBody;
    String _responseHeader;

    inline static int s_defaultResponseCode = 200;
    inline static String s_defaultResponseBody = "";
    inline static String s_defaultResponseHeader = "";
    inline static String s_lastPostBody = "";
};
```

- [ ] **Step 6: Commit**

```bash
git add test/mocks/WiFi.h test/mocks/WiFi.cpp test/mocks/WiFiManager.h \
        test/mocks/WiFiClientSecure.h test/mocks/HTTPClient.h
git commit -m "test: add WiFi, WiFiManager, and HTTPClient mocks"
```

---

### Task 3: Mock — esp_websocket_client.h, mqtt_client.h, ezTime

**Files:**
- Create: `test/mocks/esp_websocket_client.h`
- Create: `test/mocks/mqtt_client.h`
- Create: `test/mocks/ezTime.h`
- Create: `test/mocks/ezTime.cpp`

- [ ] **Step 1: Create esp_websocket_client.h mock**

```cpp
#pragma once
// Mock esp_websocket_client for native testing

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>

// ESP-IDF event system types (minimal mock)
typedef const char* esp_event_base_t;
#define WEBSOCKET_EVENTS "WEBSOCKET_EVENTS"

typedef enum {
    WEBSOCKET_EVENT_ANY = -1,
    WEBSOCKET_EVENT_ERROR = 0,
    WEBSOCKET_EVENT_CONNECTED,
    WEBSOCKET_EVENT_DISCONNECTED,
    WEBSOCKET_EVENT_DATA,
    WEBSOCKET_EVENT_CLOSED,
    WEBSOCKET_EVENT_BEFORE_CONNECT,
    WEBSOCKET_EVENT_MAX
} esp_websocket_event_id_t;

typedef struct {
    const char* data_ptr;
    int data_len;
    int payload_len;
    int payload_offset;
    uint8_t op_code;  // 0x01 = text, 0x02 = binary
} esp_websocket_event_data_t;

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
#ifndef ESP_FAIL
#define ESP_FAIL -1
#endif

#ifndef portMAX_DELAY
#define portMAX_DELAY 0xFFFFFFFF
#endif

class MockWebSocketClient;
typedef MockWebSocketClient* esp_websocket_client_handle_t;

typedef void (*esp_event_handler_t)(void* event_handler_arg,
                                     esp_event_base_t event_base,
                                     int32_t event_id,
                                     void* event_data);

typedef struct {
    const char* uri;
    const char* cert_pem;
    bool disable_auto_reconnect;
    int pingpong_timeout_sec;
    void* user_context;
    int task_stack;
    int buffer_size;
} esp_websocket_client_config_t;

class MockWebSocketClient {
public:
    MockWebSocketClient() { s_lastInstance = this; s_instanceCount++; }
    ~MockWebSocketClient() = default;

    std::string uri;
    std::string cert_pem;
    bool disable_auto_reconnect = false;
    int pingpong_timeout_sec = 0;

    bool connected = false;
    bool started = false;
    bool stopped = false;
    bool destroyed = false;

    esp_event_handler_t eventHandler = nullptr;
    void* eventHandlerArg = nullptr;

    static constexpr int MAX_SENT = 16;
    std::string sentMessages[MAX_SENT];
    int sendCount = 0;
    int binarySendCount = 0;

    void simulateConnect() {
        connected = true;
        if (eventHandler) {
            eventHandler(eventHandlerArg, WEBSOCKET_EVENTS,
                        WEBSOCKET_EVENT_CONNECTED, nullptr);
        }
    }

    void simulateDisconnect() {
        connected = false;
        if (eventHandler) {
            eventHandler(eventHandlerArg, WEBSOCKET_EVENTS,
                        WEBSOCKET_EVENT_DISCONNECTED, nullptr);
        }
    }

    void simulateTextMessage(const char* message) {
        int len = strlen(message);
        esp_websocket_event_data_t data;
        data.data_ptr = message;
        data.data_len = len;
        data.payload_len = len;
        data.payload_offset = 0;
        data.op_code = 0x01;
        if (eventHandler) {
            eventHandler(eventHandlerArg, WEBSOCKET_EVENTS,
                        WEBSOCKET_EVENT_DATA, &data);
        }
    }

    void simulateBinaryMessage(const uint8_t* payload, size_t len) {
        esp_websocket_event_data_t data;
        data.data_ptr = (const char*)payload;
        data.data_len = (int)len;
        data.payload_len = (int)len;
        data.payload_offset = 0;
        data.op_code = 0x02;
        if (eventHandler) {
            eventHandler(eventHandlerArg, WEBSOCKET_EVENTS,
                        WEBSOCKET_EVENT_DATA, &data);
        }
    }

    static MockWebSocketClient* lastInstance() { return s_lastInstance; }
    static int instanceCount() { return s_instanceCount; }
    static void resetInstanceCount() { s_instanceCount = 0; }

private:
    inline static MockWebSocketClient* s_lastInstance = nullptr;
    inline static int s_instanceCount = 0;
};

// IDF API mock implementations

inline esp_websocket_client_handle_t esp_websocket_client_init(
    const esp_websocket_client_config_t* config)
{
    auto* client = new MockWebSocketClient();
    if (config->uri) client->uri = config->uri;
    if (config->cert_pem) client->cert_pem = config->cert_pem;
    client->disable_auto_reconnect = config->disable_auto_reconnect;
    client->pingpong_timeout_sec = config->pingpong_timeout_sec;
    return client;
}

inline esp_err_t esp_websocket_register_events(
    esp_websocket_client_handle_t client,
    esp_websocket_event_id_t event,
    esp_event_handler_t handler,
    void* handler_arg)
{
    (void)event;
    client->eventHandler = handler;
    client->eventHandlerArg = handler_arg;
    return ESP_OK;
}

inline esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client) {
    client->started = true;
    client->stopped = false;
    return ESP_OK;
}

inline esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client) {
    client->stopped = true;
    client->started = false;
    client->connected = false;
    return ESP_OK;
}

inline esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client) {
    client->destroyed = true;
    delete client;
    return ESP_OK;
}

inline int esp_websocket_client_send_text(
    esp_websocket_client_handle_t client,
    const char* data, int len, uint32_t timeout)
{
    (void)timeout;
    if (!client->connected) return -1;
    if (client->sendCount < MockWebSocketClient::MAX_SENT) {
        client->sentMessages[client->sendCount] = std::string(data, len);
    }
    client->sendCount++;
    return len;
}

inline int esp_websocket_client_send_bin(
    esp_websocket_client_handle_t client,
    const char* data, int len, uint32_t timeout)
{
    (void)data; (void)timeout;
    if (!client->connected) return -1;
    client->binarySendCount++;
    return len;
}

inline bool esp_websocket_client_is_connected(esp_websocket_client_handle_t client) {
    return client && client->connected;
}
```

- [ ] **Step 2: Create mqtt_client.h mock**

```cpp
#pragma once
// Mock esp_mqtt_client (mqtt_client.h) for native testing

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>

// ESP-IDF types (guard against double definition from esp_websocket_client.h)
#ifndef ESP_OK
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#endif

#ifndef ESP_EVENT_BASE_T_DEFINED
#define ESP_EVENT_BASE_T_DEFINED
#ifndef WEBSOCKET_EVENTS
typedef const char* esp_event_base_t;
#endif
#endif

typedef void (*esp_event_handler_t_mqtt)(void* event_handler_arg,
                                         esp_event_base_t event_base,
                                         int32_t event_id,
                                         void* event_data);

typedef enum {
    MQTT_EVENT_ANY = -1,
    MQTT_EVENT_ERROR = 0,
    MQTT_EVENT_CONNECTED,
    MQTT_EVENT_DISCONNECTED,
    MQTT_EVENT_SUBSCRIBED,
    MQTT_EVENT_UNSUBSCRIBED,
    MQTT_EVENT_PUBLISHED,
    MQTT_EVENT_DATA,
    MQTT_EVENT_BEFORE_CONNECT,
    MQTT_EVENT_DELETED
} esp_mqtt_event_id_t;

struct esp_mqtt_error_codes_mock_t {
    int error_type;
    int connect_return_code;
    int esp_tls_last_esp_err;
};

struct esp_mqtt_event_t {
    const char* topic;
    int topic_len;
    const char* data;
    int data_len;
    int total_data_len;
    int current_data_offset;
    int msg_id;
    esp_mqtt_error_codes_mock_t* error_handle;
};

typedef esp_mqtt_event_t* esp_mqtt_event_handle_t;

class MockMqttClient;
typedef MockMqttClient* esp_mqtt_client_handle_t;

typedef struct {
    const char* uri;
    const char* cert_pem;
    const char* client_id;
    bool disable_auto_reconnect;
    void* user_context;
    int task_stack;
    int buffer_size;
} esp_mqtt_client_config_t;

class MockMqttClient {
public:
    MockMqttClient() { s_lastInstance = this; s_instanceCount++; }
    ~MockMqttClient() = default;

    std::string uri;
    std::string cert_pem;
    std::string clientId;
    bool disable_auto_reconnect = false;

    bool connected = false;
    bool started = false;
    bool stopped = false;
    bool destroyed = false;

    esp_event_handler_t_mqtt eventHandler = nullptr;
    void* eventHandlerArg = nullptr;

    static constexpr int MAX_SUBSCRIPTIONS = 16;
    std::string subscribedTopics[MAX_SUBSCRIPTIONS];
    int subscriptionCount = 0;

    std::string unsubscribedTopics[MAX_SUBSCRIPTIONS];
    int unsubscribeCount = 0;

    std::string lastPublishTopic;
    std::string lastPublishPayload;
    int publishCount = 0;

    void simulateConnect() {
        connected = true;
        subscriptionCount = 0;
        unsubscribeCount = 0;
        if (eventHandler) {
            esp_mqtt_event_t event = {};
            eventHandler(eventHandlerArg, "MQTT_EVENTS",
                        MQTT_EVENT_CONNECTED, &event);
        }
    }

    void simulateDisconnect() {
        connected = false;
        if (eventHandler) {
            esp_mqtt_event_t event = {};
            eventHandler(eventHandlerArg, "MQTT_EVENTS",
                        MQTT_EVENT_DISCONNECTED, &event);
        }
    }

    void simulateMessage(const char* topic, const char* payload) {
        if (eventHandler) {
            int len = strlen(payload);
            esp_mqtt_event_t event = {};
            event.topic = topic;
            event.topic_len = strlen(topic);
            event.data = payload;
            event.data_len = len;
            event.total_data_len = len;
            event.current_data_offset = 0;
            eventHandler(eventHandlerArg, "MQTT_EVENTS",
                        MQTT_EVENT_DATA, &event);
        }
    }

    void simulateError() {
        if (eventHandler) {
            esp_mqtt_error_codes_mock_t err = {0, 0, 0};
            esp_mqtt_event_t event = {};
            event.error_handle = &err;
            eventHandler(eventHandlerArg, "MQTT_EVENTS",
                        MQTT_EVENT_ERROR, &event);
        }
    }

    static MockMqttClient* lastInstance() { return s_lastInstance; }
    static int instanceCount() { return s_instanceCount; }
    static void resetInstanceCount() { s_instanceCount = 0; }

private:
    inline static MockMqttClient* s_lastInstance = nullptr;
    inline static int s_instanceCount = 0;
};

// IDF API mock implementations

inline esp_mqtt_client_handle_t esp_mqtt_client_init(
    const esp_mqtt_client_config_t* config)
{
    auto* client = new MockMqttClient();
    if (config->uri) client->uri = config->uri;
    if (config->cert_pem) client->cert_pem = config->cert_pem;
    if (config->client_id) client->clientId = config->client_id;
    client->disable_auto_reconnect = config->disable_auto_reconnect;
    return client;
}

inline esp_err_t esp_mqtt_client_register_event(
    esp_mqtt_client_handle_t client,
    esp_mqtt_event_id_t event,
    esp_event_handler_t_mqtt handler,
    void* handler_arg)
{
    (void)event;
    client->eventHandler = handler;
    client->eventHandlerArg = handler_arg;
    return ESP_OK;
}

inline esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t client) {
    client->started = true;
    client->stopped = false;
    return ESP_OK;
}

inline esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t client) {
    client->stopped = true;
    client->started = false;
    client->connected = false;
    return ESP_OK;
}

inline esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t client) {
    client->destroyed = true;
    delete client;
    return ESP_OK;
}

inline int esp_mqtt_client_subscribe(
    esp_mqtt_client_handle_t client,
    const char* topic, int qos)
{
    (void)qos;
    if (client->subscriptionCount < MockMqttClient::MAX_SUBSCRIPTIONS) {
        client->subscribedTopics[client->subscriptionCount++] = topic;
    }
    return 0;
}

inline int esp_mqtt_client_publish(
    esp_mqtt_client_handle_t client,
    const char* topic, const char* data, int len,
    int qos, int retain)
{
    (void)qos; (void)retain;
    if (!client->connected) return -1;
    client->lastPublishTopic = topic;
    if (len == 0 && data) {
        client->lastPublishPayload = data;
    } else if (data) {
        client->lastPublishPayload = std::string(data, len);
    }
    client->publishCount++;
    return 0;
}

inline int esp_mqtt_client_unsubscribe(
    esp_mqtt_client_handle_t client,
    const char* topic)
{
    if (client->unsubscribeCount < MockMqttClient::MAX_SUBSCRIPTIONS) {
        client->unsubscribedTopics[client->unsubscribeCount++] = topic;
    }
    return 0;
}

inline esp_err_t esp_mqtt_client_disconnect(esp_mqtt_client_handle_t client) {
    client->connected = false;
    return ESP_OK;
}
```

- [ ] **Step 3: Create ezTime.h mock**

```cpp
#pragma once
// Mock ezTime for native testing

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
```

- [ ] **Step 4: Create ezTime.cpp**

```cpp
#include "ezTime.h"

Timezone UTC;
```

- [ ] **Step 5: Commit**

```bash
git add test/mocks/esp_websocket_client.h test/mocks/mqtt_client.h \
        test/mocks/ezTime.h test/mocks/ezTime.cpp
git commit -m "test: add ESP-IDF WebSocket, MQTT, and ezTime mocks"
```

---

### Task 4: Unit test — platformio.ini and test_ws_transport

**Files:**
- Create: `test/unit/platformio.ini`
- Create: `test/unit/test/test_ws_transport/test_ws_transport.cpp`

- [ ] **Step 1: Create platformio.ini**

```ini
[env:native]
platform = native
build_flags =
    -std=c++17
    -I ../mocks
    -I ../../src
    -D UNITY_INCLUDE_DOUBLE
test_framework = unity
lib_compat_mode = off
lib_deps =
    throwtheswitch/Unity
    bblanchon/ArduinoJson
build_src_filter = +<../../src/Courier*.cpp> +<../mocks/*.cpp>
```

Note: `build_src_filter` includes the library source files and mock .cpp files so they get compiled into the test binary alongside the test files.

- [ ] **Step 2: Create test_ws_transport.cpp**

```cpp
#include <unity.h>
#include <CourierWSTransport.h>
#include <esp_websocket_client.h>
#include <cstring>

// Track messages delivered through the transport's callback
static int deliveredMessageCount = 0;
static char lastDeliveredPayload[512] = "";

static void onMessageCallback(const char* payload, size_t length) {
    deliveredMessageCount++;
    size_t copyLen = length < sizeof(lastDeliveredPayload) - 1 ? length : sizeof(lastDeliveredPayload) - 1;
    memcpy(lastDeliveredPayload, payload, copyLen);
    lastDeliveredPayload[copyLen] = '\0';
}

// Track connection events
static int connectionEventCount = 0;
static bool lastConnectionState = false;

static void onConnectionCallback(CourierTransport* transport, bool connected) {
    connectionEventCount++;
    lastConnectionState = connected;
}

static CourierWSTransport* ws = nullptr;

void setUp(void) {
    MockWebSocketClient::resetInstanceCount();
    ws = new CourierWSTransport();
    ws->setMessageCallback(onMessageCallback);
    ws->setConnectionCallback(onConnectionCallback);
    deliveredMessageCount = 0;
    lastDeliveredPayload[0] = '\0';
    connectionEventCount = 0;
    lastConnectionState = false;
}

void tearDown(void) {
    delete ws;
    ws = nullptr;
}

void test_name_is_ws() {
    TEST_ASSERT_EQUAL_STRING("WebSocket", ws->name());
}

void test_begin_creates_client_with_wss_uri() {
    ws->begin("example.com", 443, "/ws/abc123");

    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL_STRING("wss://example.com:443/ws/abc123", client->uri.c_str());
}

void test_begin_sets_config_no_cert_by_default() {
    ws->begin("host", 443, "/path");

    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_TRUE(client->cert_pem.empty());
    TEST_ASSERT_TRUE(client->disable_auto_reconnect);
    TEST_ASSERT_EQUAL(20, client->pingpong_timeout_sec);
}

void test_begin_starts_client() {
    ws->begin("host", 443, "/path");

    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_TRUE(client->started);
}

void test_connected_after_connect_event() {
    ws->begin("host", 443, "/path");
    TEST_ASSERT_FALSE(ws->isConnected());

    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();

    TEST_ASSERT_TRUE(ws->isConnected());
}

void test_disconnected_after_disconnect_event() {
    ws->begin("host", 443, "/path");

    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();
    TEST_ASSERT_TRUE(ws->isConnected());

    client->simulateDisconnect();
    ws->loop();
    TEST_ASSERT_FALSE(ws->isConnected());
}

void test_message_delivered_to_callback() {
    ws->begin("host", 443, "/path");

    auto* client = MockWebSocketClient::lastInstance();
    client->simulateTextMessage("{\"type\":\"test\",\"value\":42}");
    ws->loop();

    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"test\",\"value\":42}", lastDeliveredPayload);
}

void test_connection_callback_on_connect() {
    ws->begin("host", 443, "/path");

    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();

    TEST_ASSERT_EQUAL(1, connectionEventCount);
    TEST_ASSERT_TRUE(lastConnectionState);
}

void test_connection_callback_on_disconnect() {
    ws->begin("host", 443, "/path");

    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();
    client->simulateDisconnect();
    ws->loop();

    TEST_ASSERT_EQUAL(2, connectionEventCount);
    TEST_ASSERT_FALSE(lastConnectionState);
}

void test_send_when_connected() {
    ws->begin("host", 443, "/path");

    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();

    bool result = ws->send("{\"type\":\"test\"}");
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL(1, client->sendCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"test\"}", client->sentMessages[0].c_str());
}

void test_send_fails_when_disconnected() {
    ws->begin("host", 443, "/path");

    bool result = ws->send("{\"type\":\"test\"}");
    TEST_ASSERT_FALSE(result);
}

void test_disconnect_sets_not_connected() {
    ws->begin("host", 443, "/path");

    auto* client = MockWebSocketClient::lastInstance();
    client->simulateConnect();
    ws->loop();
    TEST_ASSERT_TRUE(ws->isConnected());

    ws->disconnect();
    TEST_ASSERT_FALSE(ws->isConnected());
}

void test_reconnect_creates_new_client() {
    ws->begin("host", 443, "/path1");
    TEST_ASSERT_EQUAL(1, MockWebSocketClient::instanceCount());

    ws->begin("host", 443, "/path2");
    TEST_ASSERT_EQUAL(2, MockWebSocketClient::instanceCount());

    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("wss://host:443/path2", client->uri.c_str());
}

void test_config_cert_pem_passed_to_client() {
    delete ws;
    static const char* MY_CERT = "-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n";
    CourierWSTransportConfig cfg;
    cfg.cert_pem = MY_CERT;
    ws = new CourierWSTransport(cfg);
    ws->setMessageCallback(onMessageCallback);
    ws->setConnectionCallback(onConnectionCallback);

    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING(MY_CERT, client->cert_pem.c_str());
}

void test_config_no_cert_by_default_nullptr() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_TRUE(client->cert_pem.empty());
}

void test_on_configure_called_before_init() {
    bool called = false;
    ws->onConfigure([&](esp_websocket_client_config_t& config) {
        called = true;
        config.cert_pem = "HOOK_CERT";
    });

    ws->begin("host", 443, "/path");
    TEST_ASSERT_TRUE(called);

    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("HOOK_CERT", client->cert_pem.c_str());
}

void test_on_configure_can_override_config_cert() {
    delete ws;
    static const char* ORIGINAL_CERT = "ORIGINAL";
    static const char* OVERRIDE_CERT = "OVERRIDE";
    CourierWSTransportConfig cfg;
    cfg.cert_pem = ORIGINAL_CERT;
    ws = new CourierWSTransport(cfg);
    ws->setMessageCallback(onMessageCallback);

    ws->onConfigure([](esp_websocket_client_config_t& config) {
        config.cert_pem = OVERRIDE_CERT;
    });

    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("OVERRIDE", client->cert_pem.c_str());
}

void test_on_configure_not_set_works() {
    ws->begin("host", 443, "/path");
    auto* client = MockWebSocketClient::lastInstance();
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_TRUE(client->started);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_name_is_ws);
    RUN_TEST(test_begin_creates_client_with_wss_uri);
    RUN_TEST(test_begin_sets_config_no_cert_by_default);
    RUN_TEST(test_begin_starts_client);
    RUN_TEST(test_connected_after_connect_event);
    RUN_TEST(test_disconnected_after_disconnect_event);
    RUN_TEST(test_message_delivered_to_callback);
    RUN_TEST(test_connection_callback_on_connect);
    RUN_TEST(test_connection_callback_on_disconnect);
    RUN_TEST(test_send_when_connected);
    RUN_TEST(test_send_fails_when_disconnected);
    RUN_TEST(test_disconnect_sets_not_connected);
    RUN_TEST(test_reconnect_creates_new_client);
    RUN_TEST(test_config_cert_pem_passed_to_client);
    RUN_TEST(test_config_no_cert_by_default_nullptr);
    RUN_TEST(test_on_configure_called_before_init);
    RUN_TEST(test_on_configure_can_override_config_cert);
    RUN_TEST(test_on_configure_not_set_works);

    return UNITY_END();
}
```

- [ ] **Step 3: Run the test to verify it compiles and passes**

```bash
cd test/unit && pio test -e native -v
```

Expected: All 17 WS transport tests pass.

- [ ] **Step 4: Commit**

```bash
git add test/unit/platformio.ini test/unit/test/test_ws_transport/test_ws_transport.cpp
git commit -m "test: add WebSocket transport unit tests (17 tests)"
```

---

### Task 5: Unit test — test_mqtt_transport

**Files:**
- Create: `test/unit/test/test_mqtt_transport/test_mqtt_transport.cpp`

- [ ] **Step 1: Create test_mqtt_transport.cpp**

Use the full test file from the reference (49 tests). Key difference from reference: replace all topic strings like `"devices/dev123/command"` with generic topic names — the test helper `createWithTopics()` should use generic names:

```cpp
#include <unity.h>
#include <CourierMqttTransport.h>
#include <mqtt_client.h>
#include <cstring>
#include <string>

static int deliveredMessageCount = 0;
static constexpr size_t DELIVERED_BUF_SIZE = 12288;
static char lastDeliveredPayload[DELIVERED_BUF_SIZE] = "";
static size_t lastDeliveredLength = 0;

static void onMessageCallback(const char* payload, size_t length) {
    deliveredMessageCount++;
    lastDeliveredLength = length;
    size_t copyLen = length < DELIVERED_BUF_SIZE - 1 ? length : DELIVERED_BUF_SIZE - 1;
    memcpy(lastDeliveredPayload, payload, copyLen);
    lastDeliveredPayload[copyLen] = '\0';
}

static int connectionEventCount = 0;
static bool lastConnectionState = false;

static void onConnectionCallback(CourierTransport* transport, bool connected) {
    connectionEventCount++;
    lastConnectionState = connected;
}

static CourierMqttTransport* mqtt = nullptr;

void setUp(void) {
    MockMqttClient::resetInstanceCount();
    deliveredMessageCount = 0;
    lastDeliveredPayload[0] = '\0';
    lastDeliveredLength = 0;
    connectionEventCount = 0;
    lastConnectionState = false;
}

void tearDown(void) {
    delete mqtt;
    mqtt = nullptr;
}

// Helper: create a transport with topics configured
static CourierMqttTransport* createWithTopics(const char* deviceId = "dev123",
                                                const char* deviceType = "sensor")
{
    std::string commandTopic = std::string("devices/") + deviceId + "/command";
    std::string statusTopic  = std::string("devices/") + deviceId + "/status";
    std::string eventTopic   = std::string("devices/") + deviceId + "/event";
    std::string allEvents    = "devices/+/event";

    CourierMqttTransportConfig cfg;
    cfg.topics = {commandTopic, statusTopic, allEvents};
    cfg.defaultPublishTopic = eventTopic.c_str();
    std::string clientId = std::string(deviceType) + "-" + deviceId;
    cfg.clientId = clientId.c_str();

    auto* t = new CourierMqttTransport(cfg);
    t->setMessageCallback(onMessageCallback);
    t->setConnectionCallback(onConnectionCallback);
    return t;
}

void test_name_is_mqtt() {
    mqtt = new CourierMqttTransport();
    TEST_ASSERT_EQUAL_STRING("MQTT", mqtt->name());
}

void test_begin_constructs_wss_uri() {
    mqtt = createWithTopics();
    mqtt->begin("example.com", 443, "/agents/broker/room456");

    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL_STRING("wss://example.com:443/agents/broker/room456", client->uri.c_str());
}

void test_begin_sets_client_id() {
    mqtt = createWithTopics("dev123", "sensor");
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("sensor-dev123", client->clientId.c_str());
}

void test_begin_without_client_id_uses_empty() {
    mqtt = new CourierMqttTransport();
    mqtt->setMessageCallback(onMessageCallback);
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->clientId.empty());
}

void test_begin_starts_client() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->started);
}

void test_begin_no_cert_by_default_and_disables_auto_reconnect() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->cert_pem.empty());
    TEST_ASSERT_TRUE(client->disable_auto_reconnect);
}

void test_subscribes_to_configured_topics() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();

    TEST_ASSERT_EQUAL(3, client->subscriptionCount);
    TEST_ASSERT_EQUAL_STRING("devices/dev123/command", client->subscribedTopics[0].c_str());
    TEST_ASSERT_EQUAL_STRING("devices/dev123/status", client->subscribedTopics[1].c_str());
    TEST_ASSERT_EQUAL_STRING("devices/+/event", client->subscribedTopics[2].c_str());
}

void test_connected_after_connect_event() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    TEST_ASSERT_FALSE(mqtt->isConnected());

    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    TEST_ASSERT_TRUE(mqtt->isConnected());
    TEST_ASSERT_EQUAL(1, connectionEventCount);
    TEST_ASSERT_TRUE(lastConnectionState);
}

void test_disconnected_after_disconnect_event() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    client->simulateDisconnect();
    mqtt->loop();

    TEST_ASSERT_FALSE(mqtt->isConnected());
    TEST_ASSERT_EQUAL(2, connectionEventCount);
    TEST_ASSERT_FALSE(lastConnectionState);
}

void test_command_message_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/command", "{\"type\":\"test\",\"value\":42}");
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"test\",\"value\":42}", lastDeliveredPayload);
}

void test_status_message_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/status", "{\"type\":\"ota\",\"action\":\"check\"}");
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
}

void test_own_event_topic_message_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/event", "{\"type\":\"app_event\",\"name\":\"test\"}");
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
}

void test_other_device_event_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/other789/event", "{\"type\":\"app_event\",\"name\":\"test\"}");
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
}

void test_send_publishes_to_default_publish_topic() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    bool result = mqtt->send("{\"type\":\"app_event\"}");

    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("devices/dev123/event", client->lastPublishTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"app_event\"}", client->lastPublishPayload.c_str());
}

void test_send_fails_when_disconnected() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    bool result = mqtt->send("{\"type\":\"test\"}");
    TEST_ASSERT_FALSE(result);
}

void test_send_fails_without_publish_topic() {
    mqtt = new CourierMqttTransport();
    mqtt->setMessageCallback(onMessageCallback);
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    bool result = mqtt->send("{\"type\":\"test\"}");
    TEST_ASSERT_FALSE(result);
}

void test_disconnect_sets_not_connected() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    mqtt->disconnect();
    TEST_ASSERT_FALSE(mqtt->isConnected());
}

void test_reconnect_after_disconnect() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    TEST_ASSERT_TRUE(mqtt->isConnected());

    client->simulateDisconnect();
    mqtt->loop();
    TEST_ASSERT_FALSE(mqtt->isConnected());

    mqtt->begin("host", 443, "/path");
    client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->started);

    client->simulateConnect();
    mqtt->loop();
    TEST_ASSERT_TRUE(mqtt->isConnected());
    TEST_ASSERT_EQUAL(3, client->subscriptionCount);
    TEST_ASSERT_EQUAL_STRING("devices/dev123/command", client->subscribedTopics[0].c_str());
    TEST_ASSERT_EQUAL_STRING("devices/dev123/status", client->subscribedTopics[1].c_str());
    TEST_ASSERT_EQUAL_STRING("devices/+/event", client->subscribedTopics[2].c_str());
}

void test_reconnect_with_new_path() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/agents/broker/room456");

    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    mqtt->begin("host", 443, "/agents/broker/room789");

    client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("wss://host:443/agents/broker/room789", client->uri.c_str());

    client->simulateConnect();
    TEST_ASSERT_EQUAL(3, client->subscriptionCount);
}

void test_send_uses_default_publish_topic() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    mqtt->disconnect();
    mqtt->begin("host", 443, "/path");
    client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    mqtt->send("{\"type\":\"test\"}");
    TEST_ASSERT_EQUAL_STRING("devices/dev123/event", client->lastPublishTopic.c_str());
}

void test_multiple_connect_disconnect_cycles() {
    mqtt = createWithTopics();

    for (int cycle = 0; cycle < 3; cycle++) {
        mqtt->begin("host", 443, "/path");
        auto* client = MockMqttClient::lastInstance();

        client->simulateConnect();
        mqtt->loop();
        TEST_ASSERT_TRUE(mqtt->isConnected());
        TEST_ASSERT_EQUAL(3, client->subscriptionCount);

        client->simulateDisconnect();
        mqtt->loop();
        TEST_ASSERT_FALSE(mqtt->isConnected());
        mqtt->disconnect();
    }

    TEST_ASSERT_EQUAL(6, connectionEventCount);
    TEST_ASSERT_EQUAL(3, MockMqttClient::instanceCount());
}

void test_reconnect_creates_fresh_client() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    TEST_ASSERT_EQUAL(1, MockMqttClient::instanceCount());

    mqtt->begin("host", 443, "/path");
    TEST_ASSERT_EQUAL(2, MockMqttClient::instanceCount());

    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(0, client->subscriptionCount);
    TEST_ASSERT_TRUE(client->started);
}

void test_reconnect_exact_subscription_count() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/agents/broker/room456");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    TEST_ASSERT_EQUAL(3, client->subscriptionCount);

    mqtt->disconnect();
    mqtt->begin("host", 443, "/agents/broker/room789");
    client = MockMqttClient::lastInstance();
    client->simulateConnect();

    TEST_ASSERT_EQUAL(3, client->subscriptionCount);
}

void test_reconnect_message_delivered_exactly_once() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    mqtt->disconnect();
    mqtt->begin("host", 443, "/path");
    client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    client->simulateMessage("devices/dev123/command", "{\"type\":\"test\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
}

void test_reconnect_connect_event_fires_once() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, connectionEventCount);

    mqtt->disconnect();
    connectionEventCount = 0;

    mqtt->begin("host", 443, "/path");
    client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, connectionEventCount);
}

void test_multiple_room_changes_no_accumulation() {
    const char* rooms[] = {"roomA", "roomB", "roomC"};

    mqtt = createWithTopics();

    for (int i = 0; i < 3; i++) {
        if (i > 0) mqtt->disconnect();

        std::string path = "/agents/broker/";
        path += rooms[i];
        mqtt->begin("host", 443, path.c_str());

        auto* client = MockMqttClient::lastInstance();
        client->simulateConnect();
        mqtt->loop();

        TEST_ASSERT_EQUAL(3, client->subscriptionCount);
    }

    TEST_ASSERT_EQUAL(3, MockMqttClient::instanceCount());

    deliveredMessageCount = 0;
    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/command", "{\"type\":\"test\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
}

void test_large_command_message_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    std::string largePayload = "{\"type\":\"app\",\"code\":\"";
    largePayload.append(8000, 'x');
    largePayload += "\"}";

    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/command", largePayload.c_str());
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
    TEST_ASSERT_EQUAL(largePayload.size(), lastDeliveredLength);
    TEST_ASSERT_EQUAL_STRING_LEN("{\"type\":\"app\"", lastDeliveredPayload, 13);
}

void test_message_at_buffer_limit_delivered() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    std::string payload(10239, 'A');

    auto* client = MockMqttClient::lastInstance();
    client->simulateMessage("devices/dev123/command", payload.c_str());
    mqtt->loop();

    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
    TEST_ASSERT_EQUAL(payload.size(), lastDeliveredLength);
}

void test_single_slot_queue_drops_when_pending() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();

    client->simulateMessage("devices/dev123/command", "{\"type\":\"first\"}");
    client->simulateMessage("devices/dev123/command", "{\"type\":\"second\"}");

    mqtt->loop();

    TEST_ASSERT_EQUAL(1, deliveredMessageCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"first\"}", lastDeliveredPayload);
}

void test_second_message_after_drain() {
    mqtt = createWithTopics();
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();

    client->simulateMessage("devices/dev123/command", "{\"type\":\"first\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(1, deliveredMessageCount);

    client->simulateMessage("devices/dev123/command", "{\"type\":\"second\"}");
    mqtt->loop();
    TEST_ASSERT_EQUAL(2, deliveredMessageCount);
    TEST_ASSERT_EQUAL_STRING("{\"type\":\"second\"}", lastDeliveredPayload);
}

void test_set_client_id_before_begin() {
    mqtt = new CourierMqttTransport();
    mqtt->setMessageCallback(onMessageCallback);
    mqtt->setClientId("my-custom-id");
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("my-custom-id", client->clientId.c_str());
}

void test_set_default_publish_topic() {
    mqtt = new CourierMqttTransport();
    mqtt->setMessageCallback(onMessageCallback);
    mqtt->setDefaultPublishTopic("my/publish/topic");
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    client->simulateConnect();
    mqtt->loop();

    bool result = mqtt->send("{\"type\":\"test\"}");
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("my/publish/topic", client->lastPublishTopic.c_str());
}

void test_config_cert_pem_passed_to_mqtt_client() {
    static const char* MY_CERT = "-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n";
    CourierMqttTransportConfig cfg;
    cfg.cert_pem = MY_CERT;
    mqtt = new CourierMqttTransport(cfg);
    mqtt->setMessageCallback(onMessageCallback);
    mqtt->begin("host", 443, "/path");

    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING(MY_CERT, client->cert_pem.c_str());
}

void test_mqtt_no_cert_by_default() {
    mqtt = new CourierMqttTransport();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_TRUE(client->cert_pem.empty());
}

void test_mqtt_on_configure_called_before_init() {
    mqtt = new CourierMqttTransport();
    mqtt->setMessageCallback(onMessageCallback);

    bool called = false;
    mqtt->onConfigure([&](esp_mqtt_client_config_t& config) {
        called = true;
        config.cert_pem = "HOOK_CERT";
    });

    mqtt->begin("host", 443, "/path");
    TEST_ASSERT_TRUE(called);

    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("HOOK_CERT", client->cert_pem.c_str());
}

void test_mqtt_on_configure_can_override_config_cert() {
    static const char* ORIGINAL_CERT = "ORIGINAL";
    static const char* OVERRIDE_CERT = "OVERRIDE";
    CourierMqttTransportConfig cfg;
    cfg.cert_pem = ORIGINAL_CERT;
    mqtt = new CourierMqttTransport(cfg);
    mqtt->setMessageCallback(onMessageCallback);

    mqtt->onConfigure([](esp_mqtt_client_config_t& config) {
        config.cert_pem = OVERRIDE_CERT;
    });

    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_EQUAL_STRING("OVERRIDE", client->cert_pem.c_str());
}

void test_mqtt_on_configure_not_set_works() {
    mqtt = new CourierMqttTransport();
    mqtt->begin("host", 443, "/path");
    auto* client = MockMqttClient::lastInstance();
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_TRUE(client->started);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_name_is_mqtt);
    RUN_TEST(test_begin_constructs_wss_uri);
    RUN_TEST(test_begin_sets_client_id);
    RUN_TEST(test_begin_without_client_id_uses_empty);
    RUN_TEST(test_begin_starts_client);
    RUN_TEST(test_begin_no_cert_by_default_and_disables_auto_reconnect);
    RUN_TEST(test_subscribes_to_configured_topics);
    RUN_TEST(test_connected_after_connect_event);
    RUN_TEST(test_disconnected_after_disconnect_event);
    RUN_TEST(test_command_message_delivered);
    RUN_TEST(test_status_message_delivered);
    RUN_TEST(test_own_event_topic_message_delivered);
    RUN_TEST(test_other_device_event_delivered);
    RUN_TEST(test_send_publishes_to_default_publish_topic);
    RUN_TEST(test_send_fails_when_disconnected);
    RUN_TEST(test_send_fails_without_publish_topic);
    RUN_TEST(test_disconnect_sets_not_connected);
    RUN_TEST(test_reconnect_after_disconnect);
    RUN_TEST(test_reconnect_with_new_path);
    RUN_TEST(test_send_uses_default_publish_topic);
    RUN_TEST(test_multiple_connect_disconnect_cycles);
    RUN_TEST(test_reconnect_creates_fresh_client);
    RUN_TEST(test_reconnect_exact_subscription_count);
    RUN_TEST(test_reconnect_message_delivered_exactly_once);
    RUN_TEST(test_reconnect_connect_event_fires_once);
    RUN_TEST(test_multiple_room_changes_no_accumulation);
    RUN_TEST(test_large_command_message_delivered);
    RUN_TEST(test_message_at_buffer_limit_delivered);
    RUN_TEST(test_single_slot_queue_drops_when_pending);
    RUN_TEST(test_second_message_after_drain);
    RUN_TEST(test_set_client_id_before_begin);
    RUN_TEST(test_set_default_publish_topic);
    RUN_TEST(test_config_cert_pem_passed_to_mqtt_client);
    RUN_TEST(test_mqtt_no_cert_by_default);
    RUN_TEST(test_mqtt_on_configure_called_before_init);
    RUN_TEST(test_mqtt_on_configure_can_override_config_cert);
    RUN_TEST(test_mqtt_on_configure_not_set_works);

    return UNITY_END();
}
```

- [ ] **Step 2: Run the tests**

```bash
cd test/unit && pio test -e native -v
```

Expected: All 36 MQTT transport tests pass.

- [ ] **Step 3: Commit**

```bash
git add test/unit/test/test_mqtt_transport/test_mqtt_transport.cpp
git commit -m "test: add MQTT transport unit tests (36 tests)"
```

---

### Task 6: Unit test — test_courier

**Files:**
- Create: `test/unit/test/test_courier/test_courier.cpp`

- [ ] **Step 1: Create test_courier.cpp**

```cpp
#include <unity.h>
#include <Courier.h>
#include <CourierTransport.h>
#include <CourierMqttTransport.h>
#include <mqtt_client.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cstring>

// Minimal mock transport for multi-transport tests
class MockTransport : public CourierTransport {
public:
    bool _connected = false;
    bool _started = false;
    bool _suspended = false;
    int sendCount = 0;
    static constexpr int MAX_SENT = 8;
    std::string sentMessages[MAX_SENT];

    void begin(const char* host, uint16_t port, const char* path) override {
        _started = true;
    }
    void disconnect() override {
        _connected = false;
        _started = false;
    }
    bool isConnected() const override { return _connected; }
    bool send(const char* payload) override {
        if (!_connected) return false;
        if (sendCount < MAX_SENT) sentMessages[sendCount] = payload;
        sendCount++;
        return true;
    }
    bool sendBinary(const uint8_t* data, size_t len) override {
        return _connected;
    }
    const char* name() const override { return "MockTransport"; }
    void suspend() override { _suspended = true; _connected = false; }
    void resume() override { _suspended = false; }

    void simulateMessage(const char* payload) {
        queueIncomingMessage(payload, strlen(payload));
    }

    void simulateConnect() {
        _connected = true;
        queueConnectionChange(true);
    }

    void simulateDisconnect() {
        _connected = false;
        queueConnectionChange(false);
    }
};

static Courier* courier = nullptr;

static void advanceToConnected() {
    courier->setup();
    courier->loop();  // WIFI_CONNECTING -> WIFI_CONNECTED
    courier->loop();  // WIFI_CONNECTED -> TRANSPORTS_CONNECTING
    courier->loop();  // TRANSPORTS_CONNECTING: calls begin(), creates mock WS client

    auto* mock = MockWebSocketClient::lastInstance();
    mock->simulateConnect();

    courier->loop();  // Transport loop drains connect -> CONNECTED
}

void setUp(void) {
    _mock_millis = 0;
    WiFi.resetMock();
    HTTPClient::setDefaultMockResponse(200, "{}");
    HTTPClient::setDefaultMockHeader("Tue, 18 Feb 2026 12:00:00 GMT");
    Serial.stopCapture();

    CourierConfig config = {
        .host = "test.example.com",
        .port = 443,
        .path = "/ws"
    };
    courier = new Courier(config);
}

void tearDown(void) {
    delete courier;
    courier = nullptr;
    HTTPClient::resetMockDefaults();
}

void test_initial_state() {
    TEST_ASSERT_EQUAL(COURIER_BOOTING, courier->getState());
}

void test_setup_transitions_to_wifi_connecting() {
    courier->setup();
    TEST_ASSERT_EQUAL(COURIER_WIFI_CONNECTING, courier->getState());
}

void test_builtin_ws_registered() {
    CourierTransport* ws = courier->getTransport("ws");
    TEST_ASSERT_NOT_NULL(ws);
}

void test_add_transport() {
    MockTransport mt;
    courier->addTransport("custom", &mt);
    TEST_ASSERT_EQUAL_PTR(&mt, courier->getTransport("custom"));
}

void test_remove_transport() {
    MockTransport mt;
    courier->addTransport("custom", &mt);
    TEST_ASSERT_NOT_NULL(courier->getTransport("custom"));

    courier->removeTransport("custom");
    TEST_ASSERT_NULL(courier->getTransport("custom"));
}

void test_get_transport_unknown() {
    TEST_ASSERT_NULL(courier->getTransport("nonexistent"));
}

void test_on_message_callback() {
    int callCount = 0;
    std::string receivedType;

    courier->onMessage([&](const char* type, JsonDocument& doc) {
        callCount++;
        receivedType = type;
    });

    advanceToConnected();

    auto* mock = MockWebSocketClient::lastInstance();
    mock->simulateTextMessage("{\"type\":\"test_msg\"}");
    courier->loop();

    TEST_ASSERT_EQUAL(1, callCount);
    TEST_ASSERT_EQUAL_STRING("test_msg", receivedType.c_str());
}

void test_on_message_multiple_callbacks() {
    int callCount1 = 0;
    int callCount2 = 0;

    courier->onMessage([&](const char* type, JsonDocument& doc) { callCount1++; });
    courier->onMessage([&](const char* type, JsonDocument& doc) { callCount2++; });

    advanceToConnected();

    auto* mock = MockWebSocketClient::lastInstance();
    mock->simulateTextMessage("{\"type\":\"multi\"}");
    courier->loop();

    TEST_ASSERT_EQUAL(1, callCount1);
    TEST_ASSERT_EQUAL(1, callCount2);
}

void test_on_raw_message_callback() {
    int callCount = 0;
    std::string receivedPayload;
    size_t receivedLength = 0;

    courier->onRawMessage([&](const char* payload, size_t length) {
        callCount++;
        receivedPayload = std::string(payload, length);
        receivedLength = length;
    });

    advanceToConnected();

    const char* msg = "{\"type\":\"raw_test\",\"data\":123}";
    auto* mock = MockWebSocketClient::lastInstance();
    mock->simulateTextMessage(msg);
    courier->loop();

    TEST_ASSERT_EQUAL(1, callCount);
    TEST_ASSERT_EQUAL_STRING(msg, receivedPayload.c_str());
    TEST_ASSERT_EQUAL(strlen(msg), receivedLength);
}

void test_send_text_when_connected() {
    advanceToConnected();

    bool result = courier->send("{\"hello\":true}");
    TEST_ASSERT_TRUE(result);

    auto* mock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL(1, mock->sendCount);
    TEST_ASSERT_EQUAL_STRING("{\"hello\":true}", mock->sentMessages[0].c_str());
}

void test_send_text_when_disconnected() {
    courier->setup();
    bool result = courier->send("{\"hello\":true}");
    TEST_ASSERT_FALSE(result);
}

void test_send_targets_default_transport() {
    MockTransport mt;
    courier->addTransport("extra", &mt);

    advanceToConnected();

    mt.simulateConnect();
    mt.loop();

    bool result = courier->send("{\"targeted\":true}");
    TEST_ASSERT_TRUE(result);

    auto* wsMock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL(1, wsMock->sendCount);
    TEST_ASSERT_EQUAL_STRING("{\"targeted\":true}", wsMock->sentMessages[0].c_str());

    TEST_ASSERT_EQUAL(0, mt.sendCount);
}

void test_suspend_resume() {
    advanceToConnected();

    auto* mock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_TRUE(mock->started);

    courier->suspendTransports();
    TEST_ASSERT_TRUE(mock->stopped);
    TEST_ASSERT_FALSE(mock->started);

    courier->resumeTransports();
    TEST_ASSERT_TRUE(mock->started);
}

void test_send_to_named_transport() {
    advanceToConnected();

    bool result = courier->sendTo("ws", "{\"targeted\":true}");
    TEST_ASSERT_TRUE(result);

    auto* mock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL(1, mock->sendCount);
    TEST_ASSERT_EQUAL_STRING("{\"targeted\":true}", mock->sentMessages[0].c_str());
}

void test_send_to_unknown_transport() {
    advanceToConnected();

    bool result = courier->sendTo("nonexistent", "{\"x\":1}");
    TEST_ASSERT_FALSE(result);
}

void test_send_to_disconnected() {
    MockTransport mt;
    courier->addTransport("extra", &mt);

    advanceToConnected();

    bool result = courier->sendTo("extra", "{\"x\":1}");
    TEST_ASSERT_FALSE(result);
    TEST_ASSERT_EQUAL(0, mt.sendCount);
}

void test_publish_to_mqtt() {
    MockMqttClient::resetInstanceCount();

    CourierMqttTransport mqttTransport;
    courier->addTransport("mqtt", &mqttTransport);

    advanceToConnected();

    mqttTransport.begin("test.example.com", 443, "/mqtt");
    auto* mqttMock = MockMqttClient::lastInstance();
    TEST_ASSERT_NOT_NULL(mqttMock);
    mqttMock->simulateConnect();

    TEST_ASSERT_TRUE(mqttTransport.isConnected());

    bool result = courier->publishTo("mqtt", "my/topic", "{\"msg\":1}");
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_EQUAL_STRING("my/topic", mqttMock->lastPublishTopic.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"msg\":1}", mqttMock->lastPublishPayload.c_str());
    TEST_ASSERT_EQUAL(1, mqttMock->publishCount);
}

void test_publish_to_non_mqtt() {
    advanceToConnected();

    bool result = courier->publishTo("ws", "ignored/topic", "{\"fallback\":true}");
    TEST_ASSERT_TRUE(result);

    auto* mock = MockWebSocketClient::lastInstance();
    TEST_ASSERT_EQUAL(1, mock->sendCount);
    TEST_ASSERT_EQUAL_STRING("{\"fallback\":true}", mock->sentMessages[0].c_str());
}

void test_on_error_callback_registered() {
    int errorCount = 0;
    std::string lastCategory;
    std::string lastMessage;

    courier->onError([&](const char* category, const char* message) {
        errorCount++;
        lastCategory = category;
        lastMessage = message;
    });

    advanceToConnected();
    TEST_ASSERT_EQUAL(COURIER_CONNECTED, courier->getState());

    WiFi.setMockStatus(WL_DISCONNECTED);

    for (int i = 0; i < 3; i++) {
        _mock_millis += 5001;
        courier->loop();
    }

    TEST_ASSERT_GREATER_THAN(0, errorCount);
    TEST_ASSERT_EQUAL_STRING("WIFI", lastCategory.c_str());

    WiFi.resetMock();
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_initial_state);
    RUN_TEST(test_setup_transitions_to_wifi_connecting);
    RUN_TEST(test_builtin_ws_registered);
    RUN_TEST(test_add_transport);
    RUN_TEST(test_remove_transport);
    RUN_TEST(test_get_transport_unknown);
    RUN_TEST(test_on_message_callback);
    RUN_TEST(test_on_message_multiple_callbacks);
    RUN_TEST(test_on_raw_message_callback);
    RUN_TEST(test_send_text_when_connected);
    RUN_TEST(test_send_text_when_disconnected);
    RUN_TEST(test_send_targets_default_transport);
    RUN_TEST(test_suspend_resume);
    RUN_TEST(test_send_to_named_transport);
    RUN_TEST(test_send_to_unknown_transport);
    RUN_TEST(test_send_to_disconnected);
    RUN_TEST(test_publish_to_mqtt);
    RUN_TEST(test_publish_to_non_mqtt);
    RUN_TEST(test_on_error_callback_registered);

    return UNITY_END();
}
```

- [ ] **Step 2: Run the tests**

```bash
cd test/unit && pio test -e native -v
```

Expected: All 19 Courier tests pass.

- [ ] **Step 3: Commit**

```bash
git add test/unit/test/test_courier/test_courier.cpp
git commit -m "test: add Courier core unit tests (19 tests)"
```

---

### Task 7: Build verification — PlatformIO example projects

**Files:**
- Create: `test/build-platformio/basic-websocket/platformio.ini`
- Create: `test/build-platformio/mqtt-pubsub/platformio.ini`

- [ ] **Step 1: Create basic-websocket/platformio.ini**

```ini
; Build verification — compiles the basic-websocket example against ESP32
[env:esp32]
platform = espressif32
board = esp32dev
framework = arduino
src_dir = ../../../examples/basic-websocket
lib_extra_dirs = ../../../
lib_deps =
    tzapu/WiFiManager@^2.0.17
    bblanchon/ArduinoJson@^7.4.2
    ropg/ezTime@^0.8.3
```

- [ ] **Step 2: Create mqtt-pubsub/platformio.ini**

```ini
; Build verification — compiles the mqtt-pubsub example against ESP32
[env:esp32]
platform = espressif32
board = esp32dev
framework = arduino
src_dir = ../../../examples/mqtt-pubsub
lib_extra_dirs = ../../../
lib_deps =
    tzapu/WiFiManager@^2.0.17
    bblanchon/ArduinoJson@^7.4.2
    ropg/ezTime@^0.8.3
```

- [ ] **Step 3: Test locally — build both (compile only, no upload)**

```bash
cd test/build-platformio/basic-websocket && pio run
cd test/build-platformio/mqtt-pubsub && pio run
```

Expected: Both compile successfully.

- [ ] **Step 4: Commit**

```bash
git add test/build-platformio/basic-websocket/platformio.ini \
        test/build-platformio/mqtt-pubsub/platformio.ini
git commit -m "test: add PlatformIO build verification for examples"
```

---

### Task 8: ESP-IDF example and build verification

**Files:**
- Create: `examples/espidf-basic/main/main.cpp`
- Create: `examples/espidf-basic/main/CMakeLists.txt`
- Create: `test/build-espidf/sample-project/CMakeLists.txt`
- Create: `test/build-espidf/sample-project/sdkconfig.defaults`

- [ ] **Step 1: Create examples/espidf-basic/main/main.cpp**

```cpp
// ESP-IDF example — demonstrates Courier with both WebSocket and MQTT transports.
// This uses the Arduino component for ESP-IDF (not Arduino IDE).

#include <Courier.h>
#include <CourierMqttTransport.h>

Courier courier({
    .host = "example.com",
    .port = 443,
    .path = "/ws"
});

CourierMqttTransport mqtt({
    .topics = {"devices/my-device/command"},
    .defaultPublishTopic = "devices/my-device/event",
    .clientId = "my-device-001"
});

extern "C" void app_main() {
    courier.onConnected([]() {
        courier.send("{\"type\":\"hello\"}");
    });

    courier.onMessage([](const char* type, JsonDocument& doc) {
        // Handle incoming messages by type
    });

    courier.onError([](const char* category, const char* message) {
        // Handle errors
    });

    courier.addTransport("mqtt", &mqtt);
    courier.setup();

    while (true) {
        courier.loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

- [ ] **Step 2: Create examples/espidf-basic/main/CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES courier
)
```

- [ ] **Step 3: Create test/build-espidf/sample-project/CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.16)

# Point to the courier component at repo root
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
)

# Use the example's main/ as our main component
set(COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../examples/espidf-basic/main"
    ${EXTRA_COMPONENT_DIRS}
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(courier-espidf-test)
```

Note: The exact CMake incantation may need adjustment during implementation. The key is that `EXTRA_COMPONENT_DIRS` finds the courier component at the repo root, and the main component comes from the example. Test by running `idf.py build` in this directory.

- [ ] **Step 4: Create test/build-espidf/sample-project/sdkconfig.defaults**

```
CONFIG_IDF_TARGET="esp32"
```

- [ ] **Step 5: Test locally (requires ESP-IDF toolchain or Docker)**

```bash
cd test/build-espidf/sample-project && idf.py build
```

Or with Docker:
```bash
docker run --rm -v /Users/matt/code/courier:/project -w /project/test/build-espidf/sample-project espressif/idf:v5.3 idf.py build
```

Expected: Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add examples/espidf-basic/main/main.cpp examples/espidf-basic/main/CMakeLists.txt \
        test/build-espidf/sample-project/CMakeLists.txt \
        test/build-espidf/sample-project/sdkconfig.defaults
git commit -m "feat: add ESP-IDF example and build verification project"
```

---

### Task 9: Test runner — tools/run-tests.py

**Files:**
- Create: `tools/run-tests.py`

- [ ] **Step 1: Create run-tests.py**

```python
#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "click>=8.1",
# ]
# ///

import subprocess
import sys
from pathlib import Path

import click

# Project root (parent of tools/)
ROOT = Path(__file__).parent.parent

CPPCHECK_SUPPRESSIONS = [
    "missingIncludeSystem",
    "unmatchedSuppression",
    # Transport classes own connections and are not copyable by design
    "noCopyConstructor",
    "noOperatorEq",
]


def run_cmd(cmd: list[str], cwd: Path | None = None, label: str = "") -> bool:
    """Run a command, return True if successful."""
    if label:
        click.echo(click.style(f"  → {label}", fg="cyan"))
    result = subprocess.run(cmd, cwd=cwd)
    return result.returncode == 0


@click.group(context_settings={"help_option_names": ["-h", "--help"]})
def cli() -> None:
    """Run tests for the Courier library."""
    pass


@cli.command("static-analysis")
def static_analysis() -> None:
    """Run cppcheck static analysis on src/."""
    click.echo(click.style("Static Analysis", fg="white", bold=True))

    cmd = ["cppcheck", "--enable=warning", "--error-exitcode=1"]
    for suppression in CPPCHECK_SUPPRESSIONS:
        cmd.append(f"--suppress={suppression}")
    cmd.append(str(ROOT / "src"))

    if run_cmd(cmd, cwd=ROOT, label="cppcheck src/"):
        click.echo(click.style("✓ Static analysis passed", fg="green"))
    else:
        click.echo(click.style("✗ Static analysis failed", fg="red"))
        sys.exit(1)


@cli.command("unit")
def unit_tests() -> None:
    """Run unit tests on native platform."""
    click.echo(click.style("Unit Tests", fg="white", bold=True))

    test_dir = ROOT / "test" / "unit"
    if not run_cmd(["pio", "test", "-e", "native"], cwd=test_dir, label="pio test -e native"):
        click.echo(click.style("✗ Unit tests failed", fg="red"))
        sys.exit(1)

    click.echo(click.style("✓ Unit tests passed", fg="green"))


@cli.command("build")
def build_verification() -> None:
    """Build-verify PlatformIO examples and ESP-IDF project."""
    click.echo(click.style("Build Verification", fg="white", bold=True))
    success = True

    # PlatformIO builds
    pio_dir = ROOT / "test" / "build-platformio"
    for project in sorted(pio_dir.iterdir()):
        if not project.is_dir():
            continue
        ini = project / "platformio.ini"
        if not ini.exists():
            continue
        if not run_cmd(["pio", "run"], cwd=project, label=f"pio run ({project.name})"):
            success = False

    # ESP-IDF build
    espidf_dir = ROOT / "test" / "build-espidf" / "sample-project"
    if espidf_dir.exists():
        if not run_cmd(["idf.py", "build"], cwd=espidf_dir, label="idf.py build (sample-project)"):
            success = False

    if success:
        click.echo(click.style("✓ All builds passed", fg="green"))
    else:
        click.echo(click.style("✗ Some builds failed", fg="red"))
        sys.exit(1)


@cli.command("all")
@click.pass_context
def run_all(ctx: click.Context) -> None:
    """Run static analysis, unit tests, and build verification."""
    ctx.invoke(static_analysis)
    click.echo()
    ctx.invoke(unit_tests)
    click.echo()
    ctx.invoke(build_verification)


if __name__ == "__main__":
    cli()
```

- [ ] **Step 2: Make it executable**

```bash
chmod +x tools/run-tests.py
```

- [ ] **Step 3: Test the runner locally**

```bash
./tools/run-tests.py unit
```

Expected: Runs all unit tests via PlatformIO.

- [ ] **Step 4: Commit**

```bash
git add tools/run-tests.py
git commit -m "feat: add test runner (tools/run-tests.py)"
```

---

### Task 10: CI workflow — .github/workflows/ci.yml

**Files:**
- Create: `.github/workflows/ci.yml`

- [ ] **Step 1: Create ci.yml**

```yaml
name: CI

on:
  push:
    branches: [main]
  pull_request:
    branches: [main]

jobs:
  static-analysis:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install cppcheck
        run: sudo apt-get update && sudo apt-get install -y cppcheck
      - name: Run static analysis
        run: ./tools/run-tests.py static-analysis

  unit-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: "3.11"
      - name: Install PlatformIO
        run: pip install platformio
      - name: Run unit tests
        run: ./tools/run-tests.py unit

  build-platformio:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: "3.11"
      - name: Install PlatformIO
        run: pip install platformio
      - name: Build basic-websocket example
        run: cd test/build-platformio/basic-websocket && pio run
      - name: Build mqtt-pubsub example
        run: cd test/build-platformio/mqtt-pubsub && pio run

  build-espidf:
    runs-on: ubuntu-latest
    container:
      image: espressif/idf:v5.3
    steps:
      - uses: actions/checkout@v4
      - name: Build ESP-IDF sample project
        run: |
          . $IDF_PATH/export.sh
          cd test/build-espidf/sample-project
          idf.py build
```

- [ ] **Step 2: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: add GitHub Actions workflow for tests and builds"
```

---

### Task 11: Branch protection

- [ ] **Step 1: Push the branch and create/update PR so CI runs**

Push all commits to the `test-suite-design` branch (or a new implementation branch). CI must run at least once so the status check names exist in GitHub before branch protection can reference them.

- [ ] **Step 2: Wait for CI to complete**

The four jobs (`static-analysis`, `unit-tests`, `build-platformio`, `build-espidf`) must each run at least once (pass or fail) for GitHub to recognize them as available status checks.

- [ ] **Step 3: Configure branch protection via gh CLI**

```bash
gh api repos/inanimate-tech/courier/branches/main/protection \
  --method PUT \
  --input - << 'EOF'
{
  "required_status_checks": {
    "strict": true,
    "contexts": [
      "static-analysis",
      "unit-tests",
      "build-platformio",
      "build-espidf"
    ]
  },
  "enforce_admins": false,
  "required_pull_request_reviews": null,
  "restrictions": null,
  "allow_force_pushes": false,
  "allow_deletions": false
}
EOF
```

- [ ] **Step 4: Verify branch protection is active**

```bash
gh api repos/inanimate-tech/courier/branches/main/protection
```

Expected: Response shows required status checks with the four job names.

- [ ] **Step 5: Commit any remaining changes**

No code changes expected here — this is configuration only.

---

### Task 12: Update CLAUDE.md with test commands

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Add test commands section to CLAUDE.md**

Add after the "Build System" section:

```markdown
## Testing

Run all tests locally:
```
./tools/run-tests.py all                # everything
./tools/run-tests.py static-analysis    # cppcheck on src/
./tools/run-tests.py unit               # native unit tests
./tools/run-tests.py build              # PlatformIO + ESP-IDF builds
```

Unit tests run on native platform (not on device). They use mocks in `test/mocks/` for all ESP-IDF and Arduino dependencies.
```

- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: add test commands to CLAUDE.md"
```
