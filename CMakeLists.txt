idf_component_register(
    SRCS "src/Courier.cpp"
         "src/WebSocketTransport.cpp"
         "src/MqttTransport.cpp"
         "src/UdpTransport.cpp"
    INCLUDE_DIRS "src"
    REQUIRES espressif__arduino-esp32 espressif__esp_websocket_client mqtt ezTime ArduinoJson WiFiManager
    PRIV_REQUIRES esp_http_client esp-tls
)
