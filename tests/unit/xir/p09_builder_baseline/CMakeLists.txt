cmake_minimum_required(VERSION 3.21)
project(xir_builder_baseline LANGUAGES C)
if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "The baseline requires Ninja")
endif()
get_filename_component(XR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)

enable_testing()
foreach(scenario short digits chunks reserve join snapshot)

    set_tests_properties(${scenario} PROPERTIES TIMEOUT 120 LABELS "measurement;legacy-builder")
endforeach()
