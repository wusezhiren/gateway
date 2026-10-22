cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 版本号可在构建时覆盖：idf.py -DPROJECT_VER=0.2.1 build
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "0.3.1")
endif()
# 工程名区分固件种类，节点只接受同名固件的升级：不同硬件配置的板子要用不同的名字，
# 例如 Korvo 板：idf.py -DNODE_PROJECT=esp_s3_korvo ...（不超过 16 个字符，协议里只传 16 个）
if(NOT DEFINED NODE_PROJECT)
    set(NODE_PROJECT esp_s3_node)
endif()
project(${NODE_PROJECT})
