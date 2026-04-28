# Courier API Reference

Authoritative reference for the 0.4.0 API. For a tutorial-style introduction see [README.md](../README.md). For migration from 0.3.x see [migration-0.3-to-0.4.md](migration-0.3-to-0.4.md).

## Header includes

| Header | Provides |
|--------|----------|
| `<Courier.h>` | `Courier::Client`, `Courier::Config`, `Courier::State`, `Courier::Endpoint`, and the WebSocket transport (always pulled in) |
| `<MqttTransport.h>` | `Courier::MqttTransport` |
| `<UdpTransport.h>` | `Courier::UdpTransport` |
| `<WebSocketTransport.h>` | already included by `Courier.h`; include directly only if you reference the type without the manager |
| `<Transport.h>` | `Courier::Transport` base class — needed only when subclassing |
| `<Endpoint.h>` | `Courier::Endpoint` (also pulled in by `Courier.h`) |
| `<SpscQueue.h>` | `Courier::SpscQueue<T, N>` — exposed for custom transports |

## Namespace

All public types live in `namespace Courier`. Examples in this document use fully qualified names.

### Arduino `Client` collision

Arduino's `<Arduino.h>` defines a global `class Client` (the base for `WiFiClient`, `EthernetClient`, etc.). A blanket `using namespace Courier;` therefore makes any unqualified reference to `Client` ambiguous.

The pragmatic mitigation: declare the `Courier::Client` instance fully qualified once at file scope, then use `using namespace Courier;` for the rest of the file. No other Courier type collides with anything in `<Arduino.h>`.

```cpp
#include <Courier.h>

Courier::Client courier(makeConfig());   // qualified once

void setup() {
    using namespace Courier;             // safe everywhere below
    auto& mqtt = courier.addTransport<MqttTransport>("mqtt", mqttCfg);
    mqtt.subscribe("commands/#");
}
```

## `Courier::Config`

Configuration for the manager. Aggregate-style initialization or constructor.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `host` | `const char*` | `nullptr` | Server hostname for the built-in WebSocket transport. If `nullptr` or empty, the built-in WS is **not** registered. |
| `port` | `uint16_t` | `443` | Server port for the built-in WebSocket transport |
| `path` | `const char*` | `"/"` | Path on the server |
| `apName` | `const char*` | `nullptr` | WiFi captive portal AP name (defaults to a generated name if null) |
| `dns1` | `uint32_t` | `0` | Primary DNS server (`0` = use DHCP). Cast from `IPAddress`. |
| `dns2` | `uint32_t` | `0` | Secondary DNS server (`0` = none). Cast from `IPAddress`. |

```cpp
Courier::Config makeConfig() {
    Courier::Config cfg;
    cfg.host = "api.example.com";
    cfg.port = 443;
    cfg.path = "/ws";
    cfg.apName = "MyDevice";
    return cfg;
}
```

DNS:

```cpp
cfg.dns1 = (uint32_t)IPAddress(8, 8, 8, 8);
cfg.dns2 = (uint32_t)IPAddress(1, 1, 1, 1);
```

## `Courier::Client`

The manager class. One instance per process (WiFiManager requires a static C-style callback, so the singleton is enforced internally).

```cpp
Courier::Client courier(makeConfig());
```

### Lifecycle

```cpp
courier.setup();    // Initialize WiFi and start transports
courier.loop();     // Call from your main loop / a FreeRTOS task
```

`suspend()` and `resume()` tear transports down and bring them back — used to free SRAM / task stacks during OTA updates.

```cpp
courier.suspend();
// ... perform OTA ...
courier.resume();
```

### State

```cpp
courier.isConnected();     // true when all persistent transports are connected
courier.getState();        // Courier::State
courier.isTimeSynced();    // true after NTP or HTTP Date sync
```

## State machine

```cpp
enum class Courier::State {
    Booting,
    WifiConnecting,
    WifiConnected,
    WifiConfiguring,
    TransportsConnecting,
    Connected,
    Reconnecting,
    ConnectionFailed,
};
```

```
Booting -> WifiConnecting -> WifiConnected -> TransportsConnecting -> Connected
                                                       ^                  |
                                                  Reconnecting <----------+
                                                       |
                                               ConnectionFailed
```

`onConnectionChange` fires on every transition. `onError` fires alongside transitions caused by failures, with a category string (`"WIFI"`, `"TRANSPORT"`, `"TIME_SYNC"`, etc.) and a reason.

## Transports — registration and access

Client owns transports via `std::unique_ptr`. The registry is a fixed-size array (max 4 entries).

```cpp
template <typename T, typename... Args>
T& addTransport(const char* name, Args&&... args);

template <typename T>
T& transport(const char* name);   // asserts on miss

void removeTransport(const char* name);
```

`addTransport<T>(name, args...)` constructs a `T` in-place from `args...`, registers it, and returns a reference. Asserts if the name is already taken or the registry is full.

```cpp
auto& mqtt = courier.addTransport<Courier::MqttTransport>("mqtt", mqttCfg);
auto& udp  = courier.addTransport<Courier::UdpTransport>("udp");
```

`transport<T>(name)` looks up by name and returns `T&`. The returned type must match what was registered — ESP32 builds default to `-fno-rtti`, so this is a static cast with an `assert` on the name.

```cpp
courier.transport<Courier::WebSocketTransport>("ws").send(payload);
```

### The built-in `"ws"` transport

When `Config::host` is non-null and non-empty, Client auto-registers a `WebSocketTransport` under the name `"ws"`. Access it via `courier.transport<Courier::WebSocketTransport>("ws")`.

If `Config::host` is null or empty, no built-in is registered — the user is expected to add their own transports explicitly. WS-only stacks set `host`; non-WS stacks (MQTT-only, UDP-only) leave `host` null.

## `Client::onMessage` — JSON dispatch

```cpp
using MessageCallback = std::function<void(const char* type, JsonDocument& doc)>;
void onMessage(MessageCallback cb);
```

Fires when a text payload arrives on **any** registered transport and parses as JSON with a `"type"` field. The `type` argument is the JSON `"type"` field; the `doc` argument is the parsed document.

Text payloads that do not parse as JSON are silently dropped at the Client layer. Per-transport text/binary hooks still receive the raw bytes — `Client::onMessage` is purely the JSON convenience layer over the per-transport stream.

```cpp
courier.onMessage([](const char* type, JsonDocument& doc) {
    if (strcmp(type, "config") == 0) { /* ... */ }
});
```

## Connection events on Client

All single-slot — last registration wins.

```cpp
courier.onConnected([]() { /* all transports up */ });
courier.onDisconnected([]() { /* one or more transports down */ });
courier.onConnectionChange([](Courier::State state) { /* every transition */ });
courier.onError([](const char* category, const char* msg) { /* any failure */ });

// Lifecycle hooks. Use these for token exchange or registration that must
// complete before transports connect / right after they connect.
courier.onTransportsWillConnect([]() { /* before transports start */ });
courier.onTransportsDidConnect([]()  { /* after transports connect */ });
```

## WiFi configuration

```cpp
courier.setAPName("MyDevice");

courier.onConfigureWiFi([](WiFiManager& wm) {
    wm.setConnectTimeout(30);
    wm.setHostname("my-device");
});
```

The `onConfigureWiFi` callback fires before `WiFiManager::autoConnect()` — use it for timeouts, hostname, custom parameters, etc.

## `Courier::Transport` (base)

Abstract base. Subclass this only when implementing a custom transport (see [Custom transports](#custom-transports) below).

Pure-virtual surface:

```cpp
virtual void begin(const char* host, uint16_t port, const char* path) = 0;
virtual void disconnect() = 0;
virtual bool isConnected() const = 0;
virtual bool send(const char* payload) = 0;
virtual const char* name() const = 0;
```

Optional overrides (with sensible defaults):

| Method | Default | Description |
|--------|---------|-------------|
| `loop()` | `drainPending()` | Called every main-loop iteration |
| `sendBinary(data, len)` | returns `false` | Binary write |
| `publish(topic, payload)` | calls `send(payload)` | Topic-addressed write |
| `topicRequired()` | returns `false` | If true, publishes require a topic |
| `isPersistent()` | returns `true` | If false, excluded from failure escalation |
| `suspend()` / `resume()` | no-ops | OTA hook |

## `Courier::WebSocketTransport`

Wraps `esp_websocket_client`. Always implicitly available via `<Courier.h>`.

### `Config`

```cpp
Courier::WebSocketTransport::Config wsCfg;
wsCfg.cert_pem = MY_PEM;          // override the default bundle
wsCfg.use_default_certs = true;   // use Courier's built-in GTS Root R4
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `cert_pem` | `const char*` | `nullptr` | Specific CA cert PEM; overrides the built-in bundle when set |
| `use_default_certs` | `bool` | `true` | Use Courier's built-in GTS Root R4 certificate |

### Methods

```cpp
void begin(const char* host, uint16_t port, const char* path);
bool send(const char* payload);
bool sendBinary(const uint8_t* data, size_t len);

void onText  (TextCallback cb);     // (const char* payload, size_t len)
void onBinary(BinaryCallback cb);   // (const uint8_t* data, size_t len)

void onConfigure(ConfigureCallback cb);   // raw esp_websocket_client_config_t&
void useDefaultCerts();
```

`onText` fires for every text frame. **It also fires `Client::onMessage(type, doc)` when the payload parses as JSON** — the layers coexist. `onBinary` is the only path for binary frames; `Client::onMessage` does not see them.

```cpp
auto& ws = courier.transport<Courier::WebSocketTransport>("ws");

ws.onText([](const char* p, size_t l) { /* raw text */ });
ws.onBinary([](const uint8_t* d, size_t l) { /* binary frames */ });

ws.onConfigure([](esp_websocket_client_config_t& cfg) {
    cfg.headers = "Authorization: Bearer token\r\n";
    cfg.subprotocol = "graphql-ws";
});
```

## `Courier::MqttTransport`

Wraps `esp_mqtt_client`. Available via `<MqttTransport.h>`.

### `Config`

```cpp
Courier::MqttTransport::Config mqttCfg;
mqttCfg.topics   = {"sensors/+/data", "commands/me"};
mqttCfg.clientId = "my-device-001";
mqttCfg.cert_pem = MY_PEM;
mqttCfg.task_stack = 8192;
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `topics` | `std::vector<std::string>` | `{}` | Auto-subscribed on every (re)connect |
| `clientId` | `const char*` | `nullptr` | MQTT client ID (`nullptr` = ESP-IDF generates one) |
| `cert_pem` | `const char*` | `nullptr` | TLS certificate PEM |
| `task_stack` | `int` | `8192` | MQTT task stack size in bytes |

### Methods

```cpp
void begin(const char* host, uint16_t port, const char* path);

bool publish(const char* topic, const char* payload);                   // QoS 0, no retain
bool publish(const char* topic, const char* payload,
             int qos, bool retain);

void subscribe  (const char* topic, int qos = 0);
void unsubscribe(const char* topic);

void setClientId(const char* clientId);   // before begin()

void onMessage(TopicMessageCallback cb);  // (topic, payload, len)

void onConfigure(ConfigureCallback cb);   // raw esp_mqtt_client_config_t&
```

`subscribe` / `unsubscribe` mutate a managed topic list. Subscriptions are reapplied automatically on every (re)connect.

`onMessage(topic, payload, len)` fires for every incoming MQTT message. **`Client::onMessage(type, doc)` also fires** when the payload parses as JSON. For non-JSON or topic-routed code, use the per-transport hook.

There is no `MqttTransport::send()`. Every publish spells out the topic.

```cpp
auto& mqtt = courier.transport<Courier::MqttTransport>("mqtt");
mqtt.publish("sensors/me/data", payload);
mqtt.publish("alerts/critical", payload, /*qos=*/1, /*retain=*/true);

mqtt.onMessage([](const char* topic, const char* p, size_t len) {
    // topic-aware dispatch
});

mqtt.onConfigure([](esp_mqtt_client_config_t& cfg) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    cfg.credentials.username = "user";
    cfg.credentials.authentication.password = "pass";
    cfg.session.last_will.topic = "/status";
    cfg.session.last_will.msg = "offline";
#else
    cfg.username = "user";
    cfg.password = "pass";
#endif
});
```

## `Courier::UdpTransport`

Multicast UDP, wrapping `AsyncUDP`. Available via `<UdpTransport.h>`.

```cpp
auto& udp = courier.addTransport<Courier::UdpTransport>("udp");
udp.begin("239.1.2.3", 5000, "");   // group, port, path (ignored)
udp.send(R"({"type":"discover"})");
```

`begin(host, port, path)`: `host` is the multicast group address, `path` is ignored.

UDP is **non-persistent** — `isPersistent()` returns `false`, so it is excluded from failure escalation. A UDP transport going down does not trigger a WiFi reconnect.

Incoming packets are dispatched to `Client::onMessage` if they parse as JSON. There is no per-transport receive hook on `UdpTransport`.

## `Courier::Endpoint`

Per-transport endpoint override. Used internally by Client. End users typically set the endpoint by calling `transport.begin(host, port, path)` directly when ready (e.g. inside `onTransportsWillConnect`).

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `host` | `const char*` | `nullptr` | Host (`nullptr` = use Config::host) |
| `port` | `uint16_t` | `0` | Port (`0` = use Config::port) |
| `path` | `const char*` | `nullptr` | Path (`nullptr` = use Config::path) |

## Custom transports

Subclass `Courier::Transport`. Override the pure-virtuals and any optional hooks you need.

```cpp
#include <Transport.h>

class MyTransport : public Courier::Transport {
public:
    void begin(const char* host, uint16_t port, const char* path) override;
    void disconnect() override;
    bool isConnected() const override;
    bool send(const char* payload) override;
    const char* name() const override { return "MyTransport"; }
};
```

The base class provides an SPSC queue (`Courier::SpscQueue<PendingMessage, 8>`) plus drain logic. From your event handler — which may run on a different FreeRTOS task — call:

```cpp
queueIncomingMessage(payload, len);     // text
queueIncomingBinary(data, len);         // binary
queueConnectionChange(connected);       // connection state change
queueTransportFailed();                 // unrecoverable failure
```

These are drained on the main task by `loop()` (default implementation calls `drainPending()`). Override `loop()` only if you need topic-aware dispatch or other custom drain behaviour — call `drainSignals()` at the end so connection-state and failure flags still fire.

## Memory ordering / threading model

- Each transport's event handler typically runs on a transport-owned FreeRTOS task.
- `Client::loop()` runs on the caller's task (typically Arduino's `loop()`, or a dedicated task in ESP-IDF).
- The `SpscQueue<T, N>` in the transport base handles handoff: single-producer (transport task) / single-consumer (main task), lock-free, acquire/release on indices. Identical semantics on host and ESP32 — no `#ifdef ESP_PLATFORM` in the queue.
- Capacity is 8 messages by default. Bursts smaller than that are absorbed; sustained overload drops the oldest in-flight push.

## C++ standard

Library is C++17. PlatformIO's `espressif32@6.x` defaults to `gnu++17`; ESP-IDF v5.x defaults to `gnu++23`. Either is sufficient.

C++20 `using enum Courier::State;` works for consumers who opt in but is not required:

```cpp
// C++17
courier.onConnectionChange([](Courier::State s) {
    using S = Courier::State;
    if (s == S::Reconnecting) { /* ... */ }
});

// C++20 opt-in
courier.onConnectionChange([](Courier::State s) {
    using enum Courier::State;
    if (s == Reconnecting) { /* ... */ }
});
```

## Limits

| Constant | Value | Description |
|----------|-------|-------------|
| `MAX_TRANSPORTS` | 4 | Maximum registered transports |
| `MIN_RECONNECT_INTERVAL` | 5000 ms | Initial backoff delay |
| `MAX_RECONNECT_INTERVAL` | 60000 ms | Maximum backoff delay |
| `MAX_RECONNECT_ATTEMPTS` | 10 | Hard limit before `State::ConnectionFailed` |
| Per-transport queue depth | 8 | SPSC FIFO capacity for incoming frames |
| Self-heal timeout | 60000 ms | Per-transport disconnect window before failure is reported |
