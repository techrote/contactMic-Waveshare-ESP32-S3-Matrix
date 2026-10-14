cmake_minimum_required(VERSION 3.16)
project(contactmic_native LANGUAGES C CXX)

include(CTest)
if(NOT BUILD_TESTING)
    message(FATAL_ERROR "This project is the native test harness; BUILD_TESTING must be ON.")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Later tests compile the production pure-core .c/.cpp sources directly.
# Never create a second implementation just for native tests.
add_library(contactmic_headers INTERFACE)
foreach(component IN ITEMS board acquisition protocol ble)
    target_include_directories(contactmic_headers INTERFACE
        "${CMAKE_CURRENT_LIST_DIR}/../../firmware/components/contactmic_${component}/include")
endforeach()

foreach(language IN ITEMS c cpp)
    add_executable(headers_${language} headers.${language})
    target_link_libraries(headers_${language} PRIVATE contactmic_headers)
    if(MSVC)
        target_compile_options(headers_${language} PRIVATE /W4 /WX)
    else()
        target_compile_options(headers_${language} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME native.headers_${language} COMMAND headers_${language})
endforeach()

add_executable(protocol_transport protocol_transport.c
    ../../firmware/components/contactmic_protocol/contactmic_protocol.c
    ../../firmware/components/contactmic_ble/contactmic_transport.c)
target_link_libraries(protocol_transport PRIVATE contactmic_headers)
target_compile_definitions(protocol_transport PRIVATE VECTOR_DIR="${CMAKE_CURRENT_LIST_DIR}/../vectors/v1")
if(MSVC)
    target_compile_options(protocol_transport PRIVATE /W4 /WX)
else()
    target_compile_options(protocol_transport PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME native.protocol_transport COMMAND protocol_transport)

add_executable(qmi_driver qmi_driver.c
    ../../firmware/components/contactmic_board/contactmic_board.c)
target_link_libraries(qmi_driver PRIVATE contactmic_headers)
if(MSVC)
    target_compile_options(qmi_driver PRIVATE /W4 /WX)
else()
    target_compile_options(qmi_driver PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME native.qmi_driver COMMAND qmi_driver)
