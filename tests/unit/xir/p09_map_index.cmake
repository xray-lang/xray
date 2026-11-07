

set(MAP_INDEX_C "${CMAKE_BINARY_DIR}/generated/xir_p09_map_index.c")

add_test(NAME test_xir_p09_map_index77_vectors COMMAND ${XRAY_PYTHON}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_map_index77_vectors.py"
    --source "${PROJECT_SOURCE_DIR}" --output "${CMAKE_BINARY_DIR}/p09-map-index77-vectors")
set_tests_properties(test_xir_p09_map_index77_vectors PROPERTIES TIMEOUT 30 LABELS "unit;xir;hash;codec")

foreach(suffix "")
    set(target "test_xir_p09_map_index_resources${suffix}")
    add_executable(${target} "${PROJECT_SOURCE_DIR}/tests/unit/xir/${target}.c")
    if(NOT suffix STREQUAL "")
        target_sources(${target} PRIVATE "${MAP_INDEX_C}")
    endif()
    if(suffix STREQUAL "_native")
        target_link_libraries(${target} PRIVATE xray_xir_scalar)
    else()
        target_link_libraries(${target} PRIVATE xray_xir_source )
    endif()
    target_compile_definitions(${target} PRIVATE XR_P09_HASH_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${target}_normal COMMAND ${target} --normal)
    add_test(NAME ${target}_cancel COMMAND ${target} --cancel)
    set_tests_properties(${target}_normal ${target}_cancel PROPERTIES TIMEOUT 120
        LABELS "unit;xir;hash;source;ownership;resources;cancellation")
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE COST 600
        LABELS "unit;xir;hash;source;ownership;resources;fault-injection;cancellation")
endforeach()
