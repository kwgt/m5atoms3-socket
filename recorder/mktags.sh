#! /bin/sh

PLATFORM_PREFIX="${HOME}/.platformio/packages/framework-arduinoespressif32"

ctags -R \
    include \
    src \
    .pio/libdeps/m5stack-atom \
    "${PLATFORM_PREFIX}/tools/sdk/esp32" \
    "${PLATFORM_PREFIX}/libraries"
