#!/usr/bin/env bash
# Fetches Arduino library dependencies required by courier and wraps each as
# an ESP-IDF component under examples/espidf-basic/components/. These libs
# aren't on the ESP Component Registry, so a pure `idf.py build` can't resolve
# them. Keeping them out of git avoids vendoring third-party source.
#
# Safe to re-run — clones are skipped if the target dir already exists.
set -euo pipefail

DIR="$(cd "$(dirname "$0")/.." && pwd)"
COMPONENTS="$DIR/components"
mkdir -p "$COMPONENTS"

fetch() {
    local name="$1" url="$2" ref="$3"
    local target="$COMPONENTS/$name"
    if [[ -d "$target" ]]; then
        echo "  $name already present — skipping"
        return
    fi
    echo "  fetching $name @ $ref"
    git clone --depth=1 --branch="$ref" --quiet "$url" "$target"
    rm -rf "$target/.git"
}

echo "Fetching Arduino library deps into $COMPONENTS"
fetch ArduinoJson   https://github.com/bblanchon/ArduinoJson.git v7.4.3
fetch ezTime        https://github.com/ropg/ezTime.git           0.8.3
fetch WiFiManager   https://github.com/tzapu/WiFiManager.git     v2.0.17

# Shim CMakeLists for each — the upstream repos ship Arduino/PlatformIO
# build files, not IDF ones.
cat > "$COMPONENTS/ArduinoJson/CMakeLists.txt" <<'EOF'
idf_component_register(INCLUDE_DIRS "src")
EOF

cat > "$COMPONENTS/ezTime/CMakeLists.txt" <<'EOF'
idf_component_register(
    SRCS "src/ezTime.cpp"
    INCLUDE_DIRS "src"
    REQUIRES espressif__arduino-esp32
)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-int-in-bool-context
    -Wno-deprecated-declarations
)
EOF

cat > "$COMPONENTS/WiFiManager/CMakeLists.txt" <<'EOF'
idf_component_register(
    SRCS "WiFiManager.cpp"
    INCLUDE_DIRS "."
    REQUIRES espressif__arduino-esp32
    PRIV_REQUIRES esp_wifi esp_https_server
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)
EOF

echo "Done."
