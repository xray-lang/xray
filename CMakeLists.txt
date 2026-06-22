# CMakeLists.txt
# Author: xingleixu@gmail.com
# Xray Language Interpreter Build Configuration

cmake_minimum_required(VERSION 3.10)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(Xray
    VERSION 0.8.1
    DESCRIPTION "Xray Programming Language Interpreter"
    LANGUAGES C CXX ASM
)

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

option(XR_USE_GC "Enable garbage collector" ON)
option(ENABLE_LLVM_BACKEND "Enable LLVM backend" OFF)
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_ASAN "Enable AddressSanitizer memory detection" OFF)
option(ENABLE_TLS "Enable HTTPS/TLS support (requires OpenSSL)" ON)
option(XR_USE_MIMALLOC "Use mimalloc high-performance allocator" OFF)
option(XR_STDLIB_FROM_FILE "Load stdlib from file instead of embedded bytecode (for debugging)" OFF)

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
option(XR_STDLIB_CLUSTER    "Cluster module (distributed Named Channel)" OFF)
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
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
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

# Debug options
option(XR_DEBUG_REGS "Enable register bounds checking (for development)" OFF)
option(XR_DEBUG_COMPILER "Enable compiler debug assertions (for development)" OFF)
option(XR_DEBUG_REGALLOC "Enable register allocator statistics output" OFF)

# GC configuration
if(XR_USE_GC)
    add_compile_definitions(XR_USE_GC=1)
endif()

# GC stress testing (every alloc triggers gc_step, exposes barrier bugs)
option(XRAY_GC_STRESS "Enable GC stress mode (every alloc triggers GC step)" OFF)
if(XRAY_GC_STRESS)
    add_compile_definitions(XR_GC_STRESS=1)
    message(STATUS "GC stress mode: enabled")
endif()

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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/gc
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/module
    ${CMAKE_CURRENT_SOURCE_DIR}/src/aot
    ${CMAKE_CURRENT_SOURCE_DIR}/src/api
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/cli
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

file(GLOB BASE_SRC "src/base/*.c")
# Cross-platform abstraction layer. Each impl file is exclusive to
# its platform — no body guards, only the right subtree is built.
if(WIN32)
    file(GLOB OS_SRC "src/os/win/*.c")
    set(AOT_RANDOM_SRC "src/os/win/random_win.c")
    set(AOT_TIME_SRC "src/os/win/time_win.c")
else()
    file(GLOB OS_SRC "src/os/unix/*.c")
    set(AOT_RANDOM_SRC "src/os/unix/random_unix.c")
    set(AOT_TIME_SRC "src/os/unix/time_unix.c")
endif()
file(GLOB RUNTIME_TOP_SRC "src/runtime/*.c")
file(GLOB RUNTIME_CORE_SRC "src/runtime/core/*.c")
file(GLOB RUNTIME_VALUE_SRC "src/runtime/value/*.c")
file(GLOB RUNTIME_GC_SRC "src/runtime/gc/*.c")
file(GLOB RUNTIME_OBJECT_SRC "src/runtime/object/*.c")
file(GLOB RUNTIME_OBJECT_BUILTINS_SRC "src/runtime/object/builtins/*.c")
list(APPEND RUNTIME_OBJECT_SRC
    ${RUNTIME_OBJECT_BUILTINS_SRC}
    "src/runtime/object/xarray_vm.c"
    "src/runtime/object/xmap_vm.c"
)
list(REMOVE_DUPLICATES RUNTIME_OBJECT_SRC)
file(GLOB RUNTIME_CLASS_SRC "src/runtime/class/*.c")
list(APPEND RUNTIME_CLASS_SRC
    "src/runtime/class/xinstance_core.c"
    "src/runtime/class/xinstance_dynamic.c"
    "src/runtime/class/xinstance_destroy.c"
)
list(REMOVE_DUPLICATES RUNTIME_CLASS_SRC)
file(GLOB RUNTIME_CLOSURE_SRC "src/runtime/closure/*.c")
file(GLOB RUNTIME_SYMBOL_SRC "src/runtime/symbol/*.c")
file(GLOB CORO_SRC "src/coro/*.c")
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
file(GLOB IO_SRC "src/io/*.c")

# Stackful coroutine context switch removed — fully stackless now
file(GLOB FRONTEND_LEXER_SRC "src/frontend/lexer/*.c")
file(GLOB FRONTEND_PARSER_SRC "src/frontend/parser/*.c")
file(GLOB FRONTEND_CODEGEN_SRC "src/frontend/codegen/*.c")
file(GLOB FRONTEND_ANALYZER_SRC "src/frontend/analyzer/*.c")
file(GLOB FRONTEND_CANONICAL_SRC "src/frontend/canonical/*.c")
file(GLOB FRONTEND_FORMAT_SRC "src/frontend/format/*.c")
file(GLOB VM_SRC "src/vm/*.c")
file(GLOB IR_SRC "src/ir/*.c")
file(GLOB MODULE_SRC "src/module/*.c")
file(GLOB AOT_SRC "src/aot/*.c")
file(GLOB API_SRC "src/api/*.c")
file(GLOB NEW_CLI_SRC CONFIGURE_DEPENDS "src/app/cli/*.c")

# Standard Library (Native modules) - modular conditional compilation
# Core modules (required): prelude (built-in type registry), time, math, log,
# gc, regex, path, datetime, encoding, base64, url
file(GLOB STDLIB_CORE_SRC
    "stdlib/prelude/*.c"
    "stdlib/stdlib_cache.c"
    "stdlib/time/*.c"
    "stdlib/math/*.c"
    "stdlib/log/*.c"
    "stdlib/gc/*.c"
    "stdlib/regex/*.c"
    "stdlib/path/*.c"
    "stdlib/datetime/*.c"
    "stdlib/encoding/*.c"
    "stdlib/base64/*.c"
    "stdlib/url/*.c"
)
set(STDLIB_SRC ${STDLIB_CORE_SRC})

find_package(ZLIB QUIET)
if(XR_STDLIB_FULL)
    # Full stdlib: include all modules except those explicitly disabled
    file(GLOB_RECURSE STDLIB_ALL_SRC "stdlib/*/*.c")
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
        file(GLOB STDLIB_FS_SRC "stdlib/io/*.c" "stdlib/os/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_FS_SRC})
        add_compile_definitions(XR_HAS_FILESYSTEM=1)
        message(STATUS "Stdlib: +filesystem (io, os)")
    endif()

    if(XR_STDLIB_NET)
        file(GLOB STDLIB_NET_SRC "stdlib/net/*.c" "stdlib/http/*.c" "stdlib/ws/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_NET_SRC})
        add_compile_definitions(XR_HAS_NETWORK=1)
        message(STATUS "Stdlib: +network (net, http, ws)")
    endif()

    if(XR_STDLIB_CRYPTO)
        file(GLOB STDLIB_CRYPTO_SRC "stdlib/crypto/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_CRYPTO_SRC})
        add_compile_definitions(XR_HAS_CRYPTO=1)
        message(STATUS "Stdlib: +crypto")
    endif()

    if(XR_STDLIB_DATA)
        file(GLOB STDLIB_DATA_SRC "stdlib/csv/*.c" "stdlib/toml/*.c" "stdlib/yaml/*.c" "stdlib/xml/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_DATA_SRC})
        add_compile_definitions(XR_HAS_DATA_FORMATS=1)
        message(STATUS "Stdlib: +data formats (csv, toml, yaml, xml)")
    endif()

    if(XR_STDLIB_COMPRESS)
        file(GLOB STDLIB_COMPRESS_SRC "stdlib/compress/*.c")
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
        file(GLOB STDLIB_CLUSTER_SRC "stdlib/cluster/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_CLUSTER_SRC})
        add_compile_definitions(XR_HAS_CLUSTER=1)
        message(STATUS "Stdlib: +cluster (distributed Named Channel)")
    endif()

    if(XR_BUILD_TEST_MODULES)
        file(GLOB STDLIB_TEST_SRC "stdlib/test_yield/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_TEST_SRC})
        message(STATUS "Stdlib: +test modules")
    endif()
endif()

# C include fragments use *.inc.c for editor support, but they are included
# from owning .c files and must never be compiled as standalone translation units.
foreach(XRAY_SRC_LIST
        BASE_SRC OS_SRC RUNTIME_TOP_SRC RUNTIME_CORE_SRC RUNTIME_VALUE_SRC RUNTIME_GC_SRC
        RUNTIME_OBJECT_SRC RUNTIME_OBJECT_BUILTINS_SRC RUNTIME_CLASS_SRC
        RUNTIME_CLOSURE_SRC RUNTIME_SYMBOL_SRC CORO_SRC IO_SRC
        FRONTEND_LEXER_SRC FRONTEND_PARSER_SRC FRONTEND_CODEGEN_SRC
        FRONTEND_ANALYZER_SRC FRONTEND_CANONICAL_SRC FRONTEND_FORMAT_SRC
        VM_SRC IR_SRC MODULE_SRC AOT_SRC API_SRC NEW_CLI_SRC
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
    ${RUNTIME_GC_SRC}
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
    ${VM_SRC}
    ${IR_SRC}
    ${MODULE_SRC}
    ${AOT_SRC}
    ${API_SRC}
    ${STDLIB_SRC}
)

add_library(xray_core STATIC ${CORE_ALL_SOURCES})
add_dependencies(xray_core gen-xi-ops gen-xi-verify gen-xi-lowering gen-aot-metadata)

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
    ${RUNTIME_GC_SRC}
    ${RUNTIME_OBJECT_SRC}
    ${RUNTIME_CLASS_SRC}
    ${RUNTIME_CLOSURE_SRC}
    ${RUNTIME_SYMBOL_SRC}
    ${CORO_SRC}
    ${IO_SRC}
)
list(FILTER XRAY_RT_CORO_SOURCES EXCLUDE REGEX ".*/src/runtime/xisolate_api\\.c$")
list(FILTER XRAY_RT_CORO_SOURCES EXCLUDE REGEX ".*/src/runtime/value/xchunk\\.c$")
list(FILTER XRAY_RT_CORO_SOURCES EXCLUDE REGEX ".*/src/runtime/value/xopcode_info\\.c$")

add_library(xray_rt_coro STATIC ${XRAY_RT_CORO_SOURCES})
add_dependencies(xray_rt_coro gen-xi-ops gen-xi-verify gen-xi-lowering gen-aot-metadata)

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND MSVC)
    set_source_files_properties(
        src/vm/xvm.c
        PROPERTIES COMPILE_OPTIONS "/Od;/Ob0"
    )
endif()

# Expose development-tree paths for cmd_build's C compiler.
add_compile_definitions(
    XRT_AOT_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/aot"
    XRT_SOURCE_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include"
    XRT_BUILD_LIB_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)


# Core library include paths (detailed configuration)
target_include_directories(xray_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(xray_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

target_include_directories(xray_rt_coro PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(xray_rt_coro PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

# Link libraries
if(WIN32)
    # Math library is part of the MSVC runtime; ws2_32 for sockets,
    # synchronization for WaitOnAddress/WakeByAddressSingle
    target_link_libraries(xray_core PUBLIC ws2_32 synchronization)
    target_link_libraries(xray_rt_coro PUBLIC ws2_32 synchronization)
else()
    target_link_libraries(xray_core PUBLIC m pthread)
    target_link_libraries(xray_rt_coro PUBLIC m pthread)
endif()
if(ZLIB_FOUND)
    target_link_libraries(xray_core PUBLIC ZLIB::ZLIB)
    target_compile_definitions(xray_core PRIVATE XR_HAS_ZLIB=1)
endif()

# libffi: FFI / C interop. The bytecode VM marshals @extern calls through
# libffi; the AOT backend emits direct C calls and does not need it. Optional —
# when absent, @extern calls raise a clean runtime error in the VM (AOT still
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
    target_include_directories(xray_core PRIVATE ${LIBFFI_INCLUDE_DIRS})
    target_link_libraries(xray_core PUBLIC ${LIBFFI_LIBRARIES})
    # PUBLIC: the CLI target builds AOT link commands by hand (invoke_cc) and
    # must know xray_core embeds the libffi-based @extern invoker, so a runtime
    # program links -lffi too.
    target_compile_definitions(xray_core PUBLIC XRAY_HAVE_LIBFFI=1)
else()
    message(STATUS "libffi: not found (@extern calls limited to the AOT backend)")
endif()
if(NOT WIN32)
    target_link_libraries(xray_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# OpenSSL support
# - WebSocket stdlib needs libcrypto (SHA1 for handshake) even without TLS
# - Full TLS needs both libssl and libcrypto
if(APPLE)
    set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
endif()
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
    target_include_directories(xray_core PRIVATE ${OPENSSL_INCLUDE_DIR})
    # Always link libcrypto (needed by ws module for SHA1)
    target_link_libraries(xray_core PUBLIC OpenSSL::Crypto)
    if(ENABLE_TLS)
        target_link_libraries(xray_core PUBLIC OpenSSL::SSL)
        target_compile_definitions(xray_core PUBLIC XR_ENABLE_TLS=1)
    endif()
else()
    message(STATUS "OpenSSL: not found (ws/crypto modules may be limited)")
endif()

# ============================================================================
# Build Target - Main Executable (optional LSP and DAP)
# ============================================================================

# Base CLI source files
set(XRAY_CLI_SOURCES ${NEW_CLI_SRC})
list(APPEND XRAY_CLI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/cli/xcmd_builtin_dump.c)
list(REMOVE_DUPLICATES XRAY_CLI_SOURCES)
set(XRAY_CLI_INCLUDES
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/cli
)

# LSP optional module
if(XR_BUILD_LSP)
    file(GLOB LSP_SRC "src/app/lsp/*.c")
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
    file(GLOB DAP_SRC "src/app/dap/*.c")
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
    file(GLOB MCP_SRC "src/app/mcp/*.c")
    list(APPEND XRAY_CLI_SOURCES ${MCP_SRC})
    list(APPEND XRAY_CLI_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/mcp)

    add_compile_definitions(XR_HAS_MCP=1)
    set(_xmcp_builtin_dump ${CMAKE_CURRENT_BINARY_DIR}/xmcp_builtin_dump.json)
    add_custom_target(regen-mcp-knowledge
        COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 ${XRAY_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_language_docs.py
            --root ${CMAKE_CURRENT_SOURCE_DIR}
        COMMAND $<TARGET_FILE:xray> builtin-dump > ${_xmcp_builtin_dump}
        COMMAND ${CMAKE_COMMAND} -E env PYTHONUTF8=1 ${XRAY_PYTHON}
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen_mcp_knowledge.py
            --docs ${CMAKE_CURRENT_SOURCE_DIR}/docs/knowledge
            --spec ${CMAKE_CURRENT_SOURCE_DIR}/LANGUAGE_SPEC_CN.md
            --builtins ${_xmcp_builtin_dump}
            --out ${CMAKE_CURRENT_SOURCE_DIR}/src/app/mcp/xmcp_knowledge_generated.c
        DEPENDS xray
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        VERBATIM
        COMMENT "Regenerating language docs, dumping analyzer builtins, and regenerating MCP knowledge tables"
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
add_dependencies(xray xray_aot_core xray_rt_coro)

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

    add_test(
        NAME aot_filetests
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY_AOT_FILETEST_JOBS=${XRAY_CTEST_AOT_FILETEST_JOBS}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_filetests.sh
                --xray $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_filetests PROPERTIES
        LABELS "aot;filetests"
        PROCESSORS ${XRAY_CTEST_AOT_FILETEST_JOBS}
        TIMEOUT 120
    )

    add_test(
        NAME aot_cross_smoke
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_cross_smoke.sh
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
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_bundled_zig_smoke.sh
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
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_native_debug_smoke.sh
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

    # Link manifest contract: freestanding direct calls and runtime-backed
    # stdlib modules must not drag in the full xray_core library.
    add_test(
        NAME aot_link_command_manifest
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_link_command_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_link_command_manifest PROPERTIES
        LABELS "aot;link;stdlib;smoke"
        PROCESSORS 2
        TIMEOUT 120
    )

    # Isolate/runtime split guard (132/M0 + 133/I0/I4): AOT binaries must stay
    # free of xray_core and public isolate/VM/compiler/module-loader symbols.
    add_test(
        NAME aot_isolate_symbol_gate
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY_AOT_ISOLATE_JOBS=${XRAY_CTEST_AOT_ISOLATE_JOBS}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aot/run_aot_isolate_symbol_tests.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(aot_isolate_symbol_gate PROPERTIES
        LABELS "aot;link;isolate;smoke"
        PROCESSORS ${XRAY_CTEST_AOT_ISOLATE_JOBS}
        TIMEOUT 180
    )

    # FFI runtime/link diagnostics: missing libraries and symbols must fail
    # with stable, user-facing messages instead of cascading or opaque errors.
    add_test(
        NAME ffi_diagnostics
        COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/run_ffi_diagnostics.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(ffi_diagnostics PROPERTIES
        LABELS "ffi;aot;vm;negative;regression"
        PROCESSORS 2
        TIMEOUT 60
    )

    # FFI ASan matrix: ASan builds must exercise raw pointers, CFn callbacks,
    # and headerless repr(C) structs through VM/AOT; normal builds skip.
    add_test(
        NAME ffi_asan_smoke
        COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/run_ffi_asan_smoke.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(ffi_asan_smoke PROPERTIES
        LABELS "ffi;aot;vm;asan;regression"
        SKIP_RETURN_CODE 77
        TIMEOUT 90
    )

    # AOT @c_export smoke: generated public C ABI symbols must be linkable
    # from an external C translation unit and callable with scalar values.
    add_test(
        NAME ffi_c_export_smoke
        COMMAND bash
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/run_c_export_smoke.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(ffi_c_export_smoke PROPERTIES
        LABELS "ffi;aot;regression"
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
    set_tests_properties(compile_error_tests PROPERTIES
        LABELS "frontend;negative;regression"
        PROCESSORS 2
        TIMEOUT 60
    )

    # Borrow/ownership instrumentation: keep a machine-parseable Xi
    # retain/release budget available for RC-overhead regression work.
    add_test(
        NAME xi_rc_count_smoke
        COMMAND bash
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
        COMMAND bash
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
                XRAY_DIFF_HOT_MAX_AUTO_JOBS=8
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/diff/run_backend_diff.sh
                $<TARGET_FILE:xray>
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(backend_diff PROPERTIES
        LABELS "diff;cross;backend"
        PROCESSORS ${XRAY_CTEST_DIFF_PROCESSORS}
        TIMEOUT 900
    )

    add_test(
        NAME backend_diff_deterministic
        COMMAND ${CMAKE_COMMAND} -E env
                XRAY_CORO_DETERMINISTIC=1
                XRAY_CORO_SEED=12345
                XRAY_TEST_CACHE_ROOT=$<TARGET_FILE_DIR:xray>/.xray-test-cache/deterministic
                XRAY_DIFF_CASES_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/diff/deterministic_cases.txt
                XRAY_DIFF_JOBS=${XRAY_CTEST_DETERMINISTIC_DIFF_JOBS}
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
        NAME regression_gc_closure_self_capture_rc
        COMMAND $<TARGET_FILE:xray> test
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/regression/08_gc/0803_closure_self_capture_rc.xr
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(regression_gc_closure_self_capture_rc PROPERTIES
        LABELS "regression;gc;vm"
        TIMEOUT 30
    )
endif()

# Fuzzing tests (optional, requires Clang with libFuzzer)
if(ENABLE_FUZZING)
    add_subdirectory(tests/fuzz)
endif()

# ============================================================================
# Install Rules
# ============================================================================

install(TARGETS xray
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY stdlib/
    DESTINATION lib/xray/stdlib
    FILES_MATCHING PATTERN "*.xr"
)

# ============================================================================
# Build Information Output
# ============================================================================

message(STATUS "========================================")
message(STATUS "Xray Build Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  GC: ${XR_USE_GC}")
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
    file(GLOB_RECURSE ALL_SOURCE_FILES
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
    file(GLOB_RECURSE CORE_ANALYSIS_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/gc/*.c
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

# ========== Stdlib Bytecode Embedding (REMOVED) ==========
# Built-in stdlib modules are now pure C — no embedded bytecode needed.
# Third-party hybrid modules can still use file-system script extensions
# (stdlib/<name>/<name>.xr) which are loaded by xmodule.c at runtime.
#
# The embed_stdlib.py script has been removed (stdlib is pure C now).

# ============================================================================
# Install Configuration
# ============================================================================

# Install path
set(XRAY_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}" CACHE PATH "Xray install path")

# Install executable
install(TARGETS xray
    RUNTIME DESTINATION bin
)

# Install static library (for xray build)
install(TARGETS xray_core xray_aot_core xray_rt_coro
    ARCHIVE DESTINATION lib
)

# Install header files (to include/xray directory)
install(FILES
    ${CMAKE_SOURCE_DIR}/include/xray.h
    ${CMAKE_SOURCE_DIR}/include/xray_isolate.h
    ${CMAKE_SOURCE_DIR}/include/xray_export.h
    ${CMAKE_SOURCE_DIR}/include/xray_version.h
    DESTINATION include/xray
)

# Install stdlib source files
install(DIRECTORY ${CMAKE_SOURCE_DIR}/stdlib/
    DESTINATION lib/xray/stdlib
    FILES_MATCHING PATTERN "*.xr"
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
        )
    else()
        message(WARNING
            "Bundled Zig root '${_XRAY_ZIG_ROOT}' has no lib directory; installing executable only")
        install(PROGRAMS "${_XRAY_ZIG_REALPATH}"
            DESTINATION libexec/xray/zig
            RENAME "zig${CMAKE_EXECUTABLE_SUFFIX}"
        )
    endif()
endif()

# ============================================================================
# Packaging Configuration (CPack)
# ============================================================================

set(CPACK_PACKAGE_NAME "xray")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VENDOR "Xray Language")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Xray Programming Language")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://xray-lang.org")

# Set package type based on system
if(APPLE)
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_SYSTEM_NAME "macos-${CMAKE_SYSTEM_PROCESSOR}")
elseif(UNIX)
    set(CPACK_GENERATOR "TGZ;DEB")
    set(CPACK_SYSTEM_NAME "linux-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "xingleixu@gmail.com")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libpthread-stubs0-dev")
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_SYSTEM_NAME "windows-${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CPACK_SYSTEM_NAME}")

include(CPack)

message(STATUS "Install configuration:")
message(STATUS "  make install      - Install to ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  make package      - Generate install package")
