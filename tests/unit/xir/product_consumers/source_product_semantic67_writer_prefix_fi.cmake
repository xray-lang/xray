# A separate process preserves each cold ledger's complete allocation prefix.
set(writer_prefix_target test_source_product_semantic67_writer_prefix_fi)
add_executable(${writer_prefix_target}
    "${CMAKE_CURRENT_LIST_DIR}/semantic67_named/test_source_product_semantic67_writer_prefix_fi.c")
target_link_libraries(${writer_prefix_target} PRIVATE xray_xir_source_product)
target_include_directories(${writer_prefix_target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(${writer_prefix_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(${writer_prefix_target} PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(${writer_prefix_target} PRIVATE -Wall -Wextra -Werror)
endif()
foreach(writer_prefix_graph IN ITEMS empty occupied)
    add_test(NAME test_source_product_semantic67_writer_prefix_normal_${writer_prefix_graph}
        COMMAND ${writer_prefix_target} --normal "${writer_prefix_graph}")
    set_tests_properties(test_source_product_semantic67_writer_prefix_normal_${writer_prefix_graph} PROPERTIES
        TIMEOUT 120 LABELS "unit;xir;program-consumer;ownership;checked;public-writer;normal-resources")
endforeach()

# The owner binds counts only after normal evidence matches the exact executable and configuration.
function(source_product_register_writer_prefix_fi empty_sites occupied_sites)
    foreach(writer_prefix_graph IN ITEMS empty occupied)
        if(writer_prefix_graph STREQUAL "empty")
            set(writer_prefix_sites "${empty_sites}")
        else()
            set(writer_prefix_sites "${occupied_sites}")
        endif()
        if(NOT "${writer_prefix_sites}" MATCHES "^[1-9][0-9]*$")
            message(FATAL_ERROR "Writer prefix FI requires a fresh exact per-configuration normal allocation count")
        endif()
        math(EXPR writer_prefix_last "${writer_prefix_sites} - 1")
        foreach(writer_prefix_site RANGE 0 ${writer_prefix_last})
            add_test(NAME test_source_product_semantic67_writer_prefix_fi_${writer_prefix_graph}_${writer_prefix_site}
                COMMAND test_source_product_semantic67_writer_prefix_fi --fault "${writer_prefix_graph}"
                    "${writer_prefix_sites}" "${writer_prefix_site}")
            set_tests_properties(test_source_product_semantic67_writer_prefix_fi_${writer_prefix_graph}_${writer_prefix_site} PROPERTIES
                TIMEOUT 120 LABELS "unit;xir;program-consumer;ownership;checked;public-writer;fault-injection;allocation-prefix")
        endforeach()
    endforeach()
endfunction()


# Each whole-graph axis uses the matching normal total, including the owner's initial charge.
function(source_product_register_writer_prefix_axes empty_allocated empty_peak empty_work occupied_allocated occupied_peak occupied_work)
    foreach(writer_prefix_graph IN ITEMS empty occupied)
        if(writer_prefix_graph STREQUAL "empty")
            set(writer_prefix_totals "${empty_allocated};${empty_peak};${empty_work}")
        else()
            set(writer_prefix_totals "${occupied_allocated};${occupied_peak};${occupied_work}")
        endif()
        set(writer_prefix_index 0)
        foreach(writer_prefix_axis IN ITEMS allocated live work)
            list(GET writer_prefix_totals ${writer_prefix_index} writer_prefix_total)
            if(NOT "${writer_prefix_total}" MATCHES "^[1-9][0-9]*$")
                message(FATAL_ERROR "Writer prefix resource axes require exact fresh per-configuration normal totals")
            endif()
            foreach(writer_prefix_cut IN ITEMS exact minus1)
                add_test(NAME test_source_product_semantic67_writer_prefix_${writer_prefix_graph}_${writer_prefix_axis}_${writer_prefix_cut}
                    COMMAND test_source_product_semantic67_writer_prefix_fi --axis "${writer_prefix_graph}"
                        "${writer_prefix_axis}" "${writer_prefix_cut}" "${writer_prefix_total}")
                set_tests_properties(test_source_product_semantic67_writer_prefix_${writer_prefix_graph}_${writer_prefix_axis}_${writer_prefix_cut} PROPERTIES
                    TIMEOUT 120 LABELS "unit;xir;program-consumer;ownership;checked;public-writer;resource-axis")
            endforeach()
            math(EXPR writer_prefix_index "${writer_prefix_index} + 1")
        endforeach()
    endforeach()
endfunction()
