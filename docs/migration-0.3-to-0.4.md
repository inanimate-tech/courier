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

## Sending

The `send` / `sendTo` / `sendBinaryTo` / `publishTo` methods on Client are gone. Talk to transports directly.

Old:
```cpp
courier.send(payload);                          // default transport
courier.sendTo("ws", payload);                  // named transport
courier.sendBinaryTo("ws", data, len);          // binary
courier.publishTo("mqtt", topic, payload);      // MQTT publish
```

New:
```cpp
courier.transport<Courier::WebSocketTransport>("ws").send(payload);
courier.transport<Courier::WebSocketTransport>("ws").sendBinary(data, len);
courier.transport<Courier::MqttTransport>("mqtt").publish(topic, payload);
```

For frequent call sites, alias once:
```cpp
auto& ws()   { return courier.transport<Courier::WebSocketTransport>("ws"); }
auto& mqtt() { return courier.transport<Courier::MqttTransport>("mqtt"); }

ws().send(payload);
mqtt().publish(topic, payload);
```

## Receiving

`onRawMessage` and `onBinaryMessage` on Client are gone. Move them to per-transport hooks. `onMessage(type, doc)` is unchanged.

Old:
```cpp
courier.onMessage([](const char* type, JsonDocument& doc) { /* JSON */ });
courier.onRawMessage([](const char* p, size_t l) { /* text bytes */ });
courier.onBinaryMessage([](const uint8_t* d, size_t l) { /* binary */ });
```

New:
```cpp
courier.onMessage([](const char* type, JsonDocument& doc) {
    // JSON only — fires when the text payload parses as JSON.
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

## Adding transports

`addTransport(name, Transport*)` is now templated `addTransport<T>(name, args...)`. Client constructs and owns the transport.

Old:
```cpp
CourierMqttTransport mqtt(mqttCfg);
courier.addTransport("mqtt", &mqtt);             // user owns mqtt
courier.setEndpoint("mqtt", mqttEndpoint);
```

New:
```cpp
auto& mqtt = courier.addTransport<Courier::MqttTransport>("mqtt", mqttCfg);
// Client owns mqtt now. Set the endpoint by calling begin() directly when
// the values are ready (e.g. inside onTransportsWillConnect):
mqtt.begin(host, port, path);
```

## MQTT default topic

`MqttTransport::send()` and `setDefaultPublishTopic()` are gone. Spell the topic on every publish.

Old:
```cpp
mqtt.setDefaultPublishTopic("sensors/me/data");
mqtt.send(payload);   // -> publishes to sensors/me/data
```

New:
```cpp
mqtt.publish("sensors/me/data", payload);
```

If the default-topic ergonomic was load-bearing for your app, a thin wrapper handles it cleanly:
```cpp
class Device {
public:
    bool send(const char* payload) {
        return _courier.transport<Courier::MqttTransport>("mqtt")
            .publish(_eventTopic, payload);
    }
private:
    Courier::Client _courier;
    String _eventTopic;
};
```

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
ws.send(R"({"type":"hello"})");
```

## Multiple WebSocket transports

New in 0.4.0: you can register several at once.

```cpp
auto& primary = courier.addTransport<Courier::WebSocketTransport>("primary", primaryCfg);
auto& voice   = courier.addTransport<Courier::WebSocketTransport>("voice", voiceCfg);
primary.onText  ([](auto p, auto l) { /* ... */ });
voice  .onBinary([](auto d, auto l) { /* ... */ });
```

`MAX_TRANSPORTS` is 4; each TLS transport costs RAM. Two SSL transports is the realistic cap on ESP32.
