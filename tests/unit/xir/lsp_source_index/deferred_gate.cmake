# Configure a distinct receiver tree while the original validation inputs stay frozen.
function(xr_register_source_index_gate)
    include("${CMAKE_SOURCE_DIR}/tests/unit/xir/lsp_source_index/CMakeLists.txt")
endfunction()
cmake_language(DEFER CALL xr_register_source_index_gate)
