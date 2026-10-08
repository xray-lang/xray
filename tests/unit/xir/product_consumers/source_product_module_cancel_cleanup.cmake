set(module_cancel_cleanup_dir "${CMAKE_CURRENT_LIST_DIR}")
set(module_cancel_cleanup_generated "${CMAKE_BINARY_DIR}/generated/module-cancel-cleanup")
set(module_cancel_cleanup_sources)
foreach(graph RANGE 0 4)
    list(APPEND module_cancel_cleanup_sources "${module_cancel_cleanup_generated}/graph${graph}.c")
endforeach()
add_executable(test_source_product_module_cancel_cleanup_emitter "${module_cancel_cleanup_dir}/test_source_product_module_cancel_cleanup.c")
add_custom_command(OUTPUT ${module_cancel_cleanup_sources}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${module_cancel_cleanup_generated}"
    COMMAND $<TARGET_FILE:test_source_product_module_cancel_cleanup_emitter> emit "${module_cancel_cleanup_generated}"
    DEPENDS test_source_product_module_cancel_cleanup_emitter VERBATIM)
add_executable(test_source_product_module_cancel_cleanup "${module_cancel_cleanup_dir}/test_source_product_module_cancel_cleanup.c" ${module_cancel_cleanup_sources})
target_compile_definitions(test_source_product_module_cancel_cleanup PRIVATE XR_MODULE_CLEANUP_NATIVE=1)
foreach(target IN ITEMS test_source_product_module_cancel_cleanup_emitter test_source_product_module_cancel_cleanup)
    target_link_libraries(${target} PRIVATE xray_xir_source_product)
    target_include_directories(${target} PRIVATE "${module_cancel_cleanup_dir}/..")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(mode IN ITEMS vm native alternating-native alternating-vm)
    add_test(NAME source_product_module_cancel_cleanup_${mode} COMMAND test_source_product_module_cancel_cleanup ${mode})
    set_tests_properties(source_product_module_cancel_cleanup_${mode} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;program-consumer;module-graph;ownership")
endforeach()
add_test(NAME source_product_module_cancel_cleanup_encoding
    COMMAND ${XRAY_PYTHON} -B -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_module_cancel_cleanup.py" check)
set_tests_properties(source_product_module_cancel_cleanup_encoding PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;program-consumer;inventory")
