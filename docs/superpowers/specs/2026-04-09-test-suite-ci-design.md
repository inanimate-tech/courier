# Test Suite & CI Design for Courier

## Overview

Add comprehensive testing and CI to the Courier ESP32 connectivity library. The test suite covers unit tests (native platform), build verification (PlatformIO and ESP-IDF), and static analysis. CI runs on all PRs to `main` and enforces branch protection so PRs can only merge when checks pass.

## Unit Tests

### Location

`test/unit/` — PlatformIO project targeting `native` platform with Unity test framework.

### Test Files

Ported from the project courier was extracted from, stripped of all external references and made self-contained:

- `test_courier/test_courier.cpp` (~23 tests) — State machine transitions (BOOTING through CONNECTED), transport registry (add/get/remove), callback dispatch (onMessage, onRawMessage, onError, onConnected, onDisconnected, onConnectionChange), sending (send, sendTo, publishTo), suspend/resume transports, DNS configuration.
- `test_ws_transport/test_ws_transport.cpp` (~17 tests) — URI construction, connection state tracking, message delivery via callbacks, send success/failure, disconnect and reconnect (new client creation), TLS certificate configuration, onConfigure hook.
- `test_mqtt_transport/test_mqtt_transport.cpp` (~49 tests) — URI construction, client ID and topic configuration, subscription on connect, message delivery from topics, publishing to default topic, disconnect and reconnection with resubscription, large message support (10KB+), single-slot queue drop behavior, TLS, onConfigure hook.

### Mocks

`test/mocks/` — Standalone mock implementations for all external dependencies:

- `Arduino.h` — Core types (String, IPAddress), time functions (millis), Serial mock with capture, pin I/O stubs.
- `WiFi.h` — WiFi status codes, WiFiClass (connect/disconnect), WiFiClient, WiFiClientSecure with test helpers for connection state control.
- `HTTPClient.h` — HTTP methods, response code/body/header mocking, static defaults for all instances.
- `esp_websocket_client.h` — MockWebSocketClient with event handler registration, instance tracking, text/binary send support, event simulation helpers (simulateConnect, simulateMessage, simulateDisconnect).
- `mqtt_client.h` — MockMqttClient with subscription/publish tracking, event simulation, error handling, instance tracking.
- `ezTime.h` — Timezone with fixed date/time, NTP functions as no-ops.

Mock design principles:
- Static defaults so new instances pick up test configuration.
- Instance tracking (`lastInstance()`, `instanceCount()`) for test inspection.
- Event simulation helpers for driving state transitions in tests.
- Fresh client on reconnect to avoid callback accumulation.

### PlatformIO Config

`test/unit/platformio.ini`:
- Platform: `native`
- Test framework: Unity
- Build flags: `-std=c++17`, include paths for `../mocks/` and `../../src/`
- No framework (compiles natively on host)

## Build Verification — PlatformIO

### Location

`test/build-platformio/basic-websocket/` and `test/build-platformio/mqtt-pubsub/`

### Structure

Each directory contains:
- `platformio.ini` — targets `espressif32`, `esp32dev` board, Arduino framework. References the library via `lib_extra_dirs = ../../../` (repo root). Defines stub endpoint configs via build flags so the examples compile without real credentials.
- `src/` — copy of the corresponding example `.ino` file from `examples/`. Copies rather than symlinks for cross-platform CI compatibility.

### What It Verifies

That the library compiles against a real ESP32 Arduino target with all declared dependencies resolved. Compile-only — no upload, no run.

## Build Verification — ESP-IDF

### Location

`test/build-espidf/sample-project/`

### Structure

- `CMakeLists.txt` — Top-level. Sets IDF minimum version, adds repo root as `EXTRA_COMPONENT_DIRS` so courier is found as a component.
- `main/CMakeLists.txt` — Registers main component with dependency on courier.
- `main/main.cpp` — Instantiates Courier with both WS and MQTT transports, exercises key API surface (setup, callbacks, send). Covers enough to verify includes and linkage.
- `sdkconfig.defaults` — Minimal config targeting ESP32.

### What It Verifies

That the `CMakeLists.txt` and `idf_component.yml` at the repo root work correctly for ESP-IDF consumers. No Arduino framework dependency — pure ESP-IDF.

## Static Analysis

cppcheck on `src/` with suppressions:
- `missingIncludeSystem` — ESP-IDF system headers not available on host
- `syntaxError` for ESP-IDF-specific macros if needed

## Test Runner

### Location

`tools/run-tests.py`

### Interface

```
./tools/run-tests.py static-analysis    # cppcheck on src/
./tools/run-tests.py unit               # pio test -e native in test/unit/
./tools/run-tests.py build              # PlatformIO builds + ESP-IDF build
./tools/run-tests.py all                # runs all of the above
```

### Implementation

- Python script with `uv` script header for automatic dependency management (Click).
- `static-analysis`: Runs cppcheck with configured suppressions on `src/`.
- `unit`: Runs `pio test -e native` in `test/unit/`.
- `build`: Iterates over `test/build-platformio/*/` running `pio run`, then runs `idf.py build` in `test/build-espidf/sample-project/`.
- `all`: Runs static-analysis, unit, and build in sequence.
- Exit code reflects pass/fail. Colored output for local use.

## CI Workflow

### Location

`.github/workflows/ci.yml`

### Triggers

- `pull_request` to `main`
- `push` to `main`

### Jobs (all run in parallel)

1. **`static-analysis`** — `ubuntu-latest`. Installs cppcheck. Runs `./tools/run-tests.py static-analysis`.
2. **`unit-tests`** — `ubuntu-latest`. Installs Python 3.11, PlatformIO. Runs `./tools/run-tests.py unit`.
3. **`build-platformio`** — `ubuntu-latest`. Installs PlatformIO, ESP32 platform. Runs `pio run` on each project in `test/build-platformio/`.
4. **`build-espidf`** — `espressif/idf:v5.3` Docker container. Runs `idf.py build` in `test/build-espidf/sample-project/`.

### Status Check Names

Each job name becomes a required status check for branch protection.

## Branch Protection

Configured via `gh api` on the `main` branch of `inanimate-tech/courier`:

- Require status checks to pass: `static-analysis`, `unit-tests`, `build-platformio`, `build-espidf`
- No direct pushes to `main`
- No review requirement (CI-only gate)
- Force pushes disabled
- Branch deletions disabled

## Directory Structure

```
courier/
  .github/
    workflows/
      ci.yml
  src/                          # (existing library source)
  examples/                     # (existing examples)
  test/
    mocks/
      Arduino.h
      WiFi.h
      HTTPClient.h
      esp_websocket_client.h
      mqtt_client.h
      ezTime.h
    unit/
      platformio.ini
      test/
        test_courier/
          test_courier.cpp
        test_ws_transport/
          test_ws_transport.cpp
        test_mqtt_transport/
          test_mqtt_transport.cpp
    build-platformio/
      basic-websocket/
        platformio.ini
        src/
          basic-websocket.ino
      mqtt-pubsub/
        platformio.ini
        src/
          mqtt-pubsub.ino
    build-espidf/
      sample-project/
        CMakeLists.txt
        main/
          CMakeLists.txt
          main.cpp
        sdkconfig.defaults
  tools/
    run-tests.py
```
