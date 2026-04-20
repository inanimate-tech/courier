# M5Stick Demo

An M5StickC Plus2 connected via WebSocket to a Cloudflare Worker. Type text in a web UI, hit "Send", and the M5Stick displays it.

The device connects via WebSocket managed by [Courier](https://github.com/inanimate-tech/courier). The server is a Cloudflare Worker backed by a Durable Object.

## Structure

```
m5stick-demo/
├── device/     # PlatformIO firmware for M5StickC Plus2
└── server/     # Cloudflare Worker (Durable Object + web UI)
```

## Device

### Build & Flash

First [install the PlatformIO CLI](https://docs.platformio.org/en/stable/core/installation/index.html). Then edit `device/src/main.cpp` and replace the `cfg.host` value with your deployed Worker's hostname. Then:

```bash
cd device
pio run -t upload
pio device monitor
```

### Connect to Wi-Fi

The device manages its own connectivity. If it cannot connect to a configured Wi-Fi network, it creates an access point called **M5Stick Demo**. Connect to this AP from your phone and enter your Wi-Fi credentials via the captive portal (note: ESP32 does not support 5GHz networks).

The screen shows `Connecting...` until it joins Wi-Fi and the server, then `Connected`, then whatever text arrives from the server.

### Message format

The device listens for JSON messages of type `text`:

```json
{ "type": "text", "text": "hello, world" }
```

## Server

A Cloudflare Worker with a single Durable Object (`DeviceAgent`). The web UI is a React app that lets you type text and broadcast it to any connected device.

### Setup

```bash
cd server
npm install
npm run dev
```

Open [http://localhost:5173](http://localhost:5173) and start typing. During local dev, point the device firmware at your dev server (or use `wrangler dev --remote` with a tunnel).

### Deploy

```bash
cd server
npm run deploy
```

Then update `cfg.host` in `device/src/main.cpp` to your deployed Worker's hostname and re-flash.

### Send text via curl

```bash
curl -X POST -H "Content-Type: text/plain" \
  --data "hello from the command line" \
  https://your-worker.workers.dev/agents/device-agent/m5stick-demo
```
