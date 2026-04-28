# Migrating from Courier 0.3.x to 0.4.0

0.4.0 is a coordinated breaking release. This guide walks through every change you'll see in your code.

## Header includes

| Old | New |
|---|---|
| `#include <Courier.h>` | unchanged |
| `#include <CourierTransport.h>` | `#include <Transport.h>` |
| `#include <CourierWSTransport.h>` | `#include <WebSocketTransport.h>` |
| `#include <CourierMqttTransport.h>` | `#include <MqttTransport.h>` |
| `#include <CourierUDPTransport.h>` | `#include <UdpTransport.h>` |
| `#include <CourierEndpoint.h>` | `#include <Endpoint.h>` |

## Type renames

All public types are now in `namespace Courier`. Either qualify with `Courier::` or add `using namespace Courier;` at the top of your file (with the caveat below).

| Old | New |
|---|---|
| `Courier` (manager class) | `Courier::Client` |
| `CourierConfig` | `Courier::Config` |
| `CourierTransport` | `Courier::Transport` |
| `CourierEndpoint` | `Courier::Endpoint` |
| `CourierWSTransport` | `Courier::WebSocketTransport` |
| `CourierMqttTransport` | `Courier::MqttTransport` |
| `CourierUDPTransport` | `Courier::UdpTransport` |
| `CourierSpscQueue` | `Courier::SpscQueue` |
| `CourierWSTransportConfig` | `Courier::WebSocketTransport::Config` |
| `CourierMqttTransportConfig` | `Courier::MqttTransport::Config` |

## A note on `using namespace Courier;`

Arduino's `<Arduino.h>` defines a global `class Client` (the base class for `WiFiClient`, `EthernetClient`, etc.). If you do `using namespace Courier;` in user code, any unqualified reference to `Client` becomes ambiguous.

The pragmatic mitigation: declare your `Courier::Client` instance fully qualified once at file scope, then `using namespace Courier;` for the rest. Everything else (`Config`, `WebSocketTransport`, `MqttTransport`, `State`, `Endpoint`, `Transport`) has no Arduino collision.

```cpp
#include <Courier.h>

Courier::Client courier(makeConfig());   // qualified once

void setup() {
    using namespace Courier;             // safe everywhere below
    auto& mqtt = courier.addTransport<MqttTransport>("mqtt", mqttCfg);
    mqtt.subscribe("commands/#");
}
```

## State enum

`enum CourierState` is now `enum class Courier::State` with PascalCase values.

| Old | New |
|---|---|
| `COURIER_BOOTING` | `Courier::State::Booting` |
| `COURIER_WIFI_CONNECTING` | `Courier::State::WifiConnecting` |
| `COURIER_WIFI_CONNECTED` | `Courier::State::WifiConnected` |
| `COURIER_WIFI_CONFIGURING` | `Courier::State::WifiConfiguring` |
| `COURIER_TRANSPORTS_CONNECTING` | `Courier::State::TransportsConnecting` |
| `COURIER_CONNECTED` | `Courier::State::Connected` |
| `COURIER_RECONNECTING` | `Courier::State::Reconnecting` |
| `COURIER_CONNECTION_FAILED` | `Courier::State::ConnectionFailed` |

Old:
```cpp
courier.onConnectionChange([](CourierState state) {
    if (state == COURIER_RECONNECTING) { /* ... */ }
});
```

New (C++17):
```cpp
courier.onConnectionChange([](Courier::State state) {
    using S = Courier::State;
    if (state == S::Reconnecting) { /* ... */ }
});
```

New (C++20, opt-in):
```cpp
courier.onConnectionChange([](Courier::State state) {
    using enum Courier::State;
    if (state == Reconnecting) { /* ... */ }
});
```

The library remains C++17. Both styles work in any consumer.

## JSON-first send

The primary path for sending is now `Client::send(JsonDocument&)`. Set a default transport in Config and call `courier.send(doc)` — Courier routes to the right transport.

Old:
```cpp
courier.send(payload);                          // default transport
courier.sendTo("ws", payload);                  // named transport
courier.publishTo("mqtt", topic, payload);      // MQTT publish
```

New:
```cpp
// Set the default transport in Config:
Courier::Config cfg;
cfg.defaultTransport = "ws";
Courier::Client courier(cfg);

// Send JSON via the default transport:
JsonDocument doc;
doc["type"] = "hello";
courier.send(doc);

// MQTT with per-call topic (no stored default topic):
cfg.defaultTransport = "mqtt";
// ...
JsonDocument doc;
doc["type"] = "telemetry";
Courier::SendOptions opts;
opts.topic = "sensors/me";
courier.send(doc, opts);

// Explicit transport routing — drop down when needed:
courier.transport<Courier::WebSocketTransport>("ws").sendBinary(audio, len);
courier.transport<Courier::MqttTransport>("mqtt").publish("sensors/me", payload);
```

`sendTo`, `sendBinaryTo`, and `publishTo` are permanently removed. For per-call explicit routing, use `transport<T>(name).send(doc)` or the transport-specific methods (`sendText`, `sendBinary`, `publish`).

## Sending design principle

`Client::send(JsonDocument&)` is the golden path — pass a doc, Courier delivers it to the configured transport. Per-call options live in `Courier::SendOptions` (topic for MQTT, ignored by WS and UDP).

For raw frames or transport-specific shapes, drop down to the transport directly:

```cpp
// WS raw text:
courier.transport<Courier::WebSocketTransport>("ws").sendText(raw);

// WS binary:
courier.transport<Courier::WebSocketTransport>("ws").sendBinary(data, len);

// MQTT direct publish:
courier.transport<Courier::MqttTransport>("mqtt").publish(topic, payload);

// MQTT JSON sugar:
courier.transport<Courier::MqttTransport>("mqtt").publish(topic, doc);
```

## Receiving

`onRawMessage` and `onBinaryMessage` on Client are gone. Move them to per-transport hooks. `onMessage` callback gains `transportName` as its first argument.

Old:
```cpp
courier.onMessage([](const char* type, JsonDocument& doc) { /* JSON */ });
courier.onRawMessage([](const char* p, size_t l) { /* text bytes */ });
courier.onBinaryMessage([](const uint8_t* d, size_t l) { /* binary */ });
```

New:
```cpp
courier.onMessage([](const char* transportName, const char* type, JsonDocument& doc) {
    // JSON only — fires when the text payload parses as JSON.
    // transportName tells you which transport delivered the message ("ws", "mqtt", etc.)
});

// Per-transport hooks fire alongside Client's JSON dispatch:
auto& ws = courier.transport<Courier::WebSocketTransport>("ws");
ws.onText  ([](const char* p, size_t l)    { /* raw text frames */ });
ws.onBinary([](const uint8_t* d, size_t l) { /* binary frames */ });

auto& mqtt = courier.transport<Courier::MqttTransport>("mqtt");
mqtt.onMessage([](const char* topic, const char* p, size_t l) {
    // Per-MQTT, topic-aware. Fires for every message; if the payload is
    // JSON, Client::onMessage also fires for the same payload.
});
```

The `transportName` arg is most useful when a single device receives the same logical message type over multiple transports (e.g. WS and MQTT simultaneously):

```cpp
courier.onMessage([](const char* transportName, const char* type, JsonDocument& doc) {
    if (strcmp(type, "config") == 0) {
        Serial.printf("config arrived via %s\n", transportName);
    }
});
```

## MQTT default topic

`MqttTransport::setDefaultPublishTopic()` and the stored default topic are gone. Topic now goes per-call, either via `SendOptions.topic` through `Client::send`, or spelled out directly on `mqtt.publish`.

Old:
```cpp
mqtt.setDefaultPublishTopic("sensors/me/data");
mqtt.send(payload);   // -> publishes to sensors/me/data
```

New:
```cpp
// Via Client::send:
Courier::SendOptions opts;
opts.topic = "sensors/me/data";
courier.send(doc, opts);

// Or direct publish:
mqtt.publish("sensors/me/data", payload);

// JSON publish sugar (new in 0.4.0):
mqtt.publish("sensors/me/data", doc);
```

## WebSocket raw text

`WebSocketTransport::send(const char*)` is renamed to `sendText(const char*)`. The new `send(JsonDocument&, opts)` takes its place as the base-virtual override.

Old:
```cpp
ws.send(R"({"type":"hello"})");
```

New:
```cpp
ws.sendText(R"({"type":"hello"})");   // raw text frame

// Or let Courier serialize for you:
JsonDocument doc;
doc["type"] = "hello";
ws.send(doc);           // via transport directly
courier.send(doc);      // via Client::send (if defaultTransport = "ws")
```

## Adding transports

`addTransport(name, Transport*)` is now templated `addTransport<T>(name, args...)`. Client constructs and owns the transport, and seeds its endpoint from `Config::host`/`port`/`path` so the static-path-in-Config case keeps working with no further setup.

Old:
```cpp
CourierMqttTransport mqtt(mqttCfg);
courier.addTransport("mqtt", &mqtt);             // user owns mqtt
courier.setEndpoint("mqtt", mqttEndpoint);
```

New:
```cpp
auto& mqtt = courier.addTransport<Courier::MqttTransport>("mqtt", mqttCfg);
// Client now owns mqtt. The endpoint is already seeded from Config.
```

For late-bound endpoints (e.g. an MQTT path that depends on a registration roundtrip), call `setEndpoint(host, port, path)` from inside `onTransportsWillConnect` — the next state-machine tick reads from the transport, so writes inside the hook aren't clobbered:

```cpp
courier.onTransportsWillConnect([&]() {
    String mqttPath = registerAndGetPath();   // your registration roundtrip
    courier.transport<Courier::MqttTransport>("mqtt")
        .setEndpoint(HOST, PORT, mqttPath.c_str());
    // setEndpoint copies the strings, so the String temp dropping out
    // of scope at the end of this lambda is safe.
});
```

`setEndpoint` is virtual and accepts either three primitives or a `Courier::Endpoint` struct. The 3-arg `begin(host, port, path)` form is still available as sugar for `setEndpoint(...) + begin()` if you'd rather call begin directly.

## Manual reconnect

New: `Client::reconnect()` triggers the connection-recovery state machine. Tears down transports, fires `onDisconnected`, transitions to `State::Reconnecting`. Adaptive — the handler checks WiFi and re-runs the WiFi step if needed. `onTransportsWillConnect` re-fires on the way back through `TransportsConnecting`.

Use this for re-registration flows or any "kick the connection cycle" trigger:

```cpp
void onReregisterRequested() {
    _roomId = "";  // clear application state
    courier.reconnect();
    // onTransportsWillConnect re-fires; your registration logic re-runs.
}
```

In 0.3.x this required scattered `mqtt.disconnect()` / re-fetch / `mqtt.begin()` calls. In 0.4 it's a single method.

## Suspend / resume

Old:
```cpp
courier.suspendTransports();
courier.resumeTransports();
```

New:
```cpp
courier.suspend();
courier.resume();
```

## Built-in WS

Old: always present, accessed via `courier.builtinWS()`.

New: auto-registered as `"ws"` only when `Config::host` is non-null/non-empty; access via `courier.transport<Courier::WebSocketTransport>("ws")`. To opt out of the built-in, leave `host` null in your Config and register your own transports.

```cpp
auto& ws = courier.transport<Courier::WebSocketTransport>("ws");
ws.sendText(R"({"type":"hello"})");
```

## Custom transport authors

If you subclass `Courier::Transport` directly, three things changed in 0.4. Built-in transport users (`WebSocketTransport`, `MqttTransport`, `UdpTransport`) need no changes; this section is only for code that derives from the base class.

**1. `send` override signature.**

Old:
```cpp
bool send(const char* payload) override { /* ... */ }
```

New:
```cpp
bool send(JsonDocument& doc, const Courier::SendOptions& options = {}) override {
    // serialize doc yourself, then deliver
    String payload;
    serializeJson(doc, payload);
    // ...
}
```

**2. `begin` override is zero-arg, reads host/port/path from base members.**

Old:
```cpp
void begin(const char* host, uint16_t port, const char* path) override {
    // open a connection to host:port/path
}
```

New:
```cpp
void begin() override {
    // host/port/path are stored in the protected base members
    // _host (std::string), _port (uint16_t), _path (std::string).
    // Client::addTransport seeds them from Config; users can override
    // via setEndpoint() before begin() runs.
    open(_host.c_str(), _port, _path.c_str());
}
```

If your subclass also wants to accept the 3-arg `begin(host, port, path)` sugar form on its own type (rather than only on the base), add `using Transport::begin;` to its public section to unhide the base's non-virtual overload — otherwise C++ name-hiding makes the 3-arg form invisible on the derived type. The built-in transports do this:

```cpp
class MyTransport : public Courier::Transport {
public:
    using Transport::begin;   // unhide 3-arg sugar
    void begin() override { /* ... */ }
    // ...
};
```

**3. `sendBinary` and `publish` are no longer on the base.**

Remove those overrides unless your transport genuinely supports those operations — and if it does, they're class-specific methods, not overrides. (`WebSocketTransport::sendBinary` and `MqttTransport::publish` are class-specific in the built-in transports too — symmetric with the surface principle that wire verbs live on the transport that actually offers them.)

## Multiple WebSocket transports

New in 0.4.0: you can register several at once.

```cpp
auto& primary = courier.addTransport<Courier::WebSocketTransport>("primary", primaryCfg);
auto& voice   = courier.addTransport<Courier::WebSocketTransport>("voice", voiceCfg);
primary.onText  ([](auto p, auto l) { /* ... */ });
voice  .onBinary([](auto d, auto l) { /* ... */ });
```

`MAX_TRANSPORTS` is 4; each TLS transport costs RAM. Two SSL transports is the realistic cap on ESP32.
