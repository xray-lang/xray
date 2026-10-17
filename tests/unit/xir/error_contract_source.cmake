add_executable(test_xir_error_contract_source ${CMAKE_CURRENT_LIST_DIR}/test_xir_error_contract_source.c)
target_include_directories(test_xir_error_contract_source PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_error_contract_source PRIVATE xray_xir_source)
if(MSVC)
 target_compile_options(test_xir_error_contract_source PRIVATE /W4 /WX /utf-8)
else()
 target_compile_options(test_xir_error_contract_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_error_contract_source COMMAND test_xir_error_contract_source)
set_tests_properties(test_xir_error_contract_source PROPERTIES
 TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;source;root-effects;errors;ownership")
foreach(XR_ERROR_CONTRACT_CASE RANGE 0 3)
 add_test(NAME test_xir_error_contract_source_compiler_${XR_ERROR_CONTRACT_CASE}
  COMMAND test_xir_error_contract_source --compiler ${XR_ERROR_CONTRACT_CASE})
 set_tests_properties(test_xir_error_contract_source_compiler_${XR_ERROR_CONTRACT_CASE} PROPERTIES
  TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;source;errors;compiler-fault;ownership")
endforeach()
