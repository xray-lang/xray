add_executable(test_xir_program_call_permissions xir/test_xir_program_call_permissions.c)
target_link_libraries(test_xir_program_call_permissions PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_program_call_permissions PRIVATE
    XR_PROGRAM_CALL_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/program-call-permission-scratch")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/program-call-permission-scratch")
if(MSVC)
    target_compile_options(test_xir_program_call_permissions PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_program_call_permissions PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case normal required unknown mixed advertised_unknown)
    add_test(NAME test_xir_program_call_permissions_${case}
        COMMAND test_xir_program_call_permissions ${case})
    set_tests_properties(test_xir_program_call_permissions_${case} PROPERTIES
        LABELS "unit;xir;source;program;root;call;go;permission" TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1)
endforeach()
