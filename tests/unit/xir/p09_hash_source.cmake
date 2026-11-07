add_executable(test_xir_p09_hash_source "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_source.c")
target_link_libraries(test_xir_p09_hash_source PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm)
set(HASH_SOURCE_NATIVE_C "${CMAKE_BINARY_DIR}/generated/xir_p09_hash_source.c")
add_custom_command(OUTPUT "${HASH_SOURCE_NATIVE_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_p09_hash_source> "${HASH_SOURCE_NATIVE_C}"
    DEPENDS test_xir_p09_hash_source VERBATIM)

foreach(target IN ITEMS test_xir_p09_hash_source)
    target_compile_definitions(${target} PRIVATE XR_P09_HASH_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;source;ownership;codec;provider")
endforeach()
