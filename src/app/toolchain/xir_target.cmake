if(WIN32)
    set(XIR_NATIVE_DEFINITIONS WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set(XIR_NATIVE_OS win)
elseif(APPLE)
    set(XIR_NATIVE_DEFINITIONS _POSIX_C_SOURCE=200809L _DARWIN_C_SOURCE)
    set(XIR_NATIVE_OS unix)
else()
    set(XIR_NATIVE_DEFINITIONS _POSIX_C_SOURCE=200809L)
    set(XIR_NATIVE_OS unix)
endif()
if(WIN32 OR XR_XIR_POSIX_SDK_ARCHITECTURE)
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
    target_compile_definitions(xray_xir_target PRIVATE ${XIR_NATIVE_DEFINITIONS})
    if(MSVC)
        target_compile_options(xray_xir_target PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(xray_xir_target PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    if(WIN32)
        add_library(xray_xir_publication STATIC ${CMAKE_CURRENT_LIST_DIR}/xtc_xir_publication.c
            ${CMAKE_CURRENT_LIST_DIR}/../../base/xio_policy.c)
        target_link_libraries(xray_xir_publication PUBLIC xray_xir_target bcrypt)
        target_compile_features(xray_xir_publication PUBLIC c_std_11)
        set_target_properties(xray_xir_publication PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        target_compile_definitions(xray_xir_publication PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
        if(MSVC)
            target_compile_options(xray_xir_publication PRIVATE /W4 /WX /utf-8)
        else()
            target_compile_options(xray_xir_publication PRIVATE -Wall -Wextra -Werror -pedantic)
        endif()
    endif()
endif()
if(NOT WIN32)
    # POSIX publication stages through a parent directory descriptor. Strict C11
    # needs the POSIX.1-2008 surface requested explicitly, and Darwin hides
    # O_NOFOLLOW under that request unless its own extensions are enabled.
    add_library(xray_xir_publication STATIC ${CMAKE_CURRENT_LIST_DIR}/xtc_xir_publication.c
        ${CMAKE_CURRENT_LIST_DIR}/../../base/xio_policy.c)
    # The scalar runtime archive owns the CSPRNG object; generated programs and
    # the runtime SDK link that archive by path, so it must stay there.
    target_link_libraries(xray_xir_publication PUBLIC xray_compile_resources xray_xir_scalar)
    target_compile_features(xray_xir_publication PUBLIC c_std_11)
    set_target_properties(xray_xir_publication PROPERTIES C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_definitions(xray_xir_publication PRIVATE _POSIX_C_SOURCE=200809L)
    if(APPLE)
        target_compile_definitions(xray_xir_publication PRIVATE _DARWIN_C_SOURCE)
    endif()
    target_compile_options(xray_xir_publication PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/xir_workspace.cmake")
