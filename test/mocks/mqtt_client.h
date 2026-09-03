#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

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

typedef enum {
    MQTT_ERROR_TYPE_NONE = 0,
    MQTT_ERROR_TYPE_TCP_TRANSPORT,
    MQTT_ERROR_TYPE_CONNECTION_REFUSED,
    MQTT_ERROR_TYPE_SUBSCRIBE_FAILED
} esp_mqtt_error_type_t;

typedef enum {
    MQTT_CONNECTION_ACCEPTED = 0,
    MQTT_CONNECTION_REFUSE_PROTOCOL,
    MQTT_CONNECTION_REFUSE_ID_REJECTED,
    MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE,
    MQTT_CONNECTION_REFUSE_BAD_USERNAME,
    MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
} esp_mqtt_connect_return_code_t;

// Field order and names mirror the real esp_mqtt_error_codes_t on both
// ESP-IDF 4.4 and 5.x. Production code must not name this type — it reaches
// the fields through `auto*` off event->error_handle.
struct esp_mqtt_error_codes_mock_t {
    esp_err_t esp_tls_last_esp_err;
    int esp_tls_stack_err;
    int esp_tls_cert_verify_flags;
    esp_mqtt_error_type_t error_type;
    esp_mqtt_connect_return_code_t connect_return_code;
    int esp_transport_sock_errno;
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
    esp_err_t (*crt_bundle_attach)(void* conf);
    const char* client_id;
    bool disable_auto_reconnect;
    void* user_context;
    int task_stack;
    int buffer_size;
    int out_buffer_size;
    int network_timeout_ms;
} esp_mqtt_client_config_t;

class MockMqttClient {
public:
    MockMqttClient() { s_lastInstance = this; s_instanceCount++; }
    ~MockMqttClient() = default;

    std::string uri;
    std::string cert_pem;
    esp_err_t (*crt_bundle_attach)(void*) = nullptr;
    std::string clientId;
    bool disable_auto_reconnect = false;
    int out_buffer_size = 0;
    int network_timeout_ms = 0;

    // Set by a test to hold a publish inside the client, so the caller's
    // bounded-wait lock can be observed from another thread.
    std::atomic<bool> blockPublish{false};

    bool connected = false;
    bool started = false;
    bool stopped = false;
    bool destroyed = false;

    esp_event_handler_t_mqtt eventHandler = nullptr;
    void* eventHandlerArg = nullptr;

    static constexpr int MAX_SUBSCRIPTIONS = 16;
    std::string subscribedTopics[MAX_SUBSCRIPTIONS];
    int subscriptionCount = 0;

    int lastSubscribeQos = 0;

    std::string unsubscribedTopics[MAX_SUBSCRIPTIONS];
    int unsubscribeCount = 0;

    std::string lastPublishTopic;
    std::string lastPublishPayload;  // binary-safe: sized by lastPublishLength
    size_t lastPublishLength = 0;
    int publishCount = 0;
    int lastPublishQos = 0;
    bool lastPublishRetain = false;

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

    // Binary variant: explicit length, payload may contain embedded NULs.
    void simulateBinaryMessage(const char* topic, const uint8_t* data, size_t len) {
        if (eventHandler) {
            esp_mqtt_event_t event = {};
            event.topic = topic;
            event.topic_len = strlen(topic);
            event.data = (const char*)data;
            event.data_len = (int)len;
            event.total_data_len = (int)len;
            event.current_data_offset = 0;
            eventHandler(eventHandlerArg, "MQTT_EVENTS",
                        MQTT_EVENT_DATA, &event);
        }
    }

    // One chunk of a fragmented message, as esp-mqtt delivers them.
    void simulateBinaryChunk(const char* topic, const uint8_t* data, size_t len,
                             size_t totalLen, size_t offset) {
        if (eventHandler) {
            esp_mqtt_event_t event = {};
            if (offset == 0) {
                event.topic = topic;
                event.topic_len = strlen(topic);
            }
            event.data = (const char*)data;
            event.data_len = (int)len;
            event.total_data_len = (int)totalLen;
            event.current_data_offset = (int)offset;
            eventHandler(eventHandlerArg, "MQTT_EVENTS",
                        MQTT_EVENT_DATA, &event);
        }
    }

    void simulateError(esp_mqtt_error_type_t errorType,
                       esp_mqtt_connect_return_code_t connectReturnCode) {
        esp_mqtt_error_codes_mock_t err = {};
        err.error_type = errorType;
        err.connect_return_code = connectReturnCode;
        dispatchError(&err);
    }

    void simulateTransportError(int tlsEspErr, int tlsStackErr,
                                int certVerifyFlags, int sockErrno) {
        esp_mqtt_error_codes_mock_t err = {};
        err.error_type = MQTT_ERROR_TYPE_TCP_TRANSPORT;
        err.esp_tls_last_esp_err = tlsEspErr;
        err.esp_tls_stack_err = tlsStackErr;
        err.esp_tls_cert_verify_flags = certVerifyFlags;
        err.esp_transport_sock_errno = sockErrno;
        dispatchError(&err);
    }

    // IDF always populates error_handle, but the production code null-checks
    // it; this proves the check is real.
    void simulateErrorWithNullHandle() {
        if (eventHandler) {
            esp_mqtt_event_t event = {};
            event.error_handle = nullptr;
            eventHandler(eventHandlerArg, "MQTT_EVENTS",
                        MQTT_EVENT_ERROR, &event);
        }
    }

    void dispatchError(esp_mqtt_error_codes_mock_t* err) {
        if (eventHandler) {
            esp_mqtt_event_t event = {};
            event.error_handle = err;
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

inline esp_mqtt_client_handle_t esp_mqtt_client_init(
    const esp_mqtt_client_config_t* config)
{
    auto* client = new MockMqttClient();
    if (config->uri) client->uri = config->uri;
    if (config->cert_pem) client->cert_pem = config->cert_pem;
    client->crt_bundle_attach = config->crt_bundle_attach;
    if (config->client_id) client->clientId = config->client_id;
    client->disable_auto_reconnect = config->disable_auto_reconnect;
    client->out_buffer_size = config->out_buffer_size;
    client->network_timeout_ms = config->network_timeout_ms;
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
    client->lastSubscribeQos = qos;
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
    if (!client->connected) return -1;
    while (client->blockPublish.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    client->lastPublishTopic = topic;
    if (len == 0 && data) {
        client->lastPublishPayload = data;  // IDF strlen branch
    } else if (data) {
        client->lastPublishPayload = std::string(data, len);
    } else {
        client->lastPublishPayload.clear();
    }
    client->lastPublishLength = client->lastPublishPayload.size();
    client->lastPublishQos = qos;
    client->lastPublishRetain = (retain != 0);
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
