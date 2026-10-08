# Unmodified source declarations are checked before isolated VM execution.
set(source_allocation0_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_allocation0_fixture "${CMAKE_BINARY_DIR}/generated/source-allocation0")
file(MAKE_DIRECTORY "${source_allocation0_fixture}")
file(GENERATE OUTPUT "${source_allocation0_fixture}/root.xr" CONTENT [==[fn answer() -> i64 { return 0 }
fn exported() -> i64 { return 42 }
@test
fn checkAnswer() { assert(exported() == 42) }
]==] NEWLINE_STYLE LF)
add_executable(test_source_product_allocation_scenario0
    "${source_allocation0_dir}/test_source_product_allocation_scenario0.c")
target_link_libraries(test_source_product_allocation_scenario0 PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_allocation_scenario0 PRIVATE "${source_allocation0_dir}/..")
set_target_properties(test_source_product_allocation_scenario0 PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_allocation_scenario0 PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_allocation_scenario0 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_allocation_scenario0 PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_allocation_scenario0_normal
    COMMAND test_source_product_allocation_scenario0
        "${source_allocation0_fixture}" "${source_allocation0_fixture}/root.xr")
set_tests_properties(test_source_product_allocation_scenario0_normal PROPERTIES
    TIMEOUT 120 LABELS "unit;xir;source-product;program-consumer;source-allocation;ownership;vm-projection")

# Each process creates and releases fresh owners for its exact fault ordinals.
foreach(source_allocation0_kind IN ITEMS compiler runtime)
    set(source_allocation0_test "test_source_product_allocation_scenario0_${source_allocation0_kind}")
    add_test(NAME ${source_allocation0_test}
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_allocation_faults.py"
            --binary $<TARGET_FILE:test_source_product_allocation_scenario0>
            --root "${source_allocation0_fixture}" --file "${source_allocation0_fixture}/root.xr"
            --kind ${source_allocation0_kind} --jobs 8 --input-root "${CMAKE_SOURCE_DIR}"
            --source-file "${source_allocation0_dir}/test_source_product_allocation_scenario0.c"
            --registration-file "${CMAKE_CURRENT_LIST_FILE}"
            --evidence "${CMAKE_BINARY_DIR}/consumer-fault-evidence/source-allocation0-${source_allocation0_kind}")
    set_tests_properties(${source_allocation0_test} PROPERTIES
        TIMEOUT 600 PROCESSORS 8 RUN_SERIAL TRUE
        LABELS "unit;xir;source-product;program-consumer;source-allocation;ownership;${source_allocation0_kind}-faults")
endforeach()
# Each boundary measures its own same-argv finite compiler workload first.
foreach(source_allocation0_axis IN ITEMS allocated live work)
    foreach(source_allocation0_cut IN ITEMS exact minus1)
        set(source_allocation0_axis_test
            "test_source_product_allocation_scenario0_compiler_axis_${source_allocation0_axis}_${source_allocation0_cut}")
        add_test(NAME ${source_allocation0_axis_test}
            COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_allocation_axes.py"
                --binary $<TARGET_FILE:test_source_product_allocation_scenario0>
                --root "${source_allocation0_fixture}" --file "${source_allocation0_fixture}/root.xr"
                --axis ${source_allocation0_axis} --cut ${source_allocation0_cut}
                --input-root "${CMAKE_SOURCE_DIR}"
                --source-file "${source_allocation0_dir}/test_source_product_allocation_scenario0.c"
                --registration-file "${CMAKE_CURRENT_LIST_FILE}"
                --evidence "${CMAKE_BINARY_DIR}/consumer-axis-evidence/source-allocation0-${source_allocation0_axis}-${source_allocation0_cut}")
        set_tests_properties(${source_allocation0_axis_test} PROPERTIES
            TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE
            LABELS "unit;xir;source-product;program-consumer;source-allocation;ownership;compiler-budget")
    endforeach()
endforeach()
