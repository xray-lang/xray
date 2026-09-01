# Fuzzing tests for xray
# Requires clang with libFuzzer support
#
# Build:
#   cmake -B build-fuzz -DENABLE_FUZZING=ON -DCMAKE_C_COMPILER=clang
#   cmake --build build-fuzz --target fuzz_lexer fuzz_parser fuzz_stdlib_data
#
# Run:
#   ./build-fuzz/tests/fuzz/fuzz_lexer tests/fuzz/corpus/lexer -max_len=4096
#   ./build-fuzz/tests/fuzz/fuzz_parser tests/fuzz/corpus/parser -max_len=4096
#   ./build-fuzz/tests/fuzz/fuzz_stdlib_data tests/fuzz/corpus/stdlib_data -max_len=4096

# Check if fuzzing is enabled and compiler supports it
if(NOT ENABLE_FUZZING)
    message(STATUS "Fuzzing: disabled (use -DENABLE_FUZZING=ON to enable)")
    return()
endif()

# Check for clang (required for libFuzzer)
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(WARNING "Fuzzing requires Clang compiler, skipping fuzz targets")
    return()
endif()

message(STATUS "Fuzzing: enabled (libFuzzer)")

# Fuzzer compile options
set(FUZZ_COMPILE_OPTIONS
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
    -g
    -O1
)

set(FUZZ_LINK_OPTIONS
    -fsanitize=fuzzer,address,undefined
)

# ============================================================================
# Lexer Fuzzer (links to xray_core for all dependencies)
# ============================================================================

add_executable(fuzz_lexer
    fuzz_lexer.c
)

target_include_directories(fuzz_lexer PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

# Link to xray_core (lexer depends on SIMD utilities and other modules)
target_link_libraries(fuzz_lexer PRIVATE xray_core)
target_compile_options(fuzz_lexer PRIVATE ${FUZZ_COMPILE_OPTIONS})
target_link_options(fuzz_lexer PRIVATE ${FUZZ_LINK_OPTIONS})

# ============================================================================
# Parser Fuzzer (requires xray_core)
# ============================================================================

add_executable(fuzz_parser
    fuzz_parser.c
)

target_include_directories(fuzz_parser PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

target_link_libraries(fuzz_parser PRIVATE xray_core)
target_compile_options(fuzz_parser PRIVATE ${FUZZ_COMPILE_OPTIONS})
target_link_options(fuzz_parser PRIVATE ${FUZZ_LINK_OPTIONS})

# ============================================================================
# Stdlib Data Parser Fuzzer (CSV/TOML/XML/YAML pure-Xray parsers)
# ============================================================================

add_executable(fuzz_stdlib_data
    fuzz_stdlib_data.c
)

target_include_directories(fuzz_stdlib_data PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

target_link_libraries(fuzz_stdlib_data PRIVATE xray_core)
target_compile_options(fuzz_stdlib_data PRIVATE ${FUZZ_COMPILE_OPTIONS})
target_link_options(fuzz_stdlib_data PRIVATE ${FUZZ_LINK_OPTIONS})

add_executable(fuzz_xtp_decode
    fuzz_xtp_decode.c
    ../unit/plan/target_profile_test_fixture.c
)

target_include_directories(fuzz_xtp_decode PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

target_link_libraries(fuzz_xtp_decode PRIVATE xray_core)
target_compile_options(fuzz_xtp_decode PRIVATE ${FUZZ_COMPILE_OPTIONS})
target_link_options(fuzz_xtp_decode PRIVATE ${FUZZ_LINK_OPTIONS})

add_executable(fuzz_xr_program_decode
    fuzz_xr_program_decode.c
)

target_include_directories(fuzz_xr_program_decode PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

# The artifact decoder is a security boundary and must remain independently
# fuzzable.  Linking xray_core here would pull in the compiler, VM, AOT, and the
# generated stdlib image before a single hostile byte can be tested.
add_library(xr_program_fuzz_core STATIC
    ../../src/base/xsha256.c
    ../../src/core/xr_core_spec_gen.c
    ../../src/program/xr_core_ir.c
    ../../src/program/xr_program_decode.c
    ../../src/program/xr_program_encode.c
    ../../src/program/xr_program_verify.c
    ../../src/program/xr_reference_evaluator.c
)
add_dependencies(xr_program_fuzz_core
    gen-core-spec gen-xr-program-schema check-xr-program-semantics)
target_include_directories(xr_program_fuzz_core PRIVATE
    ${XRAY_COMMON_INCLUDES}
)
target_compile_options(xr_program_fuzz_core PRIVATE ${FUZZ_COMPILE_OPTIONS})

target_link_libraries(fuzz_xr_program_decode PRIVATE xr_program_fuzz_core)
target_compile_options(fuzz_xr_program_decode PRIVATE ${FUZZ_COMPILE_OPTIONS})
target_link_options(fuzz_xr_program_decode PRIVATE ${FUZZ_LINK_OPTIONS})

# ============================================================================
# Standalone test versions (for debugging without libFuzzer)
# ============================================================================

add_executable(fuzz_lexer_standalone
    fuzz_lexer.c
)

target_include_directories(fuzz_lexer_standalone PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

target_link_libraries(fuzz_lexer_standalone PRIVATE xray_core)
target_compile_definitions(fuzz_lexer_standalone PRIVATE FUZZ_STANDALONE)

add_executable(fuzz_parser_standalone
    fuzz_parser.c
)

target_include_directories(fuzz_parser_standalone PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

target_link_libraries(fuzz_parser_standalone PRIVATE xray_core)
target_compile_definitions(fuzz_parser_standalone PRIVATE FUZZ_STANDALONE)

add_executable(fuzz_stdlib_data_standalone
    fuzz_stdlib_data.c
)

target_include_directories(fuzz_stdlib_data_standalone PRIVATE
    ${XRAY_COMMON_INCLUDES}
)

target_link_libraries(fuzz_stdlib_data_standalone PRIVATE xray_core)
target_compile_definitions(fuzz_stdlib_data_standalone PRIVATE FUZZ_STANDALONE)

# ============================================================================
# Create corpus directories
# ============================================================================

file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/corpus/lexer)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/corpus/parser)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/corpus/stdlib_data)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/corpus/xtp)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/corpus/xr_program)

message(STATUS "  fuzz_lexer           - Lexer fuzzer")
message(STATUS "  fuzz_parser          - Parser fuzzer")
message(STATUS "  fuzz_stdlib_data     - Stdlib CSV/TOML/XML/YAML parser fuzzer")
message(STATUS "  fuzz_xtp_decode      - Typed TargetPlan artifact fuzzer")
message(STATUS "  fuzz_xr_program_decode - XrProgram decoder and semantic admission fuzzer")
message(STATUS "  fuzz_lexer_standalone  - Lexer test (no libFuzzer)")
message(STATUS "  fuzz_parser_standalone - Parser test (no libFuzzer)")
message(STATUS "  fuzz_stdlib_data_standalone - Stdlib data parser test (no libFuzzer)")
