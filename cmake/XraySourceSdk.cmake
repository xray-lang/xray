# Source embedding deliberately owns compiler and VM archives outside the AOT runtime bundle.
if(NOT WIN32)
    return()
endif()

function(xray_source_sdk_collect target)
    get_property(visited GLOBAL PROPERTY XRAY_SOURCE_SDK_TARGETS)
    if(target IN_LIST visited)
        return()
    endif()
    get_target_property(kind ${target} TYPE)
    if(NOT kind STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "Source SDK requires an explicit static archive: ${target}")
    endif()
    set_property(GLOBAL APPEND PROPERTY XRAY_SOURCE_SDK_TARGETS ${target})
    get_target_property(links ${target} INTERFACE_LINK_LIBRARIES)
    if(links)
        foreach(link IN LISTS links)
            if(NOT TARGET ${link})
                message(FATAL_ERROR "Source SDK dependency needs an owned projection: ${target}: ${link}")
            endif()
            xray_source_sdk_collect(${link})
        endforeach()
    endif()
endfunction()

xray_source_sdk_collect(xray_source_api)
get_property(source_sdk_targets GLOBAL PROPERTY XRAY_SOURCE_SDK_TARGETS)
set(source_sdk_archive_targets)
foreach(target IN LISTS source_sdk_targets)
    get_target_property(source_directory ${target} SOURCE_DIR)
    get_target_property(sources ${target} SOURCES)
    set(absolute_sources)
    foreach(source IN LISTS sources)
        get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${source_directory}")
        list(APPEND absolute_sources "${source}")
    endforeach()
    set(archive_target "source_sdk_${target}")
    add_library(${archive_target} STATIC ${absolute_sources})
    # Reuse owned source generation while compiling a distinct, explicit MD archive.
    add_dependencies(${archive_target} ${target})
    foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS)
        get_target_property(value ${target} ${property})
        if(value)
            set_property(TARGET ${archive_target} PROPERTY ${property} "${value}")
        endif()
    endforeach()
    set_target_properties(${archive_target} PROPERTIES OUTPUT_NAME ${target}
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/source-sdk-archives"
        MSVC_RUNTIME_LIBRARY MultiThreadedDLL C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    list(APPEND source_sdk_archive_targets ${archive_target})
endforeach()
set(XRAY_SOURCE_SDK_ARCHIVE_TARGETS "${source_sdk_archive_targets}")
if(ENABLE_ASAN OR ENABLE_SANITIZERS OR ENABLE_UBSAN OR ENABLE_TSAN OR ENABLE_MSAN)
    return()
endif()
set(source_sdk_root "${CMAKE_BINARY_DIR}/source-sdk")
set(source_sdk_inputs "{\"archives\":[")
set(source_sdk_separator "")
foreach(target IN LISTS source_sdk_targets)
    string(APPEND source_sdk_inputs "${source_sdk_separator}{\"target\":\"source_sdk_${target}\",\"name\":\"${target}\",\"file\":\"$<TARGET_FILE:source_sdk_${target}>\"}")
    set(source_sdk_separator ",")
endforeach()
string(APPEND source_sdk_inputs "]}\n")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/source-sdk-inputs.json" CONTENT "${source_sdk_inputs}")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_custom_command(OUTPUT "${source_sdk_root}/source_sdk_manifest.json"
    COMMAND ${XRAY_PYTHON} -B "${PROJECT_SOURCE_DIR}/scripts/build_source_sdk.py"
        --root "${PROJECT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}"
        --inputs "${CMAKE_BINARY_DIR}/source-sdk-inputs.json" --output "${source_sdk_root}"
        --provider "${CMAKE_C_COMPILER_ID}" --profile "${CMAKE_BUILD_TYPE}"
    DEPENDS ${source_sdk_archive_targets} "${CMAKE_BINARY_DIR}/source-sdk-inputs.json"
        "${PROJECT_SOURCE_DIR}/scripts/build_source_sdk.py"
        "${PROJECT_SOURCE_DIR}/include/xray_source.h" "${PROJECT_SOURCE_DIR}/include/xray_export.h"
        "${PROJECT_SOURCE_DIR}/cmake/XraySourceConfig.cmake.in"
    VERBATIM)
add_custom_target(source-sdk-bundle ALL DEPENDS "${source_sdk_root}/source_sdk_manifest.json")
add_dependencies(xray source-sdk-bundle)
foreach(target IN LISTS source_sdk_targets)
    install(FILES "${source_sdk_root}/lib/${target}${CMAKE_STATIC_LIBRARY_SUFFIX}"
        DESTINATION lib/xray/source-sdk/lib COMPONENT XraySDK)
endforeach()
install(FILES "${source_sdk_root}/include/xray_source.h" "${source_sdk_root}/include/xray_export.h"
    DESTINATION lib/xray/source-sdk/include COMPONENT XraySDK)
install(FILES "${source_sdk_root}/source_sdk_manifest.json" "${source_sdk_root}/source_sdk_recipe.json"
    DESTINATION lib/xray/source-sdk COMPONENT XraySDK)
install(FILES "${source_sdk_root}/XraySourceConfig.cmake"
    DESTINATION lib/cmake/XraySource COMPONENT XraySDK)
