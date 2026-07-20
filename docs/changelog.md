# Changelog

## v0.4.3-dev

Theme: receive-path memory — larger messages on no-PSRAM boards; `HttpTransport` hardening.

### New

- **`HttpTransport`** — opt-in HTTPS transport (blocking `fetch()`, buffered or streaming) that also participates as a full transport citizen via `send()`/`onMessage`. The existing auto-`"ws"` registration rule is unchanged (`Config::host` set AND `defaultTransport` is `"ws"`/unset) — set `defaultTransport = "https"` to skip the built-in `"ws"` and use `HttpTransport` instead. See the README's HTTPS section.

### Upgrading notes

- **Binary size.** `esp_http_client` and the IDF certificate bundle are now always linked in (Courier's time-sync bootstrap depends on them), adding roughly +80-100KB to firmware binary size even for projects that never construct an `HttpTransport`.
- **`settimeofday` now set by Courier.** `Client::syncTimeFromHttpDate()` calls `setSystemClock()` (`settimeofday`) in addition to ezTime's `UTC.setTime()` — previously only ezTime's virtual clock was set from the HTTP Date header, so the system clock (which mbedTLS/TLS cert validation reads) stayed at its boot default until NTP arrived. `Client::loop()`'s NTP bridge now also re-fires whenever ezTime and the system clock diverge by more than 5s (previously a one-shot latch), so a later genuine NTP correction can repair a poisoned or drifted system clock.

### Fixes

- **Time-bootstrap redirect trap.** The HTTP Date-header probe (`syncTimeFromHttpDate`) now sets `disable_auto_redirect` so a 301 on the plain-HTTP leg is treated as the response (Date header captured) instead of being silently followed into a cold-clock TLS handshake that fails and discards the Date. Both legs are now capped at a 5s timeout with no retries, bounding the worst-case blocked time during bootstrap.
- **Clock-forward plausibility ceiling.** `syncTimeFromHttpDate` now rejects Date headers more than 10 years past the firmware build date (alongside the existing floor that rejects dates before the build), guarding against a MITM setting the clock implausibly far forward on the unauthenticated bootstrap leg.
- **Redirects corrupting buffered/streaming `fetch()` responses.** `HttpTransport::eventHandler` now resets accumulated body/headers when a new hop's headers arrive after a previous hop's response had already been latched, and never accumulates or streams a 3xx hop's body. Previously an intermediate redirect with its own body could latch the wrong status and concatenate the redirect body with the final response's body.
- **`HttpTransport::send()`** no longer dispatches a truncated (`!complete()`) JSON reply to `onMessage` — `send()` still reports `ok()` (e.g. `true` for a 2xx that got cut short), but truncated JSON never reaches the raw hook.

### Improved

- **Zero-copy receive hand-off.** New `Transport::queueIncomingMessageOwned` / `queueIncomingBinaryOwned` transfer ownership of an already-heap-allocated payload into the pending queue instead of malloc+memcpy-ing a second copy. `WebSocketTransport` hands its fragmented-frame reassembly buffer over this way, removing a transient 2× peak that capped receivable message size on boards without PSRAM (e.g. ESP32-S3FN8 / M5Dial). The copying `queueIncomingMessage` / `queueIncomingBinary` remain for payloads the transport doesn't own (single-frame WS, MQTT, UDP).
- **Zero-copy JSON dispatch.** `Client::dispatchJSON` now parses the payload as mutable `char*`, so ArduinoJson points strings into the existing buffer instead of duplicating them into the document — large string fields (e.g. app code) are no longer held in memory twice during dispatch. Safe by the `drainPending` contract: payloads are heap-owned scratch buffers, the raw per-transport hook runs before the client hook, and the buffer outlives the dispatch callback.
- **WebSocket reassembly buffer falls back to internal RAM** when the `MALLOC_CAP_SPIRAM` allocation fails (no-PSRAM boards), instead of silently dropping every fragmented frame.
- **MQTT reassembly buffer falls back to internal RAM** the same way. `MqttTransport`'s multi-chunk reassembly malloc'd from `MALLOC_CAP_SPIRAM` with no fallback, so on no-PSRAM boards (M5Dial) every message larger than the 1 KB library buffer — including pushed app/shader payloads over MQTT — returned NULL and was dropped. Now mirrors the WebSocket path.
- **Receive-path failures now log.** Allocation failure, queue overflow, reassembly-buffer allocation failure, and JSON parse failure each emit a warning with the payload size — oversized messages no longer vanish without a trace.

In practice this raises the largest receivable JSON message on a no-PSRAM ESP32-S3 from ~5 KB to ~12 KB+ (bounded by largest contiguous free block at parse time). PSRAM boards see strictly less copying.

### Internal

- `Transport::drainPending` documents the payload-buffer contract (heap-owned, NUL-terminated, `_onMessage` before `_clientHook`, client hook may mutate in place). Overriding drains must preserve this order.

---

## v0.4.1

Theme: per-transport endpoint state + manual reconnect trigger.

### New

- **`Transport::setEndpoint(host, port, path)`** — virtual method on the Transport base. Stores host/port/path into protected base members (`std::string` copies, so `String::c_str()` temporaries are safe). The new ergonomic for late-bound endpoints (e.g. setting an MQTT path inside `onTransportsWillConnect` after a registration roundtrip): write to the transport, the next state-machine tick reads from the transport — no clobbering.
- **Zero-arg `Transport::begin()`** is now the pure virtual override point. Reads from stored endpoint members. The 3-arg `begin(host, port, path)` is non-virtual sugar that calls `setEndpoint(...)` then `begin()`. End-user call sites are unchanged.
- **`Client::addTransport<T>(name, args...)` seeds endpoint** from `Config::host/port/path` immediately after construction, so the simple "host/port/path are static, set in Config" case keeps working with no manual `setEndpoint` call.
- **`Client::reconnect()`** — manually trigger the connection-recovery state machine. Tears down transports, fires `onDisconnected`, transitions to `State::Reconnecting`. Adaptive: the handler checks WiFi and re-runs the WiFi step if needed before retrying transports. `onTransportsWillConnect` re-fires on the way back. Use for re-registration flows.
- **`setEndpoint(const Endpoint&)`** sugar overload — revives the `Endpoint` struct as a useful value type for passing host/port/path triples.

### Breaking changes (custom `Transport` subclasses only)

If you subclass `Courier::Transport` directly, the override point for `begin` changed. Migrate `void begin(const char* host, uint16_t port, const char* path) override` → `void begin() override` and read host/port/path from the base members `_host` / `_port` / `_path`. End users of built-in transports (`WebSocketTransport`, `MqttTransport`, `UdpTransport`) need no changes; the 3-arg `begin(host, port, path)` overload still works as sugar.

If your subclass wants to keep accepting the 3-arg sugar form via the same-name override, add `using Transport::begin;` in the subclass's public section to unhide the base's non-virtual 3-arg overload (C++ name-hiding rule). The built-in transports do this.

### Internal

- `Client::TransportEntry::endpoint` field removed — endpoint state now lives on each transport. `handleTransportsConnectingState` calls zero-arg `transport->begin()` directly; the per-transport-vs-Config fallback computation is gone.

---

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
