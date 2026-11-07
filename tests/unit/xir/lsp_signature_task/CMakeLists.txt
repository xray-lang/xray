if(TARGET test_lsp_signature_task)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(mode normal resources counterfactual)
    if(mode STREQUAL "counterfactual")
        set(image test_lsp_signature_task_before)
    else()
        set(image test_lsp_signature_task)
    endif()

endforeach()

foreach(mode IN ITEMS normal)
    if(mode STREQUAL "before")
        set(image test_lsp_native_signature_before)
    else()
        set(image test_lsp_signature_task)
    endif()

endforeach()
