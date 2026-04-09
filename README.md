# Courier

Batteries-included JSON messaging for ESP32. WiFi, WebSocket, MQTT, reconnection — all handled.

Courier expects JSON messages with a `"type"` field. Messages are parsed with ArduinoJson and the `type` string is passed to `onMessage` callbacks alongside the parsed document. Use `onRawMessage` for non-JSON or custom framing.

```cpp
#include <Courier.h>

Courier courier({.host = "api.example.com", .port = 443, .path = "/ws"});

void setup() {
  courier.onConnected([]() { courier.send("{\"type\":\"hello\"}"); });
  courier.onMessage([](const char* type, JsonDocument& doc) {
    Serial.printf("Got: %s\n", type);
  });
  courier.setup();
}

void loop() { courier.loop(); }
```

## What it does

- **WiFi** — captive portal config via WiFiManager, auto-reconnection
- **WebSocket** — built-in transport with TLS, ping/pong heartbeat
- **MQTT** — opt-in transport with subscribe/unsubscribe, topic-addressed publishing
- **Reconnection** — exponential backoff (5s-60s), health monitoring, automatic recovery
- **Time sync** — NTP primary (continuous drift correction) + HTTP Date header fallback
- **JSON routing** — messages parsed and dispatched by `type` field
- **Transport map** — named transports, broadcast or targeted sending

## Opinionated

Courier bundles a number of other great libraries:

- **WebSocket** — esp_websocket_client [Documentation](https://docs.espressif.com/projects/esp-protocols/esp_websocket_client/docs/latest/index.html)
- **MQTT** — esp_mqtt_client [Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mqtt.html)
- **WiFi config** — WiFiManager [GitHub](https://github.com/tzapu/WiFiManager)
- **JSON** — ArduinoJson [Documentation](https://arduinojson.org/)
- **Time** — ezTime [GitHub](https://github.com/ropg/ezTime)

Use `onConfigure` hooks to access the full configuration surface of each bundled library.

## Install

**PlatformIO:**
```ini
lib_deps = inanimate/courier
```

**ESP-IDF Component:**
```yml
dependencies:
  inanimate-tech/courier:
    version: "^0.1.0"
```

## API

### Courier

```cpp
Courier courier({
  .host = "example.com",
  .port = 443,
  .path = "/ws",
  .apName = "MyDevice",           // WiFi portal AP name
  .defaultTransport = "ws",       // which transport send() uses
  .defaultTopic = nullptr         // topic for send() if transport requires it
});

courier.setup();
courier.loop();

// State
courier.isConnected();
courier.getState();        // CourierState enum
courier.isTimeSynced();

// Sending — "To" suffix means you specify the transport
courier.send(payload);                              // default transport + topic
courier.sendTo("mqtt", payload);                    // named transport
courier.sendBinaryTo("ws", data, len);              // named transport, binary
courier.publishTo("mqtt", "my/topic", payload);     // named transport + topic

// Defaults (can also set in config)
courier.setDefaultTransport("ws");
courier.setDefaultTopic("events/mine");

// Transports
courier.addTransport("mqtt", &mqttTransport);
courier.getTransport("mqtt");
courier.removeTransport("mqtt");
courier.setEndpoint("mqtt", {.path = "/mqtt"});
courier.suspendTransports();   // free SRAM for OTA
courier.resumeTransports();

// Callbacks (multiple registrations supported)
courier.onMessage([](const char* type, JsonDocument& doc) { });
courier.onRawMessage([](const char* payload, size_t len) { });
courier.onConnected([]() { });
courier.onDisconnected([]() { });
courier.onConnectionChange([](CourierState state) { });
courier.onError([](const char* category, const char* msg) { });

// Lifecycle hooks (run in order during state transitions)
courier.onTransportsWillConnect([]() { });   // before transports start
courier.onTransportsDidConnect([]() { });    // after transports connect
```

### MQTT Transport

```cpp
#include <CourierMqttTransport.h>

CourierMqttTransport mqtt({
  .topics = {"sensors/+/data"},           // auto-subscribed on (re)connect
  .defaultPublishTopic = "my/events",     // used by send() when MQTT is default transport
  .clientId = "my-device",
  .cert_pem = nullptr                     // nullptr = use cert bundle
});

mqtt.subscribe("alerts/#");
mqtt.unsubscribe("alerts/#");
mqtt.publish("topic", "payload");                  // virtual override
mqtt.publish("topic", "payload", /*qos*/1, /*retain*/false);  // QoS/retain overload
```

### Raw IDF config access

For anything Courier doesn't bundle directly, use `onConfigure` hooks to access the underlying ESP-IDF config structs:

```cpp
// WiFiManager
courier.onConfigureWiFi([](WiFiManager& wm) {
  wm.setConnectTimeout(30);
  wm.setHostname("my-device");
});

// WebSocket (esp_websocket_client)
courier.builtinWS().onConfigure([](esp_websocket_client_config_t& cfg) {
  cfg.headers = "Authorization: Bearer token\r\n";
  cfg.subprotocol = "graphql-ws";
});

// MQTT (esp_mqtt_client)
mqtt.onConfigure([](esp_mqtt_client_config_t& cfg) {
  cfg.credentials.username = "user";
  cfg.credentials.authentication.password = "pass";
  cfg.session.last_will.topic = "/status";
  cfg.session.last_will.msg = "offline";
});
```

## Connectivity state machine

```
BOOTING -> WIFI_CONNECTING -> WIFI_CONNECTED -> TRANSPORTS_CONNECTING -> CONNECTED
                                                        ^                    |
                                                   RECONNECTING <-----------+
                                                        |
                                                CONNECTION_FAILED
```

`onConnectionChange` fires at each state transition. `onError` fires alongside transitions caused by failures, providing a category and reason (e.g. `"WIFI"`, `"connection lost"`).

## Limitations

- **Single instance** — WiFiManager requires a static callback, so only one Courier instance per process
- **Single message slot** — transport callbacks queue one pending message at a time; messages arriving before the main loop drains are dropped
- **Arduino + ESP-IDF** — depends on Arduino framework for WiFiManager, ArduinoJson, ezTime

## License

MIT
