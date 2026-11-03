set(fixed_scalar_dir "${CMAKE_CURRENT_LIST_DIR}")
set(fixed_scalar_c "${CMAKE_CURRENT_BINARY_DIR}/generated/fixed_array_scalar.c")
add_executable(test_fixed_array_scalar_source "${fixed_scalar_dir}/test_fixed_array_scalar_source.c")
target_link_libraries(test_fixed_array_scalar_source PRIVATE xray_xir_source_product)
target_compile_definitions(test_fixed_array_scalar_source PRIVATE XR_FIXED_ARRAY_ROOT="${PROJECT_SOURCE_DIR}")
add_custom_command(OUTPUT "${fixed_scalar_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_fixed_array_scalar_source> "${fixed_scalar_c}"
    DEPENDS test_fixed_array_scalar_source "${fixed_scalar_dir}/fixed_array_scalar_product.h"
    VERBATIM)
add_executable(test_fixed_array_scalar_native
    "${fixed_scalar_dir}/test_fixed_array_scalar_native.c" "${fixed_scalar_c}")
target_link_libraries(test_fixed_array_scalar_native PRIVATE xray_xir_scalar)
add_executable(test_fixed_array_scalar_value
    "${fixed_scalar_dir}/test_fixed_array_scalar_value.c"
    "${fixed_scalar_dir}/../fixed_bytes_value_alloc.c")
target_link_libraries(test_fixed_array_scalar_value PRIVATE xray_xir_scalar)
foreach(target test_fixed_array_scalar_source test_fixed_array_scalar_native test_fixed_array_scalar_value)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
add_test(NAME fixed_array_scalar_source COMMAND test_fixed_array_scalar_source)
add_test(NAME fixed_array_scalar_native COMMAND test_fixed_array_scalar_native)
add_test(NAME fixed_array_scalar_value COMMAND test_fixed_array_scalar_value)
set_tests_properties(fixed_array_scalar_source fixed_array_scalar_native fixed_array_scalar_value
    PROPERTIES TIMEOUT 180 LABELS "unit;xir;fixed;source;native;ownership")
