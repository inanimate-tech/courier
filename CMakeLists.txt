idf_component_register(
    SRCS "src/Courier.cpp"
         "src/CourierWSTransport.cpp"
         "src/CourierMqttTransport.cpp"
         "src/CourierUDPTransport.cpp"
    INCLUDE_DIRS "src"
    REQUIRES espressif__arduino-esp32 espressif__esp_websocket_client mqtt bblanchon__arduinojson
    PRIV_REQUIRES esp_http_client esp-tls esp_wifi esp_https_server
)

# ezTime and WiFiManager aren't on the ESP Component Registry, and the registry
# rejects git: deps in idf_component.yml. We vendor them via CMake FetchContent
# instead — sources land in the build dir at configure time and compile straight
# into this component. FetchContent must come *after* idf_component_register
# (see espressif/esp-idf#9929).
include(FetchContent)

# SOURCE_SUBDIR points at a non-existent path so MakeAvailable populates the
# sources without calling add_subdirectory — WiFiManager ships an
# idf_component_register CMakeLists.txt that expects to be its own top-level
# component, which would conflict with vendoring its sources into courier.
FetchContent_Declare(
    eztime
    GIT_REPOSITORY https://github.com/ropg/ezTime.git
    GIT_TAG 0.8.3
    SOURCE_SUBDIR _populate_only_
)
FetchContent_Declare(
    wifimanager
    GIT_REPOSITORY https://github.com/tzapu/WiFiManager.git
    GIT_TAG v2.0.17
    SOURCE_SUBDIR _populate_only_
)
FetchContent_MakeAvailable(eztime wifimanager)

target_sources(${COMPONENT_LIB} PRIVATE
    "${eztime_SOURCE_DIR}/src/ezTime.cpp"
    "${wifimanager_SOURCE_DIR}/WiFiManager.cpp"
)
target_include_directories(${COMPONENT_LIB} PUBLIC
    "${eztime_SOURCE_DIR}/src"
    "${wifimanager_SOURCE_DIR}"
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-int-in-bool-context
    -Wno-deprecated-declarations
    -Wno-format
)
