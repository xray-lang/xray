add_executable(test_xir_mixed_forin_resources
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_mixed_forin_resources.c" "${XIR_MIXED_FORIN_C}")
target_link_libraries(test_xir_mixed_forin_resources PRIVATE xray_xir_vm xray_xir_scalar)
set_target_properties(test_xir_mixed_forin_resources PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_forin_resources PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_forin_resources PRIVATE -Wall -Wextra -Werror)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/forin_resource_cases.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/forin_resource_fi_cases.cmake")
foreach(backend IN ITEMS 0 1 vm native)
    add_test(NAME test_xir_mixed_forin_no_argument_probe_${backend}
        COMMAND test_xir_mixed_forin_resources ${backend} census-no-argument-probe)
    set_tests_properties(test_xir_mixed_forin_no_argument_probe_${backend} PROPERTIES
        TIMEOUT 120 PROCESSORS 1 COST 10
        LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;generator;iterator;forin;resource")
endforeach()
