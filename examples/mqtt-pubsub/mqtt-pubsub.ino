#include <Courier.h>
#include <CourierMqttTransport.h>

Courier courier({
  .host = "broker.example.com",
  .port = 443,
  .path = "/ws"
});

CourierMqttTransport mqtt({
  .topics = {"sensors/+/data", "commands/my-device"},
  .defaultPublishTopic = "sensors/my-device/data",
  .clientId = "my-device-001",
  .cert_pem = nullptr  // use default certificate bundle
});

// Configure MQTT broker credentials via raw IDF config access
void configureMqtt() {
  mqtt.onConfigure([](esp_mqtt_client_config_t& config) {
    // Set username/password, LWT, keepalive, etc.
    // These fields map directly to the ESP-IDF MQTT client config.
    // See: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mqtt.html
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    config.credentials.username = "my-user";
    config.credentials.authentication.password = "my-password";
    config.session.last_will.topic = "devices/my-device/status";
    config.session.last_will.msg = "offline";
#else
    config.username = "my-user";
    config.password = "my-password";
#endif
  });
}

void setup() {
  Serial.begin(115200);

  configureMqtt();

  courier.onConnected([]() {
    Serial.println("Connected!");
  });

  courier.onMessage([](const char* type, JsonDocument& doc) {
    Serial.printf("Message type: %s\n", type);
  });

  // Add MQTT transport before setup
  courier.addTransport("mqtt", &mqtt);
  courier.setEndpoint("mqtt", {.path = "/mqtt"});

  courier.setup();
}

void loop() {
  courier.loop();

  // Publish sensor data every 10 seconds
  static unsigned long lastPublish = 0;
  if (millis() - lastPublish > 10000 && courier.isConnected()) {
    lastPublish = millis();
    mqtt.publishTo("sensors/my-device/data",
                   "{\"type\":\"reading\",\"temp\":22.5}");
  }

  // Dynamic subscription example
  static bool subscribed = false;
  if (!subscribed && courier.isConnected()) {
    subscribed = true;
    mqtt.subscribe("alerts/critical", 1);  // QoS 1
  }
}
