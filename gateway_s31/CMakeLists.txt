cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../listen_clouds")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(NOT IDF_TARGET STREQUAL "esp32s31")
    message(FATAL_ERROR "gateway_s31 requires ESP32-S31: idf.py --preview set-target esp32s31")
endif()
project(esp_s31_gateway)
