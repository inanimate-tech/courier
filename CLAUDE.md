# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Courier is a batteries-included JSON messaging library for ESP32. It manages WiFi (captive portal via WiFiManager), WebSocket, MQTT, and UDP multicast transports, self-healing reconnection with exponential backoff, and NTP/HTTP time synchronization. The opinionated stack bundles ArduinoJson 7, ezTime, WiFiManager, and ESP-IDF's built-in WebSocket/MQTT clients.

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
BOOTING → WIFI_CONNECTING → WIFI_CONNECTED → TRANSPORTS_CONNECTING → CONNECTED
                                                      ↑                    |
                                                 RECONNECTING ←-----------+
                                                      |
                                              CONNECTION_FAILED
```

WiFi health monitoring checks status every 5s. Transport self-healing: WS and MQTT use ESP-IDF auto-reconnect; if a transport stays disconnected for 60s it reports failure. When all persistent transports fail, Courier escalates to full WiFi reconnection. Reconnection uses exponential backoff (5s-60s) with a hard limit of 10 attempts.

### Core Classes (all in `src/`)

- **Courier** (`Courier.h/.cpp`): Main class. Singleton (WiFiManager requires static callbacks). Manages the state machine, transport registry (max 4 named transports), callback dispatch, WiFi, time sync, and failure escalation. A built-in WebSocket transport is always registered as `"ws"`.
- **CourierTransport** (`CourierTransport.h`): Abstract base for pluggable transports. Defines `begin()`, `disconnect()`, `loop()`, `isConnected()`, `send()`/`sendBinary()`/`publish()`, `isPersistent()`. Uses a single-slot atomic pending message buffer for cross-task delivery (drops if previous still pending). Supports failure reporting via `queueTransportFailed()`/`setFailureCallback()`.
- **CourierWSTransport** (`CourierWSTransport.h/.cpp`): WebSocket transport wrapping `esp_websocket_client`. Supports TLS, custom headers via `onConfigure()`, PSRAM reassembly buffer for fragmented messages. Self-healing via ESP-IDF auto-reconnect with 60s failure timeout.
- **CourierMqttTransport** (`CourierMqttTransport.h/.cpp`): MQTT transport wrapping `esp_mqtt_client`. Dynamic topic subscription, QoS/retain support, configurable client ID. Handles both ESP-IDF v4.x (flat config) and v5.x (nested struct) differences. Self-healing via ESP-IDF auto-reconnect with 60s failure timeout. `disconnect()` does full `destroyClient()` teardown.
- **CourierUDPTransport** (`CourierUDPTransport.h/.cpp`): Multicast UDP transport wrapping `AsyncUDP`. Non-persistent (`isPersistent()` returns `false`) — does not participate in failure escalation. The `host` parameter is the multicast group address; `path` is ignored.
- **CourierEndpoint** (`CourierEndpoint.h`): Simple struct holding host/port/path/TLS config for a transport endpoint.

### Key Design Constraints

- Fixed-size array for transports (max 4). All event callbacks are single-slot (last registration wins, like `ws.onmessage` on the web platform)
- Single-slot message buffer per transport — minimizes RAM; high-throughput needs custom queuing
- Messages are JSON with a `"type"` field used for routing to the `onMessage()` callback
- `onConfigure()` hooks on transports expose raw ESP-IDF config structs for advanced customization
- `suspendTransports()`/`resumeTransports()` exist for OTA updates (frees task stacks)
- Time sync: NTP primary (continuous drift correction via ezTime) + HTTP Date header fallback for cold boot
