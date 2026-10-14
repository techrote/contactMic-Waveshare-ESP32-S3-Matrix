cmake_minimum_required(VERSION 3.16)

include(cmake/toolchain_contract.cmake)
contactmic_check_toolchain()
# Do not silently replace an explicitly requested or cached target.
set(IDF_TARGET "esp32s3" CACHE STRING "ContactMic firmware target")

include("$ENV{IDF_PATH}/tools/cmake/project.cmake")
project(contactmic)
