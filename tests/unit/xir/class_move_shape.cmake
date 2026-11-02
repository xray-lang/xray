add_executable(test_xir_class_move_shape xir/test_xir_class_move_shape.c)
target_link_libraries(test_xir_class_move_shape PRIVATE xray_xir_source xray_xir)
if(MSVC)
    target_compile_options(test_xir_class_move_shape PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_class_move_shape PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_class_move_shape
    COMMAND test_xir_class_move_shape "${CMAKE_SOURCE_DIR}")
set_tests_properties(test_xir_class_move_shape PROPERTIES
    LABELS "unit;xir;source;ownership;nominal" TIMEOUT 180)
