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

foreach(target IN ITEMS test_xir_go_source_native_producer)
    target_include_directories(${target} PRIVATE ${XRAY_COMMON_INCLUDES})
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
