// HTTPS-only Courier: WiFi captive-portal config and time sync for free,
// with fetch() + send()/onMessage over HTTPS as the only transport.
// No WebSocket or MQTT — Config.defaultTransport = "https" means the
// built-in WS transport is never registered.
#include <Courier.h>
#include <HttpTransport.h>

Courier::Config makeConfig() {
  Courier::Config cfg;
  cfg.host = "httpbin.org";        // send() endpoint host + time-sync host
  cfg.port = 443;
  cfg.path = "/anything";          // send() POSTs here
  cfg.apName = "CourierSetup";
  cfg.defaultTransport = "https";
  return cfg;
}

Courier::Client courier(makeConfig());

void setup() {
  Serial.begin(115200);

  auto& http = courier.addTransport<Courier::HttpTransport>("https");

  // Raw reply hook (fires for any reply body). Typed replies with a JSON
  // "type" field also reach courier.onMessage below.
  http.onMessage([](const char* payload, size_t len) {
    Serial.printf("send() reply: %.*s\n", (int)len, payload);
  });

  courier.onMessage([](const char* tname, const char* type, JsonDocument& doc) {
    Serial.printf("[%s] message type=%s\n", tname, type);
  });

  courier.onConnected([]() {
    auto& http = courier.transport<Courier::HttpTransport>("https");

    // JS-shaped fetch: full URL, options, Response.
    Courier::Response r = http.get("https://httpbin.org/get");
    Serial.printf("GET -> %d (%u bytes)\n", r.status, (unsigned)r.size());

    // Messaging idiom: POST to the configured endpoint.
    JsonDocument doc;
    doc["type"] = "hello";
    doc["uptime_ms"] = millis();
    courier.send(doc);
  });

  courier.setup();
}

void loop() {
  courier.loop();
}
