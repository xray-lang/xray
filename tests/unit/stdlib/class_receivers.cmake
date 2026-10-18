add_executable(test_stdlib_class_receivers ${CMAKE_CURRENT_LIST_DIR}/test_stdlib_class_receivers.c)
target_link_libraries(test_stdlib_class_receivers PRIVATE xray_xir_source)
target_include_directories(test_stdlib_class_receivers PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
set_target_properties(test_stdlib_class_receivers PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_definitions(test_stdlib_class_receivers PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_stdlib_class_receivers PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_stdlib_class_receivers PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_stdlib_class_receivers
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/check_class_receivers.py
        --binary $<TARGET_FILE:test_stdlib_class_receivers> --root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_stdlib_class_receivers PROPERTIES
    TIMEOUT 120 LABELS "unit;stdlib;source;declarations")
