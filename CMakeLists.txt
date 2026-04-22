# CMakeLists.txt
# Author: xingleixu@gmail.com
# Xray Language Interpreter Build Configuration

cmake_minimum_required(VERSION 3.10)

project(Xray
    VERSION 0.5.2
    DESCRIPTION "Xray Programming Language Interpreter"
    LANGUAGES C CXX ASM
)

# Generate xray_version.h from template (single source of truth)
configure_file(
    "${CMAKE_SOURCE_DIR}/include/xray_version.h.in"
    "${CMAKE_SOURCE_DIR}/include/xray_version.h"
    @ONLY
)

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

    # Debug mode options
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-O0 -g3)
    else()
        add_compile_options(-O2 -g)
    endif()

    # Enable linker dead-code elimination (for xray build output size)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-ffunction-sections -fdata-sections)
    endif()

    # LTO for Release builds: enables cross-TU inlining
    # (critical for GC traverse → markvalue → markobject hot path)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-flto)
        add_link_options(-flto)
    endif()
endif()

# ============================================================================
# Project Configuration Options
# ============================================================================

option(XR_USE_GC "Enable garbage collector" ON)
option(ENABLE_LLVM_BACKEND "Enable LLVM backend" OFF)
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_ASAN "Enable AddressSanitizer memory detection" OFF)
option(ENABLE_TLS "Enable HTTPS/TLS support (requires OpenSSL)" OFF)
option(XR_USE_MIMALLOC "Use mimalloc high-performance allocator" OFF)
option(XR_STDLIB_FROM_FILE "Load stdlib from file instead of embedded bytecode (for debugging)" OFF)

# Modular build options
option(XR_BUILD_LSP "Build LSP language server" ON)
option(XR_BUILD_DAP "Build DAP debug adapter" ON)
option(XR_BUILD_MCP "Build MCP server for AI assistants" ON)

# Stdlib modular options (for security control / binary size)
option(XR_STDLIB_FULL       "Full stdlib (ignores group options below)" ON)
option(XR_STDLIB_FILESYSTEM "Filesystem modules (io, os)" ON)
option(XR_STDLIB_NET        "Network modules (net, http)" ON)
option(XR_STDLIB_CRYPTO     "Crypto modules (crypto)" ON)
option(XR_STDLIB_DATA       "Data format modules (csv, toml, yaml, xml)" ON)
option(XR_STDLIB_COMPRESS   "Compression modules (compress)" ON)
option(XR_STDLIB_CLUSTER    "Cluster module (distributed Named Channel)" OFF)

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
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# UndefinedBehaviorSanitizer configuration
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
if(ENABLE_UBSAN)
    message(STATUS "UndefinedBehaviorSanitizer enabled")
    # Core UBSan checks for undefined behavior
    # Note: We only use -fsanitize=undefined (excludes unsigned-integer-overflow)
    # which catches real undefined behavior like:
    # - Signed integer overflow
    # - Null pointer dereference
    # - Out-of-bounds array access
    # - Use after scope, etc.
    add_compile_options(
        -fsanitize=undefined
        -fno-omit-frame-pointer
    )
    add_link_options(-fsanitize=undefined)
endif()

# ThreadSanitizer configuration
option(ENABLE_TSAN "Enable ThreadSanitizer data race detection" OFF)
if(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
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

# JIT compiler (ARM64 and x86-64)
# When cross-compiling on macOS (CMAKE_OSX_ARCHITECTURES), use that instead of
# CMAKE_SYSTEM_PROCESSOR which always reports the host CPU.
if(CMAKE_OSX_ARCHITECTURES)
    set(XR_TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(XR_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(XR_TARGET_ARCH MATCHES "arm64|aarch64")
    add_compile_definitions(XRAY_HAS_JIT=1)
    message(STATUS "JIT: enabled (ARM64)")
elseif(XR_TARGET_ARCH MATCHES "x86_64|AMD64")
    add_compile_definitions(XRAY_HAS_JIT=1)
    message(STATUS "JIT: enabled (x86-64)")
else()
    message(STATUS "JIT: disabled (unsupported architecture: ${XR_TARGET_ARCH})")
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/value
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/gc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/object
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/object/builtins
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/class
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/closure
    ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/symbol
    ${CMAKE_CURRENT_SOURCE_DIR}/src/coro
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/lexer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/parser
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/codegen
    ${CMAKE_CURRENT_SOURCE_DIR}/src/frontend/analyzer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm
    ${CMAKE_CURRENT_SOURCE_DIR}/src/jit
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

file(GLOB BASE_SRC "src/base/*.c")
file(GLOB RUNTIME_TOP_SRC "src/runtime/*.c")
file(GLOB RUNTIME_VALUE_SRC "src/runtime/value/*.c")
file(GLOB RUNTIME_GC_SRC "src/runtime/gc/*.c")
file(GLOB RUNTIME_OBJECT_SRC "src/runtime/object/*.c")
file(GLOB RUNTIME_OBJECT_BUILTINS_SRC "src/runtime/object/builtins/*.c")
list(APPEND RUNTIME_OBJECT_SRC ${RUNTIME_OBJECT_BUILTINS_SRC})
file(GLOB RUNTIME_CLASS_SRC "src/runtime/class/*.c")
file(GLOB RUNTIME_CLOSURE_SRC "src/runtime/closure/*.c")
file(GLOB RUNTIME_SYMBOL_SRC "src/runtime/symbol/*.c")
file(GLOB CORO_SRC "src/coro/*.c")

# Stackful coroutine context switch removed — fully stackless now
file(GLOB FRONTEND_LEXER_SRC "src/frontend/lexer/*.c")
file(GLOB FRONTEND_PARSER_SRC "src/frontend/parser/*.c")
file(GLOB FRONTEND_CODEGEN_SRC "src/frontend/codegen/*.c")
file(GLOB FRONTEND_ANALYZER_SRC "src/frontend/analyzer/*.c")
file(GLOB VM_SRC "src/vm/*.c")
file(GLOB JIT_SRC "src/jit/*.c")
file(GLOB MODULE_SRC "src/module/*.c")
file(GLOB AOT_SRC "src/aot/*.c")
file(GLOB API_SRC "src/api/*.c")
file(GLOB NEW_CLI_SRC "src/app/cli/*.c")

# Standard Library (Native modules) - modular conditional compilation
# Core modules (required): time, math, json, log, gc, regex, path, datetime, encoding, base64, url
file(GLOB STDLIB_CORE_SRC
    "stdlib/time/*.c"
    "stdlib/math/*.c"
    "stdlib/json/*.c"
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

if(XR_STDLIB_FULL)
    # Full stdlib: include all modules
    file(GLOB_RECURSE STDLIB_ALL_SRC "stdlib/*/*.c")
    list(FILTER STDLIB_ALL_SRC EXCLUDE REGEX ".*sqlite.*\\.c$")
    set(STDLIB_SRC ${STDLIB_ALL_SRC})
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
        list(APPEND STDLIB_SRC ${STDLIB_COMPRESS_SRC})
        add_compile_definitions(XR_HAS_COMPRESS=1)
        message(STATUS "Stdlib: +compression")
    endif()

    if(XR_STDLIB_CLUSTER)
        file(GLOB STDLIB_CLUSTER_SRC "stdlib/cluster/*.c")
        list(APPEND STDLIB_SRC ${STDLIB_CLUSTER_SRC})
        add_compile_definitions(XR_HAS_CLUSTER=1)
        message(STATUS "Stdlib: +cluster (distributed Named Channel)")
    endif()
endif()

# ============================================================================
# Build Target - Core Library (all core components)
# ============================================================================

set(CORE_ALL_SOURCES
    ${BASE_SRC}
    ${RUNTIME_TOP_SRC}
    ${RUNTIME_VALUE_SRC}
    ${RUNTIME_GC_SRC}
    ${RUNTIME_OBJECT_SRC}
    ${RUNTIME_CLASS_SRC}
    ${RUNTIME_CLOSURE_SRC}
    ${RUNTIME_SYMBOL_SRC}
    ${CORO_SRC}
    ${FRONTEND_LEXER_SRC}
    ${FRONTEND_PARSER_SRC}
    ${FRONTEND_CODEGEN_SRC}
    ${FRONTEND_ANALYZER_SRC}
    ${VM_SRC}
    ${JIT_SRC}
    ${MODULE_SRC}
    ${AOT_SRC}
    ${API_SRC}
    ${STDLIB_SRC}
)

add_library(xray_core STATIC ${CORE_ALL_SOURCES})

# Disable LTO for VM interpreter: the huge dispatch loop (7000+ lines, computed goto)
# gets miscompiled when LTO cross-TU inlines GC/runtime helpers into it,
# causing incorrect register allocation on ARM64 (x27 holds a NaN-boxed value
# instead of a pointer). All other TUs keep LTO for GC hot path performance.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_source_files_properties(
        src/vm/xvm.c
        src/vm/xvm_cold_paths.c
        PROPERTIES COMPILE_OPTIONS "-fno-lto"
    )
endif()

# Expose AOT runtime header path (xrt_vm.h, xrt_common.h) for cmd_build's C compiler
add_compile_definitions(XRT_AOT_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src/aot")


# Core library include paths (detailed configuration)
target_include_directories(xray_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(xray_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

# Link libraries
target_link_libraries(xray_core PUBLIC m z)

# Link pthread (required for coroutines)
if(NOT WIN32)
    target_link_libraries(xray_core PUBLIC pthread)
endif()

# TLS/OpenSSL support
if(ENABLE_TLS)
    # macOS Homebrew OpenSSL path
    if(APPLE)
        set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
    endif()

    find_package(OpenSSL REQUIRED)
    if(OpenSSL_FOUND)
        message(STATUS "OpenSSL version: ${OPENSSL_VERSION}")
        target_include_directories(xray_core PRIVATE ${OPENSSL_INCLUDE_DIR})
        target_link_libraries(xray_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(xray_core PRIVATE XR_ENABLE_TLS=1)
    endif()
endif()

# ============================================================================
# Build Target - Main Executable (optional LSP and DAP)
# ============================================================================

# Base CLI source files
set(XRAY_CLI_SOURCES ${NEW_CLI_SRC})
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

    # DAP uses xlsp_json for JSON parsing (shared with LSP)
    # If LSP is disabled, we need to include xlsp_json.c separately
    if(NOT XR_BUILD_LSP)
        list(APPEND XRAY_CLI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/lsp/xlsp_json.c)
        list(APPEND XRAY_CLI_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/lsp)
    endif()

    add_compile_definitions(XR_HAS_DAP=1)
    message(STATUS "DAP: enabled")
else()
    message(STATUS "DAP: disabled")
endif()

# MCP optional module (uses xlsp_json for JSON)
if(XR_BUILD_MCP)
    file(GLOB MCP_SRC "src/app/mcp/*.c")
    list(APPEND XRAY_CLI_SOURCES ${MCP_SRC})
    list(APPEND XRAY_CLI_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/mcp)

    # MCP uses xlsp_json for JSON parsing (shared with LSP)
    if(NOT XR_BUILD_LSP AND NOT XR_BUILD_DAP)
        list(APPEND XRAY_CLI_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/lsp/xlsp_json.c)
        list(APPEND XRAY_CLI_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/src/app/lsp)
    endif()

    add_compile_definitions(XR_HAS_MCP=1)
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

target_link_libraries(xray PRIVATE xray_core)
if(READLINE_LIB)
    target_link_libraries(xray PRIVATE ${READLINE_LIB})
endif()

target_include_directories(xray PRIVATE ${XRAY_CLI_INCLUDES})

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

    # All unit tests are managed by tests/unit/CMakeLists.txt
    add_subdirectory(tests/unit)
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
message(STATUS "  Sanitizers:")
message(STATUS "    ASan:  ${ENABLE_ASAN}")
message(STATUS "    UBSan: ${ENABLE_UBSAN}")
message(STATUS "    TSan:  ${ENABLE_TSAN}")
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
install(TARGETS xray_core
    ARCHIVE DESTINATION lib
)

# Install header files (to include/xray directory)
install(FILES
    ${CMAKE_SOURCE_DIR}/include/xray.h
    ${CMAKE_SOURCE_DIR}/include/xray_isolate.h
    ${CMAKE_SOURCE_DIR}/include/xray_platform.h
    DESTINATION include/xray
)

# Install stdlib source files
install(DIRECTORY ${CMAKE_SOURCE_DIR}/stdlib/
    DESTINATION lib/xray/stdlib
    FILES_MATCHING PATTERN "*.xr"
)

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
