if(NOT WIN32 OR TARGET test_lsp_diagnostic_publication)
 return()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
