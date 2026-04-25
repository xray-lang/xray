# Unit tests for xray
# All unit tests are managed here (no glob in root CMakeLists.txt)

# ============================================================================
# Macro for adding unit tests that link to xray_core
# ============================================================================

macro(add_xray_unit_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(${name} PRIVATE xray_core)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

# ============================================================================
# value/ — Value system
# ============================================================================

add_xray_unit_test(test_xvalue value/test_xvalue.c)

# ============================================================================
# util/ — Hash, dynamic array, hashmap, arena
# ============================================================================

add_xray_unit_test(test_xhash util/test_xhash.c)
add_xray_unit_test(test_dynarray util/test_dynarray.c)
add_xray_unit_test(test_hashmap util/test_hashmap.c)
add_xray_unit_test(test_arena util/test_arena.c)

# ============================================================================
# gc/ — Garbage collector
# ============================================================================

add_xray_unit_test(test_immix_gc gc/test_immix_gc.c)

# ============================================================================
# object/ — Runtime objects (string, array, utf8)
# ============================================================================

add_xray_unit_test(test_utf8 object/test_utf8.c)

# TODO: fix memory management for test_typed_array
# add_xray_unit_test(test_typed_array object/test_typed_array.c)
add_xray_unit_test(test_xarray object/test_xarray.c)
add_xray_unit_test(test_xstring object/test_xstring.c)
add_xray_unit_test(test_xmap object/test_xmap.c)
add_xray_unit_test(test_xset object/test_xset.c)

# ============================================================================
# frontend/ — Lexer, parser, diagnostics
# ============================================================================

add_xray_unit_test(test_lexer frontend/test_lexer.c)
add_xray_unit_test(test_parser frontend/test_parser.c)
add_xray_unit_test(test_diag_fmt frontend/test_diag_fmt.c)
add_xray_unit_test(test_xfmt_literal frontend/test_xfmt_literal.c)
add_xray_unit_test(test_analyzer_remove_file frontend/test_analyzer_remove_file.c)
# Phase 3.5: X-01 AstNode -> XrType side table acceptance tests.
add_xray_unit_test(test_ast_side_table frontend/test_ast_side_table.c)

# Phase 3: in-tree frontend architecture lint. Walks the project src/
# tree at runtime, so it needs the absolute path baked in at build
# time. The test does NOT link xray_core (it is a pure path/text
# walker), so we add it directly without the helper macro.
add_executable(test_frontend_architecture
    frontend/test_frontend_architecture.c
)
target_include_directories(test_frontend_architecture PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(test_frontend_architecture PRIVATE
    XR_TEST_SRC_DIR="${CMAKE_SOURCE_DIR}/src"
)
add_test(NAME test_frontend_architecture
         COMMAND test_frontend_architecture)

# ============================================================================
# analyzer/ — Type inference, type checking
# ============================================================================

add_xray_unit_test(test_analyzer analyzer/test_analyzer.c)
add_xray_unit_test(test_mono analyzer/test_mono.c)

# ============================================================================
# vm/ — Instruction encoding, opcode
# ============================================================================

add_xray_unit_test(test_instruction vm/test_instruction.c)
add_xray_unit_test(test_opcode vm/test_opcode.c)

# ============================================================================
# symbol/ — Symbol table
# ============================================================================

add_xray_unit_test(test_symbol symbol/test_symbol.c)

# ============================================================================
# jit/ — XIR, ARM64 codegen, passes, liveness
# ============================================================================

add_xray_unit_test(test_xir jit/test_xir.c)
add_xray_unit_test(test_xir_bset jit/test_xir_bset.c)
add_xray_unit_test(test_xir_builder jit/test_xir_builder.c)
add_xray_unit_test(test_xir_defuse jit/test_xir_defuse.c)
add_xray_unit_test(test_xir_fold jit/test_xir_fold.c)
add_xray_unit_test(test_xir_liveness2 jit/test_xir_liveness2.c)
add_xray_unit_test(test_xir_pass jit/test_xir_pass.c)
# ARM64-specific tests (instruction encoding, peephole)
if(XR_TARGET_ARCH MATCHES "arm64|aarch64")
    add_xray_unit_test(test_arm64_emit jit/test_arm64_emit.c)
    add_xray_unit_test(test_peephole jit/test_peephole.c)
endif()
# x86-64-specific tests
if(XR_TARGET_ARCH MATCHES "x86_64|AMD64")
    add_xray_unit_test(test_x64_emit jit/test_x64_emit.c)
endif()
add_xray_unit_test(test_code_alloc jit/test_code_alloc.c)

# E2E tests: require full JIT runtime (coro + jit_ctx), excluded from default build
# Covered by regression tests (scripts/run_regression_tests.sh)
# add_xray_unit_test(test_aot_e2e jit/test_aot_e2e.c)
# add_xray_unit_test(test_jit_e2e jit/test_jit_e2e.c)

# ============================================================================
# base/ — Config, logging, SIMD, SWAR, mutex, intmap, mem_profiler
# ============================================================================

add_xray_unit_test(test_config base/test_config.c)
add_xray_unit_test(test_log base/test_log.c)
add_xray_unit_test(test_swar base/test_swar.c)
add_xray_unit_test(test_simd base/test_simd.c)
add_xray_unit_test(test_mutex base/test_mutex.c)
add_xray_unit_test(test_intmap base/test_intmap.c)
add_xray_unit_test(test_mem_profiler base/test_mem_profiler.c)
add_xray_unit_test(test_xmalloc base/test_xmalloc.c)

# ============================================================================
# coro/ — MPSC queue, work-stealing queue
# ============================================================================

add_xray_unit_test(test_mpsc_queue coro/test_mpsc_queue.c)
add_xray_unit_test(test_steal_queue coro/test_steal_queue.c)

# ============================================================================
# module/ — Resolver, semver, lockfile, source cache
# ============================================================================

add_xray_unit_test(test_resolver module/test_resolver.c)
add_xray_unit_test(test_semver module/test_semver.c)
add_xray_unit_test(test_lockfile module/test_lockfile.c)
add_xray_unit_test(test_source_cache module/test_source_cache.c)

# ============================================================================
# encoding/ — Base64, hex, URL encoding, Unicode
# ============================================================================

add_xray_unit_test(test_base64 encoding/test_base64.c)
add_xray_unit_test(test_hex_encoding encoding/test_hex_encoding.c)
add_xray_unit_test(test_url_encoding encoding/test_url_encoding.c)
add_xray_unit_test(test_unicode encoding/test_unicode.c)

# ============================================================================
# http/ — HTTP parser, cookies, multipart
# ============================================================================

add_xray_unit_test(test_http_parser http/test_http_parser.c)
add_xray_unit_test(test_http_cookie http/test_http_cookie.c)
add_xray_unit_test(test_http_multipart http/test_http_multipart.c)
add_xray_unit_test(test_http2_huffman http/test_http2_huffman.c)

# ============================================================================
# stdlib/ — DateTime, crypto, compress, XML, buffer pool
# ============================================================================

add_xray_unit_test(test_datetime stdlib/test_datetime.c)
add_xray_unit_test(test_crypto stdlib/test_crypto.c)
add_xray_unit_test(test_compress stdlib/test_compress.c)
add_xray_unit_test(test_xml_node stdlib/test_xml_node.c)
add_xray_unit_test(test_buffer_pool stdlib/test_buffer_pool.c)

# ============================================================================
# regex/ — Regex engine
# ============================================================================

add_xray_unit_test(test_regex regex/test_regex.c)

# ============================================================================
# api/ — Public API defense tests
# ============================================================================

add_xray_unit_test(test_api_defense api/test_api_defense.c)

# ============================================================================
# lsp/ — Language Server Protocol (conditional)
# ============================================================================

if(XR_BUILD_LSP)
    add_executable(test_lsp_json
        lsp/test_lsp_json.c
        ${CMAKE_SOURCE_DIR}/src/base/xjson.c
        ${CMAKE_SOURCE_DIR}/src/runtime/object/xutf8.c
    )
    target_include_directories(test_lsp_json PRIVATE
        ${CMAKE_SOURCE_DIR}/src/base
        ${CMAKE_SOURCE_DIR}/src/runtime/object
    )
    add_test(NAME test_lsp_json COMMAND test_lsp_json)

    file(GLOB LSP_SOURCES ${CMAKE_SOURCE_DIR}/src/app/lsp/*.c)
    list(REMOVE_ITEM LSP_SOURCES ${CMAKE_SOURCE_DIR}/src/app/lsp/xlsp_main.c)

    add_executable(test_lsp_document
        lsp/test_lsp_document.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_document PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_document PRIVATE xray_core)
    add_test(NAME test_lsp_document COMMAND test_lsp_document)

    add_executable(test_lsp_config
        lsp/test_lsp_config.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_config PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_config PRIVATE xray_core)
    add_test(NAME test_lsp_config COMMAND test_lsp_config)

    add_executable(test_lsp_navigation
        lsp/test_lsp_navigation.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_navigation PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_navigation PRIVATE xray_core)
    add_test(NAME test_lsp_navigation COMMAND test_lsp_navigation)

    add_executable(test_lsp_diagnostics
        lsp/test_lsp_diagnostics.c
        ${LSP_SOURCES}
    )
    target_include_directories(test_lsp_diagnostics PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/lsp
        ${CMAKE_SOURCE_DIR}/src/app/cli
        ${CMAKE_SOURCE_DIR}/src/frontend/format
    )
    target_link_libraries(test_lsp_diagnostics PRIVATE xray_core)
    add_test(NAME test_lsp_diagnostics COMMAND test_lsp_diagnostics)
endif()

# ============================================================================
# dap/ — Debug Adapter Protocol (conditional)
# ============================================================================

if(XR_BUILD_DAP)
    add_executable(test_dap_server
        dap/test_dap_server.c
        ${CMAKE_SOURCE_DIR}/src/base/xjson.c
        ${CMAKE_SOURCE_DIR}/src/runtime/object/xutf8.c
    )
    target_include_directories(test_dap_server PRIVATE
        ${CMAKE_SOURCE_DIR}/src/base
        ${CMAKE_SOURCE_DIR}/src/app/dap
        ${CMAKE_SOURCE_DIR}/src/runtime/object
    )
    add_test(NAME test_dap_server COMMAND test_dap_server)

    add_executable(test_dap_breakpoints
        dap/test_dap_breakpoints.c
    )
    target_include_directories(test_dap_breakpoints PRIVATE
        ${CMAKE_SOURCE_DIR}/src/app/dap
    )
    add_test(NAME test_dap_breakpoints COMMAND test_dap_breakpoints)

    add_executable(test_dap_transport
        dap/test_dap_transport.c
        ${CMAKE_SOURCE_DIR}/src/app/dap/xdap_transport.c
        ${CMAKE_SOURCE_DIR}/src/base/xframing.c
    )
    target_include_directories(test_dap_transport PRIVATE
        ${CMAKE_SOURCE_DIR}/src/app/dap
        ${CMAKE_SOURCE_DIR}/src/base
    )
    add_test(NAME test_dap_transport COMMAND test_dap_transport)
endif()

# ============================================================================
# mcp/ — MCP protocol, tools, resources, prompts
# ============================================================================

if(XR_BUILD_MCP)
    add_executable(test_mcp_protocol
        mcp/test_mcp_protocol.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_protocol.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_resources.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_prompts.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_knowledge.c
        ${CMAKE_SOURCE_DIR}/src/base/xjson.c
        ${CMAKE_SOURCE_DIR}/src/runtime/object/xutf8.c
    )
    target_include_directories(test_mcp_protocol PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/mcp
        ${CMAKE_SOURCE_DIR}/src/base
        ${CMAKE_SOURCE_DIR}/src/runtime/object
    )
    target_link_libraries(test_mcp_protocol PRIVATE xray_core)
    add_test(NAME test_mcp_protocol COMMAND test_mcp_protocol)
endif()

# ============================================================================
# cli/ — CLI spec, parser, dispatch, help
# ============================================================================

add_executable(test_cli_fs
    cli/test_cli_fs.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_fs.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_output.c
)
target_include_directories(test_cli_fs PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/src/app/cli
)
target_link_libraries(test_cli_fs PRIVATE xray_core)
add_test(NAME test_cli_fs COMMAND test_cli_fs)

add_executable(test_cli_spec
    cli/test_cli_spec.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_spec.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_parser.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_help.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_diag.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_fs.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_output.c
)
target_include_directories(test_cli_spec PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_SOURCE_DIR}/src/app/cli
)
target_link_libraries(test_cli_spec PRIVATE xray_core)
add_test(NAME test_cli_spec COMMAND test_cli_spec)
