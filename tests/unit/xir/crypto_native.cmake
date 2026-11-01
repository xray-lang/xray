add_executable(test_xir_crypto_native xir/test_xir_crypto_native.c)
target_link_libraries(test_xir_crypto_native PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(crypto_demo_c "${CMAKE_CURRENT_BINARY_DIR}/generated/crypto_demo.c")
set(crypto_import_c "${CMAKE_CURRENT_BINARY_DIR}/generated/crypto_import.c")
add_custom_command(OUTPUT "${crypto_demo_c}" "${crypto_import_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_crypto_native> "${PROJECT_SOURCE_DIR}" "${crypto_demo_c}" "${crypto_import_c}"
    DEPENDS test_xir_crypto_native "${PROJECT_SOURCE_DIR}/demos/07-advanced/crypto_entropy.xr"
        "${PROJECT_SOURCE_DIR}/stdlib/crypto/crypto.xr" VERBATIM)
add_executable(test_xir_crypto_native_c xir/test_xir_crypto_native_c.c "${crypto_demo_c}" "${crypto_import_c}")
target_link_libraries(test_xir_crypto_native_c PRIVATE xray_xir_scalar)
foreach(target test_xir_crypto_native test_xir_crypto_native_c)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    endif()
endforeach()
add_test(NAME xir_crypto_native COMMAND test_xir_crypto_native "${PROJECT_SOURCE_DIR}")
add_test(NAME xir_crypto_native_c COMMAND test_xir_crypto_native_c)
set_tests_properties(xir_crypto_native xir_crypto_native_c PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;ownership;generated-c")

add_executable(test_xir_crypto_suite xir/test_xir_crypto_suite.c)
target_link_libraries(test_xir_crypto_suite PRIVATE xray_xir_source_product)
set(crypto_suite_dir "${CMAKE_CURRENT_BINARY_DIR}/crypto-suite")
set(crypto_suite_c)
foreach(i RANGE 0 6)
    list(APPEND crypto_suite_c "${crypto_suite_dir}/crypto_suite_${i}.c")
endforeach()
file(GLOB crypto_suite_inputs CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/140*_crypto_*.xr")
add_custom_command(OUTPUT ${crypto_suite_c}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${crypto_suite_dir}"
    COMMAND $<TARGET_FILE:test_xir_crypto_suite> "${PROJECT_SOURCE_DIR}" "${crypto_suite_dir}"
        "${crypto_suite_dir}/emitted.json" --emit-only
    DEPENDS test_xir_crypto_suite ${crypto_suite_inputs} "${PROJECT_SOURCE_DIR}/stdlib/crypto/crypto.xr" VERBATIM)
add_executable(test_xir_crypto_suite_native xir/test_xir_crypto_suite_native.c ${crypto_suite_c})
target_link_libraries(test_xir_crypto_suite_native PRIVATE xray_xir_scalar)
foreach(target test_xir_crypto_suite test_xir_crypto_suite_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    endif()
endforeach()
set(crypto_suite_observation "${CMAKE_CURRENT_BINARY_DIR}/crypto-suite-observation")
file(MAKE_DIRECTORY "${crypto_suite_observation}")
add_test(NAME xir_crypto_suite COMMAND test_xir_crypto_suite "${PROJECT_SOURCE_DIR}" "${crypto_suite_observation}"
    "${crypto_suite_observation}/vm-report.json")
add_test(NAME xir_crypto_suite_native COMMAND test_xir_crypto_suite_native "${crypto_suite_dir}/native-report.json")
set_tests_properties(xir_crypto_suite xir_crypto_suite_native PROPERTIES TIMEOUT 300 LABELS "unit;xir;source;ownership;generated-c")

add_executable(test_xir_crypto_values xir/test_xir_crypto_values.c)
target_link_libraries(test_xir_crypto_values PRIVATE xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_crypto_values PRIVATE /W4 /WX /utf-8)
endif()
add_test(NAME xir_crypto_values COMMAND test_xir_crypto_values)
set_tests_properties(xir_crypto_values PROPERTIES TIMEOUT 120 LABELS "unit;xir;ownership")

add_executable(test_xir_crypto_controls xir/test_xir_crypto_controls.c)
target_link_libraries(test_xir_crypto_controls PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen xray_xir_ctfe)
set(crypto_controls_c "${CMAKE_CURRENT_BINARY_DIR}/generated/crypto_controls.c")
add_custom_command(OUTPUT "${crypto_controls_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_crypto_controls> "${PROJECT_SOURCE_DIR}" "${crypto_controls_c}"
    DEPENDS test_xir_crypto_controls "${CMAKE_CURRENT_LIST_DIR}/fixtures/crypto_owner.xr"
        "${CMAKE_CURRENT_LIST_DIR}/fixtures/crypto_context_library.xr" VERBATIM)
add_executable(test_xir_crypto_controls_native xir/test_xir_crypto_controls_native.c "${crypto_controls_c}")
target_link_libraries(test_xir_crypto_controls_native PRIVATE xray_xir_scalar)
foreach(target test_xir_crypto_controls test_xir_crypto_controls_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    endif()
endforeach()
add_test(NAME xir_crypto_controls COMMAND test_xir_crypto_controls "${PROJECT_SOURCE_DIR}")
add_test(NAME xir_crypto_controls_native COMMAND test_xir_crypto_controls_native)
set_tests_properties(xir_crypto_controls xir_crypto_controls_native PROPERTIES TIMEOUT 120 LABELS "unit;xir;source;ownership;generated-c")
