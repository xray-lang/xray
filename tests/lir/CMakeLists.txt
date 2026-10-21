add_executable(test_lir_core test_lir_core.c)
target_link_libraries(test_lir_core PRIVATE xray_lir)
add_executable(test_lir_allocations test_lir_allocations.c
    "${PROJECT_SOURCE_DIR}/src/lir/xlir_verify.c")
add_executable(test_lir_allocations_l3 test_lir_allocations_l3.c
    "${PROJECT_SOURCE_DIR}/src/lir/xlir_verify.c")
foreach(target test_lir_core test_lir_allocations test_lir_allocations_l3)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_features(${target} PRIVATE c_std_11)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
add_test(NAME test_lir_core COMMAND test_lir_core)
add_test(NAME test_lir_allocations COMMAND test_lir_allocations)
add_test(NAME l3_lir_allocations COMMAND test_lir_allocations_l3)
set_tests_properties(l3_lir_allocations PROPERTIES LABELS "l3;lir;allocation" TIMEOUT 30)
add_test(NAME test_lir_ownership_inventory_scope
    COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/test_inventory_scope.py"
        "${PROJECT_SOURCE_DIR}/scripts/check_meta_ownership.py" "${CMAKE_CURRENT_BINARY_DIR}/scratch")
set_tests_properties(test_lir_core test_lir_allocations test_lir_ownership_inventory_scope
    PROPERTIES LABELS "unit;lir;fast" TIMEOUT 30)
