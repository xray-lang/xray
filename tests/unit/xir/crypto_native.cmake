

set(crypto_demo_c "${CMAKE_CURRENT_BINARY_DIR}/generated/crypto_demo.c")
set(crypto_import_c "${CMAKE_CURRENT_BINARY_DIR}/generated/crypto_import.c")










set(crypto_suite_dir "${CMAKE_CURRENT_BINARY_DIR}/crypto-suite")
set(crypto_suite_c)
foreach(i RANGE 0 6)
    list(APPEND crypto_suite_c "${crypto_suite_dir}/crypto_suite_${i}.c")
endforeach()
file(GLOB crypto_suite_inputs CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tests/regression/10_stdlib/140*_crypto_*.xr")




set(crypto_suite_observation "${CMAKE_CURRENT_BINARY_DIR}/crypto-suite-observation")
file(MAKE_DIRECTORY "${crypto_suite_observation}")




add_executable(test_xir_crypto_values xir/test_xir_crypto_values.c)
target_link_libraries(test_xir_crypto_values PRIVATE xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_crypto_values PRIVATE /W4 /WX /utf-8)
endif()
add_test(NAME xir_crypto_values COMMAND test_xir_crypto_values)
set_tests_properties(xir_crypto_values PROPERTIES TIMEOUT 120 LABELS "unit;xir;ownership")



set(crypto_controls_c "${CMAKE_CURRENT_BINARY_DIR}/generated/crypto_controls.c")
