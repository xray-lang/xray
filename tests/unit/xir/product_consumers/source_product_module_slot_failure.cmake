set(module_slot_failure_dir "${CMAKE_CURRENT_LIST_DIR}")
set(module_slot_failure_generated "${CMAKE_BINARY_DIR}/generated/module-slot-failure")
set(module_slot_failure_sources)
foreach(graph RANGE 0 1)
    list(APPEND module_slot_failure_sources "${module_slot_failure_generated}/graph${graph}.c")
endforeach()
add_executable(test_source_product_module_slot_failure_emitter "${module_slot_failure_dir}/test_source_product_module_slot_failure.c")
add_custom_command(OUTPUT ${module_slot_failure_sources}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${module_slot_failure_generated}"
    COMMAND $<TARGET_FILE:test_source_product_module_slot_failure_emitter> emit "${module_slot_failure_generated}"
    DEPENDS test_source_product_module_slot_failure_emitter VERBATIM)
add_executable(test_source_product_module_slot_failure "${module_slot_failure_dir}/test_source_product_module_slot_failure.c" ${module_slot_failure_sources})
target_compile_definitions(test_source_product_module_slot_failure PRIVATE XR_MODULE_NATIVE=1)
foreach(target IN ITEMS test_source_product_module_slot_failure_emitter test_source_product_module_slot_failure)
    target_link_libraries(${target} PRIVATE xray_xir_source_product)
    target_include_directories(${target} PRIVATE "${module_slot_failure_dir}/..")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(mode IN ITEMS vm native alternating-native alternating-vm)
    add_test(NAME source_product_module_slot_failure_${mode} COMMAND test_source_product_module_slot_failure ${mode})
    set_tests_properties(source_product_module_slot_failure_${mode} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;program-consumer;module-graph;ownership")
endforeach()
add_test(NAME source_product_module_slot_failure_encoding
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_module_slot_failure.py" check)
set_tests_properties(source_product_module_slot_failure_encoding PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;inventory")
