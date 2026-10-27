add_executable(test_xir_intrinsic_registry xir/test_xir_intrinsic_registry.c)
target_sources(test_xir_intrinsic_registry PRIVATE xir/xir_string_slice_value_observer.c)
target_link_libraries(test_xir_intrinsic_registry PRIVATE
    xray_xir_source xray_xir_vm xray_xir_cgen xray_lir_lower)
set(intrinsic_contains_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/intrinsic_contains.c")
add_custom_command(OUTPUT "${intrinsic_contains_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_intrinsic_registry> --emit "${intrinsic_contains_generated}"
    DEPENDS test_xir_intrinsic_registry
    VERBATIM)
add_executable(test_xir_intrinsic_registry_native
    xir/test_xir_intrinsic_registry_native.c "${intrinsic_contains_generated}")
target_link_libraries(test_xir_intrinsic_registry_native PRIVATE xray_xir_scalar xray_compile_resources)
foreach(target test_xir_intrinsic_registry test_xir_intrinsic_registry_native)
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
add_test(NAME xir_intrinsic_contains COMMAND test_xir_intrinsic_registry
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/contains-schema33.xrc"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/scalar-schema33.xrc"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/predicates-schema34.xrc"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/scalar-schema34.xrc"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/evaluation-contract94.xrc"
    "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/time-schema38-contract98.xrc")
add_test(NAME xir_intrinsic_contains_native COMMAND test_xir_intrinsic_registry_native)
set_tests_properties(xir_intrinsic_contains xir_intrinsic_contains_native PROPERTIES
    LABELS "unit;xir;intrinsic;fast" TIMEOUT 60)

add_executable(test_xir_from_utf8 xir/test_xir_from_utf8.c)
target_link_libraries(test_xir_from_utf8 PRIVATE xray_source_api)
target_include_directories(test_xir_from_utf8 PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(test_xir_from_utf8 PRIVATE XR_PUBLIC_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
    XR_FROM_UTF8_HISTORICAL="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_intrinsics/current-schema38-contract99.xrc")
set(from_utf8_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/from_utf8.c")
add_custom_command(OUTPUT "${from_utf8_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_from_utf8> --emit "${from_utf8_generated}"
    DEPENDS test_xir_from_utf8 VERBATIM)
add_executable(test_xir_from_utf8_native xir/test_xir_from_utf8_native.c "${from_utf8_generated}")
target_link_libraries(test_xir_from_utf8_native PRIVATE xray_xir_scalar xray_compile_resources)
foreach(target test_xir_from_utf8 test_xir_from_utf8_native)
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
add_test(NAME xir_from_utf8 COMMAND test_xir_from_utf8)
add_test(NAME xir_from_utf8_native COMMAND test_xir_from_utf8_native)
set_tests_properties(xir_from_utf8 xir_from_utf8_native PROPERTIES LABELS "unit;xir;source;fast" TIMEOUT 60)

# Physical observer is a distinct consumer; canonical API/native targets keep their ordinary provider.
add_library(from_utf8_value_observer OBJECT xir/test_xir_from_utf8.c)
target_compile_definitions(from_utf8_value_observer PRIVATE XR_FROM_UTF8_VALUE_OBSERVER=1)
target_include_directories(from_utf8_value_observer PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_executable(test_xir_from_utf8_fault xir/test_xir_from_utf8.c $<TARGET_OBJECTS:from_utf8_value_observer>)
target_compile_definitions(test_xir_from_utf8_fault PRIVATE XR_FROM_UTF8_WITH_OBSERVER=1 XR_PUBLIC_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
target_include_directories(test_xir_from_utf8_fault PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(test_xir_from_utf8_fault PRIVATE xray_source_api)
foreach(target from_utf8_value_observer test_xir_from_utf8_fault)
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
add_test(NAME xir_from_utf8_physical_header COMMAND test_xir_from_utf8_fault --physical-header)
add_test(NAME xir_from_utf8_physical_bytes COMMAND test_xir_from_utf8_fault --physical-bytes)
set_tests_properties(xir_from_utf8_physical_header xir_from_utf8_physical_bytes PROPERTIES LABELS "unit;xir;source;resource" TIMEOUT 60)

add_test(NAME xir_from_utf8_cancel_after_allocation COMMAND test_xir_from_utf8_fault --cancel-after-allocation)
set_tests_properties(xir_from_utf8_cancel_after_allocation PROPERTIES LABELS "unit;xir;source;resource" TIMEOUT 60)
