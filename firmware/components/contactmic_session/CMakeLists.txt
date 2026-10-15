idf_component_register(SRCS "contactmic_session.c" "contactmic_session_idf.c"
    INCLUDE_DIRS "include" REQUIRES contactmic_acquisition contactmic_ble)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
