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
