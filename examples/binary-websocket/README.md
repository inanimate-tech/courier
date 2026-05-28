# Binary WebSocket Demo

An M5Stick streams its microphone to a Cloudflare Worker as **binary** WebSocket
frames; a web page renders a live FFT frequency-bar visualiser. This example
exercises Courier's binary *send* path (`WebSocketTransport::sendBinary`).

The device connects via WebSocket managed by
[Courier](https://github.com/inanimate-tech/courier). The server is a Cloudflare
Worker backed by a single Durable Object built on the Cloudflare
[Agents SDK](https://developers.cloudflare.com/agents/), which handles the
WebSocket lifecycle (including hibernation).

## Structure

```
binary-websocket/
├── device/     # PlatformIO firmware — a minimal modification of examples/m5stick-demo/device
└── server/     # Buildless Cloudflare Worker (Durable Object + inline visualiser page)
```

> The `device/` directory is a copy of `examples/m5stick-demo/device/`. **Only
> `src/main.cpp` is different** — everything else (`platformio.ini`,
> `partitions.csv`, `.vscode/`, `.gitignore`) is identical to that example.

## Device

Built against the **M5StickS3** (grey, ESP32-S3). The M5StickC Plus2 remains the
file's default env, so build the S3 explicitly:

```bash
cd device
pio run -e m5sticks3 -t upload
pio device monitor
```

First [install the PlatformIO CLI](https://docs.platformio.org/en/stable/core/installation/index.html).
Before flashing, edit `device/src/main.cpp` and set `cfg.host` to your deployed
Worker's hostname.

### Stream

Press the front button (`BtnA`) to start streaming: the screen turns **red**
and shows **LIVE** with "Press button to stop". The mic is captured as mono
16-bit PCM at 16 kHz and sent in 512-sample binary frames (~31 frames/sec).
Press again to stop and return to the green screen.

## Server

A Cloudflare Worker: one `worker.ts` containing the `AudioReceiver` agent
(agent, an `Agent` subclass from the [Agents SDK](https://developers.cloudflare.com/agents/)),
the routing, and the inline visualiser page.

- `GET /` serves the visualiser page.
- `GET /ws` upgrades to a WebSocket on a single fixed agent instance. The device
  connects here; the browser connects to `/ws?monitor=1`.

### Develop

```bash
cd server
npm install
npm run dev
```

Open http://localhost:8787 — the page connects and waits for a stream. Point
the device at your dev server (`wrangler dev --remote` + a tunnel, or just
deploy).

### Deploy

```bash
cd server
npm run deploy
```

Then set `cfg.host` in `device/src/main.cpp` to your deployed Worker's hostname
(e.g. `binary-websocket.YOUR-CF-ACCOUNT.workers.dev`) and re-flash.

## Audio format

| Property     | Value                              |
|--------------|------------------------------------|
| Channels     | 1 (mono)                           |
| Sample rate  | 16 kHz                             |
| Sample format| signed 16-bit PCM, little-endian   |
| Frame size   | 512 samples (1024 bytes)           |
| FFT          | 512-point, computed in the browser |
