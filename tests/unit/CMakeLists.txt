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

add_xray_unit_test(test_xarray object/test_xarray.c)
add_xray_unit_test(test_dynamic_class object/test_dynamic_class.c)
add_xray_unit_test(test_xstring object/test_xstring.c)
add_xray_unit_test(test_xmap object/test_xmap.c)
add_xray_unit_test(test_xset object/test_xset.c)
add_xray_unit_test(test_xtuple object/test_xtuple.c)

# ============================================================================
# frontend/ — Lexer, parser, diagnostics
# ============================================================================

add_xray_unit_test(test_lexer frontend/test_lexer.c)
add_xray_unit_test(test_lexer_keywords frontend/test_lexer_keywords.c)
add_xray_unit_test(test_lexer_positions frontend/test_lexer_positions.c)
add_xray_unit_test(test_lexer_trivia frontend/test_lexer_trivia.c)
add_xray_unit_test(test_parser frontend/test_parser.c)
add_xray_unit_test(test_parser_type_scope frontend/test_parser_type_scope.c)
add_xray_unit_test(test_parser_recoverable frontend/test_parser_recoverable.c)
add_xray_unit_test(test_diag_fmt frontend/test_diag_fmt.c)
add_xray_unit_test(test_xfmt_literal frontend/test_xfmt_literal.c)
add_xray_unit_test(test_formatter_comments frontend/test_formatter_comments.c)
add_xray_unit_test(test_formatter_strings frontend/test_formatter_strings.c)
if(NOT WIN32)
    add_xray_unit_test(test_fmt_roundtrip frontend/test_fmt_roundtrip.c)
endif()
add_xray_unit_test(test_analyzer_remove_file frontend/test_analyzer_remove_file.c)
add_xray_unit_test(test_analyzer_incremental frontend/test_analyzer_incremental.c)
# Phase 3.5: X-01 AstNode -> XrType side table acceptance tests.
add_xray_unit_test(test_ast_side_table frontend/test_ast_side_table.c)
add_xray_unit_test(test_symbol_binding frontend/test_symbol_binding.c)
add_xray_unit_test(test_scope_tree frontend/test_scope_tree.c)
add_xray_unit_test(test_xa_selection frontend/test_xa_selection.c)
add_xray_unit_test(test_ast_visit_coverage frontend/test_ast_visit_coverage.c)
add_xray_unit_test(test_selection_facts frontend/test_selection_facts.c)

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
add_xray_unit_test(test_interface_conformance analyzer/test_interface_conformance.c)

# ============================================================================
# vm/ — Instruction encoding, opcode
# ============================================================================

add_xray_unit_test(test_instruction vm/test_instruction.c)
add_xray_unit_test(test_opcode vm/test_opcode.c)
add_xray_unit_test(test_vm_api vm/test_vm_api.c)
add_xray_unit_test(test_ic vm/test_ic.c)
# xray_core already PUBLIC-links pthread (coroutines need it), so the
# concurrent IC test inherits the dependency through xray_core.
add_xray_unit_test(test_ic_concurrent vm/test_ic_concurrent.c)
add_xray_unit_test(test_vm_exception vm/test_vm_exception.c)

# ============================================================================
# symbol/ — Symbol table
# ============================================================================

add_xray_unit_test(test_symbol symbol/test_symbol.c)

# ============================================================================
# jit/ — XIR, ARM64 codegen, passes, liveness
# ============================================================================

add_xray_unit_test(test_xm jit/test_xm.c)
add_xray_unit_test(test_xm_bset jit/test_xm_bset.c)
add_xray_unit_test(test_xm_defuse jit/test_xm_defuse.c)
add_xray_unit_test(test_xm_fold jit/test_xm_fold.c)
add_xray_unit_test(test_xm_liveness2 jit/test_xm_liveness2.c)
add_xray_unit_test(test_xm_pass jit/test_xm_pass.c)
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

# ============================================================================
# ir/ — New typed SSA IR (Xi)
# ============================================================================

add_xray_unit_test(test_xi ir/test_xi.c)
add_xray_unit_test(test_xi_lower ir/test_xi_lower.c)
add_xray_unit_test(test_xi_opt ir/test_xi_opt.c)
add_xray_unit_test(test_xi_analysis ir/test_xi_analysis.c)
add_xray_unit_test(test_xi_emit ir/test_xi_emit.c)
add_xray_unit_test(test_xi_pipeline ir/test_xi_pipeline.c)
add_xray_unit_test(test_xi_compare ir/test_xi_compare.c)
add_xray_unit_test(test_xi_cgen ir/test_xi_cgen.c)
add_xray_unit_test(test_xi_to_xm ir/test_xi_to_xm.c)
if(NOT WIN32)
    add_xray_unit_test(test_xi_to_xm_e2e ir/test_xi_to_xm_e2e.c)
endif()
add_xray_unit_test(test_xi_lower_hardfail ir/test_xi_lower_hardfail.c)
add_xray_unit_test(test_xi_stage ir/test_xi_stage.c)
add_xray_unit_test(test_xi_intrinsic ir/test_xi_intrinsic.c)
add_xray_unit_test(test_xi_effect ir/test_xi_effect.c)
add_xray_unit_test(test_xi_escape ir/test_xi_escape.c)

# E2E tests: require full JIT runtime (coro + jit_ctx), excluded from default build
# Covered by regression tests (scripts/run_regression_tests.sh)
# add_xray_unit_test(test_aot_e2e jit/test_aot_e2e.c)
# test_jit_e2e: covers IR -> codegen -> execute end-to-end with a minimal
# fake coro/jit_ctx env (see jit_env_init() in the test file). Includes the
# §10.1-§10.6 hardening regressions from docs/tasks/010-x64-jit-hardening.md.
if(NOT WIN32)
    add_xray_unit_test(test_jit_e2e jit/test_jit_e2e.c)
endif()

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
if(XR_STDLIB_COMPRESS)
    add_xray_unit_test(test_compress stdlib/test_compress.c)
endif()
add_xray_unit_test(test_xml_node stdlib/test_xml_node.c)
add_xray_unit_test(test_buffer_pool stdlib/test_buffer_pool.c)
add_xray_unit_test(test_prelude_init stdlib/test_prelude_init.c)

# ============================================================================
# regex/ — Regex engine
# ============================================================================

add_xray_unit_test(test_regex regex/test_regex.c)

# ============================================================================
# api/ — Public API defense tests
# ============================================================================

add_xray_unit_test(test_api_defense api/test_api_defense.c)

# test_repl needs the CLI isolate-profile factory (xr_cli_isolate_*)
# in addition to xray_core, so build it like test_cli_spec with an
# explicit source list.
add_executable(test_repl
    api/test_repl.c
    ${CMAKE_SOURCE_DIR}/src/app/cli/xcli_isolate.c
)
target_include_directories(test_repl PRIVATE
    ${XRAY_COMMON_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/app/cli
    ${CMAKE_SOURCE_DIR}/src/api
)
target_link_libraries(test_repl PRIVATE xray_core)
add_test(NAME test_repl COMMAND test_repl)

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

    # LSP full-source tests require POSIX pipe/fcntl (not yet ported to Windows)
    if(NOT WIN32)
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
    endif() # NOT WIN32
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

    if(WIN32)
        set(_dap_fd_impl ${CMAKE_SOURCE_DIR}/src/os/win/fd_win.c)
    else()
        set(_dap_fd_impl ${CMAKE_SOURCE_DIR}/src/os/unix/fd_unix.c)
    endif()
    add_executable(test_dap_transport
        dap/test_dap_transport.c
        ${CMAKE_SOURCE_DIR}/src/app/dap/xdap_transport.c
        ${CMAKE_SOURCE_DIR}/src/base/xframing.c
        ${_dap_fd_impl}
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
    add_executable(test_mcp_transport
        mcp/test_mcp_transport.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_transport_stdio.c
    )
    target_include_directories(test_mcp_transport PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/src/app/mcp
        ${CMAKE_SOURCE_DIR}/src/base
    )
    target_link_libraries(test_mcp_transport PRIVATE xray_core)
    add_test(NAME test_mcp_transport COMMAND test_mcp_transport)

    add_executable(test_mcp_protocol
        mcp/test_mcp_protocol.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_jsonrpc.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_protocol.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_registry.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_tools.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_resources.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_prompts.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_knowledge.c
        ${CMAKE_SOURCE_DIR}/src/app/mcp/xmcp_knowledge_generated.c
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
    add_test(NAME test_mcp_knowledge_generation
        COMMAND python3
            ${CMAKE_SOURCE_DIR}/tests/mcp/test_knowledge_generation.py
            --root ${CMAKE_SOURCE_DIR}
            --xray $<TARGET_FILE:xray>
    )
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

# ============================================================================
# test_harness/ — xray test runner self-tests
# Verify that @test discover, execution, pass/fail/skip, and exit codes
# all behave correctly.  These run the xray binary, not C unit tests.
# ============================================================================

set(_HARNESS_DIR ${CMAKE_SOURCE_DIR}/tests/test_harness)

add_test(NAME harness_single_pass
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/single_pass.xr)
add_test(NAME harness_multi_pass
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/multi_pass.xr)
add_test(NAME harness_with_skip
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/with_skip.xr)

add_test(NAME harness_single_fail
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/single_fail.xr)
set_tests_properties(harness_single_fail PROPERTIES WILL_FAIL TRUE)

add_test(NAME harness_no_tests
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/no_tests.xr)
set_tests_properties(harness_no_tests PROPERTIES WILL_FAIL TRUE)

add_test(NAME harness_mixed_pass_fail
         COMMAND $<TARGET_FILE:xray> test ${_HARNESS_DIR}/mixed_pass_fail.xr)
set_tests_properties(harness_mixed_pass_fail PROPERTIES WILL_FAIL TRUE)
