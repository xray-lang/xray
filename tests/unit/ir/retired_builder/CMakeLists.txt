# Keep the retired registry controls independent of the legacy Core aggregate.
add_library(xray_retired_builder_compile OBJECT
    ${CMAKE_SOURCE_DIR}/src/ir/xi_semantic_intrinsic.c
    ${CMAKE_SOURCE_DIR}/src/program/xr_program_from_xi.c)
add_dependencies(xray_retired_builder_compile
    gen-xi-ops gen-xi-verify gen-xi-lowering gen-aot-metadata gen-stdlib-metadata
    gen-native-type-defs gen-core-spec gen-target-query-registry gen-xr-program-schema)

add_library(xray_retired_builder_providers OBJECT
    ${CMAKE_SOURCE_DIR}/src/frontend/analyzer/xa_intrinsic_registry.c
    ${CMAKE_SOURCE_DIR}/src/shared/xr_core_intrinsic_registry.c
    ${CMAKE_SOURCE_DIR}/src/runtime/symbol/xsymbol_table.c
    ${CMAKE_SOURCE_DIR}/src/runtime/xisolate_runtime_access.c
    ${CMAKE_SOURCE_DIR}/src/base/xhashmap.c
    ${CMAKE_SOURCE_DIR}/src/base/xio_policy.c)
if(WIN32)
    target_sources(xray_retired_builder_providers PRIVATE
        ${CMAKE_SOURCE_DIR}/src/os/win/thread_win.c)
else()
    find_package(Threads REQUIRED)
    target_sources(xray_retired_builder_providers PRIVATE
        ${CMAKE_SOURCE_DIR}/src/os/unix/thread_unix.c)
endif()
add_executable(test_xi_retired_builder ../test_xi_intrinsic.c
    $<TARGET_OBJECTS:xray_retired_builder_providers>)
if(WIN32)
    target_link_libraries(test_xi_retired_builder PRIVATE synchronization)
else()
    target_link_libraries(test_xi_retired_builder PRIVATE Threads::Threads)
endif()
target_link_libraries(test_xi_retired_builder PRIVATE xray_compile_resources)
add_dependencies(test_xi_retired_builder xray_retired_builder_compile)

foreach(builder_target xray_retired_builder_compile xray_retired_builder_providers
                       test_xi_retired_builder)
    target_include_directories(${builder_target} PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/tests/unit
        ${CMAKE_BINARY_DIR}/generated)
    set_target_properties(${builder_target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${builder_target} PRIVATE /utf-8 /Gy /Gw)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${builder_target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${builder_target} PRIVATE -ffunction-sections -fdata-sections)
    endif()
endforeach()
foreach(builder_target xray_retired_builder_compile test_xi_retired_builder)
    if(MSVC)
        target_compile_options(${builder_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${builder_target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
if(MSVC)
    target_link_options(test_xi_retired_builder PRIVATE LINKER:/OPT:REF)
elseif(APPLE)
    target_link_options(test_xi_retired_builder PRIVATE LINKER:-dead_strip)
else()
    target_link_options(test_xi_retired_builder PRIVATE LINKER:--gc-sections)
endif()

add_test(NAME test_xi_retired_builder
    COMMAND test_xi_retired_builder --retired-builder)
set_tests_properties(test_xi_retired_builder PROPERTIES
    LABELS "unit;compiler;metadata;retired-builder" TIMEOUT 30)
