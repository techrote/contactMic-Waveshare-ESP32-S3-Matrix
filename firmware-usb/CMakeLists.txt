cmake_minimum_required(VERSION 3.16)
include(${CMAKE_CURRENT_LIST_DIR}/../firmware/cmake/toolchain_contract.cmake)
contactmic_check_toolchain()
set(IDF_TARGET "esp32s3" CACHE STRING "ContactMic USB target")
# Separate project and sdkconfig/build directory: never reuse the BLE cache.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../firmware/components/contactmic_board"
    "${CMAKE_CURRENT_LIST_DIR}/../firmware/components/contactmic_acquisition")
set(COMPONENTS main)
include("$ENV{IDF_PATH}/tools/cmake/project.cmake")
project(contactmic_usb)
