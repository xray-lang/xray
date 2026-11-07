if(TARGET test_lsp_source_scope)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(scope_normal_modes normal rename-normal lens-normal binding-lenses-normal)
