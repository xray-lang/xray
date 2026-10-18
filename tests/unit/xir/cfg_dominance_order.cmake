# Literal normal/fault dominance and bounded local scheduling controls.
add_executable(test_xir_cfg_dominance_order ${CMAKE_CURRENT_LIST_DIR}/test_xir_cfg_dominance_order.c)
target_include_directories(test_xir_cfg_dominance_order PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_xir_cfg_dominance_order PRIVATE xray_xir_admission)
if(MSVC)
    target_compile_options(test_xir_cfg_dominance_order PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_cfg_dominance_order PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_cfg_dominance_order COMMAND test_xir_cfg_dominance_order)
set_tests_properties(test_xir_cfg_dominance_order PROPERTIES
    LABELS "unit;xir;memory;ownership;cfg-scheduling" TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)
