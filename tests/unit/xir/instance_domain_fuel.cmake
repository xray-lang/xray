# Runtime fuel and resume-count independence through a real sealed Program.
add_executable(test_xir_instance_domain_fuel ${CMAKE_CURRENT_LIST_DIR}/test_xir_instance_domain_fuel.c)
target_include_directories(test_xir_instance_domain_fuel PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_xir_instance_domain_fuel PRIVATE xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_instance_domain_fuel PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_instance_domain_fuel PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_instance_domain_fuel COMMAND test_xir_instance_domain_fuel)
set_tests_properties(test_xir_instance_domain_fuel PROPERTIES
    LABELS "unit;xir;memory;ownership;runtime" TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1)
