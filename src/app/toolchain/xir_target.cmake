# Native input observations have one production owner and resource ledger.
if(WIN32)
    include("${CMAKE_CURRENT_LIST_DIR}/xir_process.cmake")
    add_library(xray_toolchain_dependencies STATIC ${CMAKE_CURRENT_LIST_DIR}/xtc_dependencies.c)
    target_link_libraries(xray_toolchain_dependencies PUBLIC xray_json_cursor xray_compile_resources)
    target_compile_features(xray_toolchain_dependencies PUBLIC c_std_11)
    set_target_properties(xray_toolchain_dependencies PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(xray_toolchain_dependencies PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(xray_toolchain_dependencies PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    add_library(xray_xir_target STATIC
        ${CMAKE_CURRENT_LIST_DIR}/xtc_xir_target.c
        ${CMAKE_CURRENT_LIST_DIR}/xtc_xir_sysroot.c
        ${CMAKE_CURRENT_LIST_DIR}/xtc_xir_images.c
        ${CMAKE_CURRENT_LIST_DIR}/../../base/xsha256.c
        ${CMAKE_CURRENT_LIST_DIR}/../../base/xutf8.c)
    target_link_libraries(xray_xir_target PUBLIC xray_compile_resources)
    target_compile_features(xray_xir_target PUBLIC c_std_11)
    set_target_properties(xray_xir_target PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_definitions(xray_xir_target PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(xray_xir_target PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(xray_xir_target PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    add_library(xray_xir_invocation STATIC ${CMAKE_CURRENT_LIST_DIR}/xtc_xir_invocation.c)
    target_link_libraries(xray_xir_invocation PUBLIC
        xray_toolchain_process xray_toolchain_dependencies xray_xir_target
        xray_xir_native_projection xray_xir_runtime_sdk xray_xir_namespace)
    target_compile_features(xray_xir_invocation PUBLIC c_std_11)
    set_target_properties(xray_xir_invocation PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_definitions(xray_xir_invocation PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    if(MSVC)
        target_compile_options(xray_xir_invocation PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(xray_xir_invocation PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/xir_namespace.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/xir_workspace.cmake")
endif()
