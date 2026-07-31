# CMakeLists.txt
# Author: xingleixu@gmail.com
# Xray Language Interpreter Build Configuration

cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(Xray
    VERSION 0.9.2
    DESCRIPTION "Xray Programming Language Interpreter"
    LANGUAGES C CXX ASM
)

# ----------------------------------------------------------------------------
# Compiler cache
#
# 1087 translation units means a touched header, a generator flip, or the ASan
# lane's from-scratch rebuild of the whole compiler all pay full compile cost.
# ccache turns the repeats into hash lookups. Opt-out rather than opt-in: it is
# transparent, and XRAY_USE_COMPILER_CACHE=OFF disables it when a build must be
# measured or bit-reproduced.
#
# Silently skipped when no cache program is installed, so this never becomes a
# build prerequisite.
# ----------------------------------------------------------------------------
option(XRAY_USE_COMPILER_CACHE "Route C/C++ compiles through ccache/sccache when available" ON)
if(XRAY_USE_COMPILER_CACHE AND NOT CMAKE_C_COMPILER_LAUNCHER)
    find_program(XRAY_COMPILER_CACHE NAMES ccache sccache)
    if(XRAY_COMPILER_CACHE)
        set(CMAKE_C_COMPILER_LAUNCHER "${XRAY_COMPILER_CACHE}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${XRAY_COMPILER_CACHE}")
        message(STATUS "Compiler cache: ${XRAY_COMPILER_CACHE}")
    else()
        message(STATUS "Compiler cache: not found (install ccache to speed up rebuilds)")
    endif()
endif()

# Release identity is captured once at configure time and is shared by the
# binary, installed marker, and payload manifest.  Release automation rejects
# dirty/unknown identities; source builds remain explicit instead of silently
# pretending to be a release artifact.
find_package(Git QUIET)
set(XRAY_GIT_COMMIT "unknown")
set(XRAY_GIT_DIRTY 0)
if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _XRAY_GIT_COMMIT_RESULT
        OUTPUT_VARIABLE _XRAY_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    string(LENGTH "${_XRAY_GIT_COMMIT}" _XRAY_GIT_COMMIT_LENGTH)
    if(_XRAY_GIT_COMMIT_RESULT EQUAL 0 AND _XRAY_GIT_COMMIT_LENGTH EQUAL 40 AND
       _XRAY_GIT_COMMIT MATCHES "^[0-9a-fA-F]+$")
        string(TOLOWER "${_XRAY_GIT_COMMIT}" XRAY_GIT_COMMIT)
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _XRAY_GIT_STATUS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT _XRAY_GIT_STATUS STREQUAL "")
        set(XRAY_GIT_DIRTY 1)
    endif()
endif()
if(XRAY_GIT_DIRTY)
    set(XRAY_GIT_DIRTY_JSON true)
else()
    set(XRAY_GIT_DIRTY_JSON false)
endif()

file(STRINGS "${CMAKE_SOURCE_DIR}/src/module/xbytecode_io.h"
    _XRAY_BYTECODE_VERSION_LINE REGEX "^#define XR_BC_VERSION [0-9]+")
string(REGEX MATCH "[0-9]+" XRAY_BYTECODE_VERSION "${_XRAY_BYTECODE_VERSION_LINE}")
file(STRINGS "${CMAKE_SOURCE_DIR}/include/xray_module_sdk.h"
    _XRAY_MODULE_ABI_VERSION_LINE REGEX "^#define XRAY_MODULE_ABI_VERSION [0-9]+")
string(REGEX MATCH "[0-9]+" XRAY_MODULE_ABI_VERSION "${_XRAY_MODULE_ABI_VERSION_LINE}")
if(NOT XRAY_BYTECODE_VERSION OR NOT XRAY_MODULE_ABI_VERSION)
    message(FATAL_ERROR "Unable to derive bytecode/module ABI versions for release identity")
endif()

set(XRAY_PREFERRED_ZIG_VERSION "0.16.0" CACHE STRING
    "Preferred managed Zig version for the current toolchain protocol")
set(XRAY_REQUIRED_ZIG_RANGE ">=0.16.0 <0.17.0" CACHE STRING
    "Supported managed Zig version range")
set(XRAY_TOOLCHAIN_PROTOCOL 1 CACHE STRING "AOT toolchain provider protocol version")

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _XRAY_OSX_ARCH_COUNT)
    if(NOT _XRAY_OSX_ARCH_COUNT EQUAL 1)
        message(FATAL_ERROR
            "Release payload identity requires exactly one CMAKE_OSX_ARCHITECTURES value")
    endif()
    list(GET CMAKE_OSX_ARCHITECTURES 0 _XRAY_RELEASE_PROCESSOR)
else()
    set(_XRAY_RELEASE_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_XRAY_RELEASE_PROCESSOR}" _XRAY_RELEASE_ARCH)
if(_XRAY_RELEASE_ARCH MATCHES "^(aarch64|arm64)$")
    set(_XRAY_RELEASE_ARCH "arm64")
elseif(_XRAY_RELEASE_ARCH MATCHES "^(amd64|x86_64)$")
    set(_XRAY_RELEASE_ARCH "x86_64")
endif()
if(APPLE)
    set(XRAY_RELEASE_TARGET "macos-${_XRAY_RELEASE_ARCH}")
    if(_XRAY_RELEASE_ARCH STREQUAL "arm64")
        set(XRAY_HOST_TARGET_ABI "aarch64-apple-darwin")
    else()
        set(XRAY_HOST_TARGET_ABI "x86_64-apple-darwin")
    endif()
    set(XRAY_RUNTIME_OBJECT_FORMAT "macho")
    set(XRAY_RUNTIME_PROVIDERS_JSON "\\\"apple-clang\\\",\\\"llvm-clang\\\",\\\"zig\\\"")
    set(XRAY_RUNTIME_SYSTEM_LIBRARIES_JSON "\\\"m\\\",\\\"pthread\\\"")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(XRAY_RELEASE_TARGET "linux-${_XRAY_RELEASE_ARCH}")
    if(_XRAY_RELEASE_ARCH STREQUAL "arm64")
        set(XRAY_HOST_TARGET_ABI "aarch64-linux-gnu")
    else()
        set(XRAY_HOST_TARGET_ABI "x86_64-linux-gnu")
    endif()
    set(XRAY_RUNTIME_OBJECT_FORMAT "elf")
    set(XRAY_RUNTIME_PROVIDERS_JSON "\\\"llvm-clang\\\",\\\"gcc\\\",\\\"zig\\\"")
    set(XRAY_RUNTIME_SYSTEM_LIBRARIES_JSON "\\\"m\\\",\\\"pthread\\\"")
elseif(WIN32)
    set(XRAY_RELEASE_TARGET "windows-${_XRAY_RELEASE_ARCH}")
    if(_XRAY_RELEASE_ARCH STREQUAL "arm64")
        set(XRAY_HOST_TARGET_ABI "aarch64-windows-msvc")
    else()
        set(XRAY_HOST_TARGET_ABI "x86_64-windows-msvc")
    endif()
    set(XRAY_RUNTIME_OBJECT_FORMAT "coff")
    # Both cl.exe and Zig's Clang frontend can consume the installed MSVC-ABI
    # COFF archives. Zig is the managed fallback for generated C that uses
    # GNU/Clang expression extensions unavailable in cl.exe.
    set(XRAY_RUNTIME_PROVIDERS_JSON "\\\"msvc\\\",\\\"zig\\\"")
    set(XRAY_RUNTIME_SYSTEM_LIBRARIES_JSON "\\\"ws2_32\\\",\\\"synchronization\\\"")
else()
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _XRAY_RELEASE_OS)
    set(XRAY_RELEASE_TARGET "${_XRAY_RELEASE_OS}-${_XRAY_RELEASE_ARCH}")
endif()

find_program(XRAY_PYTHON NAMES python3 python py)
if(NOT XRAY_PYTHON)
    message(FATAL_ERROR "Python 3 interpreter is required to configure and build Xray")
endif()
execute_process(
    COMMAND ${XRAY_PYTHON} -c "import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)"
    RESULT_VARIABLE XRAY_PYTHON_VERSION_RESULT
    OUTPUT_QUIET
    ERROR_QUIET
)
if(NOT XRAY_PYTHON_VERSION_RESULT EQUAL 0)
    message(FATAL_ERROR "Python 3 interpreter is required, but ${XRAY_PYTHON} is not Python 3")
endif()

# Generate xray_version.h from template (single source of truth)
configure_file(
    "${CMAKE_SOURCE_DIR}/include/xray_version.h.in"
    "${CMAKE_SOURCE_DIR}/include/xray_version.h"
    @ONLY
)

# ============================================================================
# XISA Code Generation (Python xisagen: .def → generated headers)
# ============================================================================
# Uses tools/xisagen/xisagen.py to regenerate headers from .def sources.
# Generated headers are checked into git, so this only runs when .def changes.

set(XISAGEN ${CMAKE_SOURCE_DIR}/tools/xisagen/xisagen.py)
set(STDLIBGEN ${CMAKE_SOURCE_DIR}/tools/stdlibgen/stdlibgen.py)
set(NATIVE_TYPE_EMBEDDER ${CMAKE_SOURCE_DIR}/scripts/embed_native_types.py)
file(GLOB XRAY_STDLIB_DEF_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/stdlib/defs/*.def"
)
file(GLOB XRAY_NATIVE_TYPE_SOURCE_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/stdlib/types/*.xr"
)

set(XRAY_GEN_XI_OPS_DEPS
    ${CMAKE_SOURCE_DIR}/src/ir/xi_ops_gen.h
)

set(XRAY_GEN_XI_VERIFY_DEPS
    ${CMAKE_SOURCE_DIR}/src/ir/xi_verify_gen.h
)

set(XRAY_GEN_XI_LOWERING_DEPS
    ${CMAKE_SOURCE_DIR}/src/ir/xi_lowering_coverage_gen.h
    ${CMAKE_SOURCE_DIR}/src/ir/xi_emit_vm_gen.h
    ${CMAKE_SOURCE_DIR}/src/vm/xvm_template_width_gen.inc.c
    ${CMAKE_SOURCE_DIR}/src/vm/xvm_template_bitwise_binary_gen.inc.c
    ${CMAKE_SOURCE_DIR}/src/vm/xvm_template_bitwise_unary_gen.inc.c
    ${CMAKE_SOURCE_DIR}/src/vm/xvm_template_unary_gen.inc.c
    ${CMAKE_SOURCE_DIR}/src/vm/xvm_template_arith_binary_gen.inc.c
    ${CMAKE_SOURCE_DIR}/src/vm/xvm_template_shift_gen.inc.c
    ${CMAKE_SOURCE_DIR}/src/vm/xvm_template_compare_gen.inc.c
    ${CMAKE_SOURCE_DIR}/src/aot/xi_to_c_dispatch_gen.h
    ${CMAKE_SOURCE_DIR}/src/aot/xi_to_c_stmt_dispatch_gen.h
)

set(XRAY_GEN_AOT_METADATA_DEPS
    ${CMAKE_SOURCE_DIR}/src/aot/xaot_rep_gen.h
    ${CMAKE_SOURCE_DIR}/src/aot/xaot_abi_gen.h
    ${CMAKE_SOURCE_DIR}/src/aot/xaot_layout_gen.h
)

set(XRAY_GEN_STDLIB_METADATA_DEPS
    ${CMAKE_SOURCE_DIR}/src/aot/xstdlib_aot_methods_generated.inc.c
    ${CMAKE_SOURCE_DIR}/src/aot/xaot_stdlib_generated.inc.c
    ${CMAKE_SOURCE_DIR}/src/stdlib/xstdlib_vm_bindings_generated.inc.c
    ${CMAKE_SOURCE_DIR}/src/stdlib/xstdlib_class_bindings_generated.inc.c
    ${CMAKE_SOURCE_DIR}/src/stdlib/xstdlib_defs_generated.h
)

add_custom_command(
    OUTPUT  ${CMAKE_SOURCE_DIR}/src/ir/xi_ops_gen.h
    COMMAND ${XRAY_PYTHON} ${XISAGEN} xi-ops
            ${CMAKE_SOURCE_DIR}/xisa/xi/ops.def
            ${CMAKE_SOURCE_DIR}/src/ir/xi_ops_gen.h
    DEPENDS ${XISAGEN} ${CMAKE_SOURCE_DIR}/xisa/xi/ops.def
    COMMENT "xisagen: generating xi_ops_gen.h from ops.def"
)

add_custom_command(
    OUTPUT  ${CMAKE_SOURCE_DIR}/src/ir/xi_verify_gen.h
    COMMAND ${XRAY_PYTHON} ${XISAGEN} xi-verify
            ${CMAKE_SOURCE_DIR}/xisa/xi/ops.def
            ${CMAKE_SOURCE_DIR}/xisa/xi/verifier.def
            ${CMAKE_SOURCE_DIR}/src/ir/xi_verify_gen.h
    DEPENDS ${XISAGEN}
            ${CMAKE_SOURCE_DIR}/xisa/xi/ops.def
            ${CMAKE_SOURCE_DIR}/xisa/xi/verifier.def
    COMMENT "xisagen: generating xi_verify_gen.h from verifier.def"
)

add_custom_command(
    OUTPUT  ${XRAY_GEN_XI_LOWERING_DEPS}
    COMMAND ${XRAY_PYTHON} ${XISAGEN} xi-lowering
            ${CMAKE_SOURCE_DIR}/xisa/xi/ops.def
            ${CMAKE_SOURCE_DIR}/xisa/xi/lowering.def
            ${CMAKE_SOURCE_DIR}
    DEPENDS ${XISAGEN}
            ${CMAKE_SOURCE_DIR}/xisa/xi/ops.def
            ${CMAKE_SOURCE_DIR}/xisa/xi/lowering.def
    COMMENT "xisagen: generating xi_lowering_coverage_gen.h from lowering.def"
)

add_custom_command(
    OUTPUT  ${CMAKE_SOURCE_DIR}/src/aot/xaot_rep_gen.h
    COMMAND ${XRAY_PYTHON} ${XISAGEN} aot-rep
            ${CMAKE_SOURCE_DIR}/xisa/aot/rep.def
            ${CMAKE_SOURCE_DIR}/src/aot/xaot_rep_gen.h
    DEPENDS ${XISAGEN} ${CMAKE_SOURCE_DIR}/xisa/aot/rep.def
    COMMENT "xisagen: generating xaot_rep_gen.h from rep.def"
)

add_custom_command(
    OUTPUT  ${CMAKE_SOURCE_DIR}/src/aot/xaot_abi_gen.h
    COMMAND ${XRAY_PYTHON} ${XISAGEN} aot-abi
            ${CMAKE_SOURCE_DIR}/xisa/aot/rep.def
            ${CMAKE_SOURCE_DIR}/xisa/aot/abi.def
            ${CMAKE_SOURCE_DIR}/src/aot/xaot_abi_gen.h
    DEPENDS ${XISAGEN}
            ${CMAKE_SOURCE_DIR}/xisa/aot/rep.def
            ${CMAKE_SOURCE_DIR}/xisa/aot/abi.def
    COMMENT "xisagen: generating xaot_abi_gen.h from abi.def"
)

add_custom_command(
    OUTPUT  ${CMAKE_SOURCE_DIR}/src/aot/xaot_layout_gen.h
    COMMAND ${XRAY_PYTHON} ${XISAGEN} aot-layout
            ${CMAKE_SOURCE_DIR}/xisa/aot/rep.def
            ${CMAKE_SOURCE_DIR}/xisa/aot/layout.def
            ${CMAKE_SOURCE_DIR}/src/aot/xaot_layout_gen.h
    DEPENDS ${XISAGEN}
            ${CMAKE_SOURCE_DIR}/xisa/aot/rep.def
            ${CMAKE_SOURCE_DIR}/xisa/aot/layout.def
    COMMENT "xisagen: generating xaot_layout_gen.h from layout.def"
)

add_custom_command(
    OUTPUT  ${XRAY_GEN_STDLIB_METADATA_DEPS}
    COMMAND ${XRAY_PYTHON} ${STDLIBGEN}
            --root ${CMAKE_SOURCE_DIR}
    DEPENDS ${STDLIBGEN}
            ${XRAY_STDLIB_DEF_FILES}
    COMMENT "stdlibgen: generating stdlib metadata from stdlib/defs/*.def"
)

add_custom_command(
    OUTPUT  ${CMAKE_SOURCE_DIR}/src/frontend/analyzer/xnative_type_defs.inc.c
    COMMAND ${XRAY_PYTHON} ${NATIVE_TYPE_EMBEDDER}
            ${CMAKE_SOURCE_DIR}/stdlib/types
            --output ${CMAKE_SOURCE_DIR}/src/frontend/analyzer/xnative_type_defs.inc.c
    DEPENDS ${NATIVE_TYPE_EMBEDDER}
            ${XRAY_NATIVE_TYPE_SOURCE_FILES}
    COMMENT "embedding stdlib native type declarations"
)

add_custom_target(gen-xi-ops DEPENDS
    ${XRAY_GEN_XI_OPS_DEPS}
)

add_custom_target(gen-xi-verify DEPENDS
    ${XRAY_GEN_XI_VERIFY_DEPS}
)

add_custom_target(gen-xi-lowering DEPENDS
    ${XRAY_GEN_XI_LOWERING_DEPS}
)

add_custom_target(gen-aot-metadata DEPENDS
    ${XRAY_GEN_AOT_METADATA_DEPS}
)

add_custom_target(gen-stdlib-metadata DEPENDS
    ${XRAY_GEN_STDLIB_METADATA_DEPS}
)

add_custom_target(gen-native-type-defs DEPENDS
    ${CMAKE_SOURCE_DIR}/src/frontend/analyzer/xnative_type_defs.inc.c
)

# Linux: expose POSIX/GNU extensions (CLOCK_MONOTONIC, CPU_SET, pthread_setaffinity_np, etc.)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_GNU_SOURCE)
endif()

# ============================================================================
# Compiler Options
# ============================================================================

# C Standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# C++ Standard (for LLVM backend)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(XRAY_BUILD_PROFILE "${CMAKE_BUILD_TYPE}")
if(NOT XRAY_BUILD_PROFILE)
    set(XRAY_BUILD_PROFILE "MultiConfig")
endif()
add_compile_definitions(
    XRAY_BUILD_COMMIT="${XRAY_GIT_COMMIT}"
    XRAY_BUILD_DIRTY=${XRAY_GIT_DIRTY}
    XRAY_BUILD_PROFILE="${XRAY_BUILD_PROFILE}"
    XRAY_BUILD_TARGET="${XRAY_RELEASE_TARGET}"
)

# macOS Universal Binary support (Intel + Apple Silicon)
if(APPLE)
    option(XR_UNIVERSAL_BINARY "Build Universal Binary (x86_64 + arm64)" OFF)
    if(XR_UNIVERSAL_BINARY)
        set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64")
        message(STATUS "Building Universal Binary: x86_64 + arm64")
    endif()
endif()

# Compiler options
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra)

    # Disable floating-point contraction (FMA fusion). The interpreter rounds
    # every arithmetic op independently, so a*b+c must not fuse into a single
    # rounding here either, or the same .xr would observably diverge between
    # the VM and an AOT binary built by the host compiler.
    add_compile_options(-ffp-contract=off)

    # Debug mode options
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-O0 -g3)
    else()
        add_compile_options(-O2 -g)
    endif()

    # Enable linker dead-code elimination (for xray build output size).
    # Compile: split each function/data item into its own section.
    # Link: discard sections that are never referenced.
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-ffunction-sections -fdata-sections)
        if(APPLE)
            add_link_options(-Wl,-dead_strip)
        else()
            add_link_options(-Wl,--gc-sections)
        endif()
    endif()

elseif(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    # MSVC needs explicit opt-in for C11 atomics. /std:c11 sets the
    # language version; /experimental:c11atomics turns on the
    # _Atomic keyword path (still flagged "experimental" in VS 2022
    # but the implementation is stable and required by the
    # runtime's lock-free queues / GC marker / IC counters).
    add_compile_options(/std:c11 /experimental:c11atomics)
    # Silence the wall of CRT "deprecated" warnings on strerror,
    # strncpy and friends. These are POSIX functions whose Windows
    # _s replacements have a different signature; our portable code
    # uses the plain forms deliberately.
    # WIN32_LEAN_AND_MEAN: keep <windows.h> from pulling in the
    # legacy <winsock.h>, which collides with <winsock2.h>'s
    # struct definitions whenever the latter is included second.
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    # UTF-8 source / execution charset so string literals and the
    # console match what every other platform uses.
    add_compile_options(/utf-8)
endif()

# ============================================================================
# Project Configuration Options
# ============================================================================

option(ENABLE_LLVM_BACKEND "Enable LLVM backend" OFF)
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_ASAN "Enable AddressSanitizer memory detection" OFF)
option(ENABLE_TLS "Enable HTTPS/TLS support (requires OpenSSL)" ON)
option(XR_USE_MIMALLOC "Use mimalloc high-performance allocator" OFF)
option(XR_STDLIB_FROM_FILE "Load stdlib from file instead of embedded bytecode (for debugging)" OFF)
option(XRAY_STDLIB_VM_FASTPATHS "Enable verified VM-only stdlib fastpaths" ON)

# Modular build options
option(XR_BUILD_LSP "Build LSP language server" ON)
option(XR_BUILD_DAP "Build DAP debug adapter" ON)
option(XR_BUILD_MCP "Build MCP server for AI assistants" ON)
option(XRAY_BUNDLE_ZIG "Install a Zig executable with Xray for AOT cross targets" OFF)
set(XRAY_BUNDLED_ZIG "" CACHE FILEPATH "Zig executable to install when XRAY_BUNDLE_ZIG=ON")

# Stdlib modular options (for security control / binary size)
option(XR_STDLIB_FULL       "Full stdlib (ignores group options below)" ON)
option(XR_STDLIB_FILESYSTEM "Filesystem modules (io, os)" ON)
option(XR_STDLIB_NET        "Network modules (net, http)" ON)
option(XR_STDLIB_CRYPTO     "Crypto modules (crypto)" ON)
option(XR_STDLIB_DATA       "Data format modules (csv, toml, yaml, xml)" ON)
option(XR_STDLIB_COMPRESS   "Compression modules (compress)" ON)
option(XR_STDLIB_CLUSTER    "Cluster module (distributed Named Channel)" ON)
option(XR_BUILD_TEST_MODULES "Build internal stdlib test modules" OFF)

if(XR_BUILD_TEST_MODULES)
    add_compile_definitions(XR_HAS_TEST_MODULES=1)
endif()

# io_uring backend (Linux only, auto-detected)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(CheckIncludeFile)
    check_include_file("liburing.h" HAVE_LIBURING_H)
    if(HAVE_LIBURING_H)
        message(STATUS "io_uring: liburing found, enabling io_uring backend")
        add_compile_definitions(XR_HAS_IO_URING)
        link_libraries(uring)
    else()
        message(STATUS "io_uring: liburing not found, using epoll backend")
    endif()
endif()

# Multi-core and NaN Tagging are now the only implementation, no conditional compilation needed

# AddressSanitizer configuration
if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    add_compile_definitions(XR_BUILD_ASAN=1)
    if(MSVC)
        # MSVC's ASan instrumentation is a compiler option.  The object files
        # carry the runtime directives consumed by link.exe; /INCREMENTAL:NO
        # is required by the Windows ASan runtime and /Oy- preserves frames.
        add_compile_options(/fsanitize=address /Oy-)
        add_link_options(/INCREMENTAL:NO)
        # A generated-C fallback may be compiled by Zig while consuming the
        # MSVC-instrumented build-tree archives.  Zig does not auto-link the
        # Visual C++ ASan import library or its dynamic-runtime thunk, so make
        # those exact developer-toolchain artifacts available to the verified
        # probe and AOT link plan.  Release payloads never define these paths.
        get_filename_component(_XRAY_MSVC_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
        get_filename_component(_XRAY_MSVC_HOST_DIR "${_XRAY_MSVC_BIN_DIR}" DIRECTORY)
        get_filename_component(_XRAY_MSVC_BIN_ROOT "${_XRAY_MSVC_HOST_DIR}" DIRECTORY)
        get_filename_component(_XRAY_MSVC_TOOL_ROOT "${_XRAY_MSVC_BIN_ROOT}" DIRECTORY)
        set(_XRAY_MSVC_ASAN_LIB_DIR "${_XRAY_MSVC_TOOL_ROOT}/lib/x64")
        set(_XRAY_MSVC_ASAN_IMPORT
            "${_XRAY_MSVC_ASAN_LIB_DIR}/clang_rt.asan_dynamic-x86_64.lib")
        set(_XRAY_MSVC_ASAN_THUNK
            "${_XRAY_MSVC_ASAN_LIB_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
        if(NOT EXISTS "${_XRAY_MSVC_ASAN_IMPORT}" OR NOT EXISTS "${_XRAY_MSVC_ASAN_THUNK}")
            message(FATAL_ERROR "MSVC x64 ASan runtime libraries are missing")
        endif()
        add_compile_definitions(
            XRT_BUILD_ASAN_WINDOWS_IMPORT="${_XRAY_MSVC_ASAN_IMPORT}"
            XRT_BUILD_ASAN_WINDOWS_THUNK="${_XRAY_MSVC_ASAN_THUNK}"
        )
        # MSVC AddressSanitizer is incompatible with /RTC and incremental
        # linking. CMake's Debug defaults enable both, so remove only those
        # incompatible defaults and retain debuginfo plus frame pointers.
        foreach(_xray_flag_var
                CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG
                CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO)
            string(REPLACE "/RTC1" "" ${_xray_flag_var} "${${_xray_flag_var}}")
            string(REPLACE "/RTCsu" "" ${_xray_flag_var} "${${_xray_flag_var}}")
            string(REPLACE "/ZI" "/Zi" ${_xray_flag_var} "${${_xray_flag_var}}")
        endforeach()
        add_compile_options(/fsanitize=address /Oy-)
        add_link_options(/INCREMENTAL:NO)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
endif()

# UndefinedBehaviorSanitizer configuration
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
if(ENABLE_UBSAN)
    message(STATUS "UndefinedBehaviorSanitizer enabled")
    add_compile_definitions(XR_BUILD_UBSAN=1)
    # Core UBSan checks for undefined behavior
    # Note: We only use -fsanitize=undefined (excludes unsigned-integer-overflow)
    # which catches real undefined behavior like:
    # - Signed integer overflow
    # - Null pointer dereference
    # - Out-of-bounds array access
    # - Use after scope, etc.
    add_compile_options(
        -fsanitize=undefined
        -fno-sanitize=function
        -fno-omit-frame-pointer
    )
    add_link_options(-fsanitize=undefined -fno-sanitize=function)
endif()

# ThreadSanitizer configuration
option(ENABLE_TSAN "Enable ThreadSanitizer data race detection" OFF)
if(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_definitions(XR_BUILD_TSAN=1)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# MemorySanitizer configuration (Clang only, Linux only)
# Detects reads of uninitialized memory across heap / stack / register.
# Required by Clang; GCC has no MSan. Best effort on macOS.
option(ENABLE_MSAN "Enable MemorySanitizer (Clang only)" OFF)
if(ENABLE_MSAN)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_MSAN requires Clang (current: ${CMAKE_C_COMPILER_ID})")
    endif()
    message(STATUS "MemorySanitizer enabled (track-origins=2)")
    add_compile_definitions(XR_BUILD_MSAN=1)
    add_compile_options(
        -fsanitize=memory
        -fsanitize-memory-track-origins=2
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
    )
    add_link_options(-fsanitize=memory)
endif()

# Code Coverage configuration (gcov/lcov)
option(ENABLE_COVERAGE "Enable code coverage reporting" OFF)
if(ENABLE_COVERAGE)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        message(STATUS "Code coverage enabled")
        add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
        add_link_options(--coverage)
    else()
        message(WARNING "Code coverage requires GCC or Clang")
    endif()
endif()

# Fuzzing configuration (libFuzzer)
option(ENABLE_FUZZING "Enable fuzzing tests (requires Clang)" OFF)

# Combined Sanitizer mode (ASan + UBSan, useful for thorough testing)
option(ENABLE_SANITIZERS "Enable both ASan and UBSan" OFF)
if(ENABLE_SANITIZERS)
    message(STATUS "Combined Sanitizers enabled (ASan + UBSan)")
    add_compile_definitions(XR_BUILD_ASAN=1 XR_BUILD_UBSAN=1)
    add_compile_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    add_link_options(-fsanitize=address,undefined)
endif()

# Sanitizer builds (ASan/UBSan/TSan/MSan) run compile-heavy stdlib/AOT tests
# 5-9x slower, so scale their CTest timeouts to avoid spurious sanitizer-only
# timeouts. Non-sanitizer builds keep the tight timeouts as a real perf guard.
if(ENABLE_SANITIZERS OR ENABLE_TSAN OR ENABLE_MSAN)
    set(XR_TEST_TIMEOUT_SCALE 8)
else()
    set(XR_TEST_TIMEOUT_SCALE 1)
endif()

# Debug options
option(XR_DEBUG_REGS "Enable register bounds checking (for development)" OFF)
option(XR_DEBUG_COMPILER "Enable compiler debug assertions (for development)" OFF)
option(XR_DEBUG_REGALLOC "Enable register allocator statistics output" OFF)

# Stdlib loading mode (file vs embedded bytecode)
if(XR_STDLIB_FROM_FILE)
    add_compile_definitions(XR_STDLIB_FROM_FILE=1)
    message(STATUS "Stdlib: loading from file (debug mode)")
endif()

# Per-Coroutine GC removed (coroutine size optimization)
# All coroutines use unified memory block (XrCoroMemory), GC managed globally

# Debug mode configuration
if(XR_DEBUG_REGS)
    add_compile_definitions(XR_DEBUG_REGS=1)
    message(STATUS "Register bounds checking enabled")
endif()

if(XR_DEBUG_COMPILER)
    add_compile_definitions(XR_DEBUG_COMPILER=1)
    message(STATUS "Compiler debug assertions enabled")
endif()

if(XR_DEBUG_REGALLOC)
    add_compile_definitions(XR_DEBUG_REGALLOC=1)
    message(STATUS "Register allocator statistics output enabled")
endif()

# Target architecture detection.
# When cross-compiling on macOS (CMAKE_OSX_ARCHITECTURES), use that instead of
# CMAKE_SYSTEM_PROCESSOR which always reports the host CPU.
if(CMAKE_OSX_ARCHITECTURES)
    set(XR_TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(XR_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

# VM Profiler configuration
option(XR_ENABLE_VM_PROFILER "Enable VM instruction profiler" OFF)
option(XR_PROFILE_TIMING "Enable per-instruction timing (requires XR_ENABLE_VM_PROFILER)" OFF)

if(XR_ENABLE_VM_PROFILER)
    add_compile_definitions(XR_ENABLE_VM_PROFILER=1)
    message(STATUS "VM profiler enabled")
    if(XR_PROFILE_TIMING)
        add_compile_definitions(XR_PROFILE_TIMING=1)
        message(STATUS "VM profiler: per-instruction timing enabled")
    endif()
endif()

# mimalloc high-performance allocator configuration
if(XR_USE_MIMALLOC)
    find_package(mimalloc QUIET)
    if(mimalloc_FOUND)
        add_compile_definitions(XR_USE_MIMALLOC=1)
        # Ensure include/link paths are available globally
        get_target_property(MIMALLOC_INC mimalloc INTERFACE_INCLUDE_DIRECTORIES)
        if(MIMALLOC_INC)
            include_directories(${MIMALLOC_INC})
        endif()
        link_libraries(mimalloc)
        message(STATUS "mimalloc enabled (system install)")
    else()
        # Try using pkg-config
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(MIMALLOC mimalloc)
            if(MIMALLOC_FOUND)
                add_compile_definitions(XR_USE_MIMALLOC=1)
                message(STATUS "mimalloc enabled (pkg-config)")
            else()
                message(WARNING "mimalloc not found, using system malloc")
            endif()
        else()
            message(WARNING "mimalloc not found, using system malloc")
        endif()
    endif()
endif()

# ============================================================================
# Common Include Directories
# ============================================================================

set(XRAY_COMMON_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/base
    ${CMAKE_CURRENT_SOURCE_DIR}/src/os
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/value
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/mem
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/object
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/object/builtins
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/class
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/closure
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/symbol
    ${CMAKE_CURRENT_SOURCE_DIR}/src/coro
    ${CMAKE_CURRENT_SOURCE_DIR}/src/io
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/lexer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/parser
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/codegen
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/analyzer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/canonical
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/format
    ${CMAKE_CURRENT_SOURCE_DIR}/src/toolchain
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module
    ${CMAKE_CURRENT_SOURCE_DIR}/src/analysis
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aot
    ${CMAKE_CURRENT_SOURCE_DIR}/src/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/cli
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/toolchain
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/lsp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/dap
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/mcp
    ${CMAKE_CURRENT_SOURCE_DIR}/stdlib/net
)

# ============================================================================
# Source File Collection
# ============================================================================

function(xray_exclude_include_fragments source_list_name)
    if(DEFINED ${source_list_name})
        list(FILTER ${source_list_name} EXCLUDE REGEX "\\.inc\\.c$")
        set(${source_list_name} "${${source_list_name}}" PARENT_SCOPE)
    endif()
endfunction()

file(GLOB BASE_SRC CONFIGURE_DEPENDS "src/base/*.c")
list(APPEND BASE_SRC
    "src/shared/xr_unicode_grapheme_data.c"
    "src/shared/xr_unicode_grapheme.c"
)
# Cross-platform abstraction layer. Each impl file is exclusive to
# its platform — no body guards, only the right subtree is built.
if(WIN32)
    file(GLOB OS_SRC CONFIGURE_DEPENDS "src/os/win/*.c")
    set(AOT_RANDOM_SRC "src/os/win/random_win.c")
    set(AOT_TIME_SRC "src/os/win/time_win.c")
else()
    file(GLOB OS_SRC CONFIGURE_DEPENDS "src/os/unix/*.c")
    set(AOT_RANDOM_SRC "src/os/unix/random_unix.c")
    set(AOT_TIME_SRC "src/os/unix/time_unix.c")
endif()
file(GLOB RUNTIME_TOP_SRC CONFIGURE_DEPENDS "src/runtime/*.c")
file(GLOB RUNTIME_CORE_SRC CONFIGURE_DEPENDS "src/runtime/core/*.c")
file(GLOB RUNTIME_VALUE_SRC CONFIGURE_DEPENDS "src/runtime/value/*.c")
file(GLOB RUNTIME_MEM_SRC CONFIGURE_DEPENDS "src/runtime/mem/*.c")
file(GLOB RUNTIME_OBJECT_SRC CONFIGURE_DEPENDS "src/runtime/object/*.c")
file(GLOB RUNTIME_OBJECT_BUILTINS_SRC CONFIGURE_DEPENDS "src/runtime/object/builtins/*.c")
list(APPEND RUNTIME_OBJECT_SRC
    ${RUNTIME_OBJECT_BUILTINS_SRC}
    "src/runtime/object/xarray_vm.c"
    "src/runtime/object/xmap_vm.c"
)
list(REMOVE_DUPLICATES RUNTIME_OBJECT_SRC)
file(GLOB RUNTIME_CLASS_SRC CONFIGURE_DEPENDS "src/runtime/class/*.c")
list(APPEND RUNTIME_CLASS_SRC
    "src/runtime/class/xinstance_core.c"
    "src/runtime/class/xinstance_dynamic.c"
    "src/runtime/class/xinstance_destroy.c"
)
list(REMOVE_DUPLICATES RUNTIME_CLASS_SRC)
file(GLOB RUNTIME_CLOSURE_SRC CONFIGURE_DEPENDS "src/runtime/closure/*.c")
file(GLOB RUNTIME_SYMBOL_SRC CONFIGURE_DEPENDS "src/runtime/symbol/*.c")
file(GLOB CORO_SRC CONFIGURE_DEPENDS "src/coro/*.c")
list(APPEND CORO_SRC
    "src/coro/xaot_runtime.c"
    "src/coro/xresult_group_destroy.c"
    "src/coro/xresult_group_native.c"
    "src/coro/xresult_group_wait.c"
    "src/coro/xwork_queue_destroy.c"
    "src/coro/xwork_queue_native.c"
    "src/coro/xwork_queue_wait.c"
)
list(REMOVE_DUPLICATES CORO_SRC)
file(GLOB IO_SRC CONFIGURE_DEPENDS "src/io/*.c")

# Stackful coroutine context switch removed — fully stackless now
file(GLOB FRONTEND_LEXER_SRC CONFIGURE_DEPENDS "src/frontend/lexer/*.c")
file(GLOB FRONTEND_PARSER_SRC CONFIGURE_DEPENDS "src/frontend/parser/*.c")
file(GLOB FRONTEND_CODEGEN_SRC CONFIGURE_DEPENDS "src/frontend/codegen/*.c")
file(GLOB FRONTEND_ANALYZER_SRC CONFIGURE_DEPENDS "src/frontend/analyzer/*.c")
file(GLOB FRONTEND_CANONICAL_SRC CONFIGURE_DEPENDS "src/frontend/canonical/*.c")
file(GLOB FRONTEND_FORMAT_SRC CONFIGURE_DEPENDS "src/frontend/format/*.c")
file(GLOB TOOLCHAIN_SRC CONFIGURE_DEPENDS "src/toolchain/*.c")
file(GLOB VM_SRC CONFIGURE_DEPENDS "src/vm/*.c")
file(GLOB IR_SRC CONFIGURE_DEPENDS "src/ir/*.c")
file(GLOB MODULE_SRC CONFIGURE_DEPENDS "src/module/*.c")
file(GLOB ANALYSIS_SRC CONFIGURE_DEPENDS "src/analysis/*.c")
file(GLOB AOT_SRC CONFIGURE_DEPENDS "src/aot/*.c")
list(REMOVE_ITEM AOT_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/aot/xrt_gen_iter.c"
    "src/aot/xrt_gen_iter.c")
file(GLOB API_SRC CONFIGURE_DEPENDS "src/api/*.c")
file(GLOB NEW_CLI_SRC CONFIGURE_DEPENDS "src/app/cli/*.c")
file(GLOB APP_TOOLCHAIN_SRC CONFIGURE_DEPENDS "src/app/toolchain/*.c")

# Standard Library (Native modules) - modular conditional compilation
# Core modules (required): prelude (built-in type registry), time, math, log,
# gc, regex, path, datetime, encoding, base64, url
file(GLOB STDLIB_CORE_SRC CONFIGURE_DEPENDS
    "stdlib/prelude/*.c"
    "stdlib/stdlib_cache.c"
    "stdlib/_probe/*.c"
    "stdlib/time/*.c"
    "stdlib/math/*.c"
    "stdlib/log/*.c"
    "stdlib/mem/*.c"
    "stdlib/runtime/*.c"
    "stdlib/sync/*.c"
    "stdlib/parallel/*.c"
    "stdlib/simd/*.c"
    "stdlib/sys/*.c"
    "stdlib/regex/*.c"
    "stdlib/path/*.c"
    "stdlib/datetime/*.c"
    "stdlib/encoding/*.c"
    "stdlib/base64/*.c"
    "stdlib/url/*.c"
    "stdlib/text/*.c"
    "stdlib/strconv/*.c"
)
set(STDLIB_SRC ${STDLIB_CORE_SRC})

find_package(ZLIB QUIET)
if(XR_STDLIB_FULL)
    # Full stdlib: include all modules except those explicitly disabled
    file(GLOB_RECURSE STDLIB_ALL_SRC CONFIGURE_DEPENDS "stdlib/*/*.c")
    list(APPEND STDLIB_ALL_SRC "stdlib/stdlib_cache.c")
    list(FILTER STDLIB_ALL_SRC EXCLUDE REGEX ".*sqlite.*\\.c$")
    if(NOT XR_STDLIB_CLUSTER)
        list(FILTER STDLIB_ALL_SRC EXCLUDE REGEX ".*/cluster/.*")
    endif()
    if(NOT XR_STDLIB_COMPRESS)
        list(FILTER STDLIB_ALL_SRC EXCLUDE REGEX ".*/compress/.*")
    endif()
    if(NOT ZLIB_FOUND)
        list(FILTER STDLIB_ALL_SRC EXCLUDE REGEX "compress_zlib\\.c$")
    endif()
    if(NOT XR_BUILD_TEST_MODULES)
        list(FILTER STDLIB_ALL_SRC EXCLUDE REGEX ".*/stdlib/test_yield/.*\\.c$")
    endif()
    set(STDLIB_SRC ${STDLIB_ALL_SRC})
    # Full mode: define XR_HAS_* for modules actually included,
    # so xmodule.c guards work the same as modular mode.
    add_compile_definitions(XR_HAS_FILESYSTEM=1)
    add_compile_definitions(XR_HAS_NETWORK=1)
    add_compile_definitions(XR_HAS_CRYPTO=1)
    add_compile_definitions(XR_HAS_DATA_FORMATS=1)
    if(XR_STDLIB_COMPRESS)
        add_compile_definitions(XR_HAS_COMPRESS=1)
    endif()
    if(XR_STDLIB_CLUSTER)
        add_compile_definitions(XR_HAS_CLUSTER=1)
    endif()
    message(STATUS "Stdlib: full")
else()
    # Modular compilation - define XR_STDLIB_MODULAR to indicate modular build
    add_compile_definitions(XR_STDLIB_MODULAR=1)
    message(STATUS "Stdlib: modular build")
    if(XR_STDLIB_FILESYSTEM)
        file(GLOB STDLIB_FS_SRC CONFIGURE_DEPENDS "stdlib/io/*.c" "stdlib/os/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_FS_SRC})
        add_compile_definitions(XR_HAS_FILESYSTEM=1)
        message(STATUS "Stdlib: +filesystem (io, os)")
    endif()

    if(XR_STDLIB_NET)
        file(GLOB STDLIB_NET_SRC CONFIGURE_DEPENDS "stdlib/net/*.c" "stdlib/http/*.c" "stdlib/ws/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_NET_SRC})
        add_compile_definitions(XR_HAS_NETWORK=1)
        message(STATUS "Stdlib: +network (net, http, ws)")
    endif()

    if(XR_STDLIB_CRYPTO)
        file(GLOB STDLIB_CRYPTO_SRC CONFIGURE_DEPENDS "stdlib/crypto/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_CRYPTO_SRC})
        add_compile_definitions(XR_HAS_CRYPTO=1)
        message(STATUS "Stdlib: +crypto")
    endif()

    if(XR_STDLIB_DATA)
        file(GLOB STDLIB_DATA_SRC CONFIGURE_DEPENDS "stdlib/csv/*.c" "stdlib/toml/*.c" "stdlib/yaml/*.c" "stdlib/xml/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_DATA_SRC})
        add_compile_definitions(XR_HAS_DATA_FORMATS=1)
        message(STATUS "Stdlib: +data formats (csv, toml, yaml, xml)")
    endif()

    if(XR_STDLIB_COMPRESS)
        file(GLOB STDLIB_COMPRESS_SRC CONFIGURE_DEPENDS "stdlib/compress/*.c")
        if(NOT ZLIB_FOUND)
            list(FILTER STDLIB_COMPRESS_SRC EXCLUDE REGEX "compress_zlib\\.c$")
            message(STATUS "Stdlib: +compression (no system zlib, zlib wrapper disabled)")
        else()
            message(STATUS "Stdlib: +compression (with system zlib)")
        endif()
        list(APPEND STDLIB_SRC ${STDLIB_COMPRESS_SRC})
        add_compile_definitions(XR_HAS_COMPRESS=1)
    endif()

    if(XR_STDLIB_CLUSTER)
        file(GLOB STDLIB_CLUSTER_SRC CONFIGURE_DEPENDS "stdlib/cluster/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_CLUSTER_SRC})
        add_compile_definitions(XR_HAS_CLUSTER=1)
        message(STATUS "Stdlib: +cluster (distributed Named Channel)")
    endif()

    if(XR_BUILD_TEST_MODULES)
        file(GLOB STDLIB_TEST_SRC CONFIGURE_DEPENDS "stdlib/test_yield/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_TEST_SRC})
        message(STATUS "Stdlib: +test modules")
    endif()
endif()

file(GLOB XRAY_STDLIB_SCRIPT_SOURCES CONFIGURE_DEPENDS "stdlib/*/*.xr")
set(XRAY_STDLIB_EMBEDDED_SOURCE_INC
    "${CMAKE_CURRENT_BINARY_DIR}/generated/stdlib_embedded_sources.inc")
set(XRAY_STDLIB_BOOTSTRAP_BYTECODE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/bootstrap")
set(XRAY_STDLIB_BOOTSTRAP_BYTECODE_INC
    "${XRAY_STDLIB_BOOTSTRAP_BYTECODE_DIR}/stdlib_embedded_bytecodes.inc")
set(XRAY_STDLIB_FINAL_BYTECODE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/final")
set(XRAY_STDLIB_FINAL_BYTECODE_INC
    "${XRAY_STDLIB_FINAL_BYTECODE_DIR}/stdlib_embedded_bytecodes.inc")
set(XRAY_STDLIB_XRC_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/stdlib-bytecode")
add_custom_command(
    OUTPUT
        ${XRAY_STDLIB_EMBEDDED_SOURCE_INC}
        ${XRAY_STDLIB_BOOTSTRAP_BYTECODE_INC}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XRAY_STDLIB_BOOTSTRAP_BYTECODE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 ${XRAY_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_stdlib_embedded.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}
            --source-output ${XRAY_STDLIB_EMBEDDED_SOURCE_INC}
            --bytecode-output ${XRAY_STDLIB_BOOTSTRAP_BYTECODE_INC}
            ${XRAY_STDLIB_SCRIPT_SOURCES}
    DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_stdlib_embedded.py
        ${XRAY_STDLIB_SCRIPT_SOURCES}
    COMMENT "Generating embedded stdlib source table"
    VERBATIM
)
add_custom_target(gen-stdlib-embedded-sources
    DEPENDS ${XRAY_STDLIB_EMBEDDED_SOURCE_INC} ${XRAY_STDLIB_BOOTSTRAP_BYTECODE_INC})

# C include fragments use *.inc.c for editor support, but they are included
# from owning .c files and must never be compiled as standalone translation units.
foreach(XRAY_SRC_LIST
        BASE_SRC OS_SRC RUNTIME_TOP_SRC RUNTIME_CORE_SRC RUNTIME_VALUE_SRC RUNTIME_MEM_SRC
        RUNTIME_OBJECT_SRC RUNTIME_OBJECT_BUILTINS_SRC RUNTIME_CLASS_SRC
        RUNTIME_CLOSURE_SRC RUNTIME_SYMBOL_SRC CORO_SRC IO_SRC
        FRONTEND_LEXER_SRC FRONTEND_PARSER_SRC FRONTEND_CODEGEN_SRC
        FRONTEND_ANALYZER_SRC FRONTEND_CANONICAL_SRC FRONTEND_FORMAT_SRC TOOLCHAIN_SRC
        VM_SRC IR_SRC MODULE_SRC ANALYSIS_SRC AOT_SRC API_SRC NEW_CLI_SRC APP_TOOLCHAIN_SRC
        STDLIB_CORE_SRC STDLIB_SRC)
    xray_exclude_include_fragments(${XRAY_SRC_LIST})
endforeach()

# ============================================================================
# Build Target - Core Library (all core components)
# ============================================================================

set(CORE_ALL_SOURCES
    ${BASE_SRC}
    ${OS_SRC}
    ${RUNTIME_TOP_SRC}
    ${RUNTIME_CORE_SRC}
    ${RUNTIME_VALUE_SRC}
    ${RUNTIME_MEM_SRC}
    ${RUNTIME_OBJECT_SRC}
    ${RUNTIME_CLASS_SRC}
    ${RUNTIME_CLOSURE_SRC}
    ${RUNTIME_SYMBOL_SRC}
    ${CORO_SRC}
    ${IO_SRC}
    ${FRONTEND_LEXER_SRC}
    ${FRONTEND_PARSER_SRC}
    ${FRONTEND_CODEGEN_SRC}
    ${FRONTEND_ANALYZER_SRC}
    ${FRONTEND_CANONICAL_SRC}
    ${FRONTEND_FORMAT_SRC}
    ${TOOLCHAIN_SRC}
    ${VM_SRC}
    ${IR_SRC}
    ${MODULE_SRC}
    ${ANALYSIS_SRC}
    ${AOT_SRC}
    ${API_SRC}
    ${STDLIB_SRC}
)

add_library(xray_stdlib_bootstrap_core OBJECT ${CORE_ALL_SOURCES})
add_dependencies(xray_stdlib_bootstrap_core
    gen-xi-ops gen-xi-verify gen-xi-lowering gen-aot-metadata gen-stdlib-metadata
    gen-native-type-defs gen-stdlib-embedded-sources)

add_library(xray_core STATIC ${CORE_ALL_SOURCES})
add_dependencies(xray_core
    gen-xi-ops gen-xi-verify gen-xi-lowering gen-aot-metadata gen-stdlib-metadata
    gen-native-type-defs)

add_library(xray_aot_core STATIC
    src/aot/xrt_regex_core.c
    ${AOT_RANDOM_SRC}
    ${AOT_TIME_SRC}
    src/base/xarena.c
    src/base/xlog.c
    src/base/xunicode.c
    stdlib/compress/compress.c
    stdlib/crypto/crypto.c
    stdlib/regex/xregex.c
    stdlib/regex/xregex_compile.c
    stdlib/regex/xregex_dfa.c
    stdlib/regex/xregex_nfa.c
    stdlib/regex/xregex_parse.c
)
target_compile_definitions(xray_aot_core PRIVATE XR_COMPRESS_CORE_ONLY=1 XR_CRYPTO_CORE_ONLY=1)

set(XRAY_RT_CORO_SOURCES
    ${BASE_SRC}
    ${OS_SRC}
    ${RUNTIME_TOP_SRC}
    ${RUNTIME_CORE_SRC}
    ${RUNTIME_VALUE_SRC}
    ${RUNTIME_MEM_SRC}
    ${RUNTIME_OBJECT_SRC}
    ${RUNTIME_CLASS_SRC}
    ${RUNTIME_CLOSURE_SRC}
    ${RUNTIME_SYMBOL_SRC}
    ${CORO_SRC}
    ${IO_SRC}
    src/aot/xrt_gen_iter.c
)
list(REMOVE_DUPLICATES XRAY_RT_CORO_SOURCES)
list(FILTER XRAY_RT_CORO_SOURCES EXCLUDE REGEX ".*/src/runtime/xisolate_api\\.c$")
list(FILTER XRAY_RT_CORO_SOURCES EXCLUDE REGEX ".*/src/runtime/value/xchunk\\.c$")
list(FILTER XRAY_RT_CORO_SOURCES EXCLUDE REGEX ".*/src/runtime/value/xopcode_info\\.c$")
list(FILTER XRAY_RT_CORO_SOURCES EXCLUDE REGEX ".*/src/coro/xcoro_monitor\\.c$")
if(WIN32)
    # COFF linkers resolve undefined symbols before per-function GC, so the
    # otherwise-unused xstring_pool reference needs the lightweight accessors
    # in the archive for both MSVC and MinGW. Mach-O and ELF discard that
    # section before resolution.
    list(APPEND XRAY_RT_CORO_SOURCES src/runtime/xisolate_api.c)
endif()

add_library(xray_rt_coro STATIC ${XRAY_RT_CORO_SOURCES})
add_dependencies(xray_rt_coro gen-xi-ops gen-xi-verify gen-xi-lowering gen-aot-metadata)

set(XRAY_VM_RUNTIME_CORO_SOURCES ${CORO_SRC})
list(FILTER XRAY_VM_RUNTIME_CORO_SOURCES EXCLUDE REGEX "xaot_.*\\.c$")

set(XRAY_VM_RUNTIME_SOURCES
    ${BASE_SRC}
    ${OS_SRC}
    ${RUNTIME_TOP_SRC}
    ${RUNTIME_CORE_SRC}
    ${RUNTIME_VALUE_SRC}
    ${RUNTIME_MEM_SRC}
    ${RUNTIME_OBJECT_SRC}
    ${RUNTIME_CLASS_SRC}
    ${RUNTIME_CLOSURE_SRC}
    ${RUNTIME_SYMBOL_SRC}
    ${XRAY_VM_RUNTIME_CORO_SOURCES}
    ${IO_SRC}
    ${VM_SRC}
    src/api/xisolate.c
    src/api/xisolate_params.c
    src/api/xisolate_runtime.c
    src/api/xisolate_tls.c
    src/api/xruntime.c
    src/api/xvm_exec.c
    src/module/stdlib_embedded.c
    src/module/xbytecode_io.c
    src/module/xlockfile.c
    src/module/xmodule.c
    src/module/xmodule_resolver.c
    src/module/xnative_package.c
    src/module/xproject.c
    src/module/xsemver.c
    ${STDLIB_SRC}
)
list(REMOVE_DUPLICATES XRAY_VM_RUNTIME_SOURCES)

add_library(xray_vm_runtime STATIC ${XRAY_VM_RUNTIME_SOURCES})
add_dependencies(xray_vm_runtime gen-xi-ops gen-xi-verify gen-xi-lowering gen-aot-metadata)

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND MSVC)
    set_source_files_properties(
        src/vm/xvm.c
        PROPERTIES COMPILE_OPTIONS "/Od;/Ob0"
    )
endif()

# Expose development-tree paths for cmd_build's C compiler. Multi-config
# generators place static archives under the active configuration directory.
if(CMAKE_CONFIGURATION_TYPES)
    set(XRAY_BUILD_LIB_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
else()
    set(XRAY_BUILD_LIB_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()
add_compile_definitions(
    XRT_AOT_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/aot"
    XRT_SOURCE_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include"
    XRT_BUILD_LIB_DIR="${XRAY_BUILD_LIB_DIR}"
    XTC_BUILD_HOST_TARGET="${XRAY_HOST_TARGET_ABI}"
    XTC_STATIC_LIBRARY_PREFIX="${CMAKE_STATIC_LIBRARY_PREFIX}"
    XTC_STATIC_LIBRARY_SUFFIX="${CMAKE_STATIC_LIBRARY_SUFFIX}"
)


# Core library include paths (detailed configuration)
target_include_directories(xray_stdlib_bootstrap_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(xray_stdlib_bootstrap_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${XRAY_STDLIB_BOOTSTRAP_BYTECODE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_include_directories(xray_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(xray_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${XRAY_STDLIB_FINAL_BYTECODE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_include_directories(xray_rt_coro PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(xray_rt_coro PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

target_include_directories(xray_vm_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(xray_vm_runtime PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${XRAY_STDLIB_FINAL_BYTECODE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# Link libraries
if(WIN32)
    # Math library is part of the MSVC runtime; ws2_32 for sockets,
    # synchronization for WaitOnAddress/WakeByAddressSingle
    target_link_libraries(xray_stdlib_bootstrap_core PUBLIC ws2_32 synchronization)
    target_link_libraries(xray_core PUBLIC ws2_32 synchronization)
    target_link_libraries(xray_rt_coro PUBLIC ws2_32 synchronization)
    target_link_libraries(xray_vm_runtime PUBLIC ws2_32 synchronization)
else()
    target_link_libraries(xray_stdlib_bootstrap_core PUBLIC m pthread)
    target_link_libraries(xray_core PUBLIC m pthread)
    target_link_libraries(xray_rt_coro PUBLIC m pthread)
    target_link_libraries(xray_vm_runtime PUBLIC m pthread)
endif()
if(ZLIB_FOUND)
    target_link_libraries(xray_stdlib_bootstrap_core PUBLIC ZLIB::ZLIB)
    target_link_libraries(xray_core PUBLIC ZLIB::ZLIB)
    target_link_libraries(xray_vm_runtime PUBLIC ZLIB::ZLIB)
    target_compile_definitions(xray_stdlib_bootstrap_core PRIVATE XR_HAS_ZLIB=1)
    target_compile_definitions(xray_core PRIVATE XR_HAS_ZLIB=1)
    target_compile_definitions(xray_vm_runtime PRIVATE XR_HAS_ZLIB=1)
endif()

# libffi: FFI / C interop. The bytecode VM marshals extern calls through
# libffi; the AOT backend emits direct C calls and does not need it. Optional —
# when absent, extern calls raise a clean runtime error in the VM (AOT still
# works). dlopen/dlsym (POSIX) is provided by libdl / libc.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBFFI libffi)
endif()
if(NOT LIBFFI_FOUND)
    find_library(LIBFFI_LIBRARIES ffi)
    find_path(LIBFFI_INCLUDE_DIRS ffi.h)
    if(LIBFFI_LIBRARIES AND LIBFFI_INCLUDE_DIRS)
        set(LIBFFI_FOUND TRUE)
    endif()
endif()
if(LIBFFI_FOUND)
    message(STATUS "libffi: found (VM FFI enabled)")
    target_include_directories(xray_stdlib_bootstrap_core PRIVATE ${LIBFFI_INCLUDE_DIRS})
    target_include_directories(xray_core PRIVATE ${LIBFFI_INCLUDE_DIRS})
    target_include_directories(xray_vm_runtime PRIVATE ${LIBFFI_INCLUDE_DIRS})
    target_link_libraries(xray_stdlib_bootstrap_core PUBLIC ${LIBFFI_LIBRARIES})
    target_link_libraries(xray_core PUBLIC ${LIBFFI_LIBRARIES})
    target_link_libraries(xray_vm_runtime PUBLIC ${LIBFFI_LIBRARIES})
    # PUBLIC: the CLI target builds AOT link commands by hand (invoke_cc) and
    # must know xray_core embeds the libffi-based extern invoker, so a runtime
    # program links -lffi too.
    target_compile_definitions(xray_stdlib_bootstrap_core PUBLIC XRAY_HAVE_LIBFFI=1)
    target_compile_definitions(xray_core PUBLIC XRAY_HAVE_LIBFFI=1)
    target_compile_definitions(xray_vm_runtime PUBLIC XRAY_HAVE_LIBFFI=1)
else()
    message(STATUS "libffi: not found (extern calls limited to the AOT backend)")
endif()
if(NOT WIN32)
    target_link_libraries(xray_stdlib_bootstrap_core PUBLIC ${CMAKE_DL_LIBS})
    target_link_libraries(xray_core PUBLIC ${CMAKE_DL_LIBS})
    target_link_libraries(xray_vm_runtime PUBLIC ${CMAKE_DL_LIBS})
endif()

# OpenSSL support
# - WebSocket stdlib needs libcrypto (SHA1 for handshake) even without TLS
# - Full TLS needs both libssl and libcrypto
if(APPLE)
    # Respect a release/toolchain-provided root first, then cover both native
    # Homebrew layouts for developer builds. Do not force the arm64 prefix on
    # Intel builders.
    if(NOT OPENSSL_ROOT_DIR)
        foreach(_xray_openssl_root "/opt/homebrew/opt/openssl@3" "/usr/local/opt/openssl@3")
            if(EXISTS "${_xray_openssl_root}")
                set(OPENSSL_ROOT_DIR "${_xray_openssl_root}")
                break()
            endif()
        endforeach()
    endif()
    # Native packages cannot depend on a user's Homebrew prefix.  Link OpenSSL
    # into the Xray executable; provider-specific AOT dependencies remain
    # explicit in the generated link manifest.
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
    target_include_directories(xray_stdlib_bootstrap_core PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_include_directories(xray_core PRIVATE ${OPENSSL_INCLUDE_DIR})
    target_include_directories(xray_vm_runtime PRIVATE ${OPENSSL_INCLUDE_DIR})
    # Always link libcrypto (needed by ws module for SHA1)
    target_link_libraries(xray_stdlib_bootstrap_core PUBLIC OpenSSL::Crypto)
    target_link_libraries(xray_core PUBLIC OpenSSL::Crypto)
    target_link_libraries(xray_vm_runtime PUBLIC OpenSSL::Crypto)
    if(ENABLE_TLS)
        target_link_libraries(xray_stdlib_bootstrap_core PUBLIC OpenSSL::SSL)
        target_link_libraries(xray_core PUBLIC OpenSSL::SSL)
        target_link_libraries(xray_vm_runtime PUBLIC OpenSSL::SSL)
        target_compile_definitions(xray_stdlib_bootstrap_core PUBLIC XR_ENABLE_TLS=1)
        target_compile_definitions(xray_core PUBLIC XR_ENABLE_TLS=1)
        target_compile_definitions(xray_vm_runtime PUBLIC XR_ENABLE_TLS=1)
    endif()
else()
    message(STATUS "OpenSSL: not found (ws/crypto modules may be limited)")
endif()

add_executable(xray_stdlib_bcgen
    src/app/tools/xstdlib_bcgen.c
)
target_link_libraries(xray_stdlib_bcgen PRIVATE xray_stdlib_bootstrap_core)
target_include_directories(xray_stdlib_bcgen PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(MSVC)
    # The bootstrap compiler runs the same recursive parser/lowering passes as
    # xray.  ASan inflates those frames enough to overflow PE/COFF's default
    # 1 MiB stack while compiling the embedded stdlib, so give both compiler
    # executables the same Unix-like main-thread allowance.
    target_link_options(xray_stdlib_bcgen PRIVATE "/STACK:8388608")
endif()

add_custom_command(
    OUTPUT ${XRAY_STDLIB_FINAL_BYTECODE_INC}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XRAY_STDLIB_FINAL_BYTECODE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XRAY_STDLIB_XRC_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 ${XRAY_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_stdlib_embedded.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}
            --compiler $<TARGET_FILE:xray_stdlib_bcgen>
            --bytecode-dir ${XRAY_STDLIB_XRC_DIR}
            --require-bytecode
            --bytecode-output ${XRAY_STDLIB_FINAL_BYTECODE_INC}
            ${XRAY_STDLIB_SCRIPT_SOURCES}
    DEPENDS
        xray_stdlib_bcgen
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_stdlib_embedded.py
        ${XRAY_STDLIB_SCRIPT_SOURCES}
    COMMENT "Generating embedded stdlib bytecode table"
    VERBATIM
)
add_custom_target(gen-stdlib-embedded-bytecodes DEPENDS ${XRAY_STDLIB_FINAL_BYTECODE_INC})
add_custom_target(gen-stdlib-embedded
    DEPENDS gen-stdlib-embedded-sources gen-stdlib-embedded-bytecodes)
add_dependencies(xray_core gen-stdlib-embedded-bytecodes)
add_dependencies(xray_vm_runtime gen-stdlib-embedded-bytecodes)

# ============================================================================
# Build Target - Main Executable (optional LSP and DAP)
# ============================================================================

# Base CLI source files
set(XRAY_CLI_SOURCES ${NEW_CLI_SRC} ${APP_TOOLCHAIN_SRC})
list(APPEND XRAY_CLI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/cli/xcmd_builtin_dump.c)
list(REMOVE_DUPLICATES XRAY_CLI_SOURCES)
set(XRAY_CLI_INCLUDES
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/cli
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/toolchain
)

# LSP optional module
if(XR_BUILD_LSP)
    file(GLOB LSP_SRC CONFIGURE_DEPENDS "src/app/lsp/*.c")
    list(FILTER LSP_SRC EXCLUDE REGEX "xlsp_main\.c$")
    list(APPEND XRAY_CLI_SOURCES ${LSP_SRC})
    list(APPEND XRAY_CLI_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/lsp)
    add_compile_definitions(XR_HAS_LSP=1)
    message(STATUS "LSP: enabled")
else()
    message(STATUS "LSP: disabled")
endif()

# DAP optional module
if(XR_BUILD_DAP)
    file(GLOB DAP_SRC CONFIGURE_DEPENDS "src/app/dap/*.c")
    list(FILTER DAP_SRC EXCLUDE REGEX "xdap_main\.c$")

    # All DAP files (including xdap_debug.c) are in DAP module, not added to xray_core
    # VM communicates with DAP through debug_hooks callback interface
    list(APPEND XRAY_CLI_SOURCES ${DAP_SRC})
    list(APPEND XRAY_CLI_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/dap)

    add_compile_definitions(XR_HAS_DAP=1)
    message(STATUS "DAP: enabled")
else()
    message(STATUS "DAP: disabled")
endif()

add_custom_target(regen-language-docs
    COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 ${XRAY_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_language_docs.py
        --root ${CMAKE_CURRENT_SOURCE_DIR}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    VERBATIM
    COMMENT "Regenerating language specs and MCP knowledge projection files"
)

# Auto-regenerate LANGUAGE_SPEC*.md + docs/knowledge/ at configure time
# from spec/source/ (tracked in-repo). Keeps committed generated files
# in sync with spec source without manual developer intervention.
set(_spec_source "${CMAKE_CURRENT_SOURCE_DIR}/spec/source")
if(IS_DIRECTORY "${_spec_source}/cards")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 ${XRAY_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_language_docs.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}
            --check
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE _docs_check_result
        OUTPUT_QUIET ERROR_QUIET
    )
    if(_docs_check_result EQUAL 1)
        message(STATUS "Language docs stale — regenerating")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_language_docs.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE _docs_regen_result
        )
        if(NOT _docs_regen_result EQUAL 0)
            message(WARNING "Language docs regeneration failed (exit ${_docs_regen_result})")
        endif()
    elseif(_docs_check_result EQUAL 0)
        message(STATUS "Language docs: up-to-date")
    else()
        message(WARNING "Language docs check failed (exit ${_docs_check_result})")
    endif()
else()
    message(STATUS "Spec source not found — using committed generated files")
endif()

# MCP optional module
if(XR_BUILD_MCP)
    file(GLOB MCP_SRC CONFIGURE_DEPENDS "src/app/mcp/*.c")
    list(APPEND XRAY_CLI_SOURCES ${MCP_SRC})
    list(APPEND XRAY_CLI_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/mcp)

    add_compile_definitions(XR_HAS_MCP=1)
    set(_xmcp_builtin_dump ${CMAKE_CURRENT_BINARY_DIR}/xmcp_builtin_dump.json)
    set(_xray_api_inventory_builtin_dump ${CMAKE_CURRENT_BINARY_DIR}/xray_api_inventory_builtin_dump.json)
    set(_xmcp_api_inventory ${CMAKE_CURRENT_BINARY_DIR}/xray_api_inventory.json)
    set(_xmcp_api_inventory_html ${CMAKE_CURRENT_BINARY_DIR}/xray_api_inventory.html)
    add_custom_target(api-inventory
        COMMAND $<TARGET_FILE:xray> builtin-dump > ${_xray_api_inventory_builtin_dump}
        COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 ${XRAY_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_api_inventory.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}
            --builtin-dump ${_xray_api_inventory_builtin_dump}
            --json ${_xmcp_api_inventory}
            --html ${_xmcp_api_inventory_html}
        DEPENDS xray
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
        COMMENT "Generating source-derived Xray API inventory JSON and HTML"
    )
    add_custom_target(regen-mcp-knowledge
        COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 ${XRAY_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_language_docs.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}
        COMMAND $<TARGET_FILE:xray> builtin-dump > ${_xmcp_builtin_dump}
        COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 ${XRAY_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_api_inventory.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}
            --builtin-dump ${_xmcp_builtin_dump}
            --json ${_xmcp_api_inventory}
            --check-docs
        COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 ${XRAY_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_mcp_knowledge.py
            --docs ${CMAKE_CURRENT_SOURCE_DIR}/docs/knowledge
            --spec ${CMAKE_CURRENT_SOURCE_DIR}/LANGUAGE_SPEC_CN.md
            --api-inventory ${_xmcp_api_inventory}
            --out ${CMAKE_CURRENT_SOURCE_DIR}/src/app/mcp/xmcp_knowledge_generated.c
        DEPENDS xray
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
        COMMENT "Regenerating language docs, source API inventory, and MCP knowledge tables"
    )
    message(STATUS "MCP: enabled")
else()
    message(STATUS "MCP: disabled")
endif()

# Optional readline support for REPL
find_library(READLINE_LIB readline)
if(READLINE_LIB)
    message(STATUS "readline: found (${READLINE_LIB})")
    add_compile_definitions(HAS_READLINE=1)
else()
    message(STATUS "readline: not found (REPL will use basic input)")
endif()

add_executable(xray ${XRAY_CLI_SOURCES})
add_dependencies(xray xray_aot_core xray_rt_coro xray_vm_runtime)

# The compiler's recursive IR/AOT passes can exceed the PE/COFF default 1 MiB
# stack on larger programs. Match the usual Unix main-thread allowance so
# native Windows builds do not fail with STATUS_STACK_OVERFLOW.
if(MSVC)
    target_link_options(xray PRIVATE "/STACK:8388608")
endif()

target_link_libraries(xray PRIVATE xray_core)
if(READLINE_LIB)
    target_link_libraries(xray PRIVATE ${READLINE_LIB})
endif()

target_include_directories(xray PRIVATE ${XRAY_CLI_INCLUDES})

if(WIN32 AND CMAKE_VERSION VERSION_GREATER_EQUAL "3.21")
    add_custom_command(TARGET xray POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DOUTPUT_DIR=$<TARGET_FILE_DIR:xray>
                -DRUNTIME_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:xray>,\;>
                -P ${CMAKE_SOURCE_DIR}/cmake/copy_runtime_dlls.cmake
    )
endif()

# Build configuration summary
set(MODULES_ENABLED "")
if(XR_BUILD_LSP)
    string(APPEND MODULES_ENABLED "LSP ")
endif()
if(XR_BUILD_DAP)
    string(APPEND MODULES_ENABLED "DAP ")
endif()
if(XR_BUILD_MCP)
    string(APPEND MODULES_ENABLED "MCP ")
endif()
if(MODULES_ENABLED STREQUAL "")
    set(MODULES_ENABLED "core")
endif()
message(STATUS "Main executable: xray (${MODULES_ENABLED})")

# ============================================================================
# Unit Tests
# ============================================================================

if(BUILD_TESTS)
    enable_testing()

    # CTest does not honor a script's shebang on Windows and may hand `.sh`
    # files to WSL instead of the Git-for-Windows shell used by this workspace.
    # Resolve one explicit Bash executable so cross-platform gates have the
    # same invocation contract on Windows, macOS, and Linux.
    find_program(XRAY_TEST_BASH
        NAMES bash bash.exe
        HINTS
            "C:/Program Files/Git/bin"
            "C:/Program Files/Git/usr/bin")
    if(NOT XRAY_TEST_BASH)
        message(FATAL_ERROR "BUILD_TESTS requires Bash for the shell-based test gates")
    endif()

    include(ProcessorCount)
    ProcessorCount(XRAY_HOST_PROCESSOR_COUNT)
    if(NOT XRAY_HOST_PROCESSOR_COUNT OR XRAY_HOST_PROCESSOR_COUNT LESS 1)
        set(XRAY_HOST_PROCESSOR_COUNT 8)
    endif()
    set(XRAY_CTEST_DIFF_JOBS 16)
    if(XRAY_HOST_PROCESSOR_COUNT LESS XRAY_CTEST_DIFF_JOBS)
        set(XRAY_CTEST_DIFF_JOBS ${XRAY_HOST_PROCESSOR_COUNT})
    endif()
    set(XRAY_CTEST_DIFF_PROCESSORS ${XRAY_CTEST_DIFF_JOBS})
    set(XRAY_CTEST_DETERMINISTIC_DIFF_JOBS 4)
    if(XRAY_HOST_PROCESSOR_COUNT LESS XRAY_CTEST_DETERMINISTIC_DIFF_JOBS)
        set(XRAY_CTEST_DETERMINISTIC_DIFF_JOBS ${XRAY_HOST_PROCESSOR_COUNT})
    endif()
    set(XRAY_CTEST_AOT_FILETEST_JOBS 4)
    if(XRAY_HOST_PROCESSOR_COUNT LESS XRAY_CTEST_AOT_FILETEST_JOBS)
        set(XRAY_CTEST_AOT_FILETEST_JOBS ${XRAY_HOST_PROCESSOR_COUNT})
    endif()
    set(XRAY_CTEST_AOT_ISOLATE_JOBS 5)
    if(XRAY_HOST_PROCESSOR_COUNT LESS XRAY_CTEST_AOT_ISOLATE_JOBS)
        set(XRAY_CTEST_AOT_ISOLATE_JOBS ${XRAY_HOST_PROCESSOR_COUNT})
    endif()
    set(XRAY_CTEST_AOT_INCREMENTAL_JOBS 3)
    if(XRAY_HOST_PROCESSOR_COUNT LESS XRAY_CTEST_AOT_INCREMENTAL_JOBS)
        set(XRAY_CTEST_AOT_INCREMENTAL_JOBS ${XRAY_HOST_PROCESSOR_COUNT})
    endif()

    # All unit tests are managed by tests/unit/CMakeLists.txt
    add_subdirectory(tests/unit)

    add_executable(xray_package_payload_fixture
        tests/benchmarks/compiler/global_evidence_cache/package_payload_fixture.c
    )
    target_include_directories(xray_package_payload_fixture PRIVATE
        ${XRAY_COMMON_INCLUDES}
    )
    target_link_libraries(xray_package_payload_fixture PRIVATE xray_core)

    add_executable(bytecode_embed_runner
        tests/vm/bytecode_embed_runner.c
    )
    target_include_directories(bytecode_embed_runner PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(bytecode_embed_runner PRIVATE xray_vm_runtime)

    set(XRAY_BYTECODE_EMBED_SMOKE_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/bytecode_embed_smoke.xr)
    set(XRAY_BYTECODE_EMBED_SMOKE_XRC
        ${CMAKE_CURRENT_BINARY_DIR}/tests/vm/bytecode_embed_smoke.xrc)
    set(XRAY_BYTECODE_EMBED_PARALLEL_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/bytecode_embed_parallel_import.xr)
    set(XRAY_BYTECODE_EMBED_PARALLEL_XRC
        ${CMAKE_CURRENT_BINARY_DIR}/tests/vm/bytecode_embed_parallel_import.xrc)
    add_custom_command(
        OUTPUT ${XRAY_BYTECODE_EMBED_SMOKE_XRC}
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_CURRENT_BINARY_DIR}/tests/vm
        COMMAND $<TARGET_FILE:xray_stdlib_bcgen> compile
                ${XRAY_BYTECODE_EMBED_SMOKE_SRC}
                --output ${XRAY_BYTECODE_EMBED_SMOKE_XRC}
                --format bytecode
                --strip-debug
                --strip-source
        DEPENDS
            xray_stdlib_bcgen
            gen-stdlib-embedded-bytecodes
            ${XRAY_BYTECODE_EMBED_SMOKE_SRC}
        COMMENT "Generating bytecode-only embed smoke"
        VERBATIM
    )
    add_custom_command(
        OUTPUT ${XRAY_BYTECODE_EMBED_PARALLEL_XRC}
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_CURRENT_BINARY_DIR}/tests/vm
        COMMAND $<TARGET_FILE:xray_stdlib_bcgen> compile
                ${XRAY_BYTECODE_EMBED_PARALLEL_SRC}
                --output ${XRAY_BYTECODE_EMBED_PARALLEL_XRC}
                --format bytecode
                --strip-debug
                --strip-source
        DEPENDS
            xray_stdlib_bcgen
            gen-stdlib-embedded-bytecodes
            ${XRAY_BYTECODE_EMBED_PARALLEL_SRC}
        COMMENT "Generating bytecode-only parallel import smoke"
        VERBATIM
    )
    add_custom_target(bytecode_embed_xrc
        DEPENDS
            ${XRAY_BYTECODE_EMBED_SMOKE_XRC}
            ${XRAY_BYTECODE_EMBED_PARALLEL_XRC})
    add_dependencies(bytecode_embed_runner bytecode_embed_xrc)

    add_test(
        NAME bytecode_embed_runtime
        COMMAND $<TARGET_FILE:bytecode_embed_runner>
                ${XRAY_BYTECODE_EMBED_SMOKE_XRC}
                7
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(bytecode_embed_runtime PROPERTIES
        LABELS "vm;bytecode;embed;smoke"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME bytecode_embed_parallel_import
        COMMAND $<TARGET_FILE:bytecode_embed_runner>
                ${XRAY_BYTECODE_EMBED_PARALLEL_XRC}
                4
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(bytecode_embed_parallel_import PROPERTIES
        LABELS "vm;bytecode;embed;stdlib;parallel;smoke"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME bytecode_embed_symbol_gate
        COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/run_bytecode_embed_symbol_tests.sh
                $<TARGET_FILE:bytecode_embed_runner>
                $<TARGET_FILE:xray_vm_runtime>
                ${XRAY_BYTECODE_EMBED_SMOKE_XRC}
                7
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(bytecode_embed_symbol_gate PROPERTIES
        LABELS "vm;bytecode;embed;link;smoke"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME extern_layout_bytecode_cli_roundtrip
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/run_extern_layout_bytecode_roundtrip.sh
                $<TARGET_FILE:xray>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/diff/cases/semantics/ffi/extern_layout_bytecode_roundtrip.xr
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/diff/cases/semantics/ffi/_extern_layout_bytecode_lib.xr
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(extern_layout_bytecode_cli_roundtrip PROPERTIES
        LABELS "vm;bytecode;ffi;layout;cli"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME sys_thread_vm
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/run_sys_thread_vm_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(sys_thread_vm PROPERTIES
        LABELS "vm;thread;regression"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME eval_stdlib_overlay
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/run_eval_stdlib_overlay_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(eval_stdlib_overlay PROPERTIES
        LABELS "vm;stdlib;eval;regression"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME stdlib_embedded_layout
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/run_stdlib_embedded_layout_tests.sh
                $<TARGET_FILE:xray>
                ${XRAY_PYTHON}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    math(EXPR STDLIB_EMBED_TIMEOUT "120 * ${XR_TEST_TIMEOUT_SCALE}")
    set_tests_properties(stdlib_embedded_layout PROPERTIES
        LABELS "vm;aot;stdlib;embed;regression"
        PROCESSORS 1
        TIMEOUT ${STDLIB_EMBED_TIMEOUT}
    )

    # Under sanitizers the runner tolerates benign [sysmon] "worker stuck"
    # diagnostics on stderr (see run_parallel_plan_scaling_gate.sh); the strict
    # empty-stderr contract still holds for normal builds.
    if(ENABLE_SANITIZERS OR ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN OR ENABLE_MSAN)
        set(PARALLEL_PLAN_ENV ${CMAKE_COMMAND} -E env XRAY_SANITIZER_BUILD=1)
    else()
        set(PARALLEL_PLAN_ENV)
    endif()
    add_test(
        NAME parallel_plan_vm_scaling_gate
        COMMAND ${PARALLEL_PLAN_ENV} bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/run_parallel_plan_scaling_gate.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    math(EXPR PARALLEL_PLAN_TIMEOUT "60 * ${XR_TEST_TIMEOUT_SCALE}")
    set_tests_properties(parallel_plan_vm_scaling_gate PROPERTIES
        LABELS "vm;parallel;plan;scaling;task-193"
        PROCESSORS 1
        TIMEOUT ${PARALLEL_PLAN_TIMEOUT}
    )

    add_test(
        NAME http_static_route_full_parse
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/run_http_static_route_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    math(EXPR HTTP_STATIC_ROUTE_TIMEOUT "90 * ${XR_TEST_TIMEOUT_SCALE}")
    set_tests_properties(http_static_route_full_parse PROPERTIES
        LABELS "vm;aot;http;stdlib;regression"
        PROCESSORS 1
        TIMEOUT ${HTTP_STATIC_ROUTE_TIMEOUT}
    )

    add_test(
        NAME stdlib_metadata_sync
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tools/stdlibgen/stdlibgen.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --check
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(stdlib_metadata_sync PROPERTIES
        LABELS "stdlib;metadata;generated"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME stdlib_analyzer_builtins_sync
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_stdlib_types.py
                --check
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(stdlib_analyzer_builtins_sync PROPERTIES
        LABELS "stdlib;metadata;generated;analyzer"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME stdlib_native_type_defs_sync
        COMMAND ${XRAY_PYTHON} ${NATIVE_TYPE_EMBEDDER}
                ${CMAKE_CURRENT_SOURCE_DIR}/stdlib/types
                --check ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/analyzer/xnative_type_defs.inc.c
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(stdlib_native_type_defs_sync PROPERTIES
        LABELS "stdlib;metadata;generated;analyzer"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME stdlib_lsp_stdlib_sync
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_stdlib_types.py
                --check
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(stdlib_lsp_stdlib_sync PROPERTIES
        LABELS "stdlib;metadata;generated;lsp"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME stdlib_def_source_gate
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_stdlib_def_source.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(stdlib_def_source_gate PROPERTIES
        LABELS "stdlib;metadata;generated"
        PROCESSORS 1
        TIMEOUT 30
    )

    foreach(_xray_stdlib_boundary_check IN ITEMS manifest semantic error-model fastpath dynamic)
        add_test(
            NAME stdlib_boundary_${_xray_stdlib_boundary_check}
            COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_stdlib_boundary.py
                    --root ${CMAKE_CURRENT_SOURCE_DIR}
                    --check ${_xray_stdlib_boundary_check}
        )
        set_tests_properties(stdlib_boundary_${_xray_stdlib_boundary_check} PROPERTIES
            LABELS "stdlib;boundary;metadata"
            PROCESSORS 1
            TIMEOUT 30
        )
    endforeach()

    add_test(
        NAME test_stdlib_boundary_manifest
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/stdlib/test_stdlib_boundary_manifest.py
    )
    set_tests_properties(test_stdlib_boundary_manifest PROPERTIES
        LABELS "stdlib;boundary;unit"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME test_binary_public_native_readiness
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/stdlib/test_binary_public_native_readiness.py
    )
    set_tests_properties(test_binary_public_native_readiness PROPERTIES
        LABELS "stdlib;binary;task-198;task-200;unit"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME test_stdlib_module_merge_gate
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/stdlib/test_stdlib_module_merge_gate.py
    )
    set_tests_properties(test_stdlib_module_merge_gate PROPERTIES
        LABELS "stdlib;boundary;merge;unit"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME stdlib_migration_contracts
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/stdlib_migration.py
                check --root ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(stdlib_migration_contracts PROPERTIES
        LABELS "stdlib;contract;migration"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME stdlib_perf_manifest
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmarks/stdlib/run.py
                --validate-only
    )
    set_tests_properties(stdlib_perf_manifest PROPERTIES
        LABELS "stdlib;performance;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME stdlib_self_hosting_consistency
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/report_stdlib_self_hosting.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --check
    )
    set_tests_properties(stdlib_self_hosting_consistency PROPERTIES
        LABELS "stdlib;boundary;report"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_custom_target(stdlib-self-hosting-complete
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/report_stdlib_self_hosting.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --require-complete
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
        COMMENT "Checking final task-196 stdlib self-hosting completion criteria"
    )

    add_custom_target(stdlib-module-merge-check
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_stdlib_module_merge.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                postmerge
                --xray $<TARGET_FILE:xray>
        DEPENDS xray
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
        COMMENT "Checking task-196 S0 integrated stdlib metadata and generated artifacts"
    )

    add_custom_target(regen-stdlib-governance
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_stdlib_module_merge.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                regenerate
                --xray $<TARGET_FILE:xray>
                --artifact-dir ${CMAKE_CURRENT_BINARY_DIR}/stdlib-governance
        DEPENDS xray
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
        COMMENT "Regenerating task-196 S0 stdlib metadata, API/MCP artifacts and report"
    )

    add_test(
        NAME stdlib_def_migration_residue
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_stdlib_def_migration_residue.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(stdlib_def_migration_residue PROPERTIES
        LABELS "stdlib;metadata;generated"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME stdlib_aot_helper_residue
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_stdlib_aot_helper_residue.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(stdlib_aot_helper_residue PROPERTIES
        LABELS "stdlib;aot;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME binary_stdlib_surface_inventory
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_binary_stdlib_surface.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(binary_stdlib_surface_inventory PROPERTIES
        LABELS "stdlib;binary;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME binary_public_native_readiness
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_binary_public_native_readiness.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(binary_public_native_readiness PROPERTIES
        LABELS "stdlib;binary;task-200;readiness"
        PROCESSORS 1
        TIMEOUT 90
    )

    add_test(
        NAME binary_stdlib_kat_baseline
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_binary_stdlib_kat_baseline.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(binary_stdlib_kat_baseline PROPERTIES
        LABELS "stdlib;binary;baseline"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME binary_stdlib_runtime_baseline
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_binary_stdlib_runtime_baseline.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(binary_stdlib_runtime_baseline PROPERTIES
        LABELS "stdlib;binary;baseline;performance"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME bytes_type_residue
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_bytes_type_residue.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --fail-on-public
                --fail-on-internal-legacy
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(bytes_type_residue PROPERTIES
        LABELS "frontend;types;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME c_interop_surface_residue
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_c_interop_surface.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --fail-on-active-removed
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(c_interop_surface_residue PROPERTIES
        LABELS "frontend;ffi;task-190"
        PROCESSORS 1
        TIMEOUT 30
    )

    # Narrowing rules (LANGUAGE_SPEC §2.13) must stay reconciled across the
    # spec section, the conformance tests, and the analyzer that implements
    # them. Narrowed types reach code generation, so a dropped rule is a
    # VM/AOT divergence risk rather than a documentation gap.
    add_test(
        NAME narrowing_rules_reconciliation
        COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_narrowing_rules.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(narrowing_rules_reconciliation PROPERTIES
        LABELS "frontend;analyzer;spec"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME narrowing_conformance
        COMMAND $<TARGET_FILE:xray> test ${CMAKE_CURRENT_SOURCE_DIR}/tests/regression/16_narrowing
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(narrowing_conformance PROPERTIES
        LABELS "frontend;analyzer;spec"
        PROCESSORS 1
        TIMEOUT 60
    )

    add_test(
        NAME query_surface_residue
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_query_surface_residue.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --fail-on-public
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(query_surface_residue PROPERTIES
        LABELS "frontend;syntax;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME string_surface_residue
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_string_surface_residue.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --fail-on-public
                --fail-on-backend-legacy
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(string_surface_residue PROPERTIES
        LABELS "frontend;syntax;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME numeric_type_spelling_residue
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_numeric_type_spelling_residue.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --fail-on-public
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(numeric_type_spelling_residue PROPERTIES
        LABELS "frontend;syntax;metadata;lsp"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME byte_width_predicate_audit
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_byte_width_predicates.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(byte_width_predicate_audit PROPERTIES
        LABELS "frontend;types;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    if(XR_BUILD_LSP AND NOT WIN32)
        add_test(
            NAME byte_u8_canonical_audit
            COMMAND ${CMAKE_COMMAND} -E env
                    XRAY_BIN=$<TARGET_FILE:xray>
                    XRAY_TEST_LSP_DOCUMENT=$<TARGET_FILE:test_lsp_document>
                    XRAY_TEST_XGLOBAL_SUMMARY=$<TARGET_FILE:test_xglobal_summary>
                    bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_byte_u8_canonical_audit.sh
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(byte_u8_canonical_audit PROPERTIES
            LABELS "frontend;types;lsp;analysis"
            PROCESSORS 1
            TIMEOUT 120
        )
    endif()

    if(NOT WIN32)
        add_test(
            NAME byte_receiver_effect_audit
            COMMAND ${CMAKE_COMMAND} -E env
                    XRAY_BIN=$<TARGET_FILE:xray>
                    bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_byte_receiver_effect_audit.sh
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(byte_receiver_effect_audit PROPERTIES
            LABELS "frontend;types;ownership"
            PROCESSORS 1
            TIMEOUT 120
        )
    endif()

    add_test(
        NAME parallel_surface_convergence
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_parallel_surface_convergence.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(parallel_surface_convergence PROPERTIES
        LABELS "parallel;syntax;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME parallel_backend_abi_convergence
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_parallel_backend_abi_convergence.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(parallel_backend_abi_convergence PROPERTIES
        LABELS "parallel;abi;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME param_mode_convergence
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_param_mode_convergence.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(param_mode_convergence PROPERTIES
        LABELS "frontend;syntax;metadata"
        PROCESSORS 1
        TIMEOUT 90
    )

    add_test(
        NAME language_surface_convergence
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_language_surface_convergence.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(language_surface_convergence PROPERTIES
        LABELS "frontend;syntax;metadata;task-231"
        PROCESSORS 1
        TIMEOUT 90
    )

    add_test(
        NAME error_effect_convergence_inventory
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_error_effect_convergence.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --max-category THROW_BIT_RECOMPUTE=0
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(error_effect_convergence_inventory PROPERTIES
        LABELS "frontend;metadata;errors"
        PROCESSORS 1
        TIMEOUT 30
    )

    # Task 218 defense line 1: cross-lifetime borrow inventory. P1 drove every
    # category to zero (CGEN_BORROWED_NAME fields are now either arena/pool/
    # static-literal owned or carry an explicit `owned:` justification), so the
    # gate is now FAIL-CLOSED: any new unannotated cross-lifetime borrow makes
    # this test fail. The baseline diff stays on for a readable regression report.
    add_test(
        NAME meta_ownership_inventory
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_meta_ownership.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --baseline ${CMAKE_CURRENT_SOURCE_DIR}/scripts/meta_ownership_baseline.json
                --max-category AST_PTR_INTO_IR=0
                --max-category PTR_ACROSS_GROWTH=0
                --max-category CGEN_BORROWED_NAME=0
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(meta_ownership_inventory PROPERTIES
        LABELS "aot;ir;metadata;memory;task-218"
        PROCESSORS 1
        TIMEOUT 30
    )

    # Task 218 defenses 2/4 are part of the ordinary, non-sanitized CTest
    # surface.  Do not register nested sanitizer drivers inside an already
    # sanitized build: CI runs those configurations directly and recursive
    # sanitizer builds would only duplicate the same work.
    if(WIN32 AND NOT ENABLE_ASAN AND NOT ENABLE_UBSAN AND NOT ENABLE_TSAN AND
       NOT ENABLE_MSAN)
        add_test(
            NAME asan_focused
            COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_asan_focused_windows.ps1
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(asan_focused PROPERTIES
            LABELS "sanitizer;asan;task-218;windows"
            RUN_SERIAL TRUE
            TIMEOUT 3600
        )
    elseif(NOT ENABLE_ASAN AND NOT ENABLE_UBSAN AND NOT ENABLE_TSAN AND
           NOT ENABLE_MSAN)
        add_test(
            NAME asan_focused
            COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_asan_focused.sh
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(asan_focused PROPERTIES
            LABELS "sanitizer;asan;ubsan;task-218"
            RUN_SERIAL TRUE
            TIMEOUT 2400
        )

        # Task 218 defense line 4: strict LeakSanitizer lane. LSan runs only on
        # Linux; the script skips cleanly elsewhere. Uses scripts/lsan.supp and
        # xr_process_shutdown() for process-global registry release.
        add_test(
            NAME lsan_strict
            COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_lsan_strict.sh
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(lsan_strict PROPERTIES
            LABELS "sanitizer;lsan;leak;task-218"
            RUN_SERIAL TRUE
            TIMEOUT 2400
        )

        # Sanitize the compiler's OUTPUT, not the compiler. asan_focused above
        # instruments xray itself and only emits C for its two real workloads;
        # nothing ran the generated C under a sanitizer. Since Xray lowers to C,
        # every UB rule applies to code no human wrote, so that gap was the one
        # place a miscompile could hide indefinitely. The lane skips cleanly when
        # the host has no READY AOT toolchain.
        add_test(
            NAME aot_ubsan
            COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_aot_ubsan.sh
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(aot_ubsan PROPERTIES
            LABELS "sanitizer;ubsan;aot;codegen"
            RUN_SERIAL TRUE
            TIMEOUT 2400
        )
    endif()

    add_test(
        NAME source_unknown_convergence_inventory
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_source_unknown_convergence.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(source_unknown_convergence_inventory PROPERTIES
        LABELS "frontend;types;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME source_unknown_task_erased_result_readiness
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_source_unknown_convergence.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --max-category SOURCE_UNKNOWN_TYPE_SURFACE=1
                --max-category TASK_ERASED_RESULT_RESIDUE=2
                --max-category STDLIB_DYNAMIC_UNKNOWN_API=1
                # Native enum namespace constants use one internal tagged
                # metaobject carrier; this is not a source-level unknown API.
                --max-category TYPE_ANY_OR_DYNAMIC_SLOT_FALLBACK=326
                --max-category AOT_UNKNOWN_ERASURE_CONSUMER=77
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(source_unknown_task_erased_result_readiness PROPERTIES
        LABELS "frontend;types;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    # Task 202: categories that have fully converged to zero are locked here so
    # any regression (a formatter/LSP unknown surface or a public-spec unknown
    # type re-appearing) fails CI. These are permanent residue gates, not the
    # still-shrinking inventory floors tracked by the readiness gate above.
    add_test(
        NAME source_unknown_converged_zero_categories
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_source_unknown_convergence.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --max-category FORMATTER_LSP_UNKNOWN_SURFACE=0
                --max-category PUBLIC_SPEC_UNKNOWN_RESIDUE=0
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(source_unknown_converged_zero_categories PROPERTIES
        LABELS "frontend;types;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME source_unknown_inventory_classification
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/frontend/test_source_unknown_inventory.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(source_unknown_inventory_classification PROPERTIES
        LABELS "frontend;types;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME source_unknown_aot_baseline
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_source_unknown_aot_baseline.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(source_unknown_aot_baseline PROPERTIES
        LABELS "aot;types;metadata"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME api_inventory_docs_coverage
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_api_inventory.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --xray $<TARGET_FILE:xray>
                --check-docs
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(api_inventory_docs_coverage PROPERTIES
        LABELS "api;stdlib;metadata;docs"
        PROCESSORS 1
        TIMEOUT 30
    )

    # Sanitizer builds cannot produce the freestanding -nostdlib/-ffreestanding
    # link shape (the sanitizer runtime is injected instead), so the runner skips
    # only the freestanding flag-asserting filetests when this env marks the
    # build as sanitized. All other filetests still run.
    set(AOT_FILETEST_ENV XRAY_AOT_FILETEST_JOBS=${XRAY_CTEST_AOT_FILETEST_JOBS})
    if(ENABLE_SANITIZERS OR ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN OR ENABLE_MSAN)
        list(APPEND AOT_FILETEST_ENV XRAY_AOT_FILETEST_SANITIZER=1)
    endif()
    add_test(
        NAME aot_filetests
        COMMAND ${CMAKE_COMMAND} -E env
                ${AOT_FILETEST_ENV}
                ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_filetests.sh
                --xray $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    math(EXPR AOT_FILETESTS_TIMEOUT "120 * ${XR_TEST_TIMEOUT_SCALE}")
    set_tests_properties(aot_filetests PROPERTIES
        LABELS "aot;filetests"
        PROCESSORS ${XRAY_CTEST_AOT_FILETEST_JOBS}
        TIMEOUT ${AOT_FILETESTS_TIMEOUT}
    )

    # Sanitizer instrumentation deliberately inserts calls and conditional
    # branches, so it cannot satisfy this optimized assembly-shape gate.
    if(NOT ENABLE_ASAN AND NOT ENABLE_UBSAN AND NOT ENABLE_TSAN AND
       NOT ENABLE_MSAN AND NOT ENABLE_SANITIZERS)
        add_test(
            NAME raw_scalar_assembly_shape
        COMMAND ${XRAY_TEST_BASH}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/check_raw_scalar_assembly.sh
                    $<TARGET_FILE:xray>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(raw_scalar_assembly_shape PROPERTIES
            LABELS "aot;assembly;ffi;task-207"
            PROCESSORS 2
            SKIP_RETURN_CODE 77
            TIMEOUT 60
        )
    endif()

    add_test(
        NAME aot_cross_smoke
        COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_cross_smoke.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_cross_smoke PROPERTIES
        LABELS "aot;cross;smoke"
        PROCESSORS 4
        SKIP_RETURN_CODE 77
        TIMEOUT 120
    )

    add_test(
        NAME aot_bundled_zig_smoke
        COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_bundled_zig_smoke.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_bundled_zig_smoke PROPERTIES
        LABELS "aot;cross;toolchain;smoke"
        PROCESSORS 1
        SKIP_RETURN_CODE 77
        TIMEOUT 60
    )

    add_test(
        NAME aot_native_debug_smoke
        COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_native_debug_smoke.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_native_debug_smoke PROPERTIES
        LABELS "aot;debug;smoke"
        SKIP_RETURN_CODE 77
        TIMEOUT 120
    )

    if(XR_BUILD_DAP)
        add_test(
            NAME dap_native_smoke
            COMMAND ${CMAKE_COMMAND} -E env
                    XRAY_BUILD_DIR=$<TARGET_FILE_DIR:xray>
                    ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_dap_native_tests.sh
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(dap_native_smoke PROPERTIES
            LABELS "dap;native;debug;smoke"
            SKIP_RETURN_CODE 77
            TIMEOUT 120
        )
    endif()

    # Per-module incremental object cache: only modules whose generated C
    # changed are recompiled; dependents hit the cache and relink (114).
    add_test(
        NAME aot_incremental_cache
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY_AOT_INCREMENTAL_JOBS=${XRAY_CTEST_AOT_INCREMENTAL_JOBS}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_incremental_cache.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_incremental_cache PROPERTIES
        LABELS "aot;cache;smoke"
        PROCESSORS ${XRAY_CTEST_AOT_INCREMENTAL_JOBS}
        SKIP_RETURN_CODE 77
        TIMEOUT 120
    )

    add_test(
        NAME global_evidence_cache_package_benchmark
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY_GLOBAL_EVIDENCE_BENCH_CASES=package
                XRAY_PACKAGE_PAYLOAD_FIXTURE=$<TARGET_FILE:xray_package_payload_fixture>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmarks/compiler/global_evidence_cache/run_benchmark.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(global_evidence_cache_package_benchmark PROPERTIES
        LABELS "aot;cache;benchmark"
        PROCESSORS 2
        TIMEOUT 120
    )

    # The benchmark always enforces build/run/output-parity correctness; --gate
    # additionally fails on the perf ratio and strict codegen-shape audit.
    # Sanitizer instrumentation makes the AOT binary 5-9x slower (observed ratio
    # ~0.04 vs the 0.90 target) and deliberately alters codegen shape, so the
    # ratio/audit gate is meaningless there. Drop --gate under sanitizers to keep
    # the correctness checks without the (impossible) perf gate; normal builds
    # keep the full gate as a real perf guard.
    # This gate measures millisecond-scale binaries, so three process-launch
    # samples are too sensitive to scheduler and loader jitter. Keep the same
    # alternating C/AOT median protocol, but use enough samples for a stable
    # comparison without materially extending the suite.
    set(BYTE_ARRAY_C90_ARGS --samples 11)
    if(NOT (ENABLE_SANITIZERS OR ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN OR ENABLE_MSAN))
        list(APPEND BYTE_ARRAY_C90_ARGS --gate)
    endif()
    list(APPEND BYTE_ARRAY_C90_ARGS
         --bench byte_array_u8_index_fill_sum,byte_array_slice_bulk_hotpath)
    add_test(
        NAME byte_array_aot_c90_hotpath
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY_BIN=$<TARGET_FILE:xray>
                ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_aot_c90_benchmarks.sh
                ${BYTE_ARRAY_C90_ARGS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    math(EXPR BYTE_ARRAY_C90_TIMEOUT "120 * ${XR_TEST_TIMEOUT_SCALE}")
    set_tests_properties(byte_array_aot_c90_hotpath PROPERTIES
        LABELS "aot;benchmark;types"
        PROCESSORS 2
        TIMEOUT ${BYTE_ARRAY_C90_TIMEOUT}
    )

    # Zero-cost feature inventory: every tracked family must either have a
    # current plan/filetest pattern or an explicit gap row.
    add_test(
        NAME zero_cost_plan_inventory
        COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmarks/aot/zero_cost/plan_inventory.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(zero_cost_plan_inventory PROPERTIES
        LABELS "aot;benchmark;plan"
        TIMEOUT 20
    )

    add_test(
        NAME zero_cost_contract_inventory
        COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/benchmarks/aot/zero_cost/contracts_inventory.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(zero_cost_contract_inventory PROPERTIES
        LABELS "aot;benchmark;plan;evidence"
        TIMEOUT 20
    )

    # Link manifest contract: freestanding direct calls and runtime-backed
    # stdlib modules must not drag in the full xray_core library.
    # Under sanitizers the freestanding -nostdlib probes inevitably carry
    # __asan_*/__ubsan_* undefined symbols (the sanitizer runtime is injected),
    # so the runner filters only those sanitizer symbols from the undefined-symbol
    # audit; all non-freestanding link-contract checks still run strictly.
    if(ENABLE_SANITIZERS OR ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN OR ENABLE_MSAN)
        set(AOT_LINK_MANIFEST_CMD ${CMAKE_COMMAND} -E env XRAY_LINK_TEST_SANITIZER=1)
    else()
        set(AOT_LINK_MANIFEST_CMD)
    endif()
    add_test(
        NAME aot_link_command_manifest
        COMMAND ${AOT_LINK_MANIFEST_CMD}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_link_command_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    math(EXPR AOT_LINK_MANIFEST_TIMEOUT "120 * ${XR_TEST_TIMEOUT_SCALE}")
    set_tests_properties(aot_link_command_manifest PROPERTIES
        LABELS "aot;link;stdlib;smoke"
        PROCESSORS 2
        TIMEOUT ${AOT_LINK_MANIFEST_TIMEOUT}
    )

    # Freestanding stdlib allowlist: every committed module contract must have
    # a no-libc object probe, so the allowlist cannot silently drift.
    # Sanitizer builds link the sanitizer runtime into every object and cannot
    # honor the freestanding -nostdlib contract, so the probe's undefined-symbol
    # surface is polluted with __asan_*/__ubsan_* symbols. This is an inherent
    # freestanding-vs-sanitizer conflict, not a product defect, so skip the gate
    # under sanitizers (same rationale as raw_scalar_assembly_shape above).
    if(NOT ENABLE_ASAN AND NOT ENABLE_UBSAN AND NOT ENABLE_TSAN AND
       NOT ENABLE_MSAN AND NOT ENABLE_SANITIZERS)
        add_test(
            NAME aot_freestanding_allowlist
            COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_freestanding_allowlist_tests.sh
                    $<TARGET_FILE:xray>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(aot_freestanding_allowlist PROPERTIES
            LABELS "aot;freestanding;stdlib;smoke"
            PROCESSORS 1
            TIMEOUT 60
        )
    endif()

    add_test(
        NAME aot_freestanding_provider_abi
        COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_freestanding_provider_abi_test.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_freestanding_provider_abi PROPERTIES
        LABELS "aot;freestanding;provider;smoke"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME aot_freestanding_static_fixed_array_mutation
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_freestanding_static_fixed_array_mutation.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_freestanding_static_fixed_array_mutation PROPERTIES
        LABELS "aot;freestanding;static-data;smoke"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME aot_freestanding_numeric_core_compile
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_freestanding_numeric_core_compile.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_freestanding_numeric_core_compile PROPERTIES
        LABELS "aot;freestanding;toolchain;smoke"
        PROCESSORS 1
        SKIP_RETURN_CODE 77
        TIMEOUT 90
    )

    add_test(
        NAME aot_freestanding_qemu_smoke
        COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_freestanding_qemu_smoke.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_freestanding_qemu_smoke PROPERTIES
        LABELS "aot;freestanding;qemu;smoke"
        PROCESSORS 1
        SKIP_RETURN_CODE 77
        TIMEOUT 30
    )

    add_test(
        NAME aot_freestanding_riscv_qemu_smoke
        COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_freestanding_riscv_qemu_smoke.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_freestanding_riscv_qemu_smoke PROPERTIES
        LABELS "aot;freestanding;qemu;riscv;smoke"
        PROCESSORS 1
        SKIP_RETURN_CODE 77
        TIMEOUT 30
    )

    add_test(
        NAME aot_freestanding_thumb_qemu_smoke
        COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_freestanding_thumb_qemu_smoke.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_freestanding_thumb_qemu_smoke PROPERTIES
        LABELS "aot;freestanding;qemu;thumb;smoke"
        PROCESSORS 1
        SKIP_RETURN_CODE 77
        TIMEOUT 30
    )

    # Broad AOT manifest sweep: non-runtime diff cases must remain freestanding
    # and must not accidentally pull xray_rt_coro/xray_core.
    add_test(
        NAME aot_manifest_sweep
        COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_manifest_sweep_tests.py
                --xray $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    math(EXPR AOT_MANIFEST_SWEEP_TIMEOUT "180 * ${XR_TEST_TIMEOUT_SCALE}")
    set_tests_properties(aot_manifest_sweep PROPERTIES
        LABELS "aot;link;manifest;smoke"
        PROCESSORS 1
        TIMEOUT ${AOT_MANIFEST_SWEEP_TIMEOUT}
    )

    # Isolate/runtime split guard (132/M0 + 133/I0/I4): AOT binaries must stay
    # free of xray_core and public isolate/VM/compiler/module-loader symbols.
    # Sanitizer instrumentation inflates binary size several-fold, so the hard
    # size caps are meaningless under sanitizers; the runner then skips only the
    # size assertions while keeping the symbol-isolation checks (the real guard).
    set(AOT_ISOLATE_ENV XRAY_AOT_ISOLATE_JOBS=${XRAY_CTEST_AOT_ISOLATE_JOBS})
    if(ENABLE_SANITIZERS OR ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN OR ENABLE_MSAN)
        list(APPEND AOT_ISOLATE_ENV XRAY_ISOLATE_TEST_SANITIZER=1)
    endif()
    add_test(
        NAME aot_isolate_symbol_gate
        COMMAND ${CMAKE_COMMAND} -E env
                ${AOT_ISOLATE_ENV}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_isolate_symbol_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    math(EXPR AOT_ISOLATE_TIMEOUT "180 * ${XR_TEST_TIMEOUT_SCALE}")
    set_tests_properties(aot_isolate_symbol_gate PROPERTIES
        LABELS "aot;link;isolate;smoke"
        PROCESSORS ${XRAY_CTEST_AOT_ISOLATE_JOBS}
        TIMEOUT ${AOT_ISOLATE_TIMEOUT}
    )

    # Public install surface: xray_core is an internal development aggregate,
    # not a library users or installed AOT builds should link against.
    if(WIN32)
        add_test(
            NAME install_public_surface
            COMMAND ${XRAY_PYTHON}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/install/run_install_public_surface_tests.py
                    --project-root ${CMAKE_CURRENT_SOURCE_DIR}
                    --build-dir ${CMAKE_BINARY_DIR}
                    --binary $<TARGET_FILE:xray>
                    --config $<CONFIG>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    else()
        add_test(
            NAME install_public_surface
            COMMAND ${XRAY_TEST_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/install/run_install_public_surface_tests.sh
                    ${CMAKE_BINARY_DIR}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endif()
    set_tests_properties(install_public_surface PROPERTIES
        LABELS "install;aot;link;smoke"
        PROCESSORS 2
        TIMEOUT 120
    )

    # FFI runtime/link diagnostics: missing libraries and symbols must fail
    # with stable, user-facing messages instead of cascading or opaque errors.
    add_test(
        NAME ffi_diagnostics
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/run_ffi_diagnostics.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(ffi_diagnostics PROPERTIES
        LABELS "ffi;aot;vm;negative;regression"
        PROCESSORS 2
        TIMEOUT 60
    )

    # Native layout oracle: extern structs/unions must agree exactly with the
    # host C compiler for sizeof, alignof, and offsetof.
    add_test(
        NAME ffi_extern_layout_probe
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/run_extern_layout_probe.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(ffi_extern_layout_probe PROPERTIES
        LABELS "ffi;layout;aot;vm;regression"
        PROCESSORS 2
        TIMEOUT 60
    )

    # FFI ASan matrix: ASan builds must exercise raw pointers, CFn callbacks,
    # and headerless repr(C) structs through VM/AOT; normal builds skip.
    add_test(
        NAME ffi_asan_smoke
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/run_ffi_asan_smoke.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(ffi_asan_smoke PROPERTIES
        LABELS "ffi;aot;vm;asan;regression"
        SKIP_RETURN_CODE 77
        TIMEOUT 90
    )

    # Manifest export smoke: generated public C ABI symbols must be linkable
    # from an external C translation unit and callable with scalar values.
    add_test(
        NAME ffi_c_export_smoke
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/run_c_export_smoke.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(ffi_c_export_smoke PROPERTIES
        LABELS "ffi;aot;regression"
        PROCESSORS 2
        TIMEOUT 60
    )

    add_test(
        NAME native_output_boundary
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_native_output_boundary_tests.sh
                $<TARGET_FILE:xray>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/native_output
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(native_output_boundary PROPERTIES
        LABELS "ffi;aot;native;regression"
        PROCESSORS 2
        TIMEOUT 60
    )

    # Negative frontend/semantic cases: programs that must be rejected.
    # Keeps ownership/in/ref diagnostics in the default CTest gate.
    add_test(
        NAME compile_error_tests
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY=$<TARGET_FILE:xray>
                bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_errors/run_compile_error_tests.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    math(EXPR COMPILE_ERROR_TESTS_TIMEOUT "60 * ${XR_TEST_TIMEOUT_SCALE}")
    set_tests_properties(compile_error_tests PROPERTIES
        LABELS "frontend;negative;regression"
        PROCESSORS 2
        TIMEOUT ${COMPILE_ERROR_TESTS_TIMEOUT}
    )

    add_test(
        NAME verify_contract_cli
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/run_verify_contract_tests.sh
                $<TARGET_FILE:xray>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/verify_contract
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(verify_contract_cli PROPERTIES
        LABELS "cli;effects;frontend;regression"
        TIMEOUT 60
    )

    # Spec 8.1.1 uncaught value-return error diagnostic. A content gate: the
    # dropped-`go` shape exits 0 on both backends, so the VM/AOT differential
    # agrees even when one of them prints nothing.
    add_test(
        NAME uncaught_error_diagnostics
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/run_uncaught_error_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    math(EXPR UNCAUGHT_ERROR_TESTS_TIMEOUT "120 * ${XR_TEST_TIMEOUT_SCALE}")
    set_tests_properties(uncaught_error_diagnostics PROPERTIES
        LABELS "cli;vm;aot;regression"
        TIMEOUT ${UNCAUGHT_ERROR_TESTS_TIMEOUT}
    )

    # Panic channel counterpart: pins the shared "[Uncaught Panic] E<code>: ..."
    # report and its two parity policies (opt-in XRAY_BACKTRACE trace, TTY-gated
    # colour). Same blind spot as above — the stderr-off differential net cannot
    # catch a wording regression that hits both backends.
    add_test(
        NAME panic_report_diagnostics
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli/run_panic_report_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    math(EXPR PANIC_REPORT_TESTS_TIMEOUT "120 * ${XR_TEST_TIMEOUT_SCALE}")
    set_tests_properties(panic_report_diagnostics PROPERTIES
        LABELS "cli;vm;aot;regression"
        TIMEOUT ${PANIC_REPORT_TESTS_TIMEOUT}
    )

    # Borrow/ownership instrumentation: keep a machine-parseable Xi
    # retain/release budget available for RC-overhead regression work.
    add_test(
        NAME xi_rc_count_smoke
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/run_rc_count_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(xi_rc_count_smoke PROPERTIES
        LABELS "ir;ownership;regression"
        TIMEOUT 30
    )

    # Shared binding copy lowering: copy(x) into a shared slot must lower
    # through OP_TO_SHARED in the VM instead of COPY + SETSHARED.
    add_test(
        NAME xi_shared_copy_smoke
        COMMAND ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/run_shared_copy_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(xi_shared_copy_smoke PROPERTIES
        LABELS "ir;ownership;regression"
        TIMEOUT 30
    )

    # Cross-backend differential net: same .xr through VM / AOT must
    # produce byte-identical observable output. Safety net for the backend
    # unification work (tasks 108-118).
    add_test(
        NAME backend_diff
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY_DIFF_MAX_AUTO_JOBS=${XRAY_CTEST_DIFF_JOBS}
                XRAY_DIFF_COLD_MAX_AUTO_JOBS=4
                XRAY_DIFF_HOT_MAX_AUTO_JOBS=8
                ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/diff/run_backend_diff.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(backend_diff PROPERTIES
        LABELS "diff;cross;backend"
        PROCESSORS ${XRAY_CTEST_DIFF_PROCESSORS}
        TIMEOUT 900
    )

    foreach(_task190_diff
            extern:tests/diff/task190_extern_cases.txt
            layout:tests/diff/task190_layout_cases.txt
            mem:tests/diff/task190_mem_cases.txt)
        string(REPLACE ":" ";" _task190_diff_parts "${_task190_diff}")
        list(GET _task190_diff_parts 0 _task190_diff_name)
        list(GET _task190_diff_parts 1 _task190_diff_manifest)
        add_test(
            NAME task190_${_task190_diff_name}_backend_diff
            COMMAND ${CMAKE_COMMAND} -E env
                    XRAY_DIFF_CASES_FILE=${CMAKE_CURRENT_SOURCE_DIR}/${_task190_diff_manifest}
                    XRAY_DIFF_EXTRA_CASES_FILE=
                    XRAY_DIFF_MAX_AUTO_JOBS=4
                    ${XRAY_TEST_BASH}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/diff/run_backend_diff.sh
                    $<TARGET_FILE:xray>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
        set_tests_properties(task190_${_task190_diff_name}_backend_diff PROPERTIES
            LABELS "diff;cross;backend;ffi;task-190"
            PROCESSORS 2
            TIMEOUT 180
        )
    endforeach()

    # Reordering regressions are invisible at the default -O0 differential
    # level: LICM, GVN-PRE and the rest of the Xi middle end only run from
    # XI_OPT_FULL (-O2). These cases exist to catch code motion, so they get
    # their own lane at the level where code motion happens.
    add_test(
        NAME backend_diff_optimized
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY_DIFF_CASES_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/diff/optimizer_cases.txt
                XRAY_DIFF_EXTRA_CASES_FILE=
                XRAY_AOT_TEST_OPT=2
                XRAY_DIFF_MAX_AUTO_JOBS=4
                ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/diff/run_backend_diff.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(backend_diff_optimized PROPERTIES
        LABELS "diff;cross;backend;optimizer;memory-model"
        PROCESSORS 2
        TIMEOUT 600
    )

    # Memory-model litmus tests (spec §16.9). Same reasoning as above: the VM
    # does not reorder, so the AOT -O2 lane is the one under test and the VM
    # lane is the control.
    add_test(
        NAME concurrency_litmus
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY_LITMUS_OPT=2
                ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/concurrency_litmus/run_litmus.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(concurrency_litmus PROPERTIES
        LABELS "concurrency;memory-model;litmus"
        PROCESSORS 4
        TIMEOUT 1800
    )

    add_test(
        NAME backend_diff_deterministic
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY_CORO_DETERMINISTIC=1
                XRAY_CORO_SEED=12345
                XRAY_TEST_CACHE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/.cache/xray-test/deterministic
                XRAY_DIFF_CASES_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/diff/deterministic_cases.txt
                XRAY_DIFF_JOBS=${XRAY_CTEST_DETERMINISTIC_DIFF_JOBS}
                ${XRAY_TEST_BASH}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/diff/run_backend_diff.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(backend_diff_deterministic PROPERTIES
        LABELS "diff;cross;backend;coro;deterministic"
        PROCESSORS ${XRAY_CTEST_DETERMINISTIC_DIFF_JOBS}
        TIMEOUT 900
    )

    add_test(
        NAME regression_rc_closure_self_capture
        COMMAND $<TARGET_FILE:xray> test
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/regression/08_mem/0803_closure_self_capture_rc.xr
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(regression_rc_closure_self_capture PROPERTIES
        LABELS "regression;rc;vm"
        TIMEOUT 30
    )

    # ========================================================================
    # Task 220 — Verification asset governance gates (appended registrations)
    # Design: xray-docs/tasks/220-semantic-contract-freeze-and-verification-
    # asset-governance-cn.md. This section only registers tests; it adds no
    # build targets.
    # ========================================================================

    # P1: independent AOT VM-vs-native suite adopted into CTest with a tombstone
    # gate. Runs every tests/aot/{basic,modules,coro,negative} case and treats
    # EXACTLY the cases in tests/aot/TOMBSTONES.tsv as expected failures; any
    # other failure fails the gate (fail-closed). Allowed to be slower than the
    # main gate.
    add_test(
        NAME aot_standalone_suite
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY_AOT_TOMBSTONE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/TOMBSTONES.tsv
                XRAY_AOT_TOMBSTONE_MAX=10
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_standalone_suite PROPERTIES
        LABELS "aot;standalone;tombstone;task-220"
        PROCESSORS 4
        TIMEOUT 600
    )

    # P2: removed-surface-form drift scan. Fails closed on any live occurrence
    # of a deleted Xray surface form (`let` bindings, mem.free(), unchecked LE
    # load/store, direct Buffer indexing) across tests/, stdlib/, and knowledge
    # docs. Negative fixtures and fuzz corpus are excluded by the scanner.
    add_test(
        NAME surface_drift_scan
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_surface_drift.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --fail-on-hit
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(surface_drift_scan PROPERTIES
        LABELS "meta;surface;drift;task-220"
        PROCESSORS 1
        TIMEOUT 60
    )

    # Built-in symbol registry. stdlib/prelude/builtin_symbols.def is the single
    # source of truth for every name available without an import; this gate keeps
    # the generated specification registry, the specification's normative prose,
    # and the per-symbol probes in tests/builtin_probes locked to it. The probes
    # run both `xray check` and `xray run` because a symbol can pass analysis and
    # still fail in the post-monomorphization pass.
    add_test(
        NAME builtin_symbol_registry
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_builtin_symbol_registry.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
                --xray $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(builtin_symbol_registry PROPERTIES
        LABELS "meta;surface;prelude"
        PROCESSORS 1
        TIMEOUT 300
    )

    # P0: semantic contract anchor digests and CONTRACT-CHANGE trailers. The
    # self-test injects both an anchor drift and a missing trailer to prove the
    # gate fails closed independently of the live repository state.
    add_test(
        NAME contract_freeze
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_contract_freeze.py
                --root ${CMAKE_CURRENT_SOURCE_DIR}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(contract_freeze PROPERTIES
        LABELS "meta;contracts;freeze;task-220"
        PROCESSORS 1
        TIMEOUT 30
    )

    add_test(
        NAME contract_freeze_injection
        COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check_contract_freeze.py
                --self-test
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(contract_freeze_injection PROPERTIES
        LABELS "meta;contracts;freeze;task-220"
        PROCESSORS 1
        TIMEOUT 30
    )
endif()

# Fuzzing tests (optional, requires Clang with libFuzzer)
if(ENABLE_FUZZING)
    add_subdirectory(tests/fuzz)
endif()

# ============================================================================
# Build Information Output
# ============================================================================

message(STATUS "========================================")
message(STATUS "Xray Build Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  Build Tests: ${BUILD_TESTS}")
message(STATUS "  Bundle Zig: ${XRAY_BUNDLE_ZIG}")
message(STATUS "  Sanitizers:")
message(STATUS "    ASan:  ${ENABLE_ASAN}")
message(STATUS "    UBSan: ${ENABLE_UBSAN}")
message(STATUS "    TSan:  ${ENABLE_TSAN}")
message(STATUS "    MSan:  ${ENABLE_MSAN}")
message(STATUS "    Combined: ${ENABLE_SANITIZERS}")
message(STATUS "========================================")

# ============================================================================
# clang-format Integration
# ============================================================================

find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    # Collect all source files
    file(GLOB_RECURSE ALL_SOURCE_FILES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/stdlib/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/stdlib/*.h
    )

    # Format target: make format
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${ALL_SOURCE_FILES}
        COMMENT "Formatting all source files..."
        VERBATIM
    )

    # Check target: make format-check
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT} --dry-run --Werror ${ALL_SOURCE_FILES}
        COMMENT "Checking code format..."
        VERBATIM
    )

    message(STATUS "clang-format: ${CLANG_FORMAT}")
    message(STATUS "  make format       - Format all source files")
    message(STATUS "  make format-check - Check code format")
else()
    message(STATUS "clang-format: not found (optional)")
endif()

# ============================================================================
# clang-tidy Integration (Static Analysis)
# ============================================================================

find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY)
    # Core source files for static analysis (critical modules)
    file(GLOB_RECURSE CORE_ANALYSIS_FILES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/mem/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/value/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/object/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/analyzer/*.c
    )
    # *.inc.c files are dispatch case bodies, analyzed transitively
    # through xvm.c — running clang-tidy on them standalone breaks
    # because they reference locals defined inside the switch.
    list(FILTER CORE_ANALYSIS_FILES EXCLUDE REGEX "\\.inc\\.c$")

    # Full analysis target: make tidy (analyze core modules)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} ${CORE_ANALYSIS_FILES}
        COMMENT "Running clang-tidy on core modules..."
        VERBATIM
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    # Quick check target: make tidy-check (errors only, no warnings)
    add_custom_target(tidy-check
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR}
            --warnings-as-errors='bugprone-*,clang-analyzer-core.*'
            ${CORE_ANALYSIS_FILES}
        COMMENT "Running clang-tidy check (errors only)..."
        VERBATIM
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    message(STATUS "clang-tidy: ${CLANG_TIDY}")
    message(STATUS "  make tidy         - Run static analysis (all warnings)")
    message(STATUS "  make tidy-check   - Run static analysis (errors only)")
    message(STATUS "  Note: Run cmake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON for best results")
else()
    message(STATUS "clang-tidy: not found (optional)")
endif()

# ========== Stdlib Script Source / Bytecode Embedding ==========
# Canonical stdlib .xr files are embedded twice:
#   1. source/bootstrap metadata for early module loading;
#   2. final bytecode generated by xray_stdlib_bcgen for bytecode-only import.
# Native loaders may still anchor modules and set module->requires_script=true.

# ============================================================================
# Install Configuration
# ============================================================================

# Install path
set(XRAY_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" CACHE PATH "Xray install path")

set(_XRAY_INSTALL_METADATA_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/install")
file(MAKE_DIRECTORY "${_XRAY_INSTALL_METADATA_DIR}")
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/install-marker.json.in"
    "${_XRAY_INSTALL_METADATA_DIR}/install-marker.json"
    @ONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/GeneratePayloadManifest.cmake.in"
    "${_XRAY_INSTALL_METADATA_DIR}/GeneratePayloadManifest.cmake"
    @ONLY
)
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/GenerateRuntimeManifest.cmake.in"
    "${_XRAY_INSTALL_METADATA_DIR}/GenerateRuntimeManifest.cmake"
    @ONLY
)

# Install executable
install(TARGETS xray
    RUNTIME DESTINATION bin
    COMPONENT XrayCore
)

# Install public AOT runtime libraries. xray_core remains a build-tree-only
# aggregate for the CLI and tests; it must not become a public install target.
install(TARGETS xray_vm_runtime
    ARCHIVE DESTINATION lib/xray/vm/${XRAY_HOST_TARGET_ABI}
    COMPONENT XrayCore
)
install(TARGETS xray_aot_core xray_rt_coro
    ARCHIVE DESTINATION lib/xray/aot/${XRAY_HOST_TARGET_ABI}
    COMPONENT XraySDK
)

install(SCRIPT "${_XRAY_INSTALL_METADATA_DIR}/GenerateRuntimeManifest.cmake"
    ALL_COMPONENTS
)

# Install the public C API headers.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
    DESTINATION include/xray
    COMPONENT XraySDK
    FILES_MATCHING PATTERN "*.h"
)

# Generated AOT C includes xrt headers whose relative includes reach into the
# shared runtime tree.  Keep that tree compiler-private and preserve its source
# layout so an installed xray can compile without a source checkout.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/
    DESTINATION lib/xray/sdk/src
    COMPONENT XraySDK
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.inc.c"
        PATTERN "*.def"
)

# Install stdlib source files
install(DIRECTORY ${CMAKE_SOURCE_DIR}/stdlib/
    DESTINATION lib/xray/stdlib
    COMPONENT XrayCore
    FILES_MATCHING PATTERN "*.xr"
)

install(FILES
    ${CMAKE_SOURCE_DIR}/README.md
    ${CMAKE_SOURCE_DIR}/LICENSE
    DESTINATION .
    COMPONENT XrayCore
)

install(FILES "${_XRAY_INSTALL_METADATA_DIR}/install-marker.json"
    DESTINATION share/xray/install
    COMPONENT XrayCore
)

if(XRAY_BUNDLE_ZIG)
    set(_XRAY_ZIG_TO_INSTALL "${XRAY_BUNDLED_ZIG}")
    if(NOT _XRAY_ZIG_TO_INSTALL)
        find_program(_XRAY_ZIG_TO_INSTALL NAMES zig)
    endif()
    if(NOT _XRAY_ZIG_TO_INSTALL OR NOT EXISTS "${_XRAY_ZIG_TO_INSTALL}")
        message(FATAL_ERROR
            "XRAY_BUNDLE_ZIG=ON requires XRAY_BUNDLED_ZIG=/path/to/zig or zig in PATH")
    endif()
    get_filename_component(_XRAY_ZIG_REALPATH "${_XRAY_ZIG_TO_INSTALL}" REALPATH)
    get_filename_component(_XRAY_ZIG_ROOT "${_XRAY_ZIG_REALPATH}" DIRECTORY)
    if(EXISTS "${_XRAY_ZIG_ROOT}/lib")
        install(DIRECTORY "${_XRAY_ZIG_ROOT}/"
            DESTINATION libexec/xray/zig
            USE_SOURCE_PERMISSIONS
            COMPONENT XrayBundledZig
        )
    else()
        message(WARNING
            "Bundled Zig root '${_XRAY_ZIG_ROOT}' has no lib directory; installing executable only")
        install(PROGRAMS "${_XRAY_ZIG_REALPATH}"
            DESTINATION libexec/xray/zig
            RENAME "zig${CMAKE_EXECUTABLE_SUFFIX}"
            COMPONENT XrayBundledZig
        )
    endif()
endif()

# Run after component payload rules.  ALL_COMPONENTS also gives a Core-only or
# SDK-only staging install an exact manifest of what is actually present.
install(SCRIPT "${_XRAY_INSTALL_METADATA_DIR}/GeneratePayloadManifest.cmake"
    ALL_COMPONENTS
)

# ============================================================================
# Packaging Configuration (CPack)
# ============================================================================

set(CPACK_PACKAGE_NAME "xray")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "Xray Language")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Xray Programming Language")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://xray-lang.org")
set(CPACK_COMPONENTS_ALL XrayCore XraySDK XrayToolchainIntegration)
if(XRAY_BUNDLE_ZIG)
    list(APPEND CPACK_COMPONENTS_ALL XrayBundledZig)
endif()
set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_ARCHIVE_COMPONENT_INSTALL OFF)

# Set package type based on system
if(APPLE)
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_SYSTEM_NAME "${XRAY_RELEASE_TARGET}")
elseif(UNIX)
    set(CPACK_GENERATOR "TGZ;DEB")
    set(CPACK_SYSTEM_NAME "${XRAY_RELEASE_TARGET}")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "xingleixu@gmail.com")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libpthread-stubs0-dev")
    set(CPACK_DEB_COMPONENT_INSTALL OFF)
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_SYSTEM_NAME "${XRAY_RELEASE_TARGET}")
endif()

set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_SYSTEM_NAME}")

include(CPack)

message(STATUS "Install configuration:")
message(STATUS "  make install      - Install to ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  make package      - Generate install package")
