add_executable(test_xir_go_pending_task_producer xir/test_xir_go_pending_task_producer.c)
add_executable(test_xir_go_pending_variant_producer xir/test_xir_go_pending_variant_producer.c)
foreach(target test_xir_go_pending_task_producer test_xir_go_pending_variant_producer)
    target_link_libraries(${target} PRIVATE xray_xir_cgen xray_xir_scalar)
endforeach()
set(GO_PENDING_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/go_pending_normal")
set(GO_PENDING_TASK_CS)
set(GO_PENDING_VARIANT_CS)
foreach(case body call await)
    list(APPEND GO_PENDING_TASK_CS "${GO_PENDING_DIR}/pending_${case}.c")
endforeach()
foreach(case body nested write)
    list(APPEND GO_PENDING_VARIANT_CS "${GO_PENDING_DIR}/pending_variant_${case}.c")
endforeach()
add_custom_command(OUTPUT ${GO_PENDING_TASK_CS} ${GO_PENDING_VARIANT_CS}
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/build_go_pending_cases.py
        --task-producer $<TARGET_FILE:test_xir_go_pending_task_producer>
        --variant-producer $<TARGET_FILE:test_xir_go_pending_variant_producer> --output "${GO_PENDING_DIR}"
    DEPENDS test_xir_go_pending_task_producer test_xir_go_pending_variant_producer
        xir/build_go_pending_cases.py xir/xir_task_fault_cleanup_fixture.h
        xir/xir_cleanup_recovery_fixture.h xir/xir_pending_exit_nested_fixture.h
        xir/xir_pending_exit_variants_fixture.h
    VERBATIM)
add_executable(test_xir_go_pending_task_native xir/test_xir_go_pending_task_native.c ${GO_PENDING_TASK_CS}
    ${PROJECT_SOURCE_DIR}/src/base/xsha256.c ${PROJECT_SOURCE_DIR}/src/shared/xnative_declaration.c)
add_executable(test_xir_go_pending_variant_native xir/test_xir_go_pending_variant_native.c ${GO_PENDING_VARIANT_CS}
    ${PROJECT_SOURCE_DIR}/src/base/xsha256.c ${PROJECT_SOURCE_DIR}/src/shared/xnative_declaration.c)
foreach(target test_xir_go_pending_task_producer test_xir_go_pending_variant_producer
               test_xir_go_pending_task_native test_xir_go_pending_variant_native)
    target_include_directories(${target} PRIVATE ${XRAY_COMMON_INCLUDES})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
if(WIN32)
    target_link_libraries(test_xir_go_pending_task_native PRIVATE kernel32)
    target_link_libraries(test_xir_go_pending_variant_native PRIVATE kernel32)
endif()
foreach(backend vm native)
    foreach(case body call await)
        add_test(NAME test_xir_go_pending_${backend}_${case}
            COMMAND test_xir_go_pending_task_native ${backend} ${case})
        set_tests_properties(test_xir_go_pending_${backend}_${case} PROPERTIES
            TIMEOUT 180 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;task;cleanup;normal")
    endforeach()
    foreach(kind RANGE 0 9)
        add_test(NAME test_xir_go_pending_${backend}_variant_${kind}
            COMMAND test_xir_go_pending_variant_native ${backend} ${kind})
        set_tests_properties(test_xir_go_pending_${backend}_variant_${kind} PROPERTIES
            TIMEOUT 180 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;task;cleanup;normal")
    endforeach()
endforeach()
