// ESP-IDF example — demonstrates Courier with both WebSocket and MQTT transports.
// This uses the Arduino component for ESP-IDF (not Arduino IDE).

#include <Courier.h>
#include <CourierMqttTransport.h>

CourierConfig makeConfig() {
    CourierConfig cfg;
    cfg.host = "example.com";
    cfg.port = 443;
    cfg.path = "/ws";
    return cfg;
}

Courier courier(makeConfig());
CourierMqttTransport mqtt;

extern "C" void app_main() {
    mqtt.subscribe("devices/my-device/command");
    mqtt.setDefaultPublishTopic("devices/my-device/event");
    mqtt.setClientId("my-device-001");

    courier.onConnected([]() {
        courier.send("{\"type\":\"hello\"}");
    });

    courier.onMessage([](const char* type, JsonDocument& doc) {
        // Handle incoming messages by type
    });

    courier.onError([](const char* category, const char* message) {
        // Handle errors
    });

    courier.addTransport("mqtt", &mqtt);
    courier.setup();

    while (true) {
        courier.loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
