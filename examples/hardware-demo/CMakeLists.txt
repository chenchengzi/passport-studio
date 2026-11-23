# Official FoloToy AI Passport hardware-test demo (display, keys, audio,
# battery, Wi-Fi, BLE, low power), kept as a hardware reference and a board
# self-check. Derivative applications must design their own UI; see AGENTS.md.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/bsp")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Hardware-Demo)
