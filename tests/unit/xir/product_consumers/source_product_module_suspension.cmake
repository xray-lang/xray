set(module_suspension_dir "${CMAKE_CURRENT_LIST_DIR}")
set(module_suspension_generated "${CMAKE_BINARY_DIR}/generated/module-suspension")
set(module_suspension_sources)
foreach(graph RANGE 0 2)
    list(APPEND module_suspension_sources "${module_suspension_generated}/graph${graph}.c")
endforeach()
add_executable(test_source_product_module_suspension_emitter "${module_suspension_dir}/test_source_product_module_suspension.c")
add_custom_command(OUTPUT ${module_suspension_sources}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${module_suspension_generated}"
    COMMAND $<TARGET_FILE:test_source_product_module_suspension_emitter> emit "${module_suspension_generated}"
    DEPENDS test_source_product_module_suspension_emitter VERBATIM)
add_executable(test_source_product_module_suspension "${module_suspension_dir}/test_source_product_module_suspension.c" ${module_suspension_sources})
target_compile_definitions(test_source_product_module_suspension PRIVATE XR_MODULE_SUSPEND_NATIVE=1)
foreach(target IN ITEMS test_source_product_module_suspension_emitter test_source_product_module_suspension)
    target_link_libraries(${target} PRIVATE xray_xir_source_product)
    target_include_directories(${target} PRIVATE "${module_suspension_dir}/..")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(mode IN ITEMS vm native alternating-native alternating-vm)
    add_test(NAME source_product_module_suspension_${mode} COMMAND test_source_product_module_suspension ${mode})
    set_tests_properties(source_product_module_suspension_${mode} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;program-consumer;module-graph;ownership")
endforeach()
add_test(NAME source_product_module_suspension_encoding
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_module_suspension.py" check)
set_tests_properties(source_product_module_suspension_encoding PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;inventory")
