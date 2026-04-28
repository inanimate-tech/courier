#include <Courier.h>
#include <MqttTransport.h>

Courier::Config makeConfig() {
  Courier::Config cfg;
  cfg.host = "broker.example.com";
  cfg.port = 443;
  cfg.path = "/ws";
  cfg.defaultTransport = "mqtt";
  return cfg;
}

Courier::Client courier(makeConfig());

void setup() {
  Serial.begin(115200);

  // Build the MQTT transport via Client. addTransport<T> constructs in
  // place, registers under the name, and returns a typed reference.
  Courier::MqttTransport::Config mqttCfg;
  mqttCfg.topics = {"sensors/+/data", "commands/my-device"};
  mqttCfg.clientId = "my-device-001";
  auto& mqtt = courier.addTransport<Courier::MqttTransport>("mqtt", mqttCfg);

  mqtt.onConfigure([](esp_mqtt_client_config_t& cfg) {
    // Set username/password, LWT, keepalive, etc.
    // These fields map directly to the ESP-IDF MQTT client config.
    // See: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mqtt.html
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    cfg.credentials.username = "my-user";
    cfg.credentials.authentication.password = "my-password";
    cfg.session.last_will.topic = "devices/my-device/status";
    cfg.session.last_will.msg = "offline";
#else
    cfg.username = "my-user";
    cfg.password = "my-password";
#endif
  });

  courier.onConnected([]() {
    Serial.println("Connected!");
  });

  courier.onMessage([](const char* tname, const char* type, JsonDocument& doc) {
    Serial.printf("[%s] type=%s\n", tname, type);
  });

  courier.setup();
}

void loop() {
  courier.loop();

  // Publish sensor data every 10 seconds via the default transport.
  static unsigned long lastPublish = 0;
  if (millis() - lastPublish > 10000 && courier.isConnected()) {
    lastPublish = millis();
    JsonDocument doc;
    doc["type"] = "reading";
    doc["temp"] = 22.5;
    Courier::SendOptions opts;
    opts.topic = "sensors/my-device/data";
    courier.send(doc, opts);
  }

  // Dynamic subscription example
  static bool subscribed = false;
  if (!subscribed && courier.isConnected()) {
    subscribed = true;
    courier.transport<Courier::MqttTransport>("mqtt").subscribe("alerts/critical", 1);
  }
}
