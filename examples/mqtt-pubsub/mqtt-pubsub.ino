#include <Courier.h>
#include <CourierMqttTransport.h>

CourierConfig makeConfig() {
  CourierConfig cfg;
  cfg.host = "broker.example.com";
  cfg.port = 443;
  cfg.path = "/ws";
  return cfg;
}

Courier courier(makeConfig());
CourierMqttTransport mqtt;

// Configure MQTT broker credentials via raw IDF config access
void configureMqtt() {
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
}

void setup() {
  Serial.begin(115200);

  mqtt.subscribe("sensors/+/data");
  mqtt.subscribe("commands/my-device");
  mqtt.setDefaultPublishTopic("sensors/my-device/data");
  mqtt.setClientId("my-device-001");

  configureMqtt();

  courier.onConnected([]() {
    Serial.println("Connected!");
  });

  courier.onMessage([](const char* type, JsonDocument& doc) {
    Serial.printf("Message type: %s\n", type);
  });

  // Add MQTT transport before setup
  courier.addTransport("mqtt", &mqtt);

  CourierEndpoint mqttEndpoint;
  mqttEndpoint.path = "/mqtt";
  courier.setEndpoint("mqtt", mqttEndpoint);

  courier.setup();
}

void loop() {
  courier.loop();

  // Publish sensor data every 10 seconds
  static unsigned long lastPublish = 0;
  if (millis() - lastPublish > 10000 && courier.isConnected()) {
    lastPublish = millis();
    mqtt.publish("sensors/my-device/data",
                 R"({"type":"reading","temp":22.5})");
  }

  // Dynamic subscription example
  static bool subscribed = false;
  if (!subscribed && courier.isConnected()) {
    subscribed = true;
    mqtt.subscribe("alerts/critical", 1);  // QoS 1
  }
}
