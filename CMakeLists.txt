cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(deye_display)

# The SPIFFS "storage" image is built from data/ and flashed by PlatformIO's
# native filesystem tooling (board_build.filesystem = spiffs; `-t buildfs` /
# `-t uploadfs`). We deliberately do NOT use IDF's
# spiffs_create_partition_image(... FLASH_IN_PROJECT) here: PlatformIO drives
# its own upload path and never invokes IDF's `flash` target, so that image
# would be built but never flashed.
