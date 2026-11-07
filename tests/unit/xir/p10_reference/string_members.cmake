set(string_member_native_targets)

set(string_owned_generated "${CMAKE_CURRENT_BINARY_DIR}/generated_native/string_owned_lifetime.c")




foreach(kind IN ITEMS resources)
    add_executable(test_xir_p10_string_owned_${kind} "${CMAKE_CURRENT_LIST_DIR}/test_string_owned_${kind}.c")
    target_link_libraries(test_xir_p10_string_owned_${kind} PRIVATE xray_xir_scalar)
    add_test(NAME test_xir_p10_string_owned_${kind} COMMAND test_xir_p10_string_owned_${kind})
    set_tests_properties(test_xir_p10_string_owned_${kind} PROPERTIES
        TIMEOUT 30 LABELS "unit;xir;p10;string-member;ownership;${kind}")
endforeach()
foreach(target IN ITEMS  test_xir_p10_string_owned_resources)


    list(APPEND string_member_native_targets ${target})
endforeach()
add_custom_target(xir_p10_string_member_native DEPENDS ${string_member_native_targets})
xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
