# Independent error graph literals supplement the unchanged aggregate gates.
add_executable(test_xir_error_cfg_scheduling xir/test_xir_error_cfg_scheduling.c)
target_link_libraries(test_xir_error_cfg_scheduling PRIVATE xray_xir)
if(MSVC)
    target_compile_options(test_xir_error_cfg_scheduling PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_error_cfg_scheduling PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_error_cfg_scheduling COMMAND test_xir_error_cfg_scheduling)
add_test(NAME test_xir_error_cfg_scheduling_scale COMMAND test_xir_error_cfg_scheduling --scale)
set_tests_properties(test_xir_error_cfg_scheduling test_xir_error_cfg_scheduling_scale PROPERTIES
    LABELS "unit;xir;metadata;ownership;cfg-scheduling;partial-feedback" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)

add_test(NAME test_xir_error_cfg_scheduling_resources COMMAND test_xir_error_cfg_scheduling --resources)
set_tests_properties(test_xir_error_cfg_scheduling_resources PROPERTIES
    LABELS "unit;xir;metadata;ownership;cfg-scheduling;resources;partial-feedback" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
