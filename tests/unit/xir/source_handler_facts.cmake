add_executable(test_xir_source_handler_facts ${CMAKE_CURRENT_LIST_DIR}/test_xir_source_handler_facts.c)
target_include_directories(test_xir_source_handler_facts PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(test_xir_source_handler_facts PRIVATE xray_xir_source)
if(MSVC)
 target_compile_options(test_xir_source_handler_facts PRIVATE /W4 /WX /utf-8)
else()
 target_compile_options(test_xir_source_handler_facts PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_handler_facts COMMAND test_xir_source_handler_facts)
set_tests_properties(test_xir_source_handler_facts PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
 LABELS "unit;xir;source;errors;ownership;metadata")
foreach(XR_HANDLER_FACTS_CASE RANGE 0 5)
 add_test(NAME test_xir_source_handler_facts_compiler_${XR_HANDLER_FACTS_CASE}
  COMMAND test_xir_source_handler_facts --compiler ${XR_HANDLER_FACTS_CASE})
 set_tests_properties(test_xir_source_handler_facts_compiler_${XR_HANDLER_FACTS_CASE} PROPERTIES
  TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;source;errors;compiler-fault;ownership")
endforeach()
