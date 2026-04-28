# courier

Batteries-included JSON messaging for ESP32. WiFi and user configuration, WebSocket, MQTT, UDP multicast, self-healing reconnection — all handled.

> [!IMPORTANT]
> Courier is under active development. See [docs/changelog.md](docs/changelog.md) for changes on each release.
>
> ⚠️ This is **v0.4.0-dev** and the API has changed considerably.
>
> To pin to the previous stable version use `lib_deps = https://github.com/inanimate-tech/courier.git#v0.3.2` in your `platformio.ini`.

Motivation: When you make something neat on your [M5Stick](https://shop.m5stack.com/products/m5stickc-plus2-esp32-mini-iot-development-kit?variant=44269818216705) you want the quickest path to messaging the back-end, and you want to carry it to places to show people and configure the Wi-Fi from your phone. Courier is how you do that.

Courier expects JSON messages with a `"type"` field. Messages are parsed with ArduinoJson and the `type` string is passed to the `onMessage` callback alongside the parsed document. Use the per-transport receive hooks (`WebSocketTransport::onText`, `MqttTransport::onMessage`, etc.) for non-JSON or topic-routed payloads.

## Quick Start

1. Bring up your hardware as normal with Arduino or ESP-IDF.
2. Install Courier (see below; we recommend managing your libraries with [PlatformIO](https://platformio.org)).
3. Initialize Courier with a config struct, set up your callbacks, and call `setup()` and `loop()`.

```cpp
#include <Courier.h>

Courier::Config makeConfig() {
  Courier::Config cfg;
  cfg.host = "api.example.com";
  cfg.port = 443;
  cfg.path = "/ws";
  cfg.defaultTransport = "ws";   // enable courier.send(doc)
  return cfg;
}

Courier::Client courier(makeConfig());

void setup() {
  courier.onConnected([]() {
    JsonDocument doc;
    doc["type"] = "hello";
    courier.send(doc);
  });
  courier.onMessage([](const char* transportName, const char* type, JsonDocument& doc) {
    Serial.printf("Got: %s (via %s)\n", type, transportName);
  });
  courier.setup();
}

void loop() { courier.loop(); }
```

## What it does

- **WiFi** — captive portal config via WiFiManager, auto-reconnection
- **WebSocket** — built-in transport with TLS, ping/pong heartbeat, self-healing auto-reconnect
- **MQTT** — opt-in transport with subscribe/unsubscribe, topic-addressed publishing, self-healing auto-reconnect
- **UDP multicast** — opt-in transport for local network discovery and messaging
- **Self-healing** — transports auto-reconnect independently; if all persistent transports fail after 60s, Courier escalates to full WiFi reconnection
- **Reconnection** — exponential backoff (5s-60s), health monitoring, automatic recovery
- **Time sync** — NTP primary (continuous drift correction) + HTTP Date header fallback
- **JSON routing** — messages parsed and dispatched by `type` field
- **Transport map** — named transports, per-transport hooks for direct access

## Opinionated

Courier bundles a number of other great libraries:

- **WebSocket** — esp_websocket_client [Documentation](https://docs.espressif.com/projects/esp-protocols/esp_websocket_client/docs/latest/index.html)
- **MQTT** — esp_mqtt_client [Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mqtt.html)
- **WiFi config** — WiFiManager [GitHub](https://github.com/tzapu/WiFiManager)
- **JSON** — ArduinoJson [Documentation](https://arduinojson.org/)
- **Time** — ezTime [GitHub](https://github.com/ropg/ezTime)

Use `onConfigure` hooks to access the full configuration surface of each bundled library.

## Licenses

Courier itself is MIT. Its bundled dependencies carry their own (permissive) licenses:

- WiFiManager, ArduinoJson, ezTime — MIT
- esp_websocket_client, esp_mqtt_client, ESP-IDF — Apache 2.0
- arduino-esp32 — LGPL 2.1+

When you ship firmware built with Courier, those libraries ship with it. Follow each library's notice/attribution requirements as applicable — in particular arduino-esp32's LGPL terms around relinking if you statically link it into a closed-source binary.

## Install

### PlatformIO

**From GitHub** (recommended while in active development):
```ini
lib_deps = https://github.com/inanimate-tech/courier.git
```
Or to pin a version: `https://github.com/inanimate-tech/courier.git#v0.3.1`

**From the PlatformIO registry** (for stable versions):
```ini
lib_deps = inanimate/courier@0.3.1
```

### ESP-IDF Component

**From GitHub** (recommended while in active development):
```yml
dependencies:
  inanimate-tech/courier:
    git: https://github.com/inanimate-tech/courier.git
```
Or to pin a version, add `version: v0.3.1`

**From the ESP Component Registry** (for stable versions):
```yml
dependencies:
  inanimate-tech/courier:
    version: "0.2.0"
```

## API

See [docs/api.md](docs/api.md) for the full API reference. Migrating from 0.3.x? See [docs/migration-0.3-to-0.4.md](docs/migration-0.3-to-0.4.md).

Quick overview:

```cpp
// State
courier.isConnected();
courier.getState();        // Courier::State

// Sending via Client (routes to defaultTransport)
JsonDocument doc;
doc["type"] = "hello";
courier.send(doc);                          // WS default
Courier::SendOptions opts;
opts.topic = "sensors/me";
courier.send(doc, opts);                    // MQTT with per-call topic

// Explicit transport access — for raw frames or multi-transport setups
courier.transport<Courier::WebSocketTransport>("ws").sendText(payload);
courier.transport<Courier::WebSocketTransport>("ws").sendBinary(data, len);
courier.transport<Courier::MqttTransport>("mqtt").publish("topic", payload);

// Transports — Client constructs and owns
auto& mqtt = courier.addTransport<Courier::MqttTransport>("mqtt", mqttCfg);
courier.suspend();   // free SRAM for OTA
courier.resume();

// Callbacks (single-slot, last registration wins)
courier.onMessage([](const char* transportName, const char* type, JsonDocument& doc) { });
courier.onConnected([]() { });
courier.onDisconnected([]() { });
courier.onError([](const char* category, const char* msg) { });

// Per-transport hooks (raw / topic-aware / binary)
auto& ws = courier.transport<Courier::WebSocketTransport>("ws");
ws.onText  ([](const char* p, size_t l)    { });
ws.onBinary([](const uint8_t* d, size_t l) { });
mqtt.onMessage([](const char* topic, const char* p, size_t l) { });

// Raw ESP-IDF config access
ws.onConfigure  ([](esp_websocket_client_config_t& cfg) { });
mqtt.onConfigure([](esp_mqtt_client_config_t& cfg)      { });
courier.onConfigureWiFi([](WiFiManager& wm) { });
```

## Connectivity state machine

```
Booting -> WifiConnecting -> WifiConnected -> TransportsConnecting -> Connected
                                                     ^                    |
                                                Reconnecting <-----------+
                                                     |
                                             ConnectionFailed
```

`onConnectionChange` fires at each state transition. `onError` fires alongside transitions caused by failures, providing a category and reason (e.g. `"WIFI"`, `"connection lost"`).

## Key design constraints

- **Single instance** — WiFiManager requires a static callback, so only one `Courier::Client` instance per process
- **Single-slot callbacks** — each `on*` method is a setter (last registration wins). Application frameworks take the slot and expose virtual methods for subclasses
- **Bounded SPSC FIFO per transport** — small bounded queue (depth 8) absorbs bursts; sustained overload drops
- **Arduino + ESP-IDF** — depends on Arduino framework for WiFiManager, ArduinoJson, ezTime

## License

MIT
