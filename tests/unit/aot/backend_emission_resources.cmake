# Actual production emission, with only its physical allocator observed.
add_library(backend_emission_observed OBJECT ${CMAKE_SOURCE_DIR}/src/aot/program/xr_backend_ir_emit_c.c)
target_include_directories(backend_emission_observed PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_BINARY_DIR}/generated)
add_dependencies(backend_emission_observed xray_program_aot_compiler)
if(MSVC)
    target_compile_options(backend_emission_observed PRIVATE /W4 /WX /FI${CMAKE_CURRENT_SOURCE_DIR}/aot/xr_backend_emission_probe.h)
else()
    target_compile_options(backend_emission_observed PRIVATE -Wall -Wextra -Werror -include ${CMAKE_CURRENT_SOURCE_DIR}/aot/xr_backend_emission_probe.h)
endif()
add_executable(test_backend_emission_resources aot/test_backend_emission_resources.c
    $<TARGET_OBJECTS:backend_emission_observed> ${XR_TARGET_PROFILE_TEST_FIXTURE}
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_target_profile.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_runtime_target_authority.c
    ${CMAKE_SOURCE_DIR}/src/runtime/abi/xr_stdlib_provider_projection.c)
target_include_directories(test_backend_emission_resources PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_backend_emission_resources PRIVATE xr_program_test_core xray_program_vm_runtime xray_program_aot_compiler)
if(MSVC)
    target_compile_options(test_backend_emission_resources PRIVATE /W4 /WX)
else()
    target_compile_options(test_backend_emission_resources PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_backend_emission_resources COMMAND test_backend_emission_resources)
set_tests_properties(test_backend_emission_resources PROPERTIES TIMEOUT 300 LABELS "unit;aot;compiler;ownership;generated-c")
set(BACKEND_EMISSION_NATIVE_C ${CMAKE_CURRENT_BINARY_DIR}/backend-emission-native.c)
set(BACKEND_EMISSION_NATIVE_HEADER ${BACKEND_EMISSION_NATIVE_C}.h)
add_custom_command(OUTPUT ${BACKEND_EMISSION_NATIVE_C} ${BACKEND_EMISSION_NATIVE_HEADER}
    COMMAND $<TARGET_FILE:test_backend_emission_resources> ${BACKEND_EMISSION_NATIVE_C}
    DEPENDS test_backend_emission_resources VERBATIM)
add_executable(test_backend_emission_native ${BACKEND_EMISSION_NATIVE_C})
if(MSVC)
    target_compile_options(test_backend_emission_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_backend_emission_native PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_backend_emission_native COMMAND test_backend_emission_native)
set_tests_properties(test_backend_emission_native PROPERTIES TIMEOUT 30 LABELS "aot;native;generated-c")

set(BACKEND_EMISSION_EXPORT_C ${CMAKE_CURRENT_BINARY_DIR}/backend-emission-export.c)
add_custom_command(OUTPUT ${BACKEND_EMISSION_EXPORT_C} ${BACKEND_EMISSION_EXPORT_C}.h ${BACKEND_EMISSION_EXPORT_C}.consumer.c
    COMMAND $<TARGET_FILE:test_backend_emission_resources> --exports ${BACKEND_EMISSION_EXPORT_C}
    DEPENDS test_backend_emission_resources VERBATIM)
add_executable(test_backend_emission_header_native ${BACKEND_EMISSION_EXPORT_C} ${BACKEND_EMISSION_EXPORT_C}.consumer.c)
if(MSVC)
    target_compile_options(test_backend_emission_header_native PRIVATE /W4 /WX)
else()
    target_compile_options(test_backend_emission_header_native PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_backend_emission_header_native COMMAND test_backend_emission_header_native)
set_tests_properties(test_backend_emission_header_native PROPERTIES TIMEOUT 30 LABELS "aot;native;generated-c;abi")

add_test(NAME test_backend_emission_fatal COMMAND ${CMAKE_COMMAND}
    -DPROGRAM=$<TARGET_FILE:test_backend_emission_resources>
    -DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/aot/check_backend_emission_ice.cmake)
set_tests_properties(test_backend_emission_fatal PROPERTIES TIMEOUT 30 LABELS "unit;aot;compiler;generated-c")
