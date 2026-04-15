# Changelog

## v0.3.0-dev

### Breaking changes

**All event callbacks are now single-slot (last registration wins).** Previously, up to 4 callbacks could be registered per event type via fixed-size arrays. Now each `on*` method is a simple setter — calling it again replaces the previous callback, like `ws.onmessage` on the web platform.

This affects: `onMessage`, `onRawMessage`, `onConnected`, `onDisconnected`, `onConnectionChange`, `onError`, `onTransportsWillConnect`, `onTransportsDidConnect`.

Application frameworks (like Outrun) should take the single slot and expose virtual methods for subclasses to override — the class hierarchy replaces the callback array.

### New features

**Built-in GTS Root R4 TLS certificate for WebSocket transport.** `CourierWSTransport` now includes the Google Trust Services Root R4 CA certificate by default, so connections to Cloudflare-fronted hosts work without manual cert configuration. Use `onConfigure()` to override with a different cert if needed.

**Publish workflow.** Added `tools/publish-preflight.py` and GitHub Actions workflow for publishing to PlatformIO Registry and ESP Component Registry.

**UDP multicast transport.** New `CourierUDPTransport` class for local network discovery and messaging via multicast UDP. Non-persistent by default (does not participate in failure escalation). Uses `AsyncUDP` under the hood. The `host` parameter to `begin()` is the multicast group address; `path` is ignored. ([#1](https://github.com/inanimate-tech/courier/issues/1))

**Transport self-healing.** WebSocket and MQTT transports now use ESP-IDF's built-in auto-reconnect (`disable_auto_reconnect = false`) instead of Courier-level reconnection. Each transport tracks its disconnect time and, if it fails to reconnect within 60 seconds, reports failure via `queueTransportFailed()`. This replaces the previous aggregate health-check polling approach with per-transport timers.

**Transport failure escalation.** When all *persistent* transports report failure, Courier tears down all transports and transitions to `RECONNECTING`, which re-runs WiFi checks and the full transport connection sequence. Non-persistent transports (like UDP) are excluded from this check.

**`isPersistent()` on `CourierTransport`.** New virtual method (default: `true`) that controls whether a transport participates in failure escalation. `CourierUDPTransport` returns `false` since local multicast does not indicate server reachability.

**`queueTransportFailed()` / `setFailureCallback()` on `CourierTransport`.** Transports can now report unrecoverable failure to Courier. `queueTransportFailed()` sets an atomic flag drained by `drainPending()`, which fires the failure callback registered by Courier via `setFailureCallback()`.

### Internal

- Removed `MAX_CALLBACKS` constant (no longer needed).
- Callback storage reduced from 8 `std::function` arrays + 8 counters to 8 single `std::function` slots.
- Updated CLAUDE.md, README, and API docs for single-slot semantics.
- MQTT `disconnect()` now calls `destroyClient()` for full teardown (stop + destroy + state reset), matching the WS transport pattern.
- Removed aggregate transport health check polling from `handleConnectedState()` — replaced by per-transport self-healing timers.
- Added `TransportEntry.failed` flag, `handleTransportFailure()`, `allPersistentTransportsFailed()`, `clearTransportFailureFlags()`, and `teardownAllTransports()` to `Courier`.
- Courier constructor now wires failure callbacks on the built-in WS transport; `wireTransportCallbacks()` wires them on added transports.

---

## v0.2.0

### Breaking changes

**Config structs use constructors instead of designated initializers.**
The library now compiles under C++11 (`-std=gnu++11`), which is the default for ESP-IDF 4.4.x and PlatformIO's `espressif32` platform. Designated initializers (`.host = "..."`) are a C++20 feature and do not work with this toolchain.

Before:
```cpp
Courier courier({.host = "example.com", .port = 443, .path = "/ws"});
```

After:
```cpp
CourierConfig cfg;
cfg.host = "example.com";
cfg.port = 443;
cfg.path = "/ws";
Courier courier(cfg);
```

This affects `CourierConfig`, `CourierEndpoint`, and `CourierMqttTransportConfig`. See [API reference](api.md) for the new patterns.

**Send API renamed for consistency.**

| v0.1.0 | Latest | Notes |
|--------|--------|-------|
| `send(payload)` | `send(payload)` | Now targets a single default transport instead of broadcasting to all |
| `sendBinary(data, len)` | `sendBinaryTo(name, data, len)` | Must specify transport |
| `sendTo(name, payload)` | `sendTo(name, payload)` | Unchanged |
| `sendToTopic(name, topic, payload)` | `publishTo(name, topic, payload)` | Renamed |

`send()` previously broadcast to all connected transports. It now sends to the default transport only (configurable via `CourierConfig.defaultTransport` or `setDefaultTransport()`). If the default transport requires a topic (e.g. MQTT), `CourierConfig.defaultTopic` or `setDefaultTopic()` is used.

**Transport base class method renames.**

Custom transport subclasses must update:

| v0.1.0 | Latest |
|--------|--------|
| `sendMessage(payload)` | `send(payload)` |
| `publishTo(topic, payload, qos, retain)` | `publish(topic, payload)` |

The `publish()` override no longer accepts `qos` or `retain` parameters. The MQTT transport provides a separate `publish(topic, payload, qos, retain)` overload for explicit control.

A new virtual method `topicRequired()` was added (default: `false`). MQTT returns `true`, which tells `Courier::send()` to use the default topic.

**`CourierEndpoint` fields changed from `String` to `const char*`.**
`host` and `path` are now `const char*` instead of Arduino `String`. The pointed-to strings must outlive the endpoint.

**`CourierConfig.dns1` and `dns2` changed from `IPAddress` to `uint32_t`.**
Cast from `IPAddress` when setting: `cfg.dns1 = (uint32_t)IPAddress(8, 8, 8, 8);`

**`Courier` constructor is no longer `explicit`.**
This allows implicit conversion from `CourierConfig`, which is needed for the factory function pattern.

**`CourierMqttTransport` constructor is no longer `explicit`.**
Same reason as above.

### New features

**Configurable DNS servers.** Set `dns1`/`dns2` in `CourierConfig` to override DHCP-provided DNS. Applied after WiFi connects, before any HTTPS calls (time sync, registration). Uses the `esp_netif` API to avoid switching to static IP mode.

**Default transport and topic.** `send()` now targets a configurable default transport (default: `"ws"`) instead of broadcasting to all. Set `defaultTransport` and `defaultTopic` in config, or change at runtime with `setDefaultTransport()` and `setDefaultTopic()`.

**MQTT `publish()` overloads.** `publish(topic, payload)` for simple QoS 0 publishing. `publish(topic, payload, qos, retain)` for explicit control.

### Fixes

- **`onConnectionChange` now fires on all state transitions.** Previously, early transitions (BOOTING → WIFI_CONNECTING → WIFI_CONNECTED → TRANSPORTS_CONNECTING) and reconnection recovery paths did not fire callbacks, so consumers relying on state updates (e.g. for display) never saw intermediate states. All state changes now go through an internal `transitionTo()` method that fires callbacks consistently.
- Fixed member initializer order in `Courier` constructor to match declaration order (fixes `-Werror=reorder` on ESP-IDF v5.5.3).
- Fixed `publishTo` → `publish` API in mqtt-pubsub example.
- DNS configuration uses `esp_netif_set_dns_info()` instead of `WiFi.config()`, which was incorrectly switching to static IP mode and disabling DHCP.

### Internal

- Added 78 unit tests (native platform) covering Courier core, WebSocket transport, and MQTT transport.
- Added PlatformIO build verification for both examples (pinned to `espressif32@6.12.0`).
- Added static analysis via cppcheck.
- Added test runner (`tools/run-tests.py`) and GitHub Actions CI workflow.
- Added ESP-IDF example (`examples/espidf-basic/`).

---

## v0.1.0

First public release.
