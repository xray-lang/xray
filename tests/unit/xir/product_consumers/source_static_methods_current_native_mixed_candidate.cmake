# NOT_REGISTERED / NOT_RUN. Place beside the dedicated consumer driver.
# Only an authorized immutable producer may include
# this file. PUBLIC libraries and embedded implementation helper includes must
# resolve inside the same producer tree; formal-inputs are review anchors only.
# The Root shared material publication guard is NOT_PUBLISHED and is required
# before either configuration is qualified. This file supplies no guard/registry.
set(source_static_native_candidate "${CMAKE_CURRENT_LIST_DIR}")
set(SOURCE_STATIC_CURRENT_FORMAL_ROOT "${CMAKE_SOURCE_DIR}" CACHE PATH
    "Authorized immutable producer tree owning xray_xir_source_product and helper implementations")
set(source_static_native_shape "${source_static_native_candidate}")
set(source_static_native_fixture "${source_static_native_candidate}/fixtures/static_methods")
set(source_static_native_generated "${CMAKE_CURRENT_BINARY_DIR}/source-static-current-native-generated")
set(source_static_native_writer test_source_static_methods_current_native_writer)
set(source_static_native_runner test_source_static_methods_current_native_mixed)
set(source_static_native_driver "${source_static_native_candidate}/test_source_static_methods_current_native_mixed.c")
set(source_static_native_support "${source_static_native_candidate}/native_mixed_support.h")
if(NOT TARGET xray_xir_source_product OR NOT TARGET xray_xir_vm)
    message(FATAL_ERROR "Candidate requires the producer SourceProduct writer and public VM receiver targets")
endif()
foreach(source_static_native_helper IN ITEMS xir_source_program_compile_owner.h xir_runtime_allocations.h)
    if(NOT EXISTS "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/tests/unit/xir/${source_static_native_helper}")
        message(FATAL_ERROR "Producer helper missing: ${source_static_native_helper}")
    endif()
endforeach()
add_executable(${source_static_native_writer} "${source_static_native_driver}")
set(source_static_native_c "${source_static_native_generated}/actual_payload.c")
set(source_static_native_proof "${source_static_native_generated}/actual_proof.bin")
set(source_static_native_identity "${source_static_native_generated}/actual_identity.bin")
set(source_static_native_binding "${source_static_native_generated}/payload_binding.c")
add_custom_command(
    OUTPUT "${source_static_native_c}" "${source_static_native_proof}"
        "${source_static_native_identity}" "${source_static_native_binding}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${source_static_native_generated}"
    COMMAND $<TARGET_FILE:${source_static_native_writer}>
        "${source_static_native_fixture}" "${source_static_native_fixture}/root.xr"
        "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/stdlib"
        "${source_static_native_c}" "${source_static_native_proof}"
        "${source_static_native_identity}" "${source_static_native_binding}"
    DEPENDS ${source_static_native_writer} "${source_static_native_driver}"
        "${source_static_native_support}" "${source_static_native_fixture}/root.xr"
        "${source_static_native_shape}/static_methods_source_shape.h"
    COMMENT "Emit untouched actual C and authentic Lowered proof/identity, normal path only"
    VERBATIM)
add_executable(${source_static_native_runner} "${source_static_native_driver}"
    "${source_static_native_c}" "${source_static_native_binding}")
target_compile_definitions(${source_static_native_runner} PRIVATE XR_SOURCE_STATIC_NATIVE_RUNNER=1)
target_link_libraries(${source_static_native_writer} PRIVATE xray_xir_source_product)
# The VM's public native-cache dependency still includes Source; physical removal is unqualified.
target_link_libraries(${source_static_native_runner} PRIVATE xray_xir_vm)
foreach(source_static_native_target IN ITEMS ${source_static_native_writer} ${source_static_native_runner})
    target_include_directories(${source_static_native_target} BEFORE PRIVATE
        "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/tests/unit/xir"
        "${SOURCE_STATIC_CURRENT_FORMAL_ROOT}/src"
        "${source_static_native_shape}" "${source_static_native_candidate}")
    set_target_properties(${source_static_native_target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_definitions(${source_static_native_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${source_static_native_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${source_static_native_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(source_static_native_mode IN ITEMS 1 2 3)
    add_test(NAME ${source_static_native_runner}_mode${source_static_native_mode}
        COMMAND ${source_static_native_runner} "${source_static_native_mode}"
        "${source_static_native_proof}" "${source_static_native_identity}"
        "${source_static_native_c}")
    set_tests_properties(${source_static_native_runner}_mode${source_static_native_mode} PROPERTIES
        TIMEOUT 120 PROCESSORS 1
        WORKING_DIRECTORY "${source_static_native_generated}"
        LABELS "unit;xir;program-consumer;class;static-method;ownership;source-free;packet;native;mixed;current-interface-candidate;normal-only")
endforeach()
