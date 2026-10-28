add_executable(test_xir_program_owned_receive_probe "${CMAKE_CURRENT_LIST_DIR}/test_xir_program_owned_receive_probe.c")
target_include_directories(test_xir_program_owned_receive_probe PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(test_xir_program_owned_receive_probe PRIVATE xray_xir_vm)
target_compile_features(test_xir_program_owned_receive_probe PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_xir_program_owned_receive_probe PRIVATE /W4 /WX /utf-8)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_xir_program_owned_receive_probe PRIVATE /experimental:c11atomics)
endif()
add_test(NAME test_xir_program_owned_receive_probe COMMAND test_xir_program_owned_receive_probe)
set_tests_properties(test_xir_program_owned_receive_probe PROPERTIES TIMEOUT 60)

add_executable(xir_owned_input_emit "${CMAKE_CURRENT_LIST_DIR}/test_xir_program_owned_emit.c")
target_include_directories(xir_owned_input_emit PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(xir_owned_input_emit PRIVATE xray_xir_cgen xray_xir_vm)
target_compile_features(xir_owned_input_emit PRIVATE c_std_11)
set(OWNED_NATIVE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_owned_native.c")
add_custom_command(OUTPUT "${OWNED_NATIVE_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND xir_owned_input_emit "${OWNED_NATIVE_SOURCE}"
    DEPENDS xir_owned_input_emit VERBATIM)
add_executable(test_xir_program_owned_transports
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_program_owned_transports.c" "${OWNED_NATIVE_SOURCE}")
target_include_directories(test_xir_program_owned_transports PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(test_xir_program_owned_transports PRIVATE xray_xir_vm)
target_compile_features(test_xir_program_owned_transports PRIVATE c_std_11)
foreach(owned_consumer xir_owned_input_emit test_xir_program_owned_transports)
    if(MSVC)
        target_compile_options(${owned_consumer} PRIVATE /W4 /WX /utf-8)
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${owned_consumer} PRIVATE /experimental:c11atomics)
    endif()
endforeach()

add_executable(test_xir_program_owned_storage "${CMAKE_CURRENT_LIST_DIR}/test_xir_program_owned_storage.c")
target_include_directories(test_xir_program_owned_storage PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(test_xir_program_owned_storage PRIVATE xray_xir_vm)
target_compile_features(test_xir_program_owned_storage PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_xir_program_owned_storage PRIVATE /W4 /WX /utf-8)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_xir_program_owned_storage PRIVATE /experimental:c11atomics)
endif()
foreach(storage_case valid table-short table-interior parameter-pointer parameter-foreign
        parameter-interior zero-count-alias input-interior artifact-interior
        late-table-free late-table-alloc late-parameter-free late-parameter-alloc)
    add_test(NAME test_xir_program_owned_storage_${storage_case}
        COMMAND test_xir_program_owned_storage ${storage_case})
    set_tests_properties(test_xir_program_owned_storage_${storage_case}
        PROPERTIES TIMEOUT 60 LABELS "unit;xir;program;ownership;physical-storage")
endforeach()

add_executable(test_xir_program_input_carrier "${CMAKE_CURRENT_LIST_DIR}/test_xir_program_input_carrier.c")
target_include_directories(test_xir_program_input_carrier PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(test_xir_program_input_carrier PRIVATE xray_xir_vm)
target_compile_features(test_xir_program_input_carrier PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_xir_program_input_carrier PRIVATE /W4 /WX /utf-8)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_xir_program_input_carrier PRIVATE /experimental:c11atomics)
endif()
foreach(carrier_case valid semantic body layout)
    add_test(NAME test_xir_program_input_carrier_${carrier_case}
        COMMAND test_xir_program_input_carrier ${carrier_case})
    set_tests_properties(test_xir_program_input_carrier_${carrier_case}
        PROPERTIES TIMEOUT 60 LABELS "unit;xir;program;ownership;carrier")
endforeach()

add_test(NAME test_xir_cleanup_source_compile
    COMMAND test_xir_cleanup_source "${CMAKE_CURRENT_BINARY_DIR}/xir_cleanup_compile_probe.c")
set_tests_properties(test_xir_cleanup_source_compile PROPERTIES
    TIMEOUT 60 LABELS "unit;xir;program;ownership;compile;resource")

add_executable(test_xir_program_owned_suspension "${CMAKE_CURRENT_LIST_DIR}/test_xir_program_owned_suspension.c")
target_include_directories(test_xir_program_owned_suspension PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(test_xir_program_owned_suspension PRIVATE xray_xir_vm)
target_compile_features(test_xir_program_owned_suspension PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_xir_program_owned_suspension PRIVATE /W4 /WX /utf-8)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_xir_program_owned_suspension PRIVATE /experimental:c11atomics)
endif()
foreach(suspension_case RANGE 0 3)
    add_test(NAME test_xir_program_owned_suspension_${suspension_case}
        COMMAND test_xir_program_owned_suspension ${suspension_case})
    set_tests_properties(test_xir_program_owned_suspension_${suspension_case} PROPERTIES TIMEOUT 60)
endforeach()
add_test(NAME test_xir_program_owned_transports COMMAND test_xir_program_owned_transports)
set_tests_properties(test_xir_program_owned_transports PROPERTIES TIMEOUT 60)

add_executable(test_xir_program_owned_callbacks "${CMAKE_CURRENT_LIST_DIR}/test_xir_program_owned_callbacks.c")
target_include_directories(test_xir_program_owned_callbacks PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(test_xir_program_owned_callbacks PRIVATE xray_xir_vm)
target_compile_features(test_xir_program_owned_callbacks PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_xir_program_owned_callbacks PRIVATE /W4 /WX /utf-8)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_xir_program_owned_callbacks PRIVATE /experimental:c11atomics)
endif()
add_test(NAME test_xir_program_owned_callbacks COMMAND test_xir_program_owned_callbacks)
set_tests_properties(test_xir_program_owned_callbacks PROPERTIES TIMEOUT 60)
add_test(NAME test_xir_program_owned_final_callbacks COMMAND test_xir_program_owned_callbacks final)
set_tests_properties(test_xir_program_owned_final_callbacks PROPERTIES TIMEOUT 60)

add_executable(test_xir_program_owned_final_storage "${CMAKE_CURRENT_LIST_DIR}/test_xir_program_owned_final_storage.c")
target_include_directories(test_xir_program_owned_final_storage PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(test_xir_program_owned_final_storage PRIVATE xray_xir_vm)
target_compile_features(test_xir_program_owned_final_storage PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_xir_program_owned_final_storage PRIVATE /W4 /WX /utf-8)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_xir_program_owned_final_storage PRIVATE /experimental:c11atomics)
endif()
foreach(final_storage_attack RANGE 0 38)
    add_test(NAME test_xir_program_owned_final_storage_${final_storage_attack}
        COMMAND test_xir_program_owned_final_storage ${final_storage_attack})
    set_tests_properties(test_xir_program_owned_final_storage_${final_storage_attack} PROPERTIES TIMEOUT 60)
endforeach()

foreach(outcome_callback joint suspend task-creation throws)
    add_test(NAME test_xir_program_owned_callback_${outcome_callback}
        COMMAND test_xir_program_owned_callbacks ${outcome_callback})
    set_tests_properties(test_xir_program_owned_callback_${outcome_callback} PROPERTIES TIMEOUT 60)
endforeach()
foreach(input_header_attack RANGE 0 3)
    add_test(NAME test_xir_program_owned_input_header_${input_header_attack}
        COMMAND test_xir_program_owned_callbacks input-header-${input_header_attack})
    set_tests_properties(test_xir_program_owned_input_header_${input_header_attack} PROPERTIES TIMEOUT 60)
endforeach()

foreach(seeded_callback seeded-untyped-free seeded-untyped-alloc seeded-unknown-free seeded-unknown-alloc seed-erase-free seed-erase-alloc)
    add_test(NAME test_xir_program_owned_callback_${seeded_callback}
        COMMAND test_xir_program_owned_callbacks ${seeded_callback})
    set_tests_properties(test_xir_program_owned_callback_${seeded_callback} PROPERTIES
        TIMEOUT 60 LABELS "unit;xir;program;ownership;current-error")
endforeach()

add_executable(test_xir_program_owned_entries "${CMAKE_CURRENT_LIST_DIR}/test_xir_program_owned_entries.c")
target_include_directories(test_xir_program_owned_entries PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(test_xir_program_owned_entries PRIVATE xray_xir_vm)
target_compile_features(test_xir_program_owned_entries PRIVATE c_std_11)
if(MSVC)
    target_compile_options(test_xir_program_owned_entries PRIVATE /W4 /WX /utf-8)
endif()
if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(test_xir_program_owned_entries PRIVATE /experimental:c11atomics)
endif()
foreach(entry_axis free alloc)
    add_test(NAME test_xir_program_owned_entries_${entry_axis}
        COMMAND test_xir_program_owned_entries ${entry_axis})
    add_test(NAME test_xir_program_owned_count_${entry_axis}
        COMMAND test_xir_program_owned_callbacks entry-count-${entry_axis})
    set_tests_properties(test_xir_program_owned_entries_${entry_axis} test_xir_program_owned_count_${entry_axis}
        PROPERTIES TIMEOUT 60 LABELS "unit;xir;program;ownership;atomic-receive")
endforeach()

foreach(worker_marker_callback worker-marker-free worker-marker-alloc)
    add_test(NAME test_xir_program_owned_callback_${worker_marker_callback}
        COMMAND test_xir_program_owned_callbacks ${worker_marker_callback})
    set_tests_properties(test_xir_program_owned_callback_${worker_marker_callback} PROPERTIES
        TIMEOUT 60 LABELS "unit;xir;program;ownership;current-error")
endforeach()
