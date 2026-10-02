add_library(xray_xir_namespace STATIC
    ${CMAKE_CURRENT_LIST_DIR}/xtc_xir_namespace.c
    ${CMAKE_CURRENT_LIST_DIR}/../../base/xio_policy.c
    ${CMAKE_CURRENT_LIST_DIR}/../../base/xfileio.c
    ${CMAKE_CURRENT_LIST_DIR}/../../os/win/dir_win.c)
target_link_libraries(xray_xir_namespace PUBLIC xray_xir_target xray_compile_resources)
target_include_directories(xray_xir_namespace PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(xray_xir_namespace PUBLIC c_std_11)
set_target_properties(xray_xir_namespace PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(xray_xir_namespace PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(xray_xir_namespace PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(xray_xir_namespace PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
