# Resident Bluetooth installer. Its build also produces the bootloader (with
# bootloader_components/installer_hook) and partition table; all three are
# combined into the first-install image by tools/build_installer_bundle.py.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../components/bsp"
    "${CMAKE_CURRENT_LIST_DIR}/../components/installer_core")
set(PROJECT_VER "1.0.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Passport-Installer)
