# Source admission shares the production Source owner and its resource ledger.
add_library(xray_source_compile STATIC
    ${CMAKE_CURRENT_LIST_DIR}/xsource_compiler.c
    ${CMAKE_CURRENT_LIST_DIR}/xsource_compile.c
    ${CMAKE_CURRENT_LIST_DIR}/xsource_paths.c
    ${CMAKE_CURRENT_LIST_DIR}/xsource_authority.c
    ${CMAKE_CURRENT_LIST_DIR}/../module/xproject.c
    ${CMAKE_CURRENT_LIST_DIR}/../module/xnative_package.c)
target_link_libraries(xray_source_compile PUBLIC xray_xir_source_product xray_xir_stdlib_catalog)
target_compile_features(xray_source_compile PUBLIC c_std_11)
set_target_properties(xray_source_compile PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(xray_source_compile PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(xray_source_compile PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

# The installed header exposes opaque compiler/Program/Call/result owners.
add_library(xray_source_api STATIC ${CMAKE_CURRENT_LIST_DIR}/xsource_api.c)
target_link_libraries(xray_source_api PUBLIC xray_source_compile xray_xir_runtime_host)
target_include_directories(xray_source_api PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_compile_features(xray_source_api PUBLIC c_std_11)
set_target_properties(xray_source_api PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(xray_source_api PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(xray_source_api PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
