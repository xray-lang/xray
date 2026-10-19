# Independent runtime owner exercises the same production implementation with
# counted compiler and runtime allocators before backend integration.
add_executable(test_xir_atomic_runtime_core
 ${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_atomic_runtime_core.c
 ${PROJECT_SOURCE_DIR}/src/xir/xxir_scalar.c
 ${PROJECT_SOURCE_DIR}/src/xir/xxir_float.c
 ${PROJECT_SOURCE_DIR}/src/xir/xxir_type_arena.c
 ${PROJECT_SOURCE_DIR}/src/xir/xxir_output.c
 ${PROJECT_SOURCE_DIR}/src/xir/xxir_format.c
 ${PROJECT_SOURCE_DIR}/src/module/xmodule_identity.c
 ${PROJECT_SOURCE_DIR}/src/module/xmodule_identity_view.c
 ${PROJECT_SOURCE_DIR}/src/base/xfileio.c
 ${PROJECT_SOURCE_DIR}/src/base/xio_policy.c)
target_include_directories(test_xir_atomic_runtime_core PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_xir_atomic_runtime_core PRIVATE xray_xir_admission)
if(MSVC)
 target_compile_options(test_xir_atomic_runtime_core PRIVATE /W4 /WX)
else()
 target_compile_options(test_xir_atomic_runtime_core PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME xir_atomic_runtime_core COMMAND test_xir_atomic_runtime_core)
set_tests_properties(xir_atomic_runtime_core PROPERTIES TIMEOUT 300)
