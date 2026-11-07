

set(MAP_MEMBERSHIP_C "${CMAKE_BINARY_DIR}/generated/xir_p09_map_membership.c")

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

endforeach()

foreach(target IN ITEMS test_xir_p09_map_membership_resources)
    add_test(NAME ${target}_cancellation COMMAND ${target} --cancel)
    set_tests_properties(${target}_cancellation PROPERTIES TIMEOUT 120
        LABELS "unit;xir;hash;resources;cancellation;ownership;target-impact;provider")
endforeach()
