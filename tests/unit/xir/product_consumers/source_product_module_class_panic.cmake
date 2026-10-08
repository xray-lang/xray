set(module_class_panic_dir "${CMAKE_CURRENT_LIST_DIR}")
set(module_class_panic_generated "${CMAKE_BINARY_DIR}/generated/module-class-panic")
set(module_class_panic_sources)
foreach(graph RANGE 0 2)
    list(APPEND module_class_panic_sources "${module_class_panic_generated}/graph${graph}.c")
endforeach()
add_executable(test_source_product_module_class_panic_emitter "${module_class_panic_dir}/test_source_product_module_class_panic.c")
add_custom_command(OUTPUT ${module_class_panic_sources}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${module_class_panic_generated}"
    COMMAND $<TARGET_FILE:test_source_product_module_class_panic_emitter> emit "${module_class_panic_generated}"
    DEPENDS test_source_product_module_class_panic_emitter VERBATIM)
add_executable(test_source_product_module_class_panic "${module_class_panic_dir}/test_source_product_module_class_panic.c" ${module_class_panic_sources})
target_compile_definitions(test_source_product_module_class_panic PRIVATE XR_MODULE_NATIVE=1)
foreach(target IN ITEMS test_source_product_module_class_panic_emitter test_source_product_module_class_panic)
    target_link_libraries(${target} PRIVATE xray_xir_source_product)
    target_include_directories(${target} PRIVATE "${module_class_panic_dir}/..")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(mode IN ITEMS vm native alternating-native alternating-vm)
    add_test(NAME source_product_module_class_panic_${mode} COMMAND test_source_product_module_class_panic ${mode})
    set_tests_properties(source_product_module_class_panic_${mode} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;program-consumer;module-graph;ownership")
endforeach()
add_test(NAME source_product_module_class_panic_encoding
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_module_class_panic.py" check)
set_tests_properties(source_product_module_class_panic_encoding PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;inventory")
