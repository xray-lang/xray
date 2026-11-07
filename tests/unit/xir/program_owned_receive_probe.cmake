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

set(OWNED_NATIVE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_owned_native.c")

foreach(storage_case valid table-short table-interior parameter-pointer parameter-foreign
        parameter-interior zero-count-alias input-interior artifact-interior
        late-table-free late-table-alloc late-parameter-free late-parameter-alloc)

endforeach()

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
# Error bitmaps no longer exist; keep the original IDs of current storage cases.
foreach(final_storage_attack IN ITEMS
        0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22
        24 25 26 27 28 29 30 31 32 34 35 36 38)
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

foreach(entry_axis free alloc)

    add_test(NAME test_xir_program_owned_count_${entry_axis}
        COMMAND test_xir_program_owned_callbacks entry-count-${entry_axis})
    set_tests_properties(test_xir_program_owned_count_${entry_axis} PROPERTIES TIMEOUT 60 LABELS "unit;xir;program;ownership;atomic-receive")
endforeach()

foreach(worker_marker_callback worker-marker-free worker-marker-alloc)
    add_test(NAME test_xir_program_owned_callback_${worker_marker_callback}
        COMMAND test_xir_program_owned_callbacks ${worker_marker_callback})
    set_tests_properties(test_xir_program_owned_callback_${worker_marker_callback} PROPERTIES
        TIMEOUT 60 LABELS "unit;xir;program;ownership;current-error")
endforeach()
