set(selected_c "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_selected_library.c")
set(selected_packet "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_selected_library.chk")
add_custom_command(OUTPUT ${selected_c} ${selected_packet}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_source_derived_library> --emit-generator-only ${selected_c} ${selected_packet}
    DEPENDS test_xir_source_derived_library VERBATIM)
foreach(selected_engine IN ITEMS native mixed)
    set(selected_target "test_xir_source_selected_library_${selected_engine}")
    add_executable(${selected_target} "${CMAKE_CURRENT_LIST_DIR}/test_xir_source_derived_library_${selected_engine}.c" ${selected_c})
    target_compile_definitions(${selected_target} PRIVATE XR_DERIVED_LIBRARY_GENERATOR_ONLY=1)
    if(selected_engine STREQUAL "native")
        target_link_libraries(${selected_target} PRIVATE xray_xir_scalar)
    else()
        target_link_libraries(${selected_target} PRIVATE xray_xir_vm)
        target_compile_definitions(${selected_target} PRIVATE XR_DERIVED_LIBRARY_PACKET="${selected_packet}")
    endif()
    set_target_properties(${selected_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${selected_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${selected_target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${selected_target} COMMAND ${selected_target})
    set_tests_properties(${selected_target} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
        LABELS "unit;xir;source;ctfe;generated;library;ownership;execution;abi;receiving")
    add_test(NAME "${selected_target}_runtime_resources" COMMAND ${selected_target} --runtime-resources)
    set_tests_properties("${selected_target}_runtime_resources" PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
        LABELS "unit;xir;source;generated;library;ownership;runtime-fault;resources")
    if(selected_engine STREQUAL "mixed")
        add_test(NAME test_xir_source_selected_library_vm_runtime_resources COMMAND ${selected_target} --vm-runtime-resources)
        set_tests_properties(test_xir_source_selected_library_vm_runtime_resources PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
            LABELS "unit;xir;source;generated;library;ownership;runtime-fault;resources")
    endif()
endforeach()
