if(TARGET test_lsp_navigation_admission)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(mode normal resources counterfactual)
    if(mode STREQUAL "counterfactual")
        set(image test_lsp_navigation_admission_before)
    else()
        set(image test_lsp_navigation_admission)
    endif()

endforeach()
