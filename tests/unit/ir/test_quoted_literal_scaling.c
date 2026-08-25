/*
 * test_quoted_literal_scaling.c - Task 214 compact byte-literal scaling gate
 *
 * A fixed-byte payload is data, not syntax structure.  This test parses,
 * analyzes, and lowers 1 KiB, 64 KiB, and 1 MiB raw byte blocks and proves
 * that AST and Xi structure counts do not grow with payload length.
 */

#include "../test_framework.h"

#include "frontend/analyzer/xa_typed_program.h"
#include "frontend/analyzer/xanalyzer.h"
#include "frontend/canonical/xcanon.h"
#include "frontend/parser/xast_nodes.h"
#include "frontend/parser/xparse.h"
#include "ir/xi.h"
#include "ir/xi_emit.h"
#include "ir/xi_lower.h"
#include "runtime/value/xchunk.h"
#include "toolchain/xcompiler_session.h"
#include "xray_vm.h"

#include <stdlib.h>
#include <string.h>

static XrVMRuntime *g_iso = NULL;

typedef struct ScalingResult {
    uint32_t xi_value_count;
    uint32_t fixed_bytes_count;
    uint32_t bytecode_instruction_count;
    uint32_t fixed_bytes_opcode_count;
    uint32_t struct_area_size;
    bool payload_metadata_valid;
} ScalingResult;

static void setup(void) {
    XrVMConfig config = {0};
    g_iso = xray_vm_new_full(&config);
}

static void teardown(void) {
    if (g_iso)
        xray_vm_delete(g_iso);
    g_iso = NULL;
}

static char *make_source(size_t payload_length) {
    static const char prefix[] = "var bytes = br\"\"\"\n";
    static const char suffix[] = "\n\"\"\"\nprint(bytes[0])\n";
    size_t total = sizeof(prefix) - 1 + payload_length + sizeof(suffix);
    char *source = (char *) malloc(total);
    if (!source)
        return NULL;
    size_t cursor = 0;
    memcpy(source + cursor, prefix, sizeof(prefix) - 1);
    cursor += sizeof(prefix) - 1;
    memset(source + cursor, 'x', payload_length);
    cursor += payload_length;
    memcpy(source + cursor, suffix, sizeof(suffix));
    return source;
}

static void count_xi_values(XiFunc *func, size_t payload_length, ScalingResult *out) {
    if (!func || !out)
        return;
    for (uint32_t block_idx = 0; block_idx < func->nblocks; block_idx++) {
        XiBlock *block = func->blocks[block_idx];
        if (!block)
            continue;
        out->xi_value_count += block->nvalues;
        for (uint32_t value_idx = 0; value_idx < block->nvalues; value_idx++) {
            XiValue *value = block->values[value_idx];
            if (!value || value->op != XI_FIXED_BYTES_CONST)
                continue;
            out->fixed_bytes_count++;
            if ((size_t) value->aux_int != payload_length || !value->aux)
                out->payload_metadata_valid = false;
        }
    }
    for (uint16_t child_idx = 0; child_idx < func->nchildren; child_idx++)
        count_xi_values(func->children[child_idx], payload_length, out);
}

static bool run_scaling_case(size_t payload_length, ScalingResult *result) {
    if (!result)
        return false;
    *result = (ScalingResult) {.payload_metadata_valid = true};
    char *source = make_source(payload_length);
    if (!source)
        return false;

    XrCompilerSession *session = xr_compiler_session_current_for_isolate(g_iso);
    AstNode *program = xr_parse(session, source);
    if (!program || program->type != AST_PROGRAM || program->as.program.count != 2) {
        free(source);
        return false;
    }

    AstNode *decl = program->as.program.statements[0];
    AstNode *literal = decl && decl->type == AST_VAR_DECL ? decl->as.var_decl.initializer : NULL;
    if (!literal || literal->type != AST_FIXED_BYTES_LITERAL ||
        literal->as.fixed_bytes_literal.payload_length != payload_length ||
        !literal->as.fixed_bytes_literal.payload) {
        xr_program_destroy(program);
        free(source);
        return false;
    }

    XaAnalyzer *analyzer = xa_analyzer_new(session);
    if (!analyzer) {
        xr_program_destroy(program);
        free(source);
        return false;
    }
    xa_analyzer_analyze(analyzer, "quoted_literal_scaling.xr", program);

    XrCompilerSessionScope scope;
    bool pushed = program->as.program.arena &&
                  xr_compiler_session_push_arena(session, program->as.program.arena,
                                                 "quoted_literal_scaling.xr", &scope);
    xr_canon_program(program, analyzer, session);
    if (pushed)
        xr_compiler_session_pop_arena(&scope);

    XaTypedProgramPublishResult typed = xa_typed_program_publish(analyzer, program, NULL, 0);
    XiFunc *func = typed.program ? xi_lower_program(typed.program, g_iso, false, NULL) : NULL;
    if (func)
        count_xi_values(func, payload_length, result);

    XrProto *proto = NULL;
    XiEmitStatus emit_status = func ? xi_emit(func, g_iso, &proto) : XI_EMIT_ERR_INTERNAL;
    if (emit_status == XI_EMIT_OK && proto) {
        result->bytecode_instruction_count = (uint32_t) PROTO_CODE_COUNT(proto);
        result->struct_area_size = proto->struct_area_size;
        for (int i = 0; i < PROTO_CODE_COUNT(proto); i++) {
            if (GET_OPCODE(PROTO_CODE(proto, i)) == OP_FIXED_BYTES_CONST)
                result->fixed_bytes_opcode_count++;
        }
    }

    if (proto)
        xr_instruction_unit_free(proto);
    if (func)
        xi_func_free(func);
    xa_typed_program_free(typed.program);
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
    free(source);
    return func != NULL && emit_status == XI_EMIT_OK && proto != NULL &&
           result->payload_metadata_valid && result->struct_area_size >= payload_length;
}

TEST(payload_size_does_not_expand_ast_or_xi_structure) {
    static const size_t sizes[] = {1024, 64 * 1024, 1024 * 1024};
    ScalingResult baseline = {0};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        ScalingResult current = {0};
        ASSERT_TRUE(run_scaling_case(sizes[i], &current));
        ASSERT_EQ_INT(current.fixed_bytes_count, 1);
        ASSERT_EQ_INT(current.fixed_bytes_opcode_count, 1);
        if (i == 0) {
            baseline = current;
        } else {
            ASSERT_EQ_INT(current.xi_value_count, baseline.xi_value_count);
            ASSERT_EQ_INT(current.fixed_bytes_count, baseline.fixed_bytes_count);
            ASSERT_EQ_INT(current.bytecode_instruction_count, baseline.bytecode_instruction_count);
            ASSERT_EQ_INT(current.fixed_bytes_opcode_count, baseline.fixed_bytes_opcode_count);
        }
    }
}

TEST_MAIN_BEGIN()
setup();
RUN_TEST_SUITE("quoted literal compact scaling");
RUN_TEST(payload_size_does_not_expand_ast_or_xi_structure);
teardown();
TEST_MAIN_END()
