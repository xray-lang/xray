set(module_execution_dir "${CMAKE_CURRENT_LIST_DIR}")
set(module_execution_generated "${CMAKE_BINARY_DIR}/generated/module-execution")
set(module_execution_sources)
foreach(graph RANGE 0 3)
    list(APPEND module_execution_sources "${module_execution_generated}/graph${graph}.c")
endforeach()
add_executable(test_source_product_module_execution_emitter "${module_execution_dir}/test_source_product_module_execution.c")
add_custom_command(OUTPUT ${module_execution_sources}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${module_execution_generated}"
    COMMAND $<TARGET_FILE:test_source_product_module_execution_emitter> emit "${module_execution_generated}"
    DEPENDS test_source_product_module_execution_emitter VERBATIM)
add_executable(test_source_product_module_execution "${module_execution_dir}/test_source_product_module_execution.c" ${module_execution_sources})
target_compile_definitions(test_source_product_module_execution PRIVATE XR_MODULE_NATIVE=1)
foreach(target IN ITEMS test_source_product_module_execution_emitter test_source_product_module_execution)
    target_link_libraries(${target} PRIVATE xray_xir_source_product)
    target_include_directories(${target} PRIVATE "${module_execution_dir}/..")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(mode IN ITEMS vm native native-inits vm-inits)
    add_test(NAME source_product_module_execution_${mode} COMMAND test_source_product_module_execution ${mode})
    set_tests_properties(source_product_module_execution_${mode} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;program-consumer;module-graph;ownership")
endforeach()
add_test(NAME source_product_module_execution_encoding
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_module_execution.py" check)
set_tests_properties(source_product_module_execution_encoding PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;inventory")
