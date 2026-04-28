#include <Courier.h>

Courier::Config makeConfig() {
  Courier::Config cfg;
  cfg.host = "echo.websocket.org";
  cfg.port = 443;
  cfg.path = "/";
  cfg.defaultTransport = "ws";
  return cfg;
}

Courier::Client courier(makeConfig());

void setup() {
  Serial.begin(115200);

  courier.onConnected([]() {
    Serial.println("Connected!");
    JsonDocument doc;
    doc["type"] = "hello";
    doc["msg"] = "world";
    courier.send(doc);
  });

  courier.onDisconnected([]() {
    Serial.println("Disconnected — will auto-reconnect...");
  });

  courier.onMessage([](const char* tname, const char* type, JsonDocument& doc) {
    Serial.printf("[%s] type=%s\n", tname, type);
  });

  courier.onError([](const char* category, const char* message) {
    Serial.printf("Error [%s]: %s\n", category, message);
  });

  courier.setup();
}

void loop() {
  courier.loop();
}
