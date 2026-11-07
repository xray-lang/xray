function(xray_effect_final_proof_executable target source)
    add_executable(${target} "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/${source}")
    target_include_directories(${target} PRIVATE ${XRAY_COMMON_INCLUDES})
    target_link_libraries(${target} PRIVATE xray_xir xray_xir_scalar xray_xir_source)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

foreach(proof_kind IN ITEMS effect_input_selector invocation_inventory root_input_seeds root_marker_read)
    xray_effect_final_proof_executable(test_xir_${proof_kind} test_xir_${proof_kind}.c)
endforeach()

set(final_proof_tests)
foreach(mode RANGE 0 3)
    add_test(NAME test_xir_root_marker_read_${mode} COMMAND test_xir_root_marker_read ${mode})
    list(APPEND final_proof_tests test_xir_root_marker_read_${mode})
endforeach()

list(APPEND final_proof_tests)

list(APPEND final_proof_tests)

list(APPEND final_proof_tests)
foreach(proof_kind IN ITEMS effect_input_selector)
    add_test(NAME test_xir_${proof_kind} COMMAND test_xir_${proof_kind})
    list(APPEND final_proof_tests test_xir_${proof_kind})
endforeach()

foreach(mode RANGE 0 13)
    add_test(NAME test_xir_invocation_inventory_${mode} COMMAND test_xir_invocation_inventory ${mode})
    list(APPEND final_proof_tests test_xir_invocation_inventory_${mode})
endforeach()
foreach(shape RANGE 0 15)
    foreach(attack RANGE 0 5)
        add_test(NAME test_xir_root_input_seeds_${shape}_${attack} COMMAND test_xir_root_input_seeds ${shape} ${attack})
        list(APPEND final_proof_tests test_xir_root_input_seeds_${shape}_${attack})
    endforeach()
endforeach()
foreach(mode RANGE 0 60)

    list(APPEND final_proof_tests)
endforeach()
foreach(mode RANGE 0 30)

    list(APPEND final_proof_tests)
endforeach()
set_tests_properties(${final_proof_tests} PROPERTIES
    LABELS "unit;xir;ownership;metadata;final-proof;partial-feedback"
    TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)


foreach(mode IN ITEMS 0 31 48 49)
    foreach(variant IN ITEMS proof-fi proof-fi-large large)
        string(REPLACE "-" "_" suffix "${variant}")

    endforeach()
endforeach()

add_test(NAME test_xir_effect_input_selector_resources COMMAND test_xir_effect_input_selector resources)
set_tests_properties(test_xir_effect_input_selector_resources PROPERTIES
    LABELS "unit;xir;ownership;metadata;final-proof;compiler-fault;resources"
    TIMEOUT 600 RUN_SERIAL TRUE PROCESSORS 1)
