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

- **`Courier::Client`** (`Courier.h/.cpp` — file kept the original name): Main coordinator. Singleton (WiFiManager requires static callbacks). Manages the state machine, transport registry (max 4 named transports, owned via `std::unique_ptr`), JSON dispatch via internal hook wired on the default transport, WiFi setup, time sync, and failure escalation. The built-in `WebSocketTransport` is auto-registered as `"ws"` only when `Config::host` is non-empty AND `Config::defaultTransport` is null or `"ws"`. Transports are added via `addTransport<T>(name, args...)`, which constructs, owns, and returns a typed reference. Access later via `transport<T>(name)`. JSON-first send: `send(JsonDocument&)` and `send(JsonDocument&, const SendOptions&)` route via `Config::defaultTransport`. Receive parallels send: `Client::onMessage` delivers only the default transport's messages — other transports' messages reach their own per-transport hooks instead.
- **`Courier::Transport`** (`Transport.h`): Abstract base for pluggable transports. Pure-virtual `send(JsonDocument&, const SendOptions&)`, `begin()`, `disconnect()`, `loop()`, `isConnected()`, `isPersistent()`. Provides bounded SPSC FIFO infrastructure (`queueIncomingMessage`, `queueIncomingBinary`, `queueConnectionChange`, `queueTransportFailed`) for cross-task message handoff. `setClientHook` slot is wired by `Client` for JSON dispatch alongside user-facing callbacks.
- **`Courier::WebSocketTransport`** (`WebSocketTransport.h/.cpp`): WS transport wrapping `esp_websocket_client`. `send(doc, opts)` serializes and sends a text frame; `sendText(const char*)` for raw text frames; `sendBinary(data, len)` for binary frames. Per-transport receive hooks: `onText(cb)` — text frames `(const char*, size_t)`; `onBinary(cb)` — binary frames `(const uint8_t*, size_t)`. `onConfigure(cb)` for raw IDF config. Supports TLS, PSRAM reassembly buffer for fragmented messages. Self-healing via ESP-IDF auto-reconnect with 60s failure timeout.
- **`Courier::MqttTransport`** (`MqttTransport.h/.cpp`): MQTT transport wrapping `esp_mqtt_client`. `send(doc, opts)` requires `opts.topic`; `publish(topic, payload, qos, retain)` and `publish(topic, doc, qos, retain)` JSON overload for direct publish (both strlen-based — text only); `publishBinary(topic, data, len, qos, retain)` passes an explicit length and rejects `nullptr`/`len == 0` rather than falling into IDF's `strlen` branch. `subscribe`/`subscribeBinary`/`unsubscribe` dynamic topic management, re-applied at their registered QoS on reconnect. Per-transport receive hooks: `onMessage(topic, payload, len)` — topic-aware text; `onBinary(topic, data, len)` — payloads on filters declared via `subscribeBinary` / `Config::binaryTopics`, routed by `MqttTransport::topicMatches` (public static, MQTT 3.1.1 `+`/`#`) and never dispatched into the JSON lane. The binary receive lane is control-rate: the depth-8 inbound queue is not sized for sustained streams. `Config::out_buffer_size` and `Config::network_timeout_ms` (both `0` = leave the IDF default) expose the outbound buffer and the network-operation timeout. Publishing is task-safe: esp-mqtt serialises its own API calls, but `esp_mqtt_client_destroy` frees the handle with no lock, so `_clientLock` guards the handle's lifetime — taken with a 250 ms bound by `publish`/`publishBinary` (returns `false` rather than waiting out a teardown), unbounded by `begin`/`disconnect`/`suspend`/`resume`. `_topics` has its own `_topicsLock`, read by the IDF event task only once per connect (`subscribeAll`); the text/binary lane is decided when `loop()` drains. The two locks are never nested — see `Lock.h`. `onConfigure(cb)` for raw IDF config. Configurable client ID. Self-healing via ESP-IDF auto-reconnect with 60s failure timeout. `disconnect()` does full client teardown.
- **`Courier::UdpTransport`** (`UdpTransport.h/.cpp`): Multicast UDP transport wrapping `AsyncUDP`. `send(doc, opts)` only. Per-transport receive hook: `onText(cb)` — raw per-packet `(const char*, size_t)`, same idiom as `WebSocketTransport::onText`. Non-persistent (`isPersistent()` returns `false`) — does not participate in failure escalation. The `host` parameter is the multicast group address; `path` is ignored.
- **`Courier::HttpTransport`** (`HttpTransport.h/.cpp`): HTTPS transport wrapping `esp_http_client`. JS-shaped blocking `fetch(url, FetchOptions)` returning `Response` (`ok()`, `status`, `text()`, `json(doc)`, `header()`); `get`/`postJson` sugar. Streaming via `onResponse`/`onBody` (abortable — OTA-download shaped). `send(doc)` POSTs JSON to the Config-seeded endpoint; JSON replies dispatch through `Client::onMessage`. Fresh client per request, full teardown after (appliance posture — no keep-alive, no session state). Transport-failure retries with lwIP DNS-cache flush between attempts (anycast re-roll). TLS: IDF cert bundle by default, `cert_pem` to pin, no insecure mode. Non-persistent. `onConfigure(cb)` + per-call `FetchOptions::configure` expose raw `esp_http_client_config_t` (`event_handler`/`user_data` reserved).
- **`Courier::Endpoint`** (`Endpoint.h`): Simple struct holding host/port/path for a transport endpoint.
- **`Courier::Config`** (`Courier.h`): Struct for `Client` configuration. Fields: `host`, `port`, `path`, `apName`, `defaultTransport`, `dns1`, `dns2`.
- **`Courier::SendOptions`** (`Transport.h`): Per-call options: `topic` (required for MQTT), `qos`, `retain`. Ignored by WS and UDP.
- **`Courier::Mutex` / `Courier::TimedMutex`** (`Lock.h`): FreeRTOS mutexes on device, `std::mutex` / `std::timed_mutex` on host, plus a `LockGuard`. `TimedMutex::tryLockFor(ms)` is how a realtime sender gets "busy, defer" instead of waiting out a client teardown. Deadlock rule documented in the header: ESP-IDF clients hold their own API lock across reconnects with no timeout, so a Courier lock must never be held while waiting on it from a path an IDF-lock holder can block on — never nest these two mutexes, and never call into ESP-IDF while holding a data-only lock.
- **`Courier::SpscQueue<T, N>`** (`SpscQueue.h`): Lock-free single-producer/single-consumer ring buffer used by `Transport` for cross-task message handoff. One implementation, tested on host, identical behaviour on device.

### Key Design Constraints

- Fixed-size array for transports (max 4). All event callbacks are single-slot (last registration wins, like `ws.onmessage` on the web platform).
- Bounded SPSC FIFO per transport (depth 8) — small bounded queue absorbs bursts; sustained overload drops.
- JSON-first `Client` API — `Client::send(doc)` routes via `Config::defaultTransport`. Per-call options (topic, qos, retain) live in `SendOptions`. Per-transport native verbs (`sendText`, `sendBinary`, `publish`) are reachable via `transport<T>(name)`.
- Messages are JSON with a `"type"` field used for routing to the `Client::onMessage()` callback. Receive parallels send: `onMessage` only ever fires for the default transport (`Config::defaultTransport`), re-evaluated per dispatch so a runtime `setDefaultTransport()` switch takes effect immediately; `defaultTransport = nullptr` closes the lane. Non-default transports deliver via their own per-transport hooks (`onText`/`onBinary`/topic-aware `onMessage`) instead of the client callback.
- Transport-name in `onMessage` callback — `(const char* tname, const char* type, JsonDocument& doc)` — always the configured default transport's name, kept for signature compatibility (multi-transport dispatch-by-source now happens via per-transport hooks, not this callback).
- `onConfigure()` hooks are per-transport-specific (`WebSocketTransport`, `MqttTransport`) and expose raw ESP-IDF config structs for advanced customization.
- `suspend()`/`resume()` exist for OTA updates (frees task stacks).
- Time sync: NTP primary (continuous drift correction via ezTime) + HTTP Date header fallback for cold boot. Time sync calls `settimeofday()` (mbedTLS cert validation reads the system clock); `Client` flushes the lwIP DNS cache on every entry to `TransportsConnecting`.

## Repo hygiene

This is a public open-source repo. A few things to keep out of tracked files:

- Absolute home paths (`/Users/...`, `/home/...`). Use relative paths or `<repo-root>` placeholders.
- Per-developer Claude Code artefacts (`.claude/settings.local.json`, `.claude/napkin.md`, `.claude/memory/`, `.claude/projects/`) — all `.gitignore`-d.
- Working files from plan/spec skills (Superpowers `writing-plans`, `writing-skills`, etc.). Save them outside the repo, not under `docs/superpowers/`. That path is `.gitignore`-d as a safety net.
- Personal account identifiers in URLs and example configs. For Cloudflare Worker hostnames, use a `YOUR-CF-ACCOUNT` placeholder.
