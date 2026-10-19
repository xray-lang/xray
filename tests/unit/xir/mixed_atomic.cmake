include("${CMAKE_CURRENT_LIST_DIR}/atomic_lifetime.cmake")

add_executable(test_xir_mixed_atomic_permissions
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_mixed_atomic_permissions.c")
target_link_libraries(test_xir_mixed_atomic_permissions PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_atomic_permissions PRIVATE
    XR_P20_ATOMIC_PERMISSION_FIXTURES="${XIR_MIXED_ATOMIC_ROOT}/tests/fixtures/xir_p20_atomic_permissions")
set_target_properties(test_xir_mixed_atomic_permissions PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_mixed_atomic_permissions PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mixed_atomic_permissions PRIVATE -Wall -Wextra -Werror)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/atomic_permissions.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/atomic_compiler_resources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/atomic_resources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/atomic_control.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/atomic_control_resources.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/atomic_control_fi_census.cmake")
