

set(HASH_PROJECTION_C "${CMAKE_BINARY_DIR}/generated/xir_p09_hash_projection.c")

add_test(NAME test_xir_p09_hash_projection78_vectors COMMAND ${XRAY_PYTHON}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_hash_projection78_vectors.py"
    --source "${PROJECT_SOURCE_DIR}" --output "${CMAKE_BINARY_DIR}/p09-hash-projection78-vectors")
set_tests_properties(test_xir_p09_hash_projection78_vectors PROPERTIES TIMEOUT 30 LABELS "unit;xir;hash;codec")

add_executable(test_xir_p09_hash_projection_resources
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_projection_resources.c")
target_link_libraries(test_xir_p09_hash_projection_resources PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_p09_hash_projection_resources PRIVATE XR_P09_HASH_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_xir_p09_hash_projection_resources PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_hash_projection_resources PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_hash_projection_resources PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p09_hash_projection_resources_normal COMMAND test_xir_p09_hash_projection_resources --normal)
set_tests_properties(test_xir_p09_hash_projection_resources_normal PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;resources;target-impact")
add_test(NAME test_xir_p09_hash_projection_resources COMMAND test_xir_p09_hash_projection_resources)
set_tests_properties(test_xir_p09_hash_projection_resources PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;hash;resources;allocation;ownership;cancellation")

foreach(mode native mixed)
    set(target test_xir_p09_hash_projection_resources_${mode})

endforeach()

foreach(target IN ITEMS test_xir_p09_hash_projection_resources)
    add_test(NAME ${target}_cancellation COMMAND ${target} --cancel)
    set_tests_properties(${target}_cancellation PROPERTIES TIMEOUT 120
        LABELS "unit;xir;hash;resources;cancellation;ownership;target-impact;provider")
endforeach()

add_executable(test_xir_p09_hash_projection_value "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_projection_value.c")
target_include_directories(test_xir_p09_hash_projection_value PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base")
target_link_libraries(test_xir_p09_hash_projection_value PRIVATE xray_xir_scalar)
if(WIN32)
    target_sources(test_xir_p09_hash_projection_value PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c")
else()
    target_sources(test_xir_p09_hash_projection_value PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c")
    target_link_libraries(test_xir_p09_hash_projection_value PRIVATE Threads::Threads)
endif()
set_target_properties(test_xir_p09_hash_projection_value PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_hash_projection_value PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_hash_projection_value PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p09_hash_projection_value COMMAND test_xir_p09_hash_projection_value)
set_tests_properties(test_xir_p09_hash_projection_value PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;ownership;memory;budget")
add_test(NAME test_xir_p09_hash_projection_value_allocations COMMAND test_xir_p09_hash_projection_value --full)
set_tests_properties(test_xir_p09_hash_projection_value_allocations PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;ownership;memory;budget;fault")
