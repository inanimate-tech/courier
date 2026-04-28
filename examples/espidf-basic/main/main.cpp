// ESP-IDF example — demonstrates Courier with both WebSocket and MQTT transports.
// This uses the Arduino component for ESP-IDF (not Arduino IDE).

#include <Arduino.h>
#include <Courier.h>
#include <MqttTransport.h>

Courier::Config makeConfig() {
    Courier::Config cfg;
    cfg.host = "example.com";
    cfg.port = 443;
    cfg.path = "/ws";
    return cfg;
}

Courier::Client courier(makeConfig());

extern "C" void app_main() {
    // Bring up the Arduino runtime (Wi-Fi stack, timers, etc.) that courier
    // relies on. Required because CONFIG_AUTOSTART_ARDUINO is off — this
    // example uses app_main() directly instead of Arduino's setup()/loop().
    initArduino();

    Courier::MqttTransport::Config mqttCfg;
    mqttCfg.topics = {"devices/my-device/command"};
    mqttCfg.clientId = "my-device-001";
    courier.addTransport<Courier::MqttTransport>("mqtt", mqttCfg);

    courier.onConnected([]() {
        courier.transport<Courier::WebSocketTransport>("ws")
            .send(R"({"type":"hello"})");
    });

    courier.onMessage([](const char* type, JsonDocument& doc) {
        // Handle incoming messages by type
    });

    courier.onError([](const char* category, const char* message) {
        // Handle errors
    });

    courier.setup();

    while (true) {
        courier.loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
