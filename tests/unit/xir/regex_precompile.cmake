set(XIR_REGEX_LIBRARY_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_regex_precompile_library.h")
add_custom_command(OUTPUT ${XIR_REGEX_LIBRARY_HEADER}
    COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_LIST_DIR}/gen_regex_library_source.py"
        --input "${CMAKE_CURRENT_LIST_DIR}/fixtures/regex_precompile.xr" --output ${XIR_REGEX_LIBRARY_HEADER}
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/fixtures/regex_precompile.xr" "${CMAKE_CURRENT_LIST_DIR}/gen_regex_library_source.py" VERBATIM)
add_executable(test_xir_regex_precompile "${CMAKE_CURRENT_LIST_DIR}/test_xir_regex_precompile.c" ${XIR_REGEX_LIBRARY_HEADER})
target_include_directories(test_xir_regex_precompile PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(test_xir_regex_precompile PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set_target_properties(test_xir_regex_precompile PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
set(XR_REGEX_PRECOMPILE_WORK "${CMAKE_CURRENT_BINARY_DIR}/regex-precompile-work")
file(MAKE_DIRECTORY "${XR_REGEX_PRECOMPILE_WORK}")
target_compile_definitions(test_xir_regex_precompile PRIVATE
    XR_REGEX_PRECOMPILE_WORK="${XR_REGEX_PRECOMPILE_WORK}")
if(MSVC)
    target_compile_options(test_xir_regex_precompile PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_regex_precompile PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_regex_precompile COMMAND test_xir_regex_precompile)
set_tests_properties(test_xir_regex_precompile PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;regex;library;ownership")

set(XIR_REGEX_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_regex_precompile.c")
set(XIR_REGEX_PACKET "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_regex_precompile.chk")
add_custom_command(OUTPUT ${XIR_REGEX_C} ${XIR_REGEX_PACKET}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_regex_precompile> --emit-native ${XIR_REGEX_C} ${XIR_REGEX_PACKET}
    DEPENDS test_xir_regex_precompile "${CMAKE_CURRENT_LIST_DIR}/fixtures/regex_precompile.xr" VERBATIM)
add_executable(test_xir_regex_precompile_native "${CMAKE_CURRENT_LIST_DIR}/test_xir_regex_precompile_native.c" ${XIR_REGEX_C})
target_link_libraries(test_xir_regex_precompile_native PRIVATE xray_xir_scalar)
add_executable(test_xir_regex_precompile_mixed "${CMAKE_CURRENT_LIST_DIR}/test_xir_regex_precompile_mixed.c" ${XIR_REGEX_C})
target_link_libraries(test_xir_regex_precompile_mixed PRIVATE xray_xir_vm)
target_compile_definitions(test_xir_regex_precompile_mixed PRIVATE XR_REGEX_PRECOMPILE_PACKET="${XIR_REGEX_PACKET}")
foreach(regex_test IN ITEMS test_xir_regex_precompile_native test_xir_regex_precompile_mixed)
    set_target_properties(${regex_test} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${regex_test} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${regex_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${regex_test} COMMAND ${regex_test})
    set_tests_properties(${regex_test} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;regex;library;ownership;execution;abi")
    add_test(NAME ${regex_test}_runtime_resources COMMAND ${regex_test} --runtime-resources)
    set_tests_properties(${regex_test}_runtime_resources PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;regex;ownership;runtime-fault;resources")
endforeach()
add_test(NAME test_xir_regex_precompile_runtime_resources COMMAND test_xir_regex_precompile_mixed --vm-runtime-resources)
set_tests_properties(test_xir_regex_precompile_runtime_resources PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;regex;ownership;runtime-fault;resources")

add_test(NAME test_xir_regex_precompile_compiler_controls COMMAND test_xir_regex_precompile --compiler-controls)
set_tests_properties(test_xir_regex_precompile_compiler_controls PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;regex;ownership;budget")
add_test(NAME test_xir_regex_precompile_compiler_resources COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/run_source_derivation_compiler_faults.py"
    --exe $<TARGET_FILE:test_xir_regex_precompile>
    --output "${CMAKE_CURRENT_BINARY_DIR}/regex-precompile-compiler-fi")
set_tests_properties(test_xir_regex_precompile_compiler_resources PROPERTIES TIMEOUT 600 COST 600 PROCESSORS 8 RUN_SERIAL TRUE LABELS "unit;xir;source;ctfe;regex;ownership;compiler-fault;resources")
