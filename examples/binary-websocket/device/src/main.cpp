#include <M5Unified.h>
#include <Courier.h>

// ---------------------------------------------------------------------------
// Binary WebSocket example: stream the M5Stick mic as binary WS frames.
// Minimal modification of examples/m5stick-demo/device — only this file differs.
// ---------------------------------------------------------------------------

static Courier::Config makeConfig()
{
  Courier::Config cfg;
  cfg.host = "binary-websocket.YOUR-CF-ACCOUNT.workers.dev";  // <-- your deployed Worker
  cfg.port = 443;
  cfg.path = "/ws";
  cfg.apName = "Binary WS Demo";
  return cfg;
}

static Courier::Client courier(makeConfig());

// ---- Audio capture config ----
static constexpr uint32_t SAMPLE_RATE   = 16000;  // M5.Mic default
static constexpr size_t   FRAME_SAMPLES = 512;    // PCM samples per WebSocket frame
static constexpr size_t   RING_SLOTS    = 4;      // ring of capture buffers

static int16_t audioRing[RING_SLOTS][FRAME_SAMPLES];
static size_t  recIdx  = 2;   // slot M5.Mic.record() is filling now
static size_t  sendIdx = 0;   // completed slot ready to send (lags recIdx by 2)

static bool streaming = false;
static bool serverReady = false;  // true once Courier reports Connected

// ---- Display helpers ----
static void showStatus(const char* text)
{
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(4, 4);
  M5.Display.print(text);
}

static void showIdle()
{
  M5.Display.fillScreen(GREEN);
  M5.Display.setTextColor(BLACK, GREEN);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(4, 4);
  M5.Display.print("Press button\nto start stream");
}

static void showLive()
{
  M5.Display.fillScreen(RED);
  M5.Display.setTextColor(WHITE, RED);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(6, 10);
  M5.Display.print("LIVE");
  M5.Display.setTextSize(2);  // CTA matches the green screen's size
  M5.Display.setCursor(4, 48);
  M5.Display.print("Press button\nto stop");
}

static void startStreaming()
{
  recIdx  = 2;
  sendIdx = 0;
  streaming = true;
  showLive();
}

static void stopStreaming()
{
  streaming = false;
  showIdle();
}

void setup()
{
  Serial.begin(115200);

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  showStatus("Connecting...");

  // S3: mic and speaker share the ES8311 codec / I2S port. Begin the mic once
  // here — M5.Mic.begin() releases the speaker's I2S driver internally. We keep
  // it running and gate sending with `streaming`. (Toggling begin/end on every
  // button press logged repeated "I2S port 0 has not installed" errors.)
  M5.Mic.begin();

  courier.onConnected([]() {
    serverReady = true;
    if (!streaming) showIdle();
  });

  courier.onDisconnected([]() {
    streaming = false;
    serverReady = false;
    showStatus("Reconnecting...");
  });

  courier.setup();
}

void loop()
{
  M5.update();
  courier.loop();

  // Toggle streaming on button press, once connected.
  if (serverReady && M5.BtnA.wasPressed()) {
    if (streaming) stopStreaming();
    else           startStreaming();
  }

  // While streaming, capture a frame and send the completed (lagging) slot.
  if (streaming && M5.Mic.isEnabled()) {
    int16_t* rec = audioRing[recIdx];
    if (M5.Mic.record(rec, FRAME_SAMPLES, SAMPLE_RATE)) {
      int16_t* ready = audioRing[sendIdx];
      courier.transport<Courier::WebSocketTransport>("ws")
             .sendBinary(reinterpret_cast<const uint8_t*>(ready),
                         FRAME_SAMPLES * sizeof(int16_t));
      sendIdx = (sendIdx + 1) % RING_SLOTS;
      recIdx  = (recIdx  + 1) % RING_SLOTS;
    }
  }
}
