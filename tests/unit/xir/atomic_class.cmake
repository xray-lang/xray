add_executable(test_xir_atomic_class_source xir/test_xir_atomic_class_source.c)
target_link_libraries(test_xir_atomic_class_source PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_include_directories(test_xir_atomic_class_source PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_BINARY_DIR}/generated)
set(ATOMIC_CLASS_TARGETS test_xir_atomic_class_source)
foreach(variant IN ITEMS identity scalars)
    if(variant STREQUAL "identity")
        set(fixture "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_atomic_class")
    else()
        set(fixture "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_atomic_class_all_scalars")
    endif()
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/atomic-class-${variant}-native.c")
    add_custom_command(OUTPUT "${generated}"
        COMMAND $<TARGET_FILE:test_xir_atomic_class_source> "${fixture}"
            "${PROJECT_SOURCE_DIR}/stdlib" "${generated}"
        DEPENDS test_xir_atomic_class_source "${fixture}/root.xr" VERBATIM)
    set(target test_xir_atomic_class_${variant}_backend)
    add_executable(${target} xir/test_xir_atomic_class_backend.c "${generated}")
    target_link_libraries(${target} PRIVATE xray_xir_vm)
    target_include_directories(${target} PRIVATE ${XRAY_COMMON_INCLUDES} ${CMAKE_BINARY_DIR}/generated)
    list(APPEND ATOMIC_CLASS_TARGETS ${target})
    add_test(NAME test_xir_atomic_class_source_${variant} COMMAND test_xir_atomic_class_source
        "${fixture}" "${PROJECT_SOURCE_DIR}/stdlib")
    set_tests_properties(test_xir_atomic_class_source_${variant} PROPERTIES
        TIMEOUT 60 LABELS "unit;xir;class;atomic;source;ownership")
    foreach(direction IN ITEMS 0 1 2)
        add_test(NAME test_xir_atomic_class_backend_${variant}_${direction} COMMAND ${target} ${direction})
        set_tests_properties(test_xir_atomic_class_backend_${variant}_${direction} PROPERTIES
            TIMEOUT 60 LABELS "unit;xir;class;atomic;native;mixed;generated-c;ownership")
        add_test(NAME test_xir_atomic_class_runtime_faults_${variant}_${direction}
            COMMAND ${target} ${direction} --runtime-faults)
        set_tests_properties(test_xir_atomic_class_runtime_faults_${variant}_${direction} PROPERTIES
            TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;class;atomic;native;mixed;resources;runtime-fault;ownership")
    endforeach()
    add_test(NAME test_xir_atomic_class_compile_resources_${variant} COMMAND test_xir_atomic_class_source
        "${fixture}" "${PROJECT_SOURCE_DIR}/stdlib" --faults)
    set_tests_properties(test_xir_atomic_class_compile_resources_${variant} PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE COST 300 LABELS "unit;xir;class;atomic;resources;compiler-fault;ownership")
    add_test(NAME test_xir_atomic_class_same_owner_${variant} COMMAND test_xir_atomic_class_source
        "${fixture}" "${PROJECT_SOURCE_DIR}/stdlib" --retry)
    set_tests_properties(test_xir_atomic_class_same_owner_${variant} PROPERTIES
        TIMEOUT 600 RUN_SERIAL TRUE COST 600 LABELS "unit;xir;class;atomic;resources;compiler-fault;same-owner;ownership")
endforeach()
foreach(target IN LISTS ATOMIC_CLASS_TARGETS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
