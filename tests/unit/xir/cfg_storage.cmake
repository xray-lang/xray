# Independent physical CFG initialization regression for the compiler core.
add_executable(test_xir_cfg_storage ${CMAKE_CURRENT_LIST_DIR}/test_xir_cfg_storage.c)
target_include_directories(test_xir_cfg_storage PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_xir_cfg_storage PRIVATE xray_xir_admission)
if(MSVC)
    target_compile_options(test_xir_cfg_storage PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_cfg_storage PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_cfg_storage COMMAND test_xir_cfg_storage)
set_tests_properties(test_xir_cfg_storage PROPERTIES
    LABELS "unit;xir;memory;ownership" TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)
