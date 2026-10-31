cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 版本号可在构建时覆盖：idf.py -DPROJECT_VER=0.2.0 build
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.6.0")
endif()
project(esp_s3_gateway)
