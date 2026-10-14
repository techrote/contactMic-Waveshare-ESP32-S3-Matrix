idf_component_register(
    SRCS "contactmic_acquisition.c" "contactmic_acquisition_idf.c"
    INCLUDE_DIRS "include"
    REQUIRES contactmic_board esp_driver_gpio esp_timer freertos
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
