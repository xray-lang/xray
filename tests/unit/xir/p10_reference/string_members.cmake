set(string_member_native_targets)
foreach(case IN ITEMS last_index starts_with ends_with contains omitted constant dynamic
    replace replace_all replace_parameters replace_chain copy_bytes discard_bytes comptime)
    add_test(NAME test_xir_p10_string_member_${case}
        COMMAND test_xir_p10_reference --accept string_member_${case} 42)
    set_tests_properties(test_xir_p10_string_member_${case} PROPERTIES
        TIMEOUT 30 LABELS "unit;xir;p10;source;string-member;ownership")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated_native/string_member_${case}.c")
    add_custom_command(OUTPUT "${generated}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated_native"
        COMMAND $<TARGET_FILE:test_xir_p10_reference> --emit string_member_${case} 42 "${generated}"
        DEPENDS test_xir_p10_reference "${CMAKE_CURRENT_LIST_DIR}/fixtures/string_member_${case}.xr"
        VERBATIM)
    foreach(mode IN ITEMS native mixed)
        set(target "test_xir_p10_string_member_${mode}_${case}")
        add_executable(${target} "${CMAKE_CURRENT_LIST_DIR}/test_language_native.c" "${generated}")
        target_link_libraries(${target} PRIVATE xray_xir_vm)
        if(mode STREQUAL "mixed")
            target_compile_definitions(${target} PRIVATE XR_LANGUAGE_MIXED=1)
        else()
            target_compile_definitions(${target} PRIVATE XR_LANGUAGE_MIXED=0)
        endif()
        set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        endif()
        list(APPEND string_member_native_targets ${target})
        add_test(NAME ${target} COMMAND ${target} 42)
        set_tests_properties(${target} PROPERTIES
            TIMEOUT 30 LABELS "unit;xir;p10;string-member;ownership;${mode}")
    endforeach()
endforeach()
set(string_owned_generated "${CMAKE_CURRENT_BINARY_DIR}/generated_native/string_owned_lifetime.c")
add_custom_command(OUTPUT "${string_owned_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated_native"
    COMMAND $<TARGET_FILE:test_xir_p10_reference> --emit string_owned_lifetime 42 "${string_owned_generated}"
    DEPENDS test_xir_p10_reference "${CMAKE_CURRENT_LIST_DIR}/fixtures/string_owned_lifetime.xr"
    VERBATIM)
add_executable(test_xir_p10_string_owned_lifetime "${CMAKE_CURRENT_LIST_DIR}/test_string_owned_lifetime.c" "${string_owned_generated}")
target_link_libraries(test_xir_p10_string_owned_lifetime PRIVATE xray_xir_vm)
foreach(mode RANGE 0 3)
    add_test(NAME test_xir_p10_string_owned_lifetime_${mode} COMMAND test_xir_p10_string_owned_lifetime ${mode})
    set_tests_properties(test_xir_p10_string_owned_lifetime_${mode} PROPERTIES
        TIMEOUT 30 LABELS "unit;xir;p10;string-member;ownership;late-owned")
endforeach()
foreach(kind IN ITEMS resources checked)
    add_executable(test_xir_p10_string_owned_${kind} "${CMAKE_CURRENT_LIST_DIR}/test_string_owned_${kind}.c")
    target_link_libraries(test_xir_p10_string_owned_${kind} PRIVATE xray_xir_scalar)
    add_test(NAME test_xir_p10_string_owned_${kind} COMMAND test_xir_p10_string_owned_${kind})
    set_tests_properties(test_xir_p10_string_owned_${kind} PROPERTIES
        TIMEOUT 30 LABELS "unit;xir;p10;string-member;ownership;${kind}")
endforeach()
foreach(target IN ITEMS test_xir_p10_string_owned_lifetime test_xir_p10_string_owned_resources test_xir_p10_string_owned_checked)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    list(APPEND string_member_native_targets ${target})
endforeach()
add_custom_target(xir_p10_string_member_native DEPENDS ${string_member_native_targets})
xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
