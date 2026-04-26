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

FetchContent_Declare(
    eztime
    GIT_REPOSITORY https://github.com/ropg/ezTime.git
    GIT_TAG 0.8.3
)
FetchContent_Declare(
    wifimanager
    GIT_REPOSITORY https://github.com/tzapu/WiFiManager.git
    GIT_TAG v2.0.17
)

# Populate (not MakeAvailable) — the upstreams ship Arduino-style CMakeLists
# we don't want add_subdirectory'd.
FetchContent_GetProperties(eztime)
if(NOT eztime_POPULATED)
    FetchContent_Populate(eztime)
endif()
FetchContent_GetProperties(wifimanager)
if(NOT wifimanager_POPULATED)
    FetchContent_Populate(wifimanager)
endif()

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
