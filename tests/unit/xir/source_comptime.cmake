# Complete Source constants use the same sole compiler and scalar interpreter.
add_test(NAME test_xir_source_type_info COMMAND test_xir_source_comptime --type-info)
set_tests_properties(test_xir_source_type_info PROPERTIES TIMEOUT 60 LABELS "unit;xir;source;ctfe;description;ownership")
add_executable(test_xir_type_info_library "${CMAKE_CURRENT_LIST_DIR}/test_xir_type_info_library.c")
target_link_libraries(test_xir_type_info_library PRIVATE xray_xir_source xray_xir_vm)
set_target_properties(test_xir_type_info_library PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
set(XR_TYPE_INFO_WORK "${CMAKE_CURRENT_BINARY_DIR}/type-info-library-work")
file(MAKE_DIRECTORY "${XR_TYPE_INFO_WORK}")
target_compile_definitions(test_xir_type_info_library PRIVATE XR_TYPE_INFO_WORK="${XR_TYPE_INFO_WORK}")
if(MSVC)
    target_compile_options(test_xir_type_info_library PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_type_info_library PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_type_info_library COMMAND test_xir_type_info_library)
set_tests_properties(test_xir_type_info_library PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;description;library;ownership")
add_executable(test_xir_generated_data "${CMAKE_CURRENT_LIST_DIR}/test_xir_generated_data.c")
target_link_libraries(test_xir_generated_data PRIVATE xray_xir_source xray_xir_vm)
set_target_properties(test_xir_generated_data PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
set(XR_GENERATED_DATA_WORK "${CMAKE_CURRENT_BINARY_DIR}/generated-data-work")
file(MAKE_DIRECTORY "${XR_GENERATED_DATA_WORK}")
target_compile_definitions(test_xir_generated_data PRIVATE XR_GENERATED_DATA_WORK="${XR_GENERATED_DATA_WORK}")
if(MSVC)
    target_compile_options(test_xir_generated_data PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_generated_data PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_generated_data COMMAND test_xir_generated_data)
set_tests_properties(test_xir_generated_data PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;generated;library;ownership")
include("${CMAKE_CURRENT_LIST_DIR}/source_comptime_limits.cmake")
add_executable(test_xir_source_derivation "${CMAKE_CURRENT_LIST_DIR}/test_xir_source_derivation.c")
target_link_libraries(test_xir_source_derivation PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen xray_lsp_source_query xray_frontend_format)
set_target_properties(test_xir_source_derivation PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
set(XR_SOURCE_DERIVATION_WORK "${CMAKE_CURRENT_BINARY_DIR}/source-derivation-work")
file(MAKE_DIRECTORY "${XR_SOURCE_DERIVATION_WORK}")
target_compile_definitions(test_xir_source_derivation PRIVATE XR_SOURCE_DERIVATION_WORK="${XR_SOURCE_DERIVATION_WORK}")
if(MSVC)
    target_compile_options(test_xir_source_derivation PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_source_derivation PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_derivation COMMAND test_xir_source_derivation)
set_tests_properties(test_xir_source_derivation PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;generated;library;ownership")
add_test(NAME test_xir_source_derivation_compiler_controls COMMAND test_xir_source_derivation --compiler-controls)
set_tests_properties(test_xir_source_derivation_compiler_controls PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;generated;ownership;budget")
add_test(NAME test_xir_source_derivation_compiler_resources COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/run_source_derivation_compiler_faults.py"
    --exe $<TARGET_FILE:test_xir_source_derivation>
    --output "${CMAKE_CURRENT_BINARY_DIR}/source-derivation-compiler-fi")
set_tests_properties(test_xir_source_derivation_compiler_resources PROPERTIES TIMEOUT 600 COST 600 PROCESSORS 8 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;generated;ownership;compiler-fault;resources")
include("${CMAKE_CURRENT_LIST_DIR}/source_derivation_native.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/source_derivation_json.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/source_derivation_enum.cmake")
add_executable(test_xir_source_comptime "${CMAKE_CURRENT_LIST_DIR}/test_xir_source_comptime.c")
target_link_libraries(test_xir_source_comptime PRIVATE xray_xir_source xray_xir_vm)
set_target_properties(test_xir_source_comptime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_source_comptime PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_source_comptime PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_source_comptime COMMAND test_xir_source_comptime)
set_tests_properties(test_xir_source_comptime PROPERTIES LABELS "unit;xir;source;ctfe;ownership" TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE)
add_test(NAME test_xir_source_comptime_resources COMMAND test_xir_source_comptime --resources)
set_tests_properties(test_xir_source_comptime_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources" TIMEOUT 600 PROCESSORS 1 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_locals_resources COMMAND test_xir_source_comptime --local-resources)
set_tests_properties(test_xir_source_comptime_locals_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;locals" TIMEOUT 600 PROCESSORS 1 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_unit_resources COMMAND test_xir_source_comptime --unit-resources)
set_tests_properties(test_xir_source_comptime_unit_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_rune_resources COMMAND test_xir_source_comptime --rune-resources)
set_tests_properties(test_xir_source_comptime_rune_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;rune" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_cell_resources COMMAND test_xir_source_comptime --cell-resources)
set_tests_properties(test_xir_source_comptime_cell_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;cell" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_string_resources COMMAND test_xir_source_comptime --string-resources)
set_tests_properties(test_xir_source_comptime_string_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;string" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_string_read_resources COMMAND test_xir_source_comptime --string-read-resources)
set_tests_properties(test_xir_source_comptime_string_read_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;string" TIMEOUT 600 RUN_SERIAL TRUE COST 166)

add_test(NAME test_xir_source_comptime_tuple_resources COMMAND test_xir_source_comptime --tuple-resources)
set_tests_properties(test_xir_source_comptime_tuple_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;tuple" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_float_resources COMMAND test_xir_source_comptime --float-resources)
set_tests_properties(test_xir_source_comptime_float_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;float" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_nullable_resources COMMAND test_xir_source_comptime --nullable-resources)
set_tests_properties(test_xir_source_comptime_nullable_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;nullable" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_array_resources COMMAND test_xir_source_comptime --array-resources)
set_tests_properties(test_xir_source_comptime_array_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;array" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_array_equal_resources COMMAND test_xir_source_comptime --array-equal-resources)
set_tests_properties(test_xir_source_comptime_array_equal_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;array;equal" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_array_path_resources COMMAND test_xir_source_comptime --array-path-resources)
set_tests_properties(test_xir_source_comptime_array_path_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;array;path" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_text_resources COMMAND test_xir_source_comptime --text-resources)
set_tests_properties(test_xir_source_comptime_text_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;text" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_nominal COMMAND test_xir_source_comptime --nominal)
set_tests_properties(test_xir_source_comptime_nominal PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;nominal" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_nominal_resources COMMAND test_xir_source_comptime --nominal-resources)
set_tests_properties(test_xir_source_comptime_nominal_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;nominal" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_lexical COMMAND test_xir_source_comptime --lexical)
set_tests_properties(test_xir_source_comptime_lexical PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;lexical" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_lexical_resources COMMAND test_xir_source_comptime --lexical-resources)
set_tests_properties(test_xir_source_comptime_lexical_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;lexical" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_function COMMAND test_xir_source_comptime --function)
set_tests_properties(test_xir_source_comptime_function PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;function" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_function_resources COMMAND test_xir_source_comptime --function-resources)
set_tests_properties(test_xir_source_comptime_function_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;function" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_cleanup COMMAND test_xir_source_comptime --cleanup)
set_tests_properties(test_xir_source_comptime_cleanup PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;cleanup" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_cleanup_resources COMMAND test_xir_source_comptime --cleanup-resources)
set_tests_properties(test_xir_source_comptime_cleanup_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;cleanup" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_projection COMMAND test_xir_source_comptime --projection)
set_tests_properties(test_xir_source_comptime_projection PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;projection" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_projection_resources COMMAND test_xir_source_comptime --projection-resources)
set_tests_properties(test_xir_source_comptime_projection_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;projection" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_error COMMAND test_xir_source_comptime --error)
set_tests_properties(test_xir_source_comptime_error PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;error" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_error_resources COMMAND test_xir_source_comptime --error-resources)
set_tests_properties(test_xir_source_comptime_error_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;error" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_panic COMMAND test_xir_source_comptime --panic)
set_tests_properties(test_xir_source_comptime_panic PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;panic" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_panic_resources COMMAND test_xir_source_comptime --panic-resources)
set_tests_properties(test_xir_source_comptime_panic_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;panic" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_class COMMAND test_xir_source_comptime --class)
set_tests_properties(test_xir_source_comptime_class PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;class" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_class_resources COMMAND test_xir_source_comptime --class-resources)
set_tests_properties(test_xir_source_comptime_class_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;class" TIMEOUT 600 RUN_SERIAL TRUE)

add_test(NAME test_xir_source_comptime_output COMMAND test_xir_source_comptime --output)
set_tests_properties(test_xir_source_comptime_output PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;output" TIMEOUT 30)
add_test(NAME test_xir_source_comptime_output_resources COMMAND test_xir_source_comptime --output-resources)
set_tests_properties(test_xir_source_comptime_output_resources PROPERTIES
    LABELS "unit;xir;source;ctfe;ownership;compiler-fault;resources;output" TIMEOUT 600 RUN_SERIAL TRUE)
