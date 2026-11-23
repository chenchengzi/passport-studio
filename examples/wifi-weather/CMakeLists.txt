# Wi-Fi weather: test application for the Bluetooth installer's Wi-Fi
# provisioning and Passport Link. Build, then install the merged image through the iPhone app.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/bsp"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/installer_core"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/passport_wifi"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/passport_link")
set(PROJECT_VER "1.2.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Wifi-Weather)
