add_executable(test_xir_go_source_native_producer xir/test_xir_go_source_native_producer.c)
target_link_libraries(test_xir_go_source_native_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_go_source_native_producer PRIVATE
    XR_GO_EXECUTION_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_go_execution"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(GO_SOURCE_NATIVE_NAMES integer string generic context_ordinary context_go context_existing context_spawn
    grouped import_default const_state local_storage unknown_task value_error shadow_class context_match
    context_match_block context_match_existing)
set(GO_SOURCE_NATIVE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/go_source_native")
set(GO_SOURCE_NATIVE_CS)
set(GO_SOURCE_NATIVE_FIXTURES)
foreach(case IN LISTS GO_SOURCE_NATIVE_NAMES)
    list(APPEND GO_SOURCE_NATIVE_CS "${GO_SOURCE_NATIVE_DIR}/source17_${case}.c")
    list(APPEND GO_SOURCE_NATIVE_FIXTURES "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_go_execution/${case}.xr")
endforeach()
list(APPEND GO_SOURCE_NATIVE_FIXTURES "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_go_execution/producer.xr")
add_custom_command(OUTPUT ${GO_SOURCE_NATIVE_CS} "${GO_SOURCE_NATIVE_DIR}/source17_generated.h"
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/xir/build_go_source_native_cases.py
        --producer $<TARGET_FILE:test_xir_go_source_native_producer> --output "${GO_SOURCE_NATIVE_DIR}"
    DEPENDS test_xir_go_source_native_producer ${GO_SOURCE_NATIVE_FIXTURES}
        xir/build_go_source_native_cases.py
    VERBATIM)
add_executable(test_xir_go_source_native_resources xir/test_xir_go_source_native_resources.c
    ${GO_SOURCE_NATIVE_CS} "${GO_SOURCE_NATIVE_DIR}/source17_generated.h"
    ${PROJECT_SOURCE_DIR}/src/base/xsha256.c ${PROJECT_SOURCE_DIR}/src/shared/xnative_declaration.c)
target_include_directories(test_xir_go_source_native_resources PRIVATE "${GO_SOURCE_NATIVE_DIR}")
if(WIN32)
    target_link_libraries(test_xir_go_source_native_resources PRIVATE kernel32)
endif()
foreach(target test_xir_go_source_native_producer test_xir_go_source_native_resources)
    target_include_directories(${target} PRIVATE ${XRAY_COMMON_INCLUDES})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(case IN LISTS GO_SOURCE_NATIVE_NAMES)
    foreach(mode measure faults axes cancel)
        add_test(NAME test_xir_go_source_native_${mode}_${case}
            COMMAND test_xir_go_source_native_resources --${mode} ${case})
        set_tests_properties(test_xir_go_source_native_${mode}_${case} PROPERTIES
            LABELS "unit;xir;task;source;native;ownership;runtime" TIMEOUT 300 RUN_SERIAL TRUE PROCESSORS 1)
    endforeach()
endforeach()
