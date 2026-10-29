# Native C clients consume the canonical targets without a second CRT projection.
set(_xnr_eligible TRUE)
set(_xnr_crt "${CMAKE_MSVC_RUNTIME_LIBRARY}")
string(GENEX_STRIP "${_xnr_crt}" _xnr_crt)
if(NOT WIN32 OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR
   NOT CMAKE_C_COMPILER_VERSION STREQUAL "19.44.35219.0" OR
   NOT CMAKE_CXX_COMPILER_VERSION STREQUAL "19.44.35219.0" OR
   NOT CMAKE_C_COMPILER_ARCHITECTURE_ID STREQUAL "x64" OR
   NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT _xnr_crt STREQUAL "MultiThreaded" OR
   ENABLE_ASAN OR ENABLE_SANITIZERS OR ENABLE_UBSAN OR ENABLE_TSAN OR ENABLE_MSAN)
    set(_xnr_eligible FALSE)
endif()
foreach(_xnr_flags CMAKE_C_FLAGS CMAKE_C_FLAGS_RELEASE)
    if("${${_xnr_flags}}" MATCHES "[/-](fsanitize|clang:|fp:fast)|-ffast-math")
        set(_xnr_eligible FALSE)
    endif()
endforeach()
set(_xnr_tuple "abi1/x86_64-windows-msvc/msvc-release-mt-san0")
set(_xnr_bundle "${CMAKE_BINARY_DIR}/native-runtime-bundle")
set(_xnr_inputs "${CMAKE_BINARY_DIR}/native-runtime-inputs.json")

function(_xnr_collect target ancestry)
    get_target_property(alias ${target} ALIASED_TARGET)
    if(alias)
        set(target "${alias}")
    endif()
    if(target IN_LIST ancestry)
        message(FATAL_ERROR "Native runtime static dependency cycle: ${ancestry};${target}")
    endif()
    get_property(visited GLOBAL PROPERTY XRAY_NATIVE_PACKAGE_TARGETS)
    if(target IN_LIST visited)
        return()
    endif()
    get_target_property(kind ${target} TYPE)
    if(NOT kind STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR "Native runtime requires an owned static target: ${target}")
    endif()
    set_property(GLOBAL APPEND PROPERTY XRAY_NATIVE_PACKAGE_TARGETS "${target}")
    list(APPEND ancestry "${target}")
    get_target_property(links ${target} LINK_LIBRARIES)
    get_target_property(interface ${target} INTERFACE_LINK_LIBRARIES)
    set(resolved)
    foreach(link IN LISTS links interface)
        if(link MATCHES "-NOTFOUND$")
            continue()
        endif()
        if(link MATCHES "^\\$<LINK_ONLY:([^<>]+)>$")
            set(link "${CMAKE_MATCH_1}")
        endif()
        if(NOT TARGET "${link}")
            message(FATAL_ERROR "Unresolved native runtime dependency: ${target}: ${link}")
        endif()
        get_target_property(link_alias ${link} ALIASED_TARGET)
        if(link_alias)
            set(link "${link_alias}")
        endif()
        list(APPEND resolved "${link}")
        _xnr_collect("${link}" "${ancestry}")
    endforeach()
    list(REMOVE_DUPLICATES resolved)
    set_property(GLOBAL PROPERTY "XRAY_NATIVE_PACKAGE_LINKS_${target}" "${resolved}")
endfunction()

if(_xnr_eligible)
    _xnr_collect(xray_native_runtime "")
    get_property(_xnr_targets GLOBAL PROPERTY XRAY_NATIVE_PACKAGE_TARGETS)
    set(_xnr_json "{\"schema\":1,\"provider\":\"${CMAKE_C_COMPILER_ID}\",\"version\":\"${CMAKE_C_COMPILER_VERSION}\",\"profile\":\"${CMAKE_BUILD_TYPE}\",\"archives\":[")
    set(_xnr_separator "")
    foreach(target IN LISTS _xnr_targets)
        get_property(_xnr_links GLOBAL PROPERTY "XRAY_NATIVE_PACKAGE_LINKS_${target}")
        set(_xnr_link_json "")
        set(_xnr_link_separator "")
        foreach(link IN LISTS _xnr_links)
            string(APPEND _xnr_link_json "${_xnr_link_separator}\"${link}\"")
            set(_xnr_link_separator ",")
        endforeach()
        string(APPEND _xnr_json "${_xnr_separator}{\"target\":\"${target}\",\"dependencies\":[${_xnr_link_json}],\"file\":\"$<TARGET_FILE:${target}>\",\"crt\":\"$<TARGET_GENEX_EVAL:${target},$<TARGET_PROPERTY:${target},MSVC_RUNTIME_LIBRARY>>\"}")
        set(_xnr_separator ",")
    endforeach()
    string(APPEND _xnr_json "]}\n")
    file(GENERATE OUTPUT "${_xnr_inputs}" CONTENT "${_xnr_json}")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    add_custom_target(native-runtime-bundle
        COMMAND ${XRAY_PYTHON} -B "${PROJECT_SOURCE_DIR}/scripts/build_native_runtime_bundle.py"
            --root "${PROJECT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}"
            --inputs "${_xnr_inputs}" --output "${_xnr_bundle}"
            --from-build-target
        DEPENDS ${_xnr_targets}
        VERBATIM)
else()
    add_custom_target(native-runtime-bundle
        COMMAND ${CMAKE_COMMAND} -E echo "XrayNativeRuntime requires qualified MSVC x64 Release MT SAN0"
        COMMAND ${CMAKE_COMMAND} -E false VERBATIM)
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/XrayNativeRuntimeInstall.cmake.in"
    "${CMAKE_BINARY_DIR}/InstallXrayNativeRuntime.cmake" @ONLY)
install(SCRIPT "${CMAKE_BINARY_DIR}/InstallXrayNativeRuntime.cmake"
    COMPONENT XrayNativeRuntime EXCLUDE_FROM_ALL)
