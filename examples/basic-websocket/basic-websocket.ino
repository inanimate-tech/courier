#include <Courier.h>

Courier courier({
  .host = "echo.websocket.org",
  .port = 443,
  .path = "/"
});

void setup() {
  Serial.begin(115200);

  courier.onConnected([]() {
    Serial.println("Connected!");
    courier.send("{\"type\":\"hello\",\"msg\":\"world\"}");
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
