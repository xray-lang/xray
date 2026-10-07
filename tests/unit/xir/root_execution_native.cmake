# Real Source and CGen are confined to the producer.
set(RN_NATIVE_DIR "${CMAKE_BINARY_DIR}/generated/root_execution_native")
set(RN_NATIVE_C)
set(RN_NATIVE_FIXTURES)
foreach(fixture identity resume closing init_failure)
    list(APPEND RN_NATIVE_C "${RN_NATIVE_DIR}/${fixture}.c")
    list(APPEND RN_NATIVE_FIXTURES "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_root_execution_native/${fixture}/root.xr")
endforeach()
set(RN_NATIVE_H "${RN_NATIVE_DIR}/root_execution_native_generated.h")
add_executable(test_xir_root_execution_native_producer
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_root_execution_native_producer.c")
target_link_libraries(test_xir_root_execution_native_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_root_execution_native_producer PRIVATE
    XR_ROOT_NATIVE_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_root_execution_native")
add_custom_command(OUTPUT ${RN_NATIVE_C} "${RN_NATIVE_H}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RN_NATIVE_DIR}"
    COMMAND $<TARGET_FILE:test_xir_root_execution_native_producer> ${RN_NATIVE_C} "${RN_NATIVE_H}"
    DEPENDS test_xir_root_execution_native_producer ${RN_NATIVE_FIXTURES}
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/xir_root_execution_native_pipeline.h" VERBATIM)
add_executable(test_xir_root_execution_native
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_root_execution_native.c" ${RN_NATIVE_C} "${RN_NATIVE_H}")
target_include_directories(test_xir_root_execution_native PRIVATE "${RN_NATIVE_DIR}")
target_link_libraries(test_xir_root_execution_native PRIVATE xray_xir_scalar)
xr_enable_pure_aot_symbol_map(test_xir_root_execution_native)
foreach(target test_xir_root_execution_native_producer test_xir_root_execution_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(case root_identity resume closing init_failure)
    add_test(NAME "test_xir_root_execution_native_${case}" COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/verify_root_execution_native.py"
        --executable $<TARGET_FILE:test_xir_root_execution_native> --case "${case}")
    set_tests_properties("test_xir_root_execution_native_${case}" PROPERTIES TIMEOUT 120
        LABELS "unit;xir;root-execution;ownership;instance;generated-c;portability;abi")
endforeach()
