# Courier API Reference

Authoritative reference for the 0.5.x API. For a tutorial-style introduction see [README.md](../README.md). For migration from 0.3.x see [migration-0.3-to-0.4.md](migration-0.3-to-0.4.md).

## Header includes

| Header | Provides |
|--------|----------|
| `<Courier.h>` | `Courier::Client`, `Courier::Config`, `Courier::State`, `Courier::Endpoint`, and the WebSocket transport (always pulled in) |
| `<MqttTransport.h>` | `Courier::MqttTransport` |
| `<UdpTransport.h>` | `Courier::UdpTransport` |
| `<HttpTransport.h>` | `Courier::HttpTransport`, `Courier::Response`, and the `Courier::Http::Err*` sentinels |
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

`reconnect()` is a manual trigger for the connection-recovery state machine. It tears down all transports, fires `onDisconnected`, and transitions to `State::Reconnecting`. The state-machine handler then adapts: if WiFi is down it re-runs the WiFi connect step; if WiFi is fine it retries transports directly. `onTransportsWillConnect` re-fires on the way back through `TransportsConnecting`.

```cpp
courier.reconnect();   // re-registration / "kick the connection"
```

Use it for re-registration flows, manual reconnect triggers, or recovering from application-level state errors that warrant a fresh connect cycle.

### State

```cpp
courier.isConnected();     // true when at least one registered transport is connected
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

| Field | WebSocketTransport | MqttTransport | UdpTransport | HttpTransport |
|-------|--------------------|---------------|--------------|---------------|
| `topic` | ignored | **required** | ignored | ignored |
| `qos` | ignored | honoured | ignored | ignored |
| `retain` | ignored | honoured | ignored | ignored |

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
virtual void begin() = 0;                                // zero-arg — read host/port/path from base
virtual void disconnect() = 0;
virtual bool isConnected() const = 0;
virtual bool send(JsonDocument& doc, const SendOptions& options = {}) = 0;
virtual const char* name() const = 0;
```

Endpoint state lives on the transport itself:

```cpp
// Default impl copies the strings into protected base members
// _host (std::string), _port (uint16_t), _path (std::string).
virtual void setEndpoint(const char* host, uint16_t port, const char* path);
void setEndpoint(const Courier::Endpoint& ep);   // sugar
```

`Client::addTransport<T>(name, args...)` seeds the new transport's endpoint from `Config::host/port/path` immediately on registration. Override before `begin()` runs (e.g. inside `onTransportsWillConnect`) when a transport needs a different endpoint:

```cpp
courier.onTransportsWillConnect([&]() {
    auto& mqtt = courier.transport<Courier::MqttTransport>("mqtt");
    mqtt.setEndpoint(brokerHost.c_str(), brokerPort, "/mqtt");
});
```

A non-virtual 3-arg sugar overload — `void begin(const char* host, uint16_t port, const char* path)` — is provided on the base. It calls `setEndpoint(...)` then `begin()`. Existing call sites using the 3-arg form continue to work unchanged.

Optional overrides (with sensible defaults):

| Method | Default | Description |
|--------|---------|-------------|
| `loop()` | `drainPending()` | Called every main-loop iteration |
| `isPersistent()` | returns `true` | If false, excluded from failure escalation |
| `suspend()` / `resume()` | no-ops | OTA hook |
| `setEndpoint(host, port, path)` | copies into `_host`/`_port`/`_path` | Override only if you need custom endpoint validation |

Note: `sendBinary` and `publish` are **not** on the base. `WebSocketTransport::sendBinary` is WS-specific. `MqttTransport::publish` / `publishBinary` are MQTT-specific. Custom transports are not required to implement either.

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
wsCfg.cert_pem = MY_PEM;          // pin a CA — overrides the bundle
wsCfg.use_cert_bundle = false;    // opt out of the IDF cert bundle
wsCfg.use_default_certs = true;   // fall back to the embedded GTS Root R4
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `cert_pem` | `const char*` | `nullptr` | Pin a specific CA cert (PEM); overrides the bundle when set |
| `use_cert_bundle` | `bool` | `true` | Validate against the IDF certificate bundle (`esp_crt_bundle_attach`) |
| `use_default_certs` | `bool` | `true` | Fall back to Courier's embedded GTS Root R4 when the bundle is disabled |

TLS precedence: `cert_pem` (pin) → `use_cert_bundle` (default) → `use_default_certs` (embedded GTS Root R4, the legacy fallback for builds without `MBEDTLS_CERTIFICATE_BUNDLE`) → nothing. `useDefaultCerts()` is the runtime equivalent of setting `use_cert_bundle = false` — it disables the bundle and selects the embedded root.

### Methods

```cpp
// Endpoint seeding happens automatically on addTransport<T> from Config.
// Override before begin() runs:
void setEndpoint(const char* host, uint16_t port, const char* path);

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
mqttCfg.binaryTopics = {"devices/me/audio"};   // opaque bytes, never JSON-parsed
mqttCfg.clientId = "my-device-001";
mqttCfg.cert_pem = MY_PEM;         // pin a CA — overrides the bundle
mqttCfg.task_stack = 8192;
mqttCfg.out_buffer_size = 2048;    // 0 = IDF default (1024)
mqttCfg.network_timeout_ms = 5000; // 0 = IDF default (10000)
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `topics` | `std::vector<std::string>` | `{}` | Auto-subscribed on every (re)connect |
| `binaryTopics` | `std::vector<std::string>` | `{}` | As `topics`, but payloads are delivered to `onBinary` and never enter the text/JSON lane |
| `clientId` | `const char*` | `nullptr` | MQTT client ID (`nullptr` = ESP-IDF generates one) |
| `cert_pem` | `const char*` | `nullptr` | Pin a specific CA cert (PEM); overrides the bundle when set |
| `use_cert_bundle` | `bool` | `true` | Validate against the IDF certificate bundle (`esp_crt_bundle_attach`) |
| `task_stack` | `int` | `8192` | MQTT task stack size in bytes |
| `out_buffer_size` | `int` | `0` | Outbound buffer in bytes; `0` leaves the IDF default (1024) |
| `network_timeout_ms` | `int` | `0` | Network operation timeout; `0` leaves the IDF default (10000) |

TLS precedence: `cert_pem` (pin) → `use_cert_bundle` (default; requires `MBEDTLS_CERTIFICATE_BUNDLE`) → nothing.

### Methods

```cpp
// Endpoint seeding happens automatically on addTransport<T> from Config.
// Override before begin() runs:
void setEndpoint(const char* host, uint16_t port, const char* path);

// JSON send (base virtual override) — requires opts.topic; serializes and publishes:
bool send(JsonDocument& doc, const SendOptions& options = {});

// Raw text publish — length is strlen(payload):
bool publish(const char* topic, const char* payload, int qos = 0, bool retain = false);

// Opaque byte publish — explicit length, NUL-safe:
bool publishBinary(const char* topic, const uint8_t* data, size_t len,
                   int qos = 0, bool retain = false);

// JSON publish sugar — serializes doc and publishes:
bool publish(const char* topic, JsonDocument& doc, int qos = 0, bool retain = false);

void subscribe      (const char* topic, int qos = 0);
void subscribeBinary(const char* topic, int qos = 0);
void unsubscribe    (const char* topic);

static bool topicMatches(const char* filter, const char* topic);  // '+' and '#'

void setClientId(const char* clientId);   // before begin()

void onMessage(TopicMessageCallback cb);  // (topic, payload, len)
void onBinary (TopicBinaryCallback cb);   // (topic, data, len)
void onError  (ErrorCallback cb);         // (const ErrorInfo&)

void onConfigure(ConfigureCallback cb);   // raw esp_mqtt_client_config_t&
```

`send(doc, opts)` requires `opts.topic`. It serializes the document and publishes it via the raw `publish` overload, using `opts.qos` and `opts.retain`. Returns `false` if `opts.topic` is null.

`publish(topic, JsonDocument&, qos, retain)` is a JSON convenience overload added in 0.4.0 — it serializes `doc` and forwards to the raw text `publish`.

`subscribe` / `subscribeBinary` / `unsubscribe` mutate a managed topic list. Subscriptions are reapplied automatically on every (re)connect, at the QoS they were registered with.

### Binary payloads

`publish(topic, const char*)` takes its length from `strlen`. That is not a style choice you can ignore: `esp_mqtt_client_publish` treats a zero length as "call `strlen(data)`", so handing it a buffer that is not NUL-terminated reads past the end of the allocation. PCM audio, images and protobuf all contain NUL bytes.

Use `publishBinary` for anything that is not a C string. It passes a real length, and it rejects `data == nullptr`, `len == 0` and `len > INT_MAX` rather than letting the `strlen` branch fire. A genuinely empty payload — clearing a retained topic, say — is a text publish: `publish(topic, "", qos, retain)`.

On the receive side, MQTT 3.1.1 carries no content-type on the wire, so the subscriber declares which topics are opaque:

```cpp
mqtt.subscribeBinary("devices/me/audio");

mqtt.onBinary([](const char* topic, const uint8_t* data, size_t len) {
    // No NUL-termination contract. `len` is authoritative.
});
```

A payload arriving on a filter registered via `subscribeBinary` (or `Config::binaryTopics`) goes to `onBinary` and **never** to `onMessage` or to `Client`'s JSON lane — so `Client::onMessage` does not attempt to parse it even when MQTT is the default transport. Matching uses `topicMatches`, so wildcard filters (`devices/+/audio`) route correctly for the concrete topic the broker delivers. Registering the same filter with both `subscribe` and `subscribeBinary` is allowed; the last call decides the lane.

### Publishing from a second task

`publish` / `publishBinary` are safe to call from a task other than the one running `loop()`.

esp-mqtt is already thread-safe for concurrent calls: `esp_mqtt_client_publish`, `subscribe`, `unsubscribe`, `start` and `stop` each take an internal API lock. What it does not protect is the handle's lifetime — `esp_mqtt_client_destroy` takes no lock and frees the client outright. So Courier holds its own lock across `begin`, `disconnect`, `suspend` and `resume`, and the publish path takes it with a 250 ms bound (`MqttTransport::PUBLISH_LOCK_TIMEOUT_MS`), returning `false` rather than waiting out a teardown. A realtime sender treats that `false` as "defer, retry next tick". Without it, a reconnect escalation on the app task can free the client underneath an in-flight publish on a send task.

What the bound does **not** cover is a socket write already in progress inside ESP-IDF. IDF's API lock has no timeout, and the IDF client task holds it across a reconnect — transport connect, TLS handshake and CONNACK wait, each bounded by `network_timeout_ms` (default 10 s). A lone publisher entering a busy client still waits on IDF; the 250 ms bound is what stops a *second* caller piling up behind it or behind a teardown. Lower `network_timeout_ms` to shorten that worst case, remembering it also bounds the handshake and that hitting it aborts the connection.

`out_buffer_size` is worth setting if you publish a fixed frame size. A payload larger than the outbound buffer is written in chunks — a 1920-byte audio frame against the default 1024 is two writes and two TLS records. Sizing the buffer past the frame makes it one, for the cost of the extra internal RAM.

**Sized for control-rate traffic.** The inbound path is a depth-8 SPSC queue with a heap-allocated topic per message, drained on the app task at `loop()` cadence. That is right for commands, status and occasional blobs. It is *not* sized for a sustained high-rate stream — a 16.7 fps audio downlink overflows it within half a second of a slow `loop()`, and the drops are silent apart from a log line. Delivering a stream like that would need a different inbound path (dispatch on the MQTT task rather than through the app-task queue); treat this as a known boundary, not a bug to be filed.

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

// Bytes, with a real length:
mqtt.publishBinary("devices/me/audio", frame, frameLen);

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

#### `onError` — structured connection failures

`onError` fires for every `MQTT_EVENT_ERROR`, carrying the detail ESP-IDF
reports rather than the bare log line Courier used to emit:

```cpp
struct ErrorInfo {
    esp_mqtt_error_type_t          type;                // NONE / TCP_TRANSPORT / CONNECTION_REFUSED
    esp_mqtt_connect_return_code_t connectReturnCode;   // valid when type == CONNECTION_REFUSED
    esp_err_t tlsLastEspErr;                            // valid when type == TCP_TRANSPORT
    int       tlsStackErr;
    int       tlsCertVerifyFlags;
    int       sockErrno;

    bool isConnectionRefused() const;
    bool isNotAuthorized() const;    // CONNACK 5
    const char* describe() const;    // static string, never null
};
```

The callback runs on the app task at `loop()` cadence with no transport lock
held. It **may** call `disconnect()`/`begin()` re-entrantly. It **must not**
block: it runs inside `Client::loop()`, so a blocking HTTPS call here stalls
the WiFi health check, every other transport's drain, and the state machine.
Do not call `onError()` again from inside the callback — re-registering would
assign to the `std::function` while its target is executing, which is
undefined behaviour; the callback may reconnect, but it must not re-register
itself.

This is distinct from `Client::onError(category, message)`, which reports
client-level state transitions as strings. `MqttTransport::onError` reports
MQTT protocol detail as a struct.

**A refusal is terminal until configuration changes.** `isNotAuthorized()`
means the broker accepted the packet and rejected this client's authorization
— distinct from bad credentials (code 4) or a rejected client ID (code 2).
Retrying with the same identity fails identically, indefinitely. Courier keeps
retrying regardless; acting on it is the application's job.

**Rescuing a refused connection.** Record intent in the callback, act outside
it:

```cpp
auto& mqtt = courier.transport<Courier::MqttTransport>("mqtt");

mqtt.onError([&](const Courier::MqttTransport::ErrorInfo& err) {
    if (!err.isNotAuthorized()) return;   // TLS/socket: let self-healing work
    needsReRegistration = true;           // record only — never block here
});

// ...in loop(), outside the callback:
if (needsReRegistration) {
    needsReRegistration = false;
    if (++rescueAttempts > kMaxRescues) { surfaceToUser(); return; }

    Identity id = registerWithPlatform();   // blocking HTTPS is fine here
    if (id.valid()) {
        mqtt.unsubscribe(oldTopic.c_str());
        mqtt.setClientId(id.clientId.c_str());
        mqtt.subscribe(id.topic.c_str());
        oldTopic = id.topic;
        mqtt.begin();          // destroys + rebuilds internally
    }
}
```

`begin()` rebuilds the IDF client from the transport's current values, and the
managed topic list survives teardown and is re-applied on connect — so a
mutated identity sticks, whether you call `begin()` yourself or let Courier's
reconnection ladder do it.

Do **not** call `Client::reconnect()` here: it tears down all transports and
forces a full WiFi reconnect, which is the wrong tool when the network is
healthy. Do **not** call `disconnect()` before `begin()` — `begin()` destroys
first. Bound your retries; a rescue that keeps being refused will exhaust
Courier's reconnect budget and reach the terminal `ConnectionFailed` state.

`disconnect()` does not drain the error queue: an error queued just before a
user-initiated teardown is still delivered on the next `loop()`, potentially
after a subsequent `begin()`. It reports the error that genuinely happened —
just don't mistake it for one from the new session.

## `Courier::UdpTransport`

Multicast UDP, wrapping `AsyncUDP`. Available via `<UdpTransport.h>`.

```cpp
auto& udp = courier.addTransport<Courier::UdpTransport>("udp");
udp.setEndpoint("239.1.2.3", 5000, "");   // group, port, path (ignored)

JsonDocument doc;
doc["type"] = "discover";
udp.send(doc);
```

`host` (via `setEndpoint`) is the multicast group address; `path` is ignored. UDP uses the endpoint set on the transport — typically a multicast group, not the same host as `Config::host` — so call `setEndpoint(...)` before `begin()` runs (e.g. inside `onTransportsWillConnect`, or before `setup()`).

`send(doc, opts)` serializes the document to JSON and broadcasts it to the multicast group. `opts` fields are all ignored by the UDP transport.

UDP is **non-persistent** — `isPersistent()` returns `false`, so it is excluded from failure escalation. A UDP transport going down does not trigger a WiFi reconnect.

Incoming packets are dispatched to `Client::onMessage` if they parse as JSON. There is no per-transport receive hook on `UdpTransport`.

## `Courier::HttpTransport`

HTTPS, wrapping `esp_http_client`. Available via `<HttpTransport.h>`. A blocking, JS-shaped `fetch()` that is also a full transport citizen: `send(doc)` POSTs JSON to the `Config`-seeded endpoint, and JSON replies dispatch through `Client::onMessage`.

Appliance posture: a fresh client per request, full teardown after — no keep-alive, no session state survives between fetches. Transport-failure retries flush the lwIP DNS cache between attempts (anycast re-roll). **Non-persistent** — `isPersistent()` returns `false`, so it is excluded from failure escalation.

`fetch()` is **blocking**. Call it from `loop()`-dispatched callbacks (single task); a long fetch stalls other transports' draining for its duration.

### `Config`

```cpp
Courier::HttpTransport::Config httpCfg;
httpCfg.cert_pem = MY_PEM;          // pin a CA — overrides the bundle
httpCfg.maxResponseBytes = 32 * 1024;
httpCfg.timeoutMs = 10000;
httpCfg.retries = 3;
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `cert_pem` | `const char*` | `nullptr` | Pin a specific CA cert (PEM); overrides the bundle when set |
| `use_cert_bundle` | `bool` | `true` | Validate against the IDF certificate bundle (`esp_crt_bundle_attach`) |
| `maxResponseBytes` | `size_t` | `16384` | Buffered-body cap; a larger body fails with `Http::ErrTooLarge` |
| `timeoutMs` | `uint32_t` | `10000` | Per-request timeout (overridable per call) |
| `retries` | `uint8_t` | `3` | Transport-failure retries (overridable per call; not applied to real HTTP statuses) |

TLS precedence: `cert_pem` (pin) → `use_cert_bundle` (default) → nothing. There is no insecure mode.

### Methods

```cpp
// Endpoint seeding happens automatically on addTransport<T> from Config.
void setEndpoint(const char* host, uint16_t port, const char* path);

// Blocking fetch — returns a Response value type:
Response fetch(const char* url);
Response fetch(const char* url, const FetchOptions& opts);
Response get(const char* url);                          // sugar for fetch(url)
Response postJson(const char* url, JsonDocument& doc);  // POST doc as JSON

// JSON send (base virtual override) — POSTs to the Config-seeded endpoint;
// a JSON reply dispatches through Client::onMessage:
bool send(JsonDocument& doc, const SendOptions& options = {});

// Per-transport receive hook for send() responses (like WS onText):
void onMessage(MessageCallback cb);

// Advanced:
void onConfigure(ConfigureCallback cb);   // raw esp_http_client_config_t&
```

### `Response`

Move-only value type returned by `fetch()`. Owns its buffered body (PSRAM-preferred heap).

| Member | Description |
|--------|-------------|
| `int status` | HTTP status, or a `Http::Err*` sentinel below `100` for transport failures |
| `long contentLength` | Server-declared length, or `-1` if unstated |
| `bool ok()` | `true` for a 2xx status |
| `bool reachedServer()` | `true` when `status >= 100` (a real HTTP status, not a transport error) |
| `bool complete()` | `false` when the connection dropped before the promised body fully arrived |
| `const char* text()` | Buffered body (NUL-terminated; `""` in streaming mode) |
| `size_t size()` | Body byte length |
| `bool json(JsonDocument& doc)` | Parse the body as JSON; returns `false` on empty body or parse error |
| `const char* header(const char* name)` | Collected header (`Content-Type`, `Content-Length`, `Date`); `nullptr` if absent |

Transport-failure sentinels (in `namespace Courier::Http`): `ErrNoWifi` (`-1000`), `ErrDns` (`-1001`, reserved), `ErrConnect` (`-1002`), `ErrTimeout` (`-1003`), `ErrTooLarge` (`-1004`), `ErrAborted` (`-1005`). Retries apply only to these; any real HTTP status is returned as-is.

### `FetchOptions`

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `method` | `const char*` | `nullptr` | Defaults to `"GET"`, or `"POST"` when a body/json is set |
| `headers` / `headerCount` | `const Header*` / `size_t` | `nullptr` / `0` | Caller-owned array of `{name, value}` request headers |
| `body` / `bodyLength` | `const char*` / `size_t` | `nullptr` / `0` | Raw request body (`bodyLength = 0` → `strlen(body)`) |
| `json` | `JsonDocument*` | `nullptr` | Serialize as body with a JSON content-type |
| `timeoutMs` | `int32_t` | `-1` | `-1` inherits `Config::timeoutMs` |
| `retries` | `int16_t` | `-1` | `-1` inherits `Config::retries`; `0` disables retries |
| `onResponse` | `ResponseCallback` | — | Streaming: fires once with `(status, contentLength)` before body chunks |
| `onBody` | `BodyCallback` | — | Streaming: `(data, len)` per chunk; return `false` to abort (OTA-download shaped) |
| `configure` | `ConfigureCallback` | — | Per-call raw-config trapdoor (`esp_http_client_config_t&`) |

Setting `onBody` switches to streaming mode: the `Response` carries status and headers but no buffered body, and chunks arrive via the callback (abortable). Otherwise the body is buffered up to `maxResponseBytes`.

```cpp
auto& http = courier.addTransport<Courier::HttpTransport>("https");

// Buffered GET:
Courier::Response r = http.get("https://httpbin.org/get");
if (r.ok()) {
    JsonDocument doc;
    if (r.json(doc)) { /* ... */ }
}

// POST JSON:
JsonDocument body;
body["type"] = "reading";
Courier::Response p = http.postJson("https://example.com/ingest", body);

// Streaming download (abortable):
Courier::HttpTransport::FetchOptions opts;
opts.onResponse = [](int status, long len) { /* size the sink */ };
opts.onBody = [](const uint8_t* data, size_t len) {
    // write chunk; return false to abort
    return true;
};
http.fetch("https://example.com/firmware.bin", opts);

http.onConfigure([](esp_http_client_config_t& cfg) {
    cfg.buffer_size = 4096;   // event_handler / user_data are reserved
});
```

`send(doc)` POSTs the serialized document to the `Config`-seeded endpoint (`host`/`port`/`path`, e.g. `defaultTransport = "https"`). A JSON reply also fires `Client::onMessage(transportName, type, doc)` — but only when the response is `complete()`; a truncated 2xx reply still reports `ok()` yet never reaches the hook. See the README's HTTPS section and `examples/https-only` for full sketches.

## `Courier::Endpoint`

Plain value type for host/port/path. Accepted by `Transport::setEndpoint(const Endpoint&)` as sugar for the 3-argument `setEndpoint(host, port, path)` form.

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `host` | `const char*` | `nullptr` | Host |
| `port` | `uint16_t` | `0` | Port |
| `path` | `const char*` | `nullptr` | Path |

```cpp
Courier::Endpoint ep{"broker.example.com", 8883, "/mqtt"};
courier.transport<Courier::MqttTransport>("mqtt").setEndpoint(ep);
```

`setEndpoint` copies the strings into the transport — passing pointers from a temporary (e.g. `String::c_str()`) is safe.

## Custom transports

Subclass `Courier::Transport`. Override the pure-virtuals and any optional hooks you need.

```cpp
#include <Transport.h>

class MyTransport : public Courier::Transport {
public:
    using Transport::begin;   // unhide the 3-arg sugar overload
    void begin() override;    // read endpoint from _host / _port / _path
    void disconnect() override;
    bool isConnected() const override;
    bool send(JsonDocument& doc, const Courier::SendOptions& options = {}) override;
    const char* name() const override { return "MyTransport"; }
};

void MyTransport::begin() {
    // _host, _port, _path are seeded by Client::addTransport<T> from Config,
    // and may have been overridden by the user via setEndpoint(...).
    connectTo(_host.c_str(), _port, _path.c_str());
}
```

The `using Transport::begin;` line unhides the inherited 3-arg sugar overload — without it, the derived `begin()` declaration name-hides the base's `begin(host, port, path)`.

`setEndpoint(host, port, path)` is virtual but the default implementation (copy into `_host`/`_port`/`_path`) is what most subclasses want. Override only if you need custom endpoint validation or transformation.

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
