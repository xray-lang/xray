add_library(xray_toolchain_binding STATIC xr_toolchain_binding.c ../base/xsha256.c)
target_link_libraries(xray_toolchain_binding PUBLIC xray_compile_resources)
add_library(xray_xir_native_artifact STATIC ../aot/program/xr_native_artifact.c)
target_link_libraries(xray_xir_native_artifact PUBLIC xray_toolchain_binding)
add_library(xray_xir_native_projection STATIC ../aot/program/xr_xir_native_projection.c)
target_link_libraries(xray_xir_native_projection PUBLIC xray_xir_native_artifact xray_xir_source_product)
foreach(target xray_toolchain_binding xray_xir_native_artifact xray_xir_native_projection)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()

if(WIN32)
    set(XIR_SDK_DIR ${CMAKE_BINARY_DIR}/xir-runtime-sdk)
    file(MAKE_DIRECTORY ${XIR_SDK_DIR})
    execute_process(COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/gen_xir_runtime_sdk_recipe.py
        --root ${PROJECT_SOURCE_DIR} --output ${XIR_SDK_DIR}/sdk_recipe.inc.c
        --record ${XIR_SDK_DIR}/sdk_recipe.json COMMAND_ERROR_IS_FATAL ANY)
    file(READ ${XIR_SDK_DIR}/sdk_recipe.json XIR_SDK_RECIPE)
    string(JSON XIR_SDK_FILE_COUNT LENGTH ${XIR_SDK_RECIPE} files)
    math(EXPR XIR_SDK_FILE_LAST "${XIR_SDK_FILE_COUNT}-1")
    set(XIR_SDK_DEPENDENCIES)
    foreach(index RANGE ${XIR_SDK_FILE_LAST})
        string(JSON path GET ${XIR_SDK_RECIPE} files ${index} path)
        string(JSON kind GET ${XIR_SDK_RECIPE} files ${index} kind)
        if(NOT kind EQUAL 5)
            list(APPEND XIR_SDK_DEPENDENCIES ${PROJECT_SOURCE_DIR}/${path})
        endif()
        get_filename_component(parent ${path} DIRECTORY)
        install(FILES ${XIR_SDK_DIR}/${path} DESTINATION lib/xray/xir-runtime-sdk/${parent} COMPONENT XraySDK)
    endforeach()
    add_custom_command(OUTPUT ${XIR_SDK_DIR}/sdk_generated.h ${XIR_SDK_DIR}/sdk_manifest.json
        COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/build_xir_runtime_sdk.py
            --root ${PROJECT_SOURCE_DIR} --output ${XIR_SDK_DIR}
            --compiler ${CMAKE_C_COMPILER} --crt MD --jobs 4
        DEPENDS ${XIR_SDK_DEPENDENCIES} ${PROJECT_SOURCE_DIR}/src/xir/CMakeLists.txt
            ${PROJECT_SOURCE_DIR}/src/execution/xir_runtime_host.cmake
            ${PROJECT_SOURCE_DIR}/scripts/gen_xir_runtime_sdk_recipe.py
            ${PROJECT_SOURCE_DIR}/scripts/derive_xir_sdk_abi.py
            ${PROJECT_SOURCE_DIR}/scripts/build_xir_runtime_sdk.py
        VERBATIM)
    add_custom_target(xir-runtime-sdk-bundle DEPENDS ${XIR_SDK_DIR}/sdk_generated.h ${XIR_SDK_DIR}/sdk_manifest.json)
    add_library(xray_xir_runtime_sdk STATIC xr_xir_runtime_sdk.c)
    set_target_properties(xray_xir_runtime_sdk PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    add_dependencies(xray_xir_runtime_sdk xir-runtime-sdk-bundle)
    target_include_directories(xray_xir_runtime_sdk PRIVATE ${XIR_SDK_DIR})
    target_link_libraries(xray_xir_runtime_sdk PUBLIC xray_xir_runtime_host xray_compile_resources xray_json_cursor)
    if(MSVC)
        target_compile_options(xray_xir_runtime_sdk PRIVATE /W4 /WX)
    else()
        target_compile_options(xray_xir_runtime_sdk PRIVATE -Wall -Wextra -Werror)
    endif()
    install(FILES ${XIR_SDK_DIR}/sdk_manifest.json DESTINATION lib/xray/xir-runtime-sdk COMPONENT XraySDK)
endif()
