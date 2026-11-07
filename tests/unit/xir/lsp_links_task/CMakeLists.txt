if(TARGET test_lsp_links_task)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(mode normal resources counterfactual)
    if(mode STREQUAL "counterfactual")
        set(image test_lsp_links_task_before)
    else()
        set(image test_lsp_links_task)
    endif()

endforeach()
