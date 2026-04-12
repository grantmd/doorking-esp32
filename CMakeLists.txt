# Top-level ESP-IDF project file.
# See https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/build-system.html
cmake_minimum_required(VERSION 3.16)

# Extract version from the most recent git tag. On a tagged commit this
# produces e.g. "0.5.0"; on a development commit it produces something
# like "0.5.0-3-gabcdef". Falls back to "0.0.0-dev" for detached/shallow
# checkouts with no tags. ESP-IDF reads PROJECT_VER and bakes it into the
# binary's esp_app_desc_t, readable at runtime via esp_app_get_description().
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_VERSION)
    set(GIT_VERSION "0.0.0-dev")
endif()
string(REGEX REPLACE "^v" "" PROJECT_VER "${GIT_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(doorking)
