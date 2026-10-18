add_executable(test_xir_p09_hash_foreach "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_foreach.c")
target_link_libraries(test_xir_p09_hash_foreach PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm)
set(HASH_FOREACH_C "${CMAKE_BINARY_DIR}/generated/xir_p09_hash_foreach.c")
add_custom_command(OUTPUT "${HASH_FOREACH_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_p09_hash_foreach> "${HASH_FOREACH_C}"
    DEPENDS test_xir_p09_hash_foreach VERBATIM)
add_executable(test_xir_p09_hash_foreach_native
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_foreach_native.c" "${HASH_FOREACH_C}")
target_link_libraries(test_xir_p09_hash_foreach_native PRIVATE xray_xir_scalar)
add_executable(test_xir_p09_hash_foreach_mixed
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_foreach_mixed.c" "${HASH_FOREACH_C}")
target_link_libraries(test_xir_p09_hash_foreach_mixed PRIVATE xray_xir_source xray_xir_vm)
foreach(target test_xir_p09_hash_foreach test_xir_p09_hash_foreach_native test_xir_p09_hash_foreach_mixed)
    target_compile_definitions(${target} PRIVATE XR_P09_HASH_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;source;ownership;codec;provider")
endforeach()



add_executable(test_xir_p09_hash_position "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_position.c")
target_include_directories(test_xir_p09_hash_position PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base")
target_link_libraries(test_xir_p09_hash_position PRIVATE xray_xir_scalar)
if(WIN32)
    target_sources(test_xir_p09_hash_position PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c")
else()
    target_sources(test_xir_p09_hash_position PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c")
    target_link_libraries(test_xir_p09_hash_position PRIVATE Threads::Threads)
endif()
set_target_properties(test_xir_p09_hash_position PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_hash_position PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_hash_position PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p09_hash_position COMMAND test_xir_p09_hash_position)
set_tests_properties(test_xir_p09_hash_position PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;ownership;memory;budget;target-impact")
add_test(NAME test_xir_p09_hash_position_allocations COMMAND test_xir_p09_hash_position --full)
set_tests_properties(test_xir_p09_hash_position_allocations PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS "unit;xir;hash;ownership;memory;budget;fault")

set(HASH_FOREACH_MATRIX_C "${CMAKE_BINARY_DIR}/generated/xir_p09_hash_foreach_matrix.c")
add_executable(test_xir_p09_hash_foreach_matrix "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_foreach_matrix.c")
target_link_libraries(test_xir_p09_hash_foreach_matrix PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm)
add_custom_command(OUTPUT "${HASH_FOREACH_MATRIX_C}"
    COMMAND $<TARGET_FILE:test_xir_p09_hash_foreach_matrix> "${HASH_FOREACH_MATRIX_C}"
    DEPENDS test_xir_p09_hash_foreach_matrix VERBATIM)
foreach(mode native mixed)
    add_executable(test_xir_p09_hash_foreach_matrix_${mode}
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_foreach_matrix_${mode}.c" "${HASH_FOREACH_MATRIX_C}")
    target_link_libraries(test_xir_p09_hash_foreach_matrix_${mode} PRIVATE xray_xir_source xray_xir_vm)
endforeach()
foreach(target test_xir_p09_hash_foreach_matrix test_xir_p09_hash_foreach_matrix_native test_xir_p09_hash_foreach_matrix_mixed)
    target_compile_definitions(${target} PRIVATE XR_P09_HASH_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;source;ownership;codec;provider")
endforeach()

add_test(NAME test_xir_p09_hash_position81_vectors COMMAND ${XRAY_PYTHON}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_hash_position81_vectors.py"
    --source "${PROJECT_SOURCE_DIR}" --output "${CMAKE_BINARY_DIR}/p09-hash-position81-vectors")
set_tests_properties(test_xir_p09_hash_position81_vectors PROPERTIES TIMEOUT 30 LABELS "unit;xir;hash;codec")

add_executable(test_xir_p09_hash_foreach_resources
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_foreach_resources.c" "${HASH_FOREACH_MATRIX_C}")
target_link_libraries(test_xir_p09_hash_foreach_resources PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_p09_hash_foreach_resources PRIVATE XR_P09_HASH_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_xir_p09_hash_foreach_resources PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_hash_foreach_resources PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_hash_foreach_resources PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p09_hash_foreach_resources_normal COMMAND test_xir_p09_hash_foreach_resources --normal)
set_tests_properties(test_xir_p09_hash_foreach_resources_normal PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;resources;target-impact")
add_test(NAME test_xir_p09_hash_foreach_resources COMMAND test_xir_p09_hash_foreach_resources)
set_tests_properties(test_xir_p09_hash_foreach_resources PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;hash;resources;allocation;ownership;cancellation")

foreach(mode native mixed)
    set(target test_xir_p09_hash_foreach_resources_${mode})
    add_executable(${target}
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_foreach_resources_${mode}.c" "${HASH_FOREACH_C}" "${HASH_FOREACH_MATRIX_C}")
    target_link_libraries(${target} PRIVATE xray_xir_source xray_xir_vm)
    target_compile_definitions(${target} PRIVATE XR_P09_HASH_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${target}_normal COMMAND ${target} --normal)
    set_tests_properties(${target}_normal PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;resources;target-impact;provider")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;hash;resources;allocation;ownership;cancellation;provider")
endforeach()

foreach(target test_xir_p09_hash_foreach_resources test_xir_p09_hash_foreach_resources_native test_xir_p09_hash_foreach_resources_mixed)
    add_test(NAME ${target}_cancellation COMMAND ${target} --cancel)
    set_tests_properties(${target}_cancellation PROPERTIES TIMEOUT 120
        LABELS "unit;xir;hash;resources;cancellation;ownership;target-impact;provider")
endforeach()

add_executable(test_xir_p09_hash_foreach_compiler
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_hash_foreach_compiler.c")
target_link_libraries(test_xir_p09_hash_foreach_compiler PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm)
target_compile_definitions(test_xir_p09_hash_foreach_compiler PRIVATE XR_P09_HASH_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_xir_p09_hash_foreach_compiler PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_hash_foreach_compiler PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_hash_foreach_compiler PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p09_hash_foreach_compiler_normal COMMAND test_xir_p09_hash_foreach_compiler --normal)
add_test(NAME test_xir_p09_hash_foreach_compiler_resources COMMAND test_xir_p09_hash_foreach_compiler --resources)
set_tests_properties(test_xir_p09_hash_foreach_compiler_normal test_xir_p09_hash_foreach_compiler_resources
    PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;source;ownership;resources")
add_test(NAME test_xir_p09_hash_foreach_compiler COMMAND test_xir_p09_hash_foreach_compiler --full)
set_tests_properties(test_xir_p09_hash_foreach_compiler PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE COST 600
    LABELS "unit;xir;hash;source;ownership;fault-injection;compiler-faults")

foreach(row RANGE 0 3)
    foreach(target test_xir_p09_hash_foreach_resources test_xir_p09_hash_foreach_resources_native test_xir_p09_hash_foreach_resources_mixed)
        add_test(NAME ${target}_matrix_${row}_normal COMMAND ${target} --normal ${row})
        add_test(NAME ${target}_matrix_${row}_cancellation COMMAND ${target} --cancel ${row})
        set_tests_properties(${target}_matrix_${row}_normal ${target}_matrix_${row}_cancellation
            PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;resources;ownership;cancellation;target-impact")
        add_test(NAME ${target}_matrix_${row} COMMAND ${target} --full ${row})
        set_tests_properties(${target}_matrix_${row} PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE COST 600
            LABELS "unit;xir;hash;resources;allocation;ownership;cancellation")
    endforeach()
    add_test(NAME test_xir_p09_hash_foreach_compiler_matrix_${row}_normal COMMAND test_xir_p09_hash_foreach_compiler --normal "${PROJECT_SOURCE_DIR}/stdlib" ${row})
    add_test(NAME test_xir_p09_hash_foreach_compiler_matrix_${row}_resources COMMAND test_xir_p09_hash_foreach_compiler --resources "${PROJECT_SOURCE_DIR}/stdlib" ${row})
    set_tests_properties(test_xir_p09_hash_foreach_compiler_matrix_${row}_normal test_xir_p09_hash_foreach_compiler_matrix_${row}_resources
        PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;source;ownership;resources")
    add_test(NAME test_xir_p09_hash_foreach_compiler_matrix_${row} COMMAND test_xir_p09_hash_foreach_compiler --full "${PROJECT_SOURCE_DIR}/stdlib" ${row})
    set_tests_properties(test_xir_p09_hash_foreach_compiler_matrix_${row} PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE COST 600
        LABELS "unit;xir;hash;source;ownership;fault-injection;compiler-faults")
endforeach()
