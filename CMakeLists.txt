idf_component_register(
    SRCS "src/Courier.cpp"
         "src/CourierWSTransport.cpp"
         "src/CourierMqttTransport.cpp"
         "src/CourierUDPTransport.cpp"
    INCLUDE_DIRS "src"
    REQUIRES espressif__arduino-esp32 espressif__esp_websocket_client mqtt ezTime ArduinoJson WiFiManager
    PRIV_REQUIRES esp_http_client esp-tls
)
