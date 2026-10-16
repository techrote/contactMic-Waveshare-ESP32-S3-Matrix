idf_component_register(SRCS "cm_usb.c" "cm_usb_descriptors.c" "cm_usb_idf.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES contactmic_board contactmic_acquisition freertos esp_timer
                  esp_hw_support esp_app_format esp_system esp_mm usb esp_driver_gpio)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(${CMAKE_CURRENT_LIST_DIR}/dependencies.lock.cmake)
    include(FetchContent)
    FetchContent_Declare(cm009_tinyusb
        GIT_REPOSITORY ${CMU_TINYUSB_REPOSITORY}
        GIT_TAG ${CMU_TINYUSB_COMMIT}
        GIT_SUBMODULES ""
        UPDATE_DISCONNECTED TRUE)
    FetchContent_GetProperties(cm009_tinyusb)
    if(NOT cm009_tinyusb_POPULATED)
        FetchContent_Populate(cm009_tinyusb)
    endif()
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${cm009_tinyusb_SOURCE_DIR}
        OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(NOT actual STREQUAL CMU_TINYUSB_COMMIT)
        message(FATAL_ERROR "CM-009 TinyUSB dependency identity mismatch")
    endif()
    foreach(source IN LISTS CMU_TINYUSB_SOURCE_FILES)
        target_sources(${COMPONENT_LIB} PRIVATE ${cm009_tinyusb_SOURCE_DIR}/${source})
    endforeach()
    target_include_directories(${COMPONENT_LIB} PRIVATE ${cm009_tinyusb_SOURCE_DIR}/src)
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT revision MATCHES "^[0-9a-f]+$")
        set(revision unknown)
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CMU_SOURCE_REVISION="${revision}")
    # Keep strict project warnings without weakening upstream or other components.
    set_source_files_properties(cm_usb.c cm_usb_descriptors.c cm_usb_idf.c
        PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
endif()
