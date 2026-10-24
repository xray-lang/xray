set(P11_LIBRARY_NATIVE_DIR "${CMAKE_CURRENT_BINARY_DIR}/library-native")
foreach(kind array private multi method nested mixed builtin none local fresh dual known closed closed-method foreign)
    set(material "${P11_LIBRARY_NATIVE_DIR}/${kind}.c")
    add_custom_command(OUTPUT "${material}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${P11_LIBRARY_NATIVE_DIR}"
        COMMAND $<TARGET_FILE:test_xir_p11_ctfe_instance> library-emit ${kind} "${material}"
        DEPENDS test_xir_p11_ctfe_instance VERBATIM)
    set(consumer "test_xir_p11_library_native_${kind}")
    add_executable(${consumer} library_native.c "${material}")
    target_link_libraries(${consumer} PRIVATE xray_xir_vm xray_xir_scalar)
    target_include_directories(${consumer} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
    set_target_properties(${consumer} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${consumer} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${consumer} PRIVATE -Wall -Wextra -Werror)
    endif()
    xr_enable_pure_aot_symbol_map(${consumer})
    foreach(mode native native-vm vm-native)
        add_test(NAME test_xir_p11_library_${mode}_${kind} COMMAND ${consumer} ${mode})
        set_tests_properties(test_xir_p11_library_${mode}_${kind} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
            LABELS "unit;xir;library;comptime;native;mixed;ownership")
        foreach(control runtime-prefix runtime-bounds cancel)
            add_test(NAME test_xir_p11_library_${mode}_${kind}_${control} COMMAND ${consumer} ${mode} --${control})
            set_tests_properties(test_xir_p11_library_${mode}_${kind}_${control} PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE
                LABELS "unit;xir;library;comptime;native;mixed;ownership")
        endforeach()
        add_test(NAME test_xir_p11_library_${mode}_${kind}_runtime_FI COMMAND ${consumer} ${mode} --runtime-faults)
        set_tests_properties(test_xir_p11_library_${mode}_${kind}_runtime_FI PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE
            LABELS "resource;xir;library;native;mixed;runtime-FI" COST 120)
    endforeach()
endforeach()
