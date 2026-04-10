#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>

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
    uint8_t op_code;
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
