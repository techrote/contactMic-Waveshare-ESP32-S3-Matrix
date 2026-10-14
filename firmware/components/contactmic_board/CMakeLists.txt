idf_component_register(SRCS "contactmic_board.c" "contactmic_board_idf.c"
                       INCLUDE_DIRS "include" REQUIRES esp_driver_i2c)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
