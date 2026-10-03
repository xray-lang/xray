if(NOT WIN32)
    return()
endif()
set(build_source_cli_sources
    xcmd_build.c xcli_spec.c xcli_parser.c xcli_diag.c xcli_output.c xcli_help.c)
list(TRANSFORM build_source_cli_sources PREPEND "${PROJECT_SOURCE_DIR}/src/app/cli/")
foreach(target build_source_driver build_source_faults)
    add_executable(${target} "${CMAKE_CURRENT_LIST_DIR}/build_source_owner/build_source_driver.c"
        ${build_source_cli_sources}
        "${PROJECT_SOURCE_DIR}/src/os/win/fd_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/time_win.c")
    target_link_libraries(${target} PRIVATE xray_cli_source xray_xir_native_projection
        xray_xir_local_toolchain xray_xir_native_admission)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()
target_link_libraries(build_source_driver PRIVATE xray_xir_publication)
target_compile_definitions(build_source_faults PRIVATE BUILD_SOURCE_INJECTED=1
    xtc_xir_native_operation_close=xr_cli_test_operation_close
    xtc_xir_native_operation_cleanup_diagnostic=xr_cli_test_operation_cleanup_diagnostic)
target_link_libraries(build_source_faults PRIVATE xray_xir_target bcrypt)
add_executable(test_publication_owner "${CMAKE_CURRENT_LIST_DIR}/build_source_owner/test_publication_owner.c")
target_link_libraries(test_publication_owner PRIVATE xray_xir_publication)
add_executable(test_publication_faults "${CMAKE_CURRENT_LIST_DIR}/build_source_owner/test_publication_owner.c"
    "${PROJECT_SOURCE_DIR}/src/base/xio_policy.c")
target_compile_definitions(test_publication_faults PRIVATE PUBLICATION_INJECTED=1)
target_link_libraries(test_publication_faults PRIVATE xray_xir_target bcrypt)
foreach(target build_source_driver build_source_faults test_publication_owner test_publication_faults)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include"
        "${CMAKE_BINARY_DIR}/generated")
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /experimental:c11atomics)
    endif()
endforeach()
add_test(NAME build_source_owner COMMAND ${XRAY_PYTHON} -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/build_source_owner/build_source_cases.py"
    --root "${PROJECT_SOURCE_DIR}" --driver $<TARGET_FILE:build_source_driver>
    --output "${CMAKE_CURRENT_BINARY_DIR}/build-source-owner"
    --publication $<TARGET_FILE:test_publication_owner> --faults $<TARGET_FILE:test_publication_faults>
    --fault-driver $<TARGET_FILE:build_source_faults>)
set_tests_properties(build_source_owner PROPERTIES TIMEOUT 120 COST 3
    LABELS "unit;cli;xir;ownership;budget")
add_test(NAME build_source_native_c11 COMMAND ${XRAY_PYTHON} -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/build_source_owner/build_source_native_cases.py"
    --root "${PROJECT_SOURCE_DIR}" --driver $<TARGET_FILE:build_source_driver>
    --output "${CMAKE_CURRENT_BINARY_DIR}/build-source-native-c11"
    --sdk "${CMAKE_BINARY_DIR}/xir-runtime-sdk"
    --cc "${XIR_INVOCATION_MSVC}" --linker "${XIR_INVOCATION_LINKER}")
set_tests_properties(build_source_native_c11 PROPERTIES TIMEOUT 180 COST 5
    LABELS "unit;cli;xir;generated-c")
add_test(NAME build_source_native_admission COMMAND ${XRAY_PYTHON} -X utf8
    "${CMAKE_CURRENT_LIST_DIR}/build_source_owner/build_source_native_admission.py"
    --root "${PROJECT_SOURCE_DIR}" --driver $<TARGET_FILE:build_source_driver>
    --output "${CMAKE_CURRENT_BINARY_DIR}/build-source-native-admission"
    --cc "${XIR_INVOCATION_MSVC}" --fault-driver $<TARGET_FILE:build_source_faults>)
set_tests_properties(build_source_native_admission PROPERTIES TIMEOUT 300 COST 130 RUN_SERIAL TRUE
    LABELS "unit;cli;xir;ownership;generated-c")
