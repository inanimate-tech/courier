# Changelog

## v0.4.0

Theme: rationalising naming and shrinking surface area.

### Breaking changes

**Namespace and casing.** All public types now live in `namespace Courier`. Acronyms become PascalCase-as-words.

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
| `CourierWSTransportConfig` | `Courier::WebSocketTransport::Config` (nested) |
| `CourierMqttTransportConfig` | `Courier::MqttTransport::Config` (nested) |

Header files renamed to drop the `Courier` prefix: `CourierWSTransport.h` → `WebSocketTransport.h`, etc. Layout is flat under `src/`.

**State enum.** `enum CourierState` (with `COURIER_BOOTING` etc.) is now `enum class Courier::State` with PascalCase values (`Booting`, `WifiConnecting`, `Connected`, `Reconnecting`, `ConnectionFailed`, etc.).

**Client surface changes.** Permanently removed from `Courier::Client`:

- `sendTo(name, payload)`, `sendBinaryTo(name, data, len)`, `publishTo(name, topic, payload)` — use `transport<T>(name).send(doc)` or transport-specific methods.
- `setDefaultTopic(topic)` — topic is now per-call via `SendOptions.topic`.
- `setEndpoint(name, endpoint)` — call `transport.begin(host, port, path)` when ready.
- `builtinWS()` — access via `transport<WebSocketTransport>("ws")`.
- `onRawMessage`, `onBinaryMessage` — moved to per-transport hooks.
- `Config::defaultTopic` field — removed; topic is per-call.

Returned / reshaped on `Courier::Client`:

- `Client::send(JsonDocument&)` and `Client::send(JsonDocument&, const SendOptions&)` — JSON-first send, routes via `Config::defaultTransport`.
- `setDefaultTransport(name)` — runtime override for the default transport.
- `Config::defaultTransport` field — returned (was removed in an earlier pass).

**`MessageCallback` signature change.** Gains `transportName` as the first argument:

Old: `void(const char* type, JsonDocument& doc)`  
New: `void(const char* transportName, const char* type, JsonDocument& doc)`

Update every `onMessage` registration.

**`Transport::send` base virtual changed signature.** Custom transport subclasses must update:

Old: `bool send(const char* payload) = 0`  
New: `bool send(JsonDocument& doc, const SendOptions& options = {}) = 0`

**Surfaces removed from `Transport` base.** No longer present on the abstract base (subclass-specific only):

- `sendBinary(data, len)` — `WebSocketTransport`-specific; not on base.
- `publish(topic, payload)` — `MqttTransport`-specific; not on base.
- `topicRequired()` — no longer needed.

**`WebSocketTransport::send(const char*)` renamed.** Raw text frame delivery is now `sendText(const char*)`. `send(JsonDocument&, opts)` takes the name `send` as the base-virtual override (serializes and calls `sendText`).

**Transport registry.** `addTransport(name, Transport*)` is now templated `addTransport<T>(name, args...)` — Client constructs and owns the transport, returns a typed reference. `getTransport(name)` is replaced by `transport<T>(name)` which returns `T&` (asserts on miss). Client owns registered transports via `std::unique_ptr`.

**Lifecycle method rename.** `suspendTransports()` / `resumeTransports()` → `suspend()` / `resume()`.

**MqttTransport surface.** Removed `MqttTransport::setDefaultPublishTopic()`. Topic is now per-call: pass `opts.topic` through `Client::send(doc, opts)`, or spell it directly on `mqtt.publish(topic, ...)`.

**Built-in WS now opt-in.** Previously the Client always registered a WebSocketTransport as `"ws"`. Now it only does so if `Config::host` is non-null and non-empty. Stacks that don't use the built-in WS just leave `host` null and add their transports explicitly.

### New

**JSON-first send sugar.** `Client::send(JsonDocument&)` and `Client::send(JsonDocument&, const SendOptions&)` route to the transport named by `Config::defaultTransport` (or the runtime override set by `setDefaultTransport`). WS users call `courier.send(doc)`. MQTT users pass `opts.topic` per call.

**`Courier::SendOptions` struct.** `{const char* topic; int qos; bool retain;}` — per-call options for `send`. Defined in `<Transport.h>`. `topic` is required for MQTT; `qos` and `retain` are MQTT-only; all fields are ignored by WS and UDP.

**`MqttTransport::publish` JSON overload.** `publish(topic, JsonDocument&, qos, retain)` serializes and publishes in one call — convenience sugar alongside the existing raw-text overload.

**`MessageCallback` transport-name awareness.** The first argument to `Client::onMessage` is now the transport name. Multi-transport devices (e.g. receiving the same event type over both WS and MQTT) can discriminate by source.

**Per-transport receive hooks.**

- `WebSocketTransport::onText(cb)` — text frames; `(const char* payload, size_t len)`
- `WebSocketTransport::onBinary(cb)` — binary frames; `(const uint8_t* data, size_t len)`
- `MqttTransport::onMessage(topic, payload, len)` — topic-aware

These coexist with `Client::onMessage(transportName, type, doc)` (which fires only on JSON-parsing success).

**Binary frame routing fix.** WebSocket binary frames (op_code `0x02`) are now dispatched. Previously the IDF event handler filtered them out.

**Lock-free SPSC queue (`Courier::SpscQueue<T, N>`).** Replaces the `#ifdef ESP_PLATFORM` FreeRTOS / single-slot-host FIFO in the transport base. One implementation, tested on host, identical behaviour on device.

### Internal

- `Transport.h` no longer includes `<freertos/FreeRTOS.h>` or `<freertos/queue.h>`.
- The transport base gains an internal `setClientHook` slot that Client wires for JSON dispatch — separate from the user-facing message callback so both fire.
- `Transport::drainSignals()` extracted from `drainPending()` so transport subclasses that need custom per-message dispatch (e.g. `MqttTransport` with topic-aware delivery) can override `loop()` cleanly.
- New unit test directory `test_spsc_queue/` with primitive tests.
- Renamed `test_courier/` → `test_client/`, `test_ws_transport/` → `test_websocket_transport/`. `test_mqtt_transport/` keeps its name; contents updated.
- Burst-absorption regression tests on WS and MQTT transports verify the FIFO behaviour (previously untestable on host).

### Lockstep coordination

Downstream libraries that take `CourierState` (now `Courier::State`) in their public callback signatures must be updated in the same release cycle as the Courier pin. Pin Courier and any such dependents together.

---

## v0.3.2

### Fixes

- ESP-IDF: courier now installs cleanly from the registry. 0.3.1 left ezTime, ArduinoJson, and WiFiManager undeclared in `idf_component.yml`, so downstream consumers hit `unknown name` errors on `idf.py reconfigure`. No consumer-side change needed — drop any `fetch-arduino-deps.sh`-style workaround.

---

## v0.3.1

### Fixes

- Courier builds correctly for ESP-IDF.
- `examples/espidf-basic` builds correctly.

### New

- `examples/m5stick-demo`: firmware and Cloudflare server for M5StickC Plus2 and M5StickS3.

---

## v0.3.0

### Breaking changes

**All event callbacks are now single-slot (last registration wins).** Previously, up to 4 callbacks could be registered per event type via fixed-size arrays. Now each `on*` method is a simple setter — calling it again replaces the previous callback, like `ws.onmessage` on the web platform.

This affects: `onMessage`, `onRawMessage`, `onConnected`, `onDisconnected`, `onConnectionChange`, `onError`, `onTransportsWillConnect`, `onTransportsDidConnect`.

Application frameworks built on top of Courier should take the single slot and expose virtual methods for subclasses to override — the class hierarchy replaces the callback array.

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

---

## Usage (for agents)

### Consuming Courier

Courier is a foundational library that other projects depend on. If you are an agent working in a downstream project that depends on Courier:

1. Check the version of Courier your project currently uses (look at the dependency pin in your project's `platformio.ini` / `idf_component.yml`, or the vendored copy's `library.json` / `idf_component.yml`).
2. Check the latest version of Courier in this changelog.
3. Read every section between those two versions and update your project's code accordingly — paying particular attention to **Breaking changes**.

### Updating this changelog

Each in-progress version section is headed `## vX.Y.Z-dev (<git-hash>)`, where `<git-hash>` is the short hash of the commit that introduced the section (or the most recent commit it covers, if updated in place). Released versions drop the `-dev` suffix and the git hash.

Standard subsections, in order, omitting any that are empty:

- **Breaking changes** — API changes that require downstream code updates.
- **New features** — additions that are backward-compatible.
- **Fixes** — bug fixes.
- **Internal** — refactors, tooling, tests, docs — anything not visible to consumers.

A `-dev` version section is a work-in-progress: continue appending to it as work lands. When a semver version is released (the `-dev` suffix is removed and the version is published to the PlatformIO Registry and ESP Component Registry), that section is frozen — do not modify it. New work then opens a fresh `## vX.Y.Z-dev (<git-hash>)` section above it.

The `version` field in `library.json` and `idf_component.yml` tracks the in-progress `-dev` version while work is underway, and is updated to the released semver string at publish time (see `docs/publishing.md`).
