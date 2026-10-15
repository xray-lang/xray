add_executable(test_xir_p10_initializer_places_runtime
    "${CMAKE_CURRENT_LIST_DIR}/test_initializer_places_runtime.c"
    "${CMAKE_CURRENT_BINARY_DIR}/generated_native/init_field_places.c")
target_link_libraries(test_xir_p10_initializer_places_runtime PRIVATE xray_xir_vm)
set_target_properties(test_xir_p10_initializer_places_runtime PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p10_initializer_places_runtime PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_p10_initializer_places_runtime PRIVATE -Wall -Wextra -Werror)
endif()
foreach(mode RANGE 0 3)
    foreach(operation IN ITEMS normal fi cancel axes)
        add_test(NAME test_xir_p10_initializer_places_${operation}_${mode}
            COMMAND test_xir_p10_initializer_places_runtime ${mode} --${operation})
        set_tests_properties(test_xir_p10_initializer_places_${operation}_${mode} PROPERTIES
            TIMEOUT 300 LABELS "unit;xir;p10;init;ownership;runtime;${operation}")
        if(NOT operation STREQUAL "normal")
            set_tests_properties(test_xir_p10_initializer_places_${operation}_${mode} PROPERTIES
                RUN_SERIAL TRUE COST 100)
        endif()
    endforeach()
endforeach()
