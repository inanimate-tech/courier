// Gemini Speech-to-Speech — push-to-talk voice assistant backed by Gemini Live.
//
// This sketch demonstrates the two Courier features the simpler demos
// don't exercise:
//
//   1. Binary WS frames, both directions — mic PCM out, Gemini audio
//      PCM back in. See sendBinaryTo() and onBinaryMessage().
//   2. A burst of JSON frames on connect — session / ready / settings
//      arrive in one scheduler slice and all land in onMessage().
//
// The button-to-talk loop is deliberately the simplest thing that works:
// while A is held, grab a mic chunk, ship it; on release, send a {stop}
// so the server can flush its end-of-turn silence to Gemini.

#include <M5Unified.h>
#include <Courier.h>
#include <WiFi.h>

// ISRG Root X1 — Let's Encrypt's root. Cloudflare Workers use Let's
// Encrypt for *.workers.dev, and Courier's default cert (GTS Root R4)
// does not chain there. Pin this one so TLS verifies correctly.
static const char kIsrgRootX1Pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

namespace {

constexpr uint32_t kMicSampleRate = 16000;  // Gemini Live input format
constexpr uint32_t kSpkSampleRate = 24000;  // Gemini Live output format
constexpr size_t   kMicChunkSamples = 320;  // 20ms @ 16kHz

// Playback buffer sizing. Gemini Live sends many small inlineData parts
// per turn (each forwarded as its own WS frame). Calling playRaw per
// frame caused discontinuities between M5Unified speaker "jobs" —
// audible as crackle. Instead, append every frame into a PSRAM ring and
// drain the whole thing with a single playRaw when the speaker's free.
constexpr int kMaxPlaybackSec   = 30;                                // full reply
constexpr int kMaxPlayBytes     = kSpkSampleRate * 2 * kMaxPlaybackSec;
constexpr int kFallbackPlayBytes = kSpkSampleRate * 2 * 4;           // heap fallback
constexpr int kMinPlaybackBytes = kSpkSampleRate * 2 / 4;            // 250 ms warmup

int16_t micBuffer[kMicChunkSamples];

// Playback state — owned by onAudio (writer) + loop() (drainer). See
// advancePlayback() below for the invariant that keeps memmove from
// moving bytes out from under an in-flight M5.Speaker job.
uint8_t *playBuffer   = nullptr;
int      playCapacity = 0;
int      playWritePos = 0;
int      playReadPos  = 0;
bool     chunkInFlight   = false;
bool     playbackStarted = false;

// Track session state derived from the server's JSON burst.
bool sessionReady  = false;     // set when we see {"type":"ready"}
bool wasPressed    = false;     // for A-button edge detection
String activeChatId;            // captured from {"type":"session"}
String lastTranscript;          // latest streaming transcript line

// Path including device_id — filled in from the chip MAC in setup(), so
// each board lands in its own Durable Object on the server side. The
// buffer is static so the pointer handed to CourierConfig stays valid.
char wsPath[64] = "/ws?device_id=gemini-s2s-default";

CourierConfig makeConfig() {
  CourierConfig cfg;
  // Replace with your deployed Worker hostname before flashing.
  cfg.host   = "YOUR-WORKER.workers.dev";
  cfg.port   = 443;
  cfg.path   = wsPath;
  cfg.apName = "Gemini S2S";
  return cfg;
}

Courier courier(makeConfig());

void drawStatus(const char *line1, const char *line2 = nullptr) {
  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(4, 4);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setTextWrap(true);
  M5.Display.println(line1);
  if (line2) {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(0xC618, BLACK);  // light grey
    M5.Display.println();
    M5.Display.println(line2);
  }
}

void initPlayback() {
  playBuffer = static_cast<uint8_t *>(ps_malloc(kMaxPlayBytes));
  if (playBuffer) {
    playCapacity = kMaxPlayBytes;
  } else {
    playBuffer = static_cast<uint8_t *>(malloc(kFallbackPlayBytes));
    if (playBuffer) playCapacity = kFallbackPlayBytes;
  }
  Serial.printf("[Audio] Playback buffer: %d bytes\n", playCapacity);
}

void resetPlayback() {
  M5.Speaker.stop();
  playWritePos    = 0;
  playReadPos     = 0;
  chunkInFlight   = false;
  playbackStarted = false;
}

void compactPlaybackBuffer() {
  if (playReadPos == 0) return;
  const int unread = playWritePos - playReadPos;
  if (unread > 0) memmove(playBuffer, playBuffer + playReadPos, unread);
  playReadPos  = 0;
  playWritePos = unread;
}

// Server → device audio. Gemini's 24 kHz PCM chunks arrive as many small
// binary frames; append into the PSRAM ring and let the main loop drain
// them in one shot. playRaw holds the caller's pointer until isPlaying()
// clears, so we must not memmove while a drain is in flight.
void onAudio(const uint8_t *data, size_t len) {
  if (!playBuffer || len == 0 || (len & 1)) return;  // int16-aligned
  if (playWritePos + static_cast<int>(len) > playCapacity) {
    Serial.printf("[Audio] Playback overflow, dropping %u bytes\n",
                  static_cast<unsigned>(len));
    return;
  }
  memcpy(playBuffer + playWritePos, data, len);
  playWritePos += static_cast<int>(len);
}

// Called from loop(). Waits out any in-flight playRaw, compacts, and
// issues a single drained playRaw once the warmup threshold is met.
void advancePlayback() {
  if (!playBuffer) return;
  if (chunkInFlight && M5.Speaker.isPlaying()) return;
  if (chunkInFlight) {
    chunkInFlight = false;
    compactPlaybackBuffer();
  }

  const int available = playWritePos - playReadPos;
  if (available <= 0) return;
  if (!playbackStarted && available < kMinPlaybackBytes) return;
  playbackStarted = true;

  auto *start = reinterpret_cast<int16_t *>(playBuffer + playReadPos);
  const int samples = available / static_cast<int>(sizeof(int16_t));
  M5.Speaker.playRaw(start, samples, kSpkSampleRate,
                     /*stereo=*/false, /*repeat=*/1, /*channel=*/0);
  playReadPos   = playWritePos;
  chunkInFlight = true;
}

void startListening() {
  // M5StickS3: Mic and Speaker share I2S0 — speaker must be torn down
  // before Mic.begin(), otherwise the bus stays in a bad state and no
  // audio plays on the next turn (and the amp may whine).
  resetPlayback();
  M5.Speaker.end();
  delay(20);
  M5.Mic.begin();
  delay(20);
  sessionReady && courier.send(R"({"type":"start"})");
  drawStatus("Listening...", activeChatId.c_str());
}

void stopListening() {
  M5.Mic.end();
  delay(20);
  M5.Speaker.begin();
  M5.Speaker.setVolume(180);
  M5.Speaker.setAllChannelVolume(180);
  courier.send(R"({"type":"stop"})");
  drawStatus("Thinking...", activeChatId.c_str());
}

// Server → device control frames. These arrive as a burst on connect —
// three in the same scheduler slice — so Courier's FIFO is doing real
// work here.
void onControl(const char *type, JsonDocument &doc) {
  if (!type) return;

  if (strcmp(type, "session") == 0) {
    activeChatId = doc["chatId"].as<const char *>() ? doc["chatId"].as<const char *>() : "";
    drawStatus("Session started", activeChatId.c_str());
    return;
  }
  if (strcmp(type, "ready") == 0) {
    sessionReady = true;
    drawStatus("Hold A to talk", activeChatId.c_str());
    return;
  }
  if (strcmp(type, "settings") == 0) {
    // Applied silently — here we just log that it arrived, which is the
    // whole point: before the FIFO fix, this third frame of the burst
    // used to get dropped.
    const uint8_t vol = doc["volume"] | 128;
    M5.Speaker.setVolume(vol);
    return;
  }
  if (strcmp(type, "transcript") == 0) {
    const char *source = doc["source"] | "";
    const char *text   = doc["text"]   | "";
    lastTranscript = String(source) + ": " + text;
    drawStatus(lastTranscript.c_str(), activeChatId.c_str());
    return;
  }
  if (strcmp(type, "turn_complete") == 0) {
    drawStatus("Hold A to talk", activeChatId.c_str());
    return;
  }
  if (strcmp(type, "drop_audio") == 0) {
    resetPlayback();
    return;
  }
}

}  // namespace

void setup() {
  auto m5cfg = M5.config();
  M5.begin(m5cfg);
  M5.Display.setRotation(1);
  M5.Speaker.begin();
  M5.Speaker.setVolume(180);
  M5.Speaker.setAllChannelVolume(180);

  Serial.begin(115200);
  initPlayback();

  // Derive a per-board device_id from the chip MAC so two devices on the
  // same Worker land in separate Durable Objects.
  snprintf(wsPath, sizeof(wsPath),
           "/ws?device_id=gemini-s2s-%012llx",
           (unsigned long long)ESP.getEfuseMac());

  // Optional: SSID + password injected at flash time (e.g. via `op read`
  // from 1Password). When present, pre-connect Wi-Fi with persistence
  // disabled so nothing lands in NVS — the password lives only in this
  // firmware image and in RAM. When empty, fall through to Courier's
  // WiFiManager captive portal.
#if defined(WIFI_SSID) && defined(WIFI_PASSWORD)
  if (WIFI_SSID[0] != '\0') {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    drawStatus("Wi-Fi...", WIFI_SSID);
    for (int i = 0; i < 200 && WiFi.status() != WL_CONNECTED; ++i) {
      delay(50);
    }
  }
#endif

  drawStatus("Connecting...", nullptr);

  courier.onConnected([]() {
    drawStatus("Waiting for AI...", nullptr);
  });

  courier.onDisconnected([]() {
    sessionReady = false;
    drawStatus("Reconnecting...", nullptr);
  });

  // Override Courier's default CA (GTS Root R4). workers.dev chains to
  // ISRG Root X1 (Let's Encrypt), so pin that one here.
  courier.builtinWS().onConfigure([](esp_websocket_client_config_t &c) {
    c.cert_pem = kIsrgRootX1Pem;
  });

  courier.onMessage(onControl);
  courier.onBinaryMessage(onAudio);

  courier.onError([](const char *category, const char *message) {
    Serial.printf("[Courier] %s: %s\n", category, message);
  });

  courier.setup();
}

void loop() {
  M5.update();
  courier.loop();
  advancePlayback();

  const bool pressed = M5.BtnA.isPressed();
  if (pressed && !wasPressed && sessionReady) {
    startListening();
  } else if (!pressed && wasPressed && sessionReady) {
    stopListening();
  }
  wasPressed = pressed;

  // While the button is held, stream mic chunks as fast as we can
  // capture them. Each chunk goes out as a single binary WS frame.
  if (pressed && sessionReady && M5.Mic.isEnabled()) {
    if (M5.Mic.record(micBuffer, kMicChunkSamples, kMicSampleRate)) {
      courier.sendBinaryTo("ws",
                           reinterpret_cast<const uint8_t *>(micBuffer),
                           sizeof(micBuffer));
    }
  }
}
