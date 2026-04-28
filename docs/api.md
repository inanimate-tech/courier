# Courier API Reference

Authoritative reference for the 0.4.0 API. For a tutorial-style introduction see [README.md](../README.md). For migration from 0.3.x see [migration-0.3-to-0.4.md](migration-0.3-to-0.4.md).

## Header includes

| Header | Provides |
|--------|----------|
| `<Courier.h>` | `Courier::Client`, `Courier::Config`, `Courier::State`, `Courier::Endpoint`, and the WebSocket transport (always pulled in) |
| `<MqttTransport.h>` | `Courier::MqttTransport` |
| `<UdpTransport.h>` | `Courier::UdpTransport` |
| `<WebSocketTransport.h>` | already included by `Courier.h`; include directly only if you reference the type without the manager |
| `<Transport.h>` | `Courier::Transport` base class and `Courier::SendOptions` — needed only when subclassing |
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
| `defaultTransport` | `const char*` | `nullptr` | Name of the transport used by `Client::send`. `nullptr` disables `Client::send`. |
| `dns1` | `uint32_t` | `0` | Primary DNS server (`0` = use DHCP). Cast from `IPAddress`. |
| `dns2` | `uint32_t` | `0` | Secondary DNS server (`0` = none). Cast from `IPAddress`. |

```cpp
Courier::Config makeConfig() {
    Courier::Config cfg;
    cfg.host = "api.example.com";
    cfg.port = 443;
    cfg.path = "/ws";
    cfg.apName = "MyDevice";
    cfg.defaultTransport = "ws";   // enable Client::send
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

### Sending via Client (default transport)

`Client::send` routes to whichever transport is named by `Config::defaultTransport` (or the runtime override set by `setDefaultTransport`). Returns `false` if no default transport is configured or if the transport's `send` returns false.

```cpp
bool send(JsonDocument& doc);
bool send(JsonDocument& doc, const Courier::SendOptions& options);
void setDefaultTransport(const char* name);
```

```cpp
// Config-time default:
cfg.defaultTransport = "ws";
Courier::Client courier(cfg);

JsonDocument doc;
doc["type"] = "hello";
courier.send(doc);                       // routes to "ws"

// MQTT with per-call topic:
cfg.defaultTransport = "mqtt";
// ...
Courier::SendOptions opts;
opts.topic = "sensors/me";
courier.send(doc, opts);

// Runtime override:
courier.setDefaultTransport("secondary-ws");
```

There is no `sendTo` — for explicit per-call transport routing, drop down to `transport<T>(name).send(...)` or the transport-specific methods (`sendText`, `sendBinary`, `publish`).

## `Courier::SendOptions`

Per-call send parameters. Defined in `<Transport.h>`.

```cpp
struct Courier::SendOptions {
    const char* topic  = nullptr;  // required for MqttTransport::send; ignored by WS and UDP
    int         qos    = 0;        // MQTT QoS level (0/1/2); ignored by WS and UDP
    bool        retain = false;    // MQTT retain flag; ignored by WS and UDP
};
```

Which fields each transport honours:

| Field | WebSocketTransport | MqttTransport | UdpTransport |
|-------|--------------------|---------------|--------------|
| `topic` | ignored | **required** | ignored |
| `qos` | ignored | honoured | ignored |
| `retain` | ignored | honoured | ignored |

```cpp
// MQTT publish via Client::send:
Courier::SendOptions opts;
opts.topic  = "alerts/critical";
opts.qos    = 1;
opts.retain = true;
courier.send(doc, opts);
```

## `Client::onMessage` — JSON dispatch

```cpp
using MessageCallback = std::function<void(const char* transportName,
                                           const char* type,
                                           JsonDocument& doc)>;
void onMessage(MessageCallback cb);
```

Fires when a text payload arrives on **any** registered transport and parses as JSON with a `"type"` field. `transportName` is the registered name of the transport that received the message (e.g. `"ws"`, `"mqtt"`). `type` is the JSON `"type"` field. `doc` is the parsed document.

The `transportName` argument is useful when the same logical message type can arrive on multiple transports (e.g. a device that connects over both WS and MQTT):

```cpp
courier.onMessage([](const char* transportName, const char* type, JsonDocument& doc) {
    if (strcmp(type, "config") == 0) {
        // know which transport delivered this
        Serial.printf("config via %s\n", transportName);
    }
});
```

Text payloads that do not parse as JSON are silently dropped at the Client layer. Per-transport text/binary hooks still receive the raw bytes — `Client::onMessage` is purely the JSON convenience layer over the per-transport stream.

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
virtual bool send(JsonDocument& doc, const SendOptions& options = {}) = 0;
virtual const char* name() const = 0;
```

Optional overrides (with sensible defaults):

| Method | Default | Description |
|--------|---------|-------------|
| `loop()` | `drainPending()` | Called every main-loop iteration |
| `isPersistent()` | returns `true` | If false, excluded from failure escalation |
| `suspend()` / `resume()` | no-ops | OTA hook |

Note: `sendBinary` and `publish` are **not** on the base. `WebSocketTransport::sendBinary` is WS-specific. `MqttTransport::publish` is MQTT-specific. Custom transports are not required to implement either.

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
courier.transport<Courier::WebSocketTransport>("ws").sendText(payload);
```

### The built-in `"ws"` transport

When `Config::host` is non-null and non-empty, Client auto-registers a `WebSocketTransport` under the name `"ws"`. Access it via `courier.transport<Courier::WebSocketTransport>("ws")`.

If `Config::host` is null or empty, no built-in is registered — the user is expected to add their own transports explicitly. WS-only stacks set `host`; non-WS stacks (MQTT-only, UDP-only) leave `host` null.

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

// JSON send (base virtual override) — serializes doc and calls sendText:
bool send(JsonDocument& doc, const SendOptions& options = {});

// WS-specific raw sends:
bool sendText  (const char* payload);
bool sendBinary(const uint8_t* data, size_t len);

// Receive hooks:
void onText  (TextCallback cb);     // (const char* payload, size_t len)
void onBinary(BinaryCallback cb);   // (const uint8_t* data, size_t len)

// Advanced:
void onConfigure(ConfigureCallback cb);   // raw esp_websocket_client_config_t&
void useDefaultCerts();
```

`send(doc, opts)` serializes the document to JSON and delivers it as a text frame via `sendText`. `opts` fields (`topic`, `qos`, `retain`) are all ignored by the WS transport.

`sendText` sends a raw text frame without any serialization. Use it when you already have a JSON string, or for non-JSON text frames.

`sendBinary` is WS-specific — it is **not** on the `Transport` base. Use it for binary frame delivery (audio, packed sensor data, etc.).

`onText` fires for every text frame. **It also fires `Client::onMessage(transportName, type, doc)` when the payload parses as JSON** — the layers coexist. `onBinary` is the only path for binary frames; `Client::onMessage` does not see them.

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

// JSON send (base virtual override) — requires opts.topic; serializes and publishes:
bool send(JsonDocument& doc, const SendOptions& options = {});

// Raw text publish:
bool publish(const char* topic, const char* payload, int qos = 0, bool retain = false);

// JSON publish sugar — serializes doc and publishes:
bool publish(const char* topic, JsonDocument& doc, int qos = 0, bool retain = false);

void subscribe  (const char* topic, int qos = 0);
void unsubscribe(const char* topic);

void setClientId(const char* clientId);   // before begin()

void onMessage(TopicMessageCallback cb);  // (topic, payload, len)

void onConfigure(ConfigureCallback cb);   // raw esp_mqtt_client_config_t&
```

`send(doc, opts)` requires `opts.topic`. It serializes the document and publishes it via the raw `publish` overload, using `opts.qos` and `opts.retain`. Returns `false` if `opts.topic` is null.

`publish(topic, JsonDocument&, qos, retain)` is a JSON convenience overload added in 0.4.0 — it serializes `doc` and forwards to the raw text `publish`.

`subscribe` / `unsubscribe` mutate a managed topic list. Subscriptions are reapplied automatically on every (re)connect.

`onMessage(topic, payload, len)` fires for every incoming MQTT message. **`Client::onMessage(transportName, type, doc)` also fires** when the payload parses as JSON. For non-JSON or topic-routed code, use the per-transport hook.

```cpp
auto& mqtt = courier.transport<Courier::MqttTransport>("mqtt");

// JSON via Client::send (when defaultTransport = "mqtt"):
Courier::SendOptions opts;
opts.topic = "sensors/me/data";
courier.send(doc, opts);

// Direct publish:
mqtt.publish("sensors/me/data", payload);
mqtt.publish("alerts/critical", payload, /*qos=*/1, /*retain=*/true);

// JSON publish sugar:
mqtt.publish("sensors/me/data", doc);

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

JsonDocument doc;
doc["type"] = "discover";
udp.send(doc);
```

`begin(host, port, path)`: `host` is the multicast group address, `path` is ignored.

`send(doc, opts)` serializes the document to JSON and broadcasts it to the multicast group. `opts` fields are all ignored by the UDP transport.

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
    bool send(JsonDocument& doc, const Courier::SendOptions& options = {}) override;
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

Subclasses MAY also override `loop()`, `suspend()`, `resume()`, and `isPersistent()`.

There is no requirement to implement `sendBinary` or `publish` — those are transport-specific extensions on `WebSocketTransport` and `MqttTransport` respectively.

## Memory ordering / threading model

- Each transport's event handler typically runs on a transport-owned FreeRTOS task.
- `Client::loop()` runs on the caller's task (typically Arduino's `loop()`, or a dedicated task in ESP-IDF).
- The `SpscQueue<T, N>` in the transport base handles handoff: single-producer (transport task) / single-consumer (main task), lock-free, acquire/release on indices. Identical semantics on host and ESP32 — no `#ifdef ESP_PLATFORM` in the queue.
- Capacity is 8 messages by default. Bursts smaller than that are absorbed; sustained overload drops the oldest in-flight push.
- `send(doc, opts)` is called from the consumer's task (the task calling `Client::loop()` or calling `transport<T>(name).send(...)` directly).

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
