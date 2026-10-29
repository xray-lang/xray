add_executable(test_instruction
    ${CMAKE_CURRENT_LIST_DIR}/../test_instruction.c)
add_executable(test_opcode
    ${CMAKE_CURRENT_LIST_DIR}/../test_opcode.c
    ${PROJECT_SOURCE_DIR}/src/runtime/value/xopcode_info.c)
foreach(target test_instruction test_opcode)
    target_include_directories(${target} PRIVATE
        $<TARGET_PROPERTY:xray_core_objs,INCLUDE_DIRECTORIES>)
    target_compile_definitions(${target} PRIVATE
        $<TARGET_PROPERTY:xray_core_objs,COMPILE_DEFINITIONS>)
    target_compile_options(${target} PRIVATE
        $<TARGET_PROPERTY:xray_core_objs,COMPILE_OPTIONS>)
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY
        $<TARGET_GENEX_EVAL:xray_core_objs,$<TARGET_PROPERTY:xray_core_objs,MSVC_RUNTIME_LIBRARY>>)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8)
    endif()
    add_test(NAME ${target} COMMAND $<TARGET_FILE:${target}>)
    set_tests_properties(${target} PROPERTIES TIMEOUT 30
        LABELS "unit;vm;instruction;retirement")
endforeach()
xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
