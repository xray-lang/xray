# The original four Source inputs and nine lifecycle modes use every real route.
set(CELL_LIFE_NATIVE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/cell_lifecycle_native")
set(CELL_LIFE_NATIVE_C)
foreach(fixture RANGE 0 3)
    list(APPEND CELL_LIFE_NATIVE_C "${CELL_LIFE_NATIVE_DIR}/case${fixture}.c")
endforeach()
set(CELL_LIFE_NATIVE_H "${CELL_LIFE_NATIVE_DIR}/cell_lifecycle_generated.h")
add_executable(test_xir_cell_lifecycle_native_producer xir/test_xir_cell_loan_lifecycle.c)
target_link_libraries(test_xir_cell_lifecycle_native_producer PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_cell_lifecycle_native_producer PRIVATE XR_CELL_LIFE_PRODUCER=1
    XR_CELL_OWNER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_cell_owner")
set(CELL_LIFE_NATIVE_INPUTS)
foreach(fixture IN ITEMS lifecycle indirect_ref_lifecycle project_lifecycle class_project_lifecycle)
    list(APPEND CELL_LIFE_NATIVE_INPUTS "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_cell_owner/${fixture}.xr")
endforeach()
add_custom_command(OUTPUT ${CELL_LIFE_NATIVE_C} "${CELL_LIFE_NATIVE_H}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CELL_LIFE_NATIVE_DIR}"
    COMMAND $<TARGET_FILE:test_xir_cell_lifecycle_native_producer> --write-c ${CELL_LIFE_NATIVE_C} "${CELL_LIFE_NATIVE_H}"
    DEPENDS test_xir_cell_lifecycle_native_producer ${CELL_LIFE_NATIVE_INPUTS}
        "${CMAKE_CURRENT_LIST_DIR}/xir_cell_lifecycle_native_producer.h"
        "${CMAKE_CURRENT_LIST_DIR}/xir_cell_owner_source_pipeline.h"
    VERBATIM)
set(lifecycle_backend_index 1)
foreach(backend IN ITEMS native vm_native native_vm)
    set(target test_xir_cell_lifecycle_${backend})
    add_executable(${target} xir/test_xir_cell_loan_lifecycle.c ${CELL_LIFE_NATIVE_C} "${CELL_LIFE_NATIVE_H}")
    target_include_directories(${target} PRIVATE "${CELL_LIFE_NATIVE_DIR}")
    target_link_libraries(${target} PRIVATE xray_xir_source xray_xir_vm)
    target_compile_definitions(${target} PRIVATE XR_CELL_LIFE_BACKEND=${lifecycle_backend_index}
        XR_CELL_OWNER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_cell_owner")
    math(EXPR lifecycle_backend_index "${lifecycle_backend_index}+1")
    foreach(mode IN ITEMS normal fi axes prepare)
        add_test(NAME ${target}_${mode} COMMAND ${target} --runtime-${mode})
        set_tests_properties(${target}_${mode} PROPERTIES TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE
            LABELS "unit;xir;ownership;source;lifecycle;resources;generated-c;portability;${backend}")
    endforeach()
endforeach()
foreach(target IN ITEMS test_xir_cell_lifecycle_native_producer test_xir_cell_lifecycle_native
        test_xir_cell_lifecycle_vm_native test_xir_cell_lifecycle_native_vm)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
