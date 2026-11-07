if(TARGET test_source_calls)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

foreach(suffix "" _resources)
    if(suffix STREQUAL "")
        set(extra)
    else()
        set(extra --resources)
    endif()

endforeach()

foreach(mode IN ITEMS normal resources)
    if(mode STREQUAL "before")
        set(image test_lsp_parameter_json_before)
    else()
        set(image test_lsp_parameter_json)
    endif()

endforeach()

foreach(mode IN ITEMS normal resources)
    if(mode STREQUAL "before")
        set(image test_lsp_type_json_before)
    else()
        set(image test_lsp_type_json)
    endif()

endforeach()
