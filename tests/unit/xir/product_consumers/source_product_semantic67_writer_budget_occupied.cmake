# Keep the public graph budget and retained-output observations in one independent C11 consumer.
set(writer_budget_target test_source_product_semantic67_writer_budget_occupied)
add_executable(${writer_budget_target}
    "${CMAKE_CURRENT_LIST_DIR}/semantic67_named/test_source_product_semantic67_writer_budget_occupied.c")
target_link_libraries(${writer_budget_target} PRIVATE xray_xir_source_product)
target_include_directories(${writer_budget_target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(${writer_budget_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(${writer_budget_target} PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(${writer_budget_target} PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_semantic67_writer_occupied_normal
    COMMAND ${writer_budget_target})
set_tests_properties(test_source_product_semantic67_writer_occupied_normal PROPERTIES TIMEOUT 120
    LABELS "unit;xir;program-consumer;ownership;checked;public-writer;normal-resources")

# Explicit owner registration follows a fresh validation of the matching empty graph costs.
function(source_product_register_writer_budget_axes)
    foreach(writer_axis IN ITEMS allocated live work)
        foreach(writer_cut IN ITEMS exact minus1)
            add_test(NAME test_source_product_semantic67_writer_${writer_axis}_${writer_cut}
                COMMAND test_source_product_semantic67_writer_budget_occupied --axis "${writer_axis}" "${writer_cut}")
            set_tests_properties(test_source_product_semantic67_writer_${writer_axis}_${writer_cut} PROPERTIES
                TIMEOUT 120 LABELS "unit;xir;program-consumer;ownership;checked;public-writer;resource-axis")
        endforeach()
    endforeach()
endfunction()
