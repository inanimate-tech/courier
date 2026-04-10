# Changelog

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
