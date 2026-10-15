# The compiler-owner consumers use the same runtime foundation as production.
# Their observing compiler ledger remains independent of runtime allocations.
target_sources(xir_compile_owner_runtime PRIVATE
    "${PROJECT_SOURCE_DIR}/src/xir/xxir_scalar_core.c"
    "${PROJECT_SOURCE_DIR}/src/xir/xxir_task.c"
    "${PROJECT_SOURCE_DIR}/src/xir/xxir_task_budget.c")

add_executable(test_xir_program_correspondence "${CMAKE_CURRENT_LIST_DIR}/test_xir_program_correspondence.c")
target_include_directories(test_xir_program_correspondence PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(test_xir_program_correspondence PRIVATE xray_xir_vm)
target_compile_features(test_xir_program_correspondence PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_xir_program_correspondence PRIVATE /W4 /WX /utf-8)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_xir_program_correspondence PRIVATE /experimental:c11atomics)
endif()
add_test(NAME test_xir_program_correspondence COMMAND test_xir_program_correspondence)
set_tests_properties(test_xir_program_correspondence PROPERTIES TIMEOUT 60
    LABELS "unit;xir;program;correspondence;ownership;callback")
