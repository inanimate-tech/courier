#pragma once

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
