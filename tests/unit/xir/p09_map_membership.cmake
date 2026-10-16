add_executable(test_xir_p09_map_membership "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_map_membership.c")
target_link_libraries(test_xir_p09_map_membership PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm)
set(MAP_MEMBERSHIP_C "${CMAKE_BINARY_DIR}/generated/xir_p09_map_membership.c")
add_custom_command(OUTPUT "${MAP_MEMBERSHIP_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_p09_map_membership> "${MAP_MEMBERSHIP_C}"
    DEPENDS test_xir_p09_map_membership VERBATIM)
add_executable(test_xir_p09_map_membership_native
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_map_membership_native.c" "${MAP_MEMBERSHIP_C}")
target_link_libraries(test_xir_p09_map_membership_native PRIVATE xray_xir_scalar)
add_executable(test_xir_p09_map_membership_mixed
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_map_membership_mixed.c" "${MAP_MEMBERSHIP_C}")
target_link_libraries(test_xir_p09_map_membership_mixed PRIVATE xray_xir_source xray_xir_vm)
foreach(target test_xir_p09_map_membership test_xir_p09_map_membership_native test_xir_p09_map_membership_mixed)
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


add_test(NAME test_xir_p09_map_membership80_vectors COMMAND ${XRAY_PYTHON}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_map_membership80_vectors.py"
    --source "${PROJECT_SOURCE_DIR}" --output "${CMAKE_BINARY_DIR}/p09-map-membership80-vectors")
set_tests_properties(test_xir_p09_map_membership80_vectors PROPERTIES TIMEOUT 30 LABELS "unit;xir;hash;codec")

add_executable(test_xir_p09_map_membership_resources
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_map_membership_resources.c")
target_link_libraries(test_xir_p09_map_membership_resources PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_p09_map_membership_resources PRIVATE XR_P09_HASH_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_xir_p09_map_membership_resources PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_map_membership_resources PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_map_membership_resources PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p09_map_membership_resources_normal COMMAND test_xir_p09_map_membership_resources --normal)
set_tests_properties(test_xir_p09_map_membership_resources_normal PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;resources;target-impact")
add_test(NAME test_xir_p09_map_membership_resources COMMAND test_xir_p09_map_membership_resources)
set_tests_properties(test_xir_p09_map_membership_resources PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;hash;resources;allocation;ownership;cancellation")

foreach(mode native mixed)
    set(target test_xir_p09_map_membership_resources_${mode})
    add_executable(${target}
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_map_membership_resources_${mode}.c" "${MAP_MEMBERSHIP_C}")
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

foreach(target test_xir_p09_map_membership_resources test_xir_p09_map_membership_resources_native test_xir_p09_map_membership_resources_mixed)
    add_test(NAME ${target}_cancellation COMMAND ${target} --cancel)
    set_tests_properties(${target}_cancellation PROPERTIES TIMEOUT 120
        LABELS "unit;xir;hash;resources;cancellation;ownership;target-impact;provider")
endforeach()

add_executable(test_xir_p09_map_membership_compiler
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_map_membership_compiler.c")
target_link_libraries(test_xir_p09_map_membership_compiler PRIVATE xray_xir_source xray_xir_cgen xray_xir_vm)
target_compile_definitions(test_xir_p09_map_membership_compiler PRIVATE XR_P09_HASH_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set_target_properties(test_xir_p09_map_membership_compiler PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p09_map_membership_compiler PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_p09_map_membership_compiler PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p09_map_membership_compiler_normal COMMAND test_xir_p09_map_membership_compiler --normal)
add_test(NAME test_xir_p09_map_membership_compiler_resources COMMAND test_xir_p09_map_membership_compiler --resources)
set_tests_properties(test_xir_p09_map_membership_compiler_normal test_xir_p09_map_membership_compiler_resources
    PROPERTIES TIMEOUT 120 LABELS "unit;xir;hash;source;ownership;resources")
add_test(NAME test_xir_p09_map_membership_compiler COMMAND test_xir_p09_map_membership_compiler --full)
set_tests_properties(test_xir_p09_map_membership_compiler PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE COST 600
    LABELS "unit;xir;hash;source;ownership;fault-injection;compiler-faults")
