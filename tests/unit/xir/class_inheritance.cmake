# Independent original output and cold inherited receiver consumers.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/class-inheritance-work")
set(class_inheritance_demo_c "${CMAKE_CURRENT_BINARY_DIR}/generated/class_inheritance_demo.c")
set(class_inheritance_cold_c "${CMAKE_CURRENT_BINARY_DIR}/generated/class_inheritance_cold.c")
set(class_inheritance_hidden_c "${CMAKE_CURRENT_BINARY_DIR}/generated/class_inheritance_hidden.c")
add_executable(class_inheritance_source xir/class_inheritance/test_class_inheritance_source.c)
target_link_libraries(class_inheritance_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${class_inheritance_demo_c}" "${class_inheritance_cold_c}" "${class_inheritance_hidden_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:class_inheritance_source> --emit "${class_inheritance_demo_c}" "${class_inheritance_cold_c}" "${class_inheritance_hidden_c}"
    DEPENDS class_inheritance_source
        "${PROJECT_SOURCE_DIR}/demos/03-oop/classes.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_class_inheritance/classes.expected.txt"
    VERBATIM)
add_executable(class_inheritance_native xir/class_inheritance/test_class_inheritance_native.c
    "${class_inheritance_demo_c}" "${class_inheritance_cold_c}" "${class_inheritance_hidden_c}")
target_link_libraries(class_inheritance_native PRIVATE xray_xir_scalar)
foreach(class_inheritance_target IN ITEMS class_inheritance_source class_inheritance_native)
    target_include_directories(${class_inheritance_target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_definitions(${class_inheritance_target} PRIVATE
        XR_CLASS_INHERITANCE_ROOT="${PROJECT_SOURCE_DIR}"
        XR_CLASS_INHERITANCE_WORK="${CMAKE_CURRENT_BINARY_DIR}/class-inheritance-work"
        XR_CLASS_INHERITANCE_GOLDEN="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_class_inheritance/classes.expected.txt")
    set_target_properties(${class_inheritance_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${class_inheritance_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${class_inheritance_target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${class_inheritance_target} COMMAND ${class_inheritance_target})
    set_tests_properties(${class_inheritance_target} PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
        LABELS "unit;xir;source;class;ownership;checked;generated-c")
endforeach()
