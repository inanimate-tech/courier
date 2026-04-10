# Courier API Reference

## Configuration

All configuration uses a struct-and-assign pattern compatible with C++11:

```cpp
#include <Courier.h>

CourierConfig cfg;
cfg.host = "api.example.com";
cfg.port = 443;
cfg.path = "/ws";
cfg.apName = "MyDevice";

Courier courier(cfg);
```

For global instances, use a factory function:

```cpp
CourierConfig makeConfig() {
    CourierConfig cfg;
    cfg.host = "api.example.com";
    cfg.port = 443;
    cfg.path = "/ws";
    return cfg;
}

Courier courier(makeConfig());
```

### CourierConfig

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `host` | `const char*` | `nullptr` | Server hostname |
| `port` | `uint16_t` | `443` | Server port |
| `path` | `const char*` | `"/"` | Path on server |
| `apName` | `const char*` | `nullptr` | WiFi captive portal AP name |
| `defaultTransport` | `const char*` | `"ws"` | Which transport `send()` uses |
| `defaultTopic` | `const char*` | `nullptr` | Topic for `send()` when transport requires one |
| `dns1` | `uint32_t` | `0` | Primary DNS server (`0` = use DHCP default). Cast from `IPAddress`. |
| `dns2` | `uint32_t` | `0` | Secondary DNS server (`0` = none). Cast from `IPAddress`. |

DNS example:

```cpp
cfg.dns1 = (uint32_t)IPAddress(8, 8, 8, 8);
cfg.dns2 = (uint32_t)IPAddress(1, 1, 1, 1);
```

---

## Courier

### Lifecycle

```cpp
courier.setup();    // Initialize WiFi and transports
courier.loop();     // Call every iteration of your main loop
```

### State

```cpp
courier.isConnected();     // true when all transports are connected
courier.getState();        // CourierState enum
courier.isTimeSynced();    // true after NTP or HTTP Date sync
```

#### CourierState enum

```
COURIER_BOOTING
COURIER_WIFI_CONNECTING
COURIER_WIFI_CONNECTED
COURIER_WIFI_CONFIGURING
COURIER_TRANSPORTS_CONNECTING
COURIER_CONNECTED
COURIER_RECONNECTING
COURIER_CONNECTION_FAILED
```

State machine:

```
BOOTING → WIFI_CONNECTING → WIFI_CONNECTED → TRANSPORTS_CONNECTING → CONNECTED
                                                      ↑                    |
                                                 RECONNECTING ←-----------+
                                                      |
                                              CONNECTION_FAILED
```

### Sending

The `"To"` suffix means you specify the transport by name.

```cpp
courier.send(payload);                              // default transport + topic
courier.sendTo("mqtt", payload);                    // named transport
courier.sendBinaryTo("ws", data, len);              // named transport, binary
courier.publishTo("mqtt", "my/topic", payload);     // named transport + explicit topic
```

Change the defaults at runtime:

```cpp
courier.setDefaultTransport("mqtt");
courier.setDefaultTopic("events/mine");
```

### Transport management

A WebSocket transport is always registered as `"ws"`. Add others before calling `setup()`:

```cpp
courier.addTransport("mqtt", &mqttTransport);
courier.getTransport("mqtt");       // returns CourierTransport* or nullptr
courier.removeTransport("mqtt");
```

Override the endpoint for a specific transport (falls back to Courier-level host/port if not set):

```cpp
CourierEndpoint ep;
ep.path = "/mqtt";
courier.setEndpoint("mqtt", ep);
```

Suspend and resume transports to free SRAM during OTA updates:

```cpp
courier.suspendTransports();
// ... perform OTA ...
courier.resumeTransports();
```

Access the built-in WebSocket transport directly:

```cpp
courier.builtinWS();    // returns CourierWSTransport&
```

### Callbacks

Multiple registrations are supported for each callback type (up to 4 per type).

```cpp
// JSON messages — type is extracted from the "type" field
courier.onMessage([](const char* type, JsonDocument& doc) { });

// Raw messages — for non-JSON or custom framing
courier.onRawMessage([](const char* payload, size_t len) { });

// Connection state
courier.onConnected([]() { });
courier.onDisconnected([]() { });
courier.onConnectionChange([](CourierState state) { });

// Errors — category is "WIFI", "TRANSPORT", "TIME_SYNC", etc.
courier.onError([](const char* category, const char* msg) { });
```

### Lifecycle hooks

Called in order during state transitions. Use for registration or token exchange before transports connect.

```cpp
courier.onTransportsWillConnect([]() { });   // before transports start
courier.onTransportsDidConnect([]() { });    // after transports connect
```

### WiFi configuration

```cpp
courier.setAPName("MyDevice");

// Access the underlying WiFiManager for advanced WiFi configuration
courier.onConfigureWiFi([](WiFiManager& wm) {
    wm.setConnectTimeout(30);
    wm.setHostname("my-device");
});
```

---

## WebSocket Transport

The built-in WebSocket transport is always registered as `"ws"`. It wraps `esp_websocket_client` from ESP-IDF.

Access it via `courier.builtinWS()` to configure:

```cpp
courier.builtinWS().onConfigure([](esp_websocket_client_config_t& cfg) {
    cfg.headers = "Authorization: Bearer token\r\n";
    cfg.subprotocol = "graphql-ws";
});
```

### CourierWSTransportConfig

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `cert_pem` | `const char*` | `nullptr` | TLS certificate PEM string (`nullptr` = no cert set by Courier) |

---

## MQTT Transport

```cpp
#include <CourierMqttTransport.h>

CourierMqttTransport mqtt;
```

### Configuration

MQTT is configured via setter methods before calling `courier.setup()`:

```cpp
mqtt.subscribe("sensors/+/data");
mqtt.subscribe("commands/my-device");
mqtt.setDefaultPublishTopic("sensors/my-device/data");
mqtt.setClientId("my-device-001");

courier.addTransport("mqtt", &mqtt);
courier.setup();
```

Or via a config struct:

```cpp
CourierMqttTransportConfig mqttCfg;
mqttCfg.topics = {"sensors/+/data", "commands/my-device"};
mqttCfg.defaultPublishTopic = "sensors/my-device/data";
mqttCfg.clientId = "my-device-001";

CourierMqttTransport mqtt(mqttCfg);
```

### CourierMqttTransportConfig

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `topics` | `std::vector<std::string>` | `{}` | Auto-subscribed on every (re)connect |
| `defaultPublishTopic` | `const char*` | `nullptr` | Topic used by `send()` when MQTT is the default transport |
| `clientId` | `const char*` | `nullptr` | MQTT client ID (`nullptr` = ESP-IDF generates one) |
| `cert_pem` | `const char*` | `nullptr` | TLS certificate PEM string |
| `task_stack` | `int` | `8192` | MQTT task stack size in bytes |

### Dynamic topic management

```cpp
mqtt.subscribe("alerts/#");              // QoS 0
mqtt.subscribe("alerts/critical", 1);    // QoS 1
mqtt.unsubscribe("alerts/#");
```

Topics are added to a managed list. On reconnect, all topics are re-subscribed automatically.

### Publishing

```cpp
mqtt.publish("topic", "payload");                            // QoS 0, no retain
mqtt.publish("topic", "payload", /*qos=*/1, /*retain=*/true);  // explicit QoS/retain
```

### Raw IDF config access

```cpp
mqtt.onConfigure([](esp_mqtt_client_config_t& cfg) {
    // ESP-IDF v5.x (nested struct)
    cfg.credentials.username = "user";
    cfg.credentials.authentication.password = "pass";
    cfg.session.last_will.topic = "/status";
    cfg.session.last_will.msg = "offline";
});
```

---

## CourierEndpoint

Override the host, port, or path for a specific transport. Fields set to their defaults (`nullptr`/`0`) fall back to the Courier-level config.

```cpp
CourierEndpoint ep;
ep.path = "/mqtt";
courier.setEndpoint("mqtt", ep);
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `host` | `const char*` | `nullptr` | Override host (`nullptr` = use Courier config) |
| `port` | `uint16_t` | `0` | Override port (`0` = use Courier config) |
| `path` | `const char*` | `nullptr` | Override path (`nullptr` = use Courier config) |

---

## Custom Transports

Subclass `CourierTransport` to implement a custom transport:

```cpp
class MyTransport : public CourierTransport {
public:
    void begin(const char* host, uint16_t port, const char* path) override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(const char* payload) override;
    const char* name() const override { return "MyTransport"; }
};
```

Optional overrides:

| Method | Default | Description |
|--------|---------|-------------|
| `loop()` | calls `drainPending()` | Called every main loop iteration |
| `sendBinary(data, len)` | returns `false` | Binary send support |
| `publish(topic, payload)` | calls `send(payload)` | Topic-addressed send |
| `topicRequired()` | returns `false` | If true, `send()` on Courier uses the default topic |
| `suspend()` | no-op | Free resources for OTA |
| `resume()` | no-op | Restore after OTA |

From your transport's event handler (which may run on a different task), use the thread-safe queue methods:

```cpp
queueIncomingMessage(payload, len);     // queue a received message
queueConnectionChange(true);            // queue a connection state change
```

These are drained on the main loop by `drainPending()`.

---

## Limits

| Constant | Value | Description |
|----------|-------|-------------|
| `MAX_TRANSPORTS` | 4 | Maximum registered transports |
| `MAX_CALLBACKS` | 4 | Maximum callbacks per event type |
| `MIN_RECONNECT_INTERVAL` | 5000 ms | Initial backoff delay |
| `MAX_RECONNECT_INTERVAL` | 60000 ms | Maximum backoff delay |
| `MAX_RECONNECT_ATTEMPTS` | 10 | Hard limit before `CONNECTION_FAILED` |
