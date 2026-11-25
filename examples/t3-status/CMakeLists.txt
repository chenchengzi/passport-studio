# T3 Code status display: shows running and waiting T3 Code threads from the
# bridge on the Mac (bridge/bridge.mjs). Build, then install the merged image
# through the iPhone app.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/bsp"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/installer_core"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/passport_wifi"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/passport_link")
set(PROJECT_VER "0.4.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(T3-Status)
