set(module_class_suspend_dir "${CMAKE_CURRENT_LIST_DIR}")
set(module_class_suspend_generated "${CMAKE_BINARY_DIR}/generated/module-class-suspend")
set(module_class_suspend_sources)
foreach(graph RANGE 0 0)
    list(APPEND module_class_suspend_sources "${module_class_suspend_generated}/graph${graph}.c")
endforeach()
add_executable(test_source_product_module_class_suspend_emitter "${module_class_suspend_dir}/test_source_product_module_class_suspend.c")
add_custom_command(OUTPUT ${module_class_suspend_sources}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${module_class_suspend_generated}"
    COMMAND $<TARGET_FILE:test_source_product_module_class_suspend_emitter> emit "${module_class_suspend_generated}"
    DEPENDS test_source_product_module_class_suspend_emitter VERBATIM)
add_executable(test_source_product_module_class_suspend "${module_class_suspend_dir}/test_source_product_module_class_suspend.c" ${module_class_suspend_sources})
target_compile_definitions(test_source_product_module_class_suspend PRIVATE XR_MODULE_NATIVE=1)
foreach(target IN ITEMS test_source_product_module_class_suspend_emitter test_source_product_module_class_suspend)
    target_link_libraries(${target} PRIVATE xray_xir_source_product)
    target_include_directories(${target} PRIVATE "${module_class_suspend_dir}/..")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(mode IN ITEMS vm native alternating-native alternating-vm)
    add_test(NAME source_product_module_class_suspend_${mode} COMMAND test_source_product_module_class_suspend ${mode})
    set_tests_properties(source_product_module_class_suspend_${mode} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;program-consumer;module-graph;ownership")
endforeach()
add_test(NAME source_product_module_class_suspend_encoding
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_module_class_suspend.py" check)
set_tests_properties(source_product_module_class_suspend_encoding PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;inventory")
