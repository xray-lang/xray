# CLI admission shares the production Source owner and its resource ledger.
add_library(xray_cli_source STATIC
    ${CMAKE_CURRENT_LIST_DIR}/xcli_canonical_source.c
    ${CMAKE_CURRENT_LIST_DIR}/xcli_graph_authority.c
    ${CMAKE_CURRENT_LIST_DIR}/../../module/xproject.c
    ${CMAKE_CURRENT_LIST_DIR}/../../module/xnative_package.c)
target_link_libraries(xray_cli_source PUBLIC xray_xir_source_product)
target_compile_features(xray_cli_source PUBLIC c_std_11)
set_target_properties(xray_cli_source PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(xray_cli_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(xray_cli_source PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
