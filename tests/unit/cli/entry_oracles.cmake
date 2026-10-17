if(NOT WIN32)
    return()
endif()
foreach(group vm native api)
    add_test(NAME cli_entry_oracles_${group} COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/entry_oracles/run_entry_oracles.py"
        --root "${PROJECT_SOURCE_DIR}" --cli $<TARGET_FILE:xray>
        --api $<TARGET_FILE:source_entry_oracles> --physical $<TARGET_FILE:source_entry_oracles_physical>
        --resources $<TARGET_FILE:test_source_public_faults>
        --injected $<TARGET_FILE:run_owner_injected> --group ${group}
        --output "${CMAKE_CURRENT_BINARY_DIR}/entry-oracles-${group}")
    set_tests_properties(cli_entry_oracles_${group} PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE COST 4
        LABELS "unit;cli;source;ownership;entry-oracle")
endforeach()
