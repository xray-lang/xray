add_executable(test_xir_root_callback xir/test_xir_root_callback.c)
target_link_libraries(test_xir_root_callback PRIVATE xray_xir_source xray_xir_vm)
set_target_properties(test_xir_root_callback PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_root_callback PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_root_callback PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_root_callback PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
set(root_callback_inputs "${CMAKE_CURRENT_BINARY_DIR}/root-callback-inputs")
file(MAKE_DIRECTORY "${root_callback_inputs}")
add_test(NAME test_xir_root_callback COMMAND test_xir_root_callback
    "${PROJECT_SOURCE_DIR}/stdlib" "${root_callback_inputs}")
set_tests_properties(test_xir_root_callback PROPERTIES
    LABELS "unit;xir;callback;ownership;runtime" TIMEOUT 120)
