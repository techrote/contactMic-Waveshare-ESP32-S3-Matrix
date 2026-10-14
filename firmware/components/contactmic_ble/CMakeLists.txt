idf_component_register(SRCS "contactmic_transport.c" "contactmic_ble_idf.c"
                       INCLUDE_DIRS "include" REQUIRES contactmic_protocol
                       PRIV_REQUIRES bt nvs_flash esp_hw_support)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
