# NOT_REGISTERED / NOT_RUN. Deploy beside the dedicated receiver driver.
# Static interface reference: 98f439d88b033ed82699d3b8cb7a8e71148e6240,
# Checked 27/72, Library C interface 2, Value 22 / Call 28 / Program 29.
# This file grants no producer authorization or same-source material approval.
# Root archive/provider guard remains NOT_PUBLISHED / NOT_BOUND.
set(source_typed_library_receiver_target test_source_typed_library_receiver_current_vm)
set(SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT "${CMAKE_SOURCE_DIR}" CACHE PATH
    "Authorized immutable producer tree owning the linked targets, helpers, fixtures and stdlib")
set(source_typed_library_receiver_driver
    "${CMAKE_CURRENT_LIST_DIR}/test_source_typed_library_receiver_current_vm.c")
set(source_typed_library_receiver_fixture
    "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/tests/unit/xir/product_consumers/fixtures/module_receiver_suspend")
set(source_typed_library_receiver_private
    "${CMAKE_BINARY_DIR}/generated/current-typed-library-receiver-r1/private-source")
foreach(source_typed_library_receiver_product IN ITEMS xray_xir_source_product xray_xir_vm)
    if(NOT TARGET ${source_typed_library_receiver_product})
        message(FATAL_ERROR "Candidate requires the authorized producer ${source_typed_library_receiver_product}")
    endif()
endforeach()
foreach(source_typed_library_receiver_helper IN ITEMS xir_source_program_compile_owner.h xir_runtime_allocations.h)
    if(NOT EXISTS "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/tests/unit/xir/${source_typed_library_receiver_helper}")
        message(FATAL_ERROR "Producer helper missing: ${source_typed_library_receiver_helper}")
    endif()
endforeach()
# Only the directory exists before the test. The driver uses CREATE_NEW for its
# own leaves, preserves foreign files and removes only its own created inputs.
file(MAKE_DIRECTORY "${source_typed_library_receiver_private}")
add_executable(${source_typed_library_receiver_target} "${source_typed_library_receiver_driver}")
target_link_libraries(${source_typed_library_receiver_target} PRIVATE xray_xir_source_product xray_xir_vm)
# These observers embed product implementations; use the same immutable
# producer helper/src roots as the linked product targets. No worker fallback.
target_include_directories(${source_typed_library_receiver_target} BEFORE PRIVATE
    "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/tests/unit/xir"
    "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/src")
set_target_properties(${source_typed_library_receiver_target} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_definitions(${source_typed_library_receiver_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${source_typed_library_receiver_target} PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(${source_typed_library_receiver_target} PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME ${source_typed_library_receiver_target} COMMAND ${source_typed_library_receiver_target}
    "${source_typed_library_receiver_private}"
    "${source_typed_library_receiver_fixture}"
    "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/stdlib")
set_tests_properties(${source_typed_library_receiver_target} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE
    LABELS "unit;xir;program-consumer;typed-library;module-graph;class;receiver;coroutine;ownership;source;vm;normal-only;current-interface-candidate")
