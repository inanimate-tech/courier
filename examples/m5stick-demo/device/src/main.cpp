#include <M5Unified.h>
#include <Courier.h>

Courier::Config makeConfig()
{
  Courier::Config cfg;
  cfg.host = "m5stick-demo.genmon.workers.dev";
  cfg.port = 443;
  cfg.path = "/agents/device-agent/m5stick-demo";
  cfg.apName = "M5Stick Demo";
  return cfg;
}

Courier::Client courier(makeConfig());

static void showStatus(const char *text)
{
  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(4, 4);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(2);
  M5.Display.print(text);
}

static void showText(const char *text)
{
  M5.Display.fillScreen(BLUE);
  M5.Display.setCursor(4, 4);
  M5.Display.setTextColor(WHITE, BLUE);
  M5.Display.setTextSize(2);
  M5.Display.setTextWrap(true);
  M5.Display.print(text);
}

void setup()
{
  Serial.begin(115200);

  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  showStatus("Connecting...");

  courier.onConnected([]()
                      { showStatus("Ready"); });

  courier.onDisconnected([]()
                         { showStatus("Reconnecting..."); });

  courier.onMessage([](const char *tname, const char *type, JsonDocument &doc)
                    {
    if (strcmp(type, "message") == 0) {
      const char* text = doc["text"] | "";
      showText(text);
    } });

  courier.setup();
}

void loop()
{
  M5.update();
  courier.loop();
}
