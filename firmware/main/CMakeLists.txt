idf_component_register(
    SRCS "app_main.c"
    PRIV_REQUIRES log contactmic_board contactmic_acquisition contactmic_protocol contactmic_ble contactmic_session
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
