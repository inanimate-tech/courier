# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Courier is a batteries-included JSON messaging library for ESP32. It manages WiFi (captive portal via WiFiManager), WebSocket, MQTT, and UDP multicast transports, self-healing reconnection with exponential backoff, and NTP/HTTP time synchronization. The opinionated stack bundles ArduinoJson 7, ezTime, WiFiManager, and ESP-IDF's built-in WebSocket/MQTT clients.

The main coordinator class is `Courier::Client`.

## Build System

This is an ESP32 library supporting two build systems:

- **PlatformIO** (Arduino framework): configured via `library.json`
- **ESP-IDF component**: configured via `CMakeLists.txt` and `idf_component.yml`

To use in a PlatformIO project, add as a dependency. To use with ESP-IDF, register as a component. See `docs/publishing.md` for publishing to PlatformIO Registry and ESP Component Registry.

## Testing

Run all tests locally:
```
./tools/run-tests.py all                # everything
./tools/run-tests.py static-analysis    # cppcheck on src/
./tools/run-tests.py unit               # native unit tests
./tools/run-tests.py build              # PlatformIO example builds
```

Unit tests run on native platform (not on device). They use mocks in `test/mocks/` for all ESP-IDF and Arduino dependencies.

## Architecture

### State Machine

```
Booting → WifiConnecting → WifiConnected → TransportsConnecting → Connected
                                                   ↑                    |
                                              Reconnecting ←-----------+
                                                   |
                                           ConnectionFailed
```

State values are `enum class Courier::State` with PascalCase names: `Booting`, `WifiConnecting`, `WifiConnected`, `WifiConfiguring`, `TransportsConnecting`, `Connected`, `Reconnecting`, `ConnectionFailed`.

WiFi health monitoring checks status every 5s. Transport self-healing: WS and MQTT use ESP-IDF auto-reconnect; if a transport stays disconnected for 60s it reports failure. When all persistent transports fail, `Courier::Client` escalates to full WiFi reconnection. Reconnection uses exponential backoff (5s-60s) with a hard limit of 10 attempts.

### Core Classes (all in `src/`)

- **`Courier::Client`** (`Client.h/.cpp`): Main coordinator. Singleton (WiFiManager requires static callbacks). Manages the state machine, transport registry (max 4 named transports, owned via `std::unique_ptr`), JSON dispatch via internal hook wired on each transport, WiFi setup, time sync, and failure escalation. The built-in `WebSocketTransport` is auto-registered as `"ws"` only when `Config::host` is non-null and non-empty. Transports are added via `addTransport<T>(name, args...)`, which constructs, owns, and returns a typed reference. Access later via `transport<T>(name)`. JSON-first send: `send(JsonDocument&)` and `send(JsonDocument&, const SendOptions&)` route via `Config::defaultTransport`.
- **`Courier::Transport`** (`Transport.h`): Abstract base for pluggable transports. Pure-virtual `send(JsonDocument&, const SendOptions&)`, `begin()`, `disconnect()`, `loop()`, `isConnected()`, `isPersistent()`. Provides bounded SPSC FIFO infrastructure (`queueIncomingMessage`, `queueIncomingBinary`, `queueConnectionChange`, `queueTransportFailed`) for cross-task message handoff. `setClientHook` slot is wired by `Client` for JSON dispatch alongside user-facing callbacks.
- **`Courier::WebSocketTransport`** (`WebSocketTransport.h/.cpp`): WS transport wrapping `esp_websocket_client`. `send(doc, opts)` serializes and sends a text frame; `sendText(const char*)` for raw text frames; `sendBinary(data, len)` for binary frames. Per-transport receive hooks: `onText(cb)` — text frames `(const char*, size_t)`; `onBinary(cb)` — binary frames `(const uint8_t*, size_t)`. `onConfigure(cb)` for raw IDF config. Supports TLS, PSRAM reassembly buffer for fragmented messages. Self-healing via ESP-IDF auto-reconnect with 60s failure timeout.
- **`Courier::MqttTransport`** (`MqttTransport.h/.cpp`): MQTT transport wrapping `esp_mqtt_client`. `send(doc, opts)` requires `opts.topic`; `publish(topic, payload, qos, retain)` and `publish(topic, doc, qos, retain)` JSON overload for direct publish; `subscribe`/`unsubscribe` dynamic topic management. Per-transport receive hook: `onMessage(topic, payload, len)` — topic-aware. `onConfigure(cb)` for raw IDF config. Configurable client ID. Self-healing via ESP-IDF auto-reconnect with 60s failure timeout. `disconnect()` does full client teardown.
- **`Courier::UdpTransport`** (`UdpTransport.h/.cpp`): Multicast UDP transport wrapping `AsyncUDP`. `send(doc, opts)` only. Non-persistent (`isPersistent()` returns `false`) — does not participate in failure escalation. The `host` parameter is the multicast group address; `path` is ignored.
- **`Courier::Endpoint`** (`Endpoint.h`): Simple struct holding host/port/path for a transport endpoint.
- **`Courier::Config`** (`Client.h`): Struct for `Client` configuration. Fields: `host`, `port`, `path`, `apName`, `defaultTransport`, `dns1`, `dns2`.
- **`Courier::SendOptions`** (`Transport.h`): Per-call options: `topic` (required for MQTT), `qos`, `retain`. Ignored by WS and UDP.
- **`Courier::SpscQueue<T, N>`** (`SpscQueue.h`): Lock-free single-producer/single-consumer ring buffer used by `Transport` for cross-task message handoff. One implementation, tested on host, identical behaviour on device.

### Key Design Constraints

- Fixed-size array for transports (max 4). All event callbacks are single-slot (last registration wins, like `ws.onmessage` on the web platform).
- Bounded SPSC FIFO per transport (depth 8) — small bounded queue absorbs bursts; sustained overload drops.
- JSON-first `Client` API — `Client::send(doc)` routes via `Config::defaultTransport`. Per-call options (topic, qos, retain) live in `SendOptions`. Per-transport native verbs (`sendText`, `sendBinary`, `publish`) are reachable via `transport<T>(name)`.
- Messages are JSON with a `"type"` field used for routing to the `Client::onMessage()` callback.
- Transport-name in `onMessage` callback — `(const char* tname, const char* type, JsonDocument& doc)` — multi-transport users can dispatch by source.
- `onConfigure()` hooks are per-transport-specific (`WebSocketTransport`, `MqttTransport`) and expose raw ESP-IDF config structs for advanced customization.
- `suspend()`/`resume()` exist for OTA updates (frees task stacks).
- Time sync: NTP primary (continuous drift correction via ezTime) + HTTP Date header fallback for cold boot.
