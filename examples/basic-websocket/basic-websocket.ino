#include <Courier.h>

Courier::Config makeConfig() {
  Courier::Config cfg;
  cfg.host = "echo.websocket.org";
  cfg.port = 443;
  cfg.path = "/";
  return cfg;
}

Courier::Client courier(makeConfig());

void setup() {
  Serial.begin(115200);

  courier.onConnected([]() {
    Serial.println("Connected!");
    courier.send(R"({"type":"hello","msg":"world"})");
  });

  courier.onDisconnected([]() {
    Serial.println("Disconnected — will auto-reconnect...");
  });

  courier.onMessage([](const char* type, JsonDocument& doc) {
    Serial.printf("Message type: %s\n", type);
  });

  courier.onError([](const char* category, const char* message) {
    Serial.printf("Error [%s]: %s\n", category, message);
  });

  courier.setup();
}

void loop() {
  courier.loop();
}
