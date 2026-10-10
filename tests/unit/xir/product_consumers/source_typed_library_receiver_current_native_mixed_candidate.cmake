# STATIC_NOT_RUN / NOT_REGISTERED. Public Git reference 9031dbc is not an adopted producer.
# Checked28/73, LibraryC2, Value23/Call29/Program30 require one fresh immutable provider.
# Root must publish and bind its complete source/library/tool/material guard before qualification.
# Four exclusive outputs are generated once in a fresh dedicated build directory.
set(SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT "${CMAKE_SOURCE_DIR}" CACHE PATH
    "Immutable provider owning product targets, embedded helpers, headers and stdlib")
set(source_typed_native_candidate "${CMAKE_CURRENT_LIST_DIR}")
set(source_typed_native_driver "${source_typed_native_candidate}/test_source_typed_library_receiver_current_native_mixed.c")
set(source_typed_native_support "${source_typed_native_candidate}/typed_library_native_mixed_support.h")
set(source_typed_native_fixture
    "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/tests/unit/xir/product_consumers/fixtures/module_receiver_suspend")
set(source_typed_native_generated "${CMAKE_BINARY_DIR}/generated/current-typed-library-native-mixed-r1")
set(source_typed_native_private "${source_typed_native_generated}/private-source")
set(source_typed_native_material "${source_typed_native_generated}/material")
set(source_typed_native_writer test_source_typed_library_receiver_current_native_writer)
set(source_typed_native_runner test_source_typed_library_receiver_current_native_mixed)
foreach(source_typed_native_product IN ITEMS xray_xir_source_product xray_xir_vm)
    if(NOT TARGET ${source_typed_native_product})
        message(FATAL_ERROR "Candidate requires immutable provider ${source_typed_native_product}")
    endif()
endforeach()
foreach(source_typed_native_helper IN ITEMS xir_source_program_compile_owner.h xir_runtime_allocations.h)
    if(NOT EXISTS "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/tests/unit/xir/${source_typed_native_helper}")
        message(FATAL_ERROR "Provider helper missing: ${source_typed_native_helper}")
    endif()
endforeach()
foreach(source_typed_native_leaf IN ITEMS root.xr library.xr)
    if(NOT EXISTS "${source_typed_native_fixture}/${source_typed_native_leaf}")
        message(FATAL_ERROR "Complete original typed Library Source input missing: ${source_typed_native_leaf}")
    endif()
endforeach()
add_executable(${source_typed_native_writer} "${source_typed_native_driver}" "${source_typed_native_support}")
target_compile_definitions(${source_typed_native_writer} PRIVATE XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_WRITER=1)
target_link_libraries(${source_typed_native_writer} PRIVATE xray_xir_source_product)
set(source_typed_native_c "${source_typed_native_material}/actual_payload.c")
set(source_typed_native_proof "${source_typed_native_material}/actual_proof.bin")
set(source_typed_native_identity "${source_typed_native_material}/actual_identity.bin")
set(source_typed_native_binding "${source_typed_native_material}/payload_binding.c")
add_custom_command(
    OUTPUT "${source_typed_native_c}" "${source_typed_native_proof}"
        "${source_typed_native_identity}" "${source_typed_native_binding}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${source_typed_native_private}" "${source_typed_native_material}"
    COMMAND $<TARGET_FILE:${source_typed_native_writer}>
        "${source_typed_native_private}" "${source_typed_native_fixture}"
        "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/stdlib"
        "${source_typed_native_c}" "${source_typed_native_proof}"
        "${source_typed_native_identity}" "${source_typed_native_binding}"
    DEPENDS ${source_typed_native_writer} "${source_typed_native_driver}" "${source_typed_native_support}"
        "${source_typed_native_fixture}/root.xr" "${source_typed_native_fixture}/library.xr"
        "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/stdlib/types/coro.xr"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Emit real Library native C and authentic full Closed packet/identity without runtime calls"
    VERBATIM)
add_executable(${source_typed_native_runner} "${source_typed_native_driver}" "${source_typed_native_support}"
    "${source_typed_native_c}" "${source_typed_native_binding}")
target_link_libraries(${source_typed_native_runner} PRIVATE xray_xir_vm)
# Cancellation reuses the same four authentic outputs and real host C/binding inputs.
set(source_typed_native_cancel_runner test_source_typed_library_receiver_current_native_mixed_cancel_prefix)
add_executable(${source_typed_native_cancel_runner} "${source_typed_native_driver}" "${source_typed_native_support}"
    "${source_typed_native_c}" "${source_typed_native_binding}")
target_compile_definitions(${source_typed_native_cancel_runner} PRIVATE XR_SOURCE_TYPED_LIBRARY_NATIVE_PACKET_CANCEL_RECEIVER=1)
target_link_libraries(${source_typed_native_cancel_runner} PRIVATE xray_xir_vm)
# The public VM native-cache closure still includes Source; physical SourceDelete is OPEN.
foreach(source_typed_native_target IN ITEMS ${source_typed_native_writer} ${source_typed_native_runner} ${source_typed_native_cancel_runner})
    target_include_directories(${source_typed_native_target} BEFORE PRIVATE
        "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/tests/unit/xir"
        "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/src" "${source_typed_native_candidate}")
    set_target_properties(${source_typed_native_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_definitions(${source_typed_native_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${source_typed_native_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${source_typed_native_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(source_typed_native_mode IN ITEMS 1 2 3)
    add_test(NAME ${source_typed_native_runner}_mode${source_typed_native_mode}
        COMMAND ${source_typed_native_runner} "${source_typed_native_mode}"
        "${source_typed_native_proof}" "${source_typed_native_identity}" "${source_typed_native_c}")
    set_tests_properties(${source_typed_native_runner}_mode${source_typed_native_mode} PROPERTIES
        WORKING_DIRECTORY "${source_typed_native_material}" TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;program-consumer;typed-library;module-graph;class;receiver;coroutine;ownership;packet;native;mixed;normal-only;current-interface-candidate")
endforeach()
# The original three ordinary tests retain their frozen independent facet.
# This separate facet drives only one call at a time with two initialized live Instances.
foreach(source_typed_native_mode IN ITEMS 1 2 3)
    add_test(NAME ${source_typed_native_cancel_runner}_mode${source_typed_native_mode}
        COMMAND ${source_typed_native_cancel_runner} "${source_typed_native_mode}"
        "${source_typed_native_proof}" "${source_typed_native_identity}" "${source_typed_native_c}")
    set_tests_properties(${source_typed_native_cancel_runner}_mode${source_typed_native_mode} PROPERTIES
        WORKING_DIRECTORY "${source_typed_native_material}" TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;program-consumer;typed-library;module-graph;receiver;coroutine;ownership;packet;native;mixed;cancel-prefix;normal-only;current-interface-candidate")
endforeach()
