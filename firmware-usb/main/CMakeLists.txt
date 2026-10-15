idf_component_register(SRCS "app_main.c" PRIV_REQUIRES contactmic_board contactmic_usb log)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
