cmake_minimum_required(VERSION 3.21)
project(xray_unit_framework_tests LANGUAGES C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Unit framework tests require Ninja")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(test_unit_framework_probe test_reporting.c)
set_target_properties(test_unit_framework_probe PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_unit_framework_probe PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_unit_framework_probe PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
enable_testing()
foreach(mode success direct_failure helper_failure)
    add_test(NAME unit_framework_reports_${mode}
        COMMAND ${Python3_EXECUTABLE} -X utf8 ${CMAKE_CURRENT_SOURCE_DIR}/check_reporting.py
            $<TARGET_FILE:test_unit_framework_probe> ${mode})
    set_tests_properties(unit_framework_reports_${mode} PROPERTIES
        TIMEOUT 15 LABELS "unit;quality")
endforeach()
