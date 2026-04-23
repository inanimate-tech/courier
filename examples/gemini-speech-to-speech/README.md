# Gemini Speech-to-Speech

Push-to-talk voice chat with Google's Gemini Live API, relayed through a
Cloudflare Worker. An M5Stick captures mic audio, streams it over a
WebSocket to the Worker, and plays back the Gemini audio response.

```
M5Stick  ◀──WebSocket──▶  Cloudflare Worker (DO)  ◀──WebSocket──▶  Gemini Live API
  mic + speaker               session relay                          speech-to-speech
```

## What this example shows

Two parts of Courier the simpler examples don't exercise:

- **Binary WS frames** (`sendBinaryTo` / `onBinaryMessage`) — mic PCM
  out, Gemini audio PCM in. Raw, not base64-in-JSON.
- **Bursts of JSON frames on connect** — the Worker sends
  `session` → `ready` → `settings` in one scheduler slice; Courier's
  transport FIFO keeps all three.

## Structure

```
gemini-speech-to-speech/
├── device/     # PlatformIO firmware for M5StickC Plus2 / S3
└── server/     # Cloudflare Worker — Durable Object that relays to Gemini
```

Order of operations: deploy the server first, paste the resulting
hostname into `device/src/main.cpp` (`cfg.host`), then build and flash
the device.

## Wire protocol

**Device → Server**

| Frame            | Type   | Meaning                                   |
| ---------------- | ------ | ----------------------------------------- |
| `{"type":"start"}` | text   | Button pressed, start of turn             |
| raw `int16` PCM  | binary | 16 kHz mono audio, ~20 ms chunks           |
| `{"type":"stop"}`  | text   | Button released, end of turn              |

**Server → Device**

| Frame                          | Type   | Meaning                                        |
| ------------------------------ | ------ | ---------------------------------------------- |
| `{"type":"session",...}`       | text   | Chat id (first of the burst on connect)        |
| `{"type":"ready"}`             | text   | Gemini is ready (second of the burst)          |
| `{"type":"settings",...}`      | text   | Device-side config (third of the burst)        |
| `{"type":"transcript",...}`    | text   | Streaming user / model transcripts             |
| `{"type":"turn_complete"}`     | text   | Model finished speaking                        |
| `{"type":"drop_audio"}`        | text   | Interrupted — drop any buffered playback       |
| raw 24 kHz `int16` PCM         | binary | Gemini audio chunks                            |

## Server

A Cloudflare Worker with one Durable Object (`LiveSession`) bridging each
device WebSocket to Gemini's Live API.

### Setup

```bash
cd server
npm install

# Set your Gemini API key for local dev
cp .dev.vars.example .dev.vars
# edit .dev.vars with a GEMINI_API_KEY from https://aistudio.google.com/

npm run dev
```

### Deploy

```bash
cd server
npx wrangler secret put GEMINI_API_KEY
npm run deploy
```

Copy the deployed hostname into `device/src/main.cpp` (the `cfg.host`
line) before building the device firmware in the next section.

## Device

Two M5Stick variants are supported:

- **M5StickS3** (grey) — ESP32-S3. Proper speaker + MEMS mic. Pick this
  one for audible voice.
- **M5StickC Plus2** (yellow, default env) — ESP32-PICO-V3-02. Buzzer
  only, so audio playback is limited.

### Build & flash

```bash
cd device

# M5StickC Plus2 (default):
pio run -t upload
pio device monitor

# M5StickS3:
pio run -e m5sticks3 -t upload
pio device monitor
```

### Connect to Wi-Fi

On first boot the device starts an access point called **Gemini S2S**.
Connect to it from your phone and enter your Wi-Fi credentials via the
captive portal. Courier persists them for next boot. (ESP32 is 2.4 GHz
only.)

The firmware also supports pre-connecting by reading `WIFI_SSID` and
`WIFI_PASSWORD` from the shell at build time (see the `build_flags` in
`device/platformio.ini`). When both are unset, the build falls through
to the captive portal.

### Using it

1. Hold button A to talk.
2. Release when you're done.
3. Listen to the response through the speaker.
4. Transcripts stream on the display as they arrive.

## Notes

- Audio in is 16 kHz mono `int16`; audio out is 24 kHz mono `int16` —
  Gemini Live is fixed at these rates.
- `sendBinaryTo("ws", ...)` and `onBinaryMessage` live alongside the
  text `send()` / `onMessage()` APIs — use both in the same session.
