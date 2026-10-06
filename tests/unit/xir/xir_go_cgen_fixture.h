/* Fixed GO/AWAIT graph; executable lowering remains gated until the shared closure is ready. */
#ifndef XIR_GO_CGEN_FIXTURE_H
#define XIR_GO_CGEN_FIXTURE_H
#include "xir/xxir_types.h"
typedef struct GoCGenFixture {
    XrXirTypeNode nodes[2]; XrXirTypes types; XrXirType parameter;
    XrXirInstruction worker[2], text_worker[3], root[12], text_root[6], initializer;
    XrXirBlock worker_block, text_worker_block, root_blocks[5], text_blocks[3], init_block;
    XrXirFunction functions[5]; XrXirFunctionIdentity identities[5];
    uint32_t operand; XrXirSourceModule source; XrXirLiteral literal;
    XrXirDeclarations declarations; XrXirModule module;
} GoCGenFixture;
static void go_cgen_fixture(GoCGenFixture *f) {
    *f = (GoCGenFixture){0};
    f->nodes[0] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_I64};
    f->nodes[1] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = XR_XIR_STRING};
    f->types = (XrXirTypes){f->nodes, 2, NULL, NULL}; f->parameter = XR_XIR_I64;
    f->worker[0] = (XrXirInstruction){.op = XR_XIR_SUSPEND};
    f->worker[1] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {0}};
    f->worker_block = (XrXirBlock){.count = 2};
    f->text_worker[0] = (XrXirInstruction){.op = XR_XIR_CONST_STRING, .type = XR_XIR_STRING};
    f->text_worker[1] = (XrXirInstruction){.op = XR_XIR_SUSPEND};
    f->text_worker[2] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {0}};
    f->text_worker_block = (XrXirBlock){.count = 3};
    f->root[0] = (XrXirInstruction){.op = XR_XIR_CONST_INT, .type = XR_XIR_I64, .immediate = 42};
    f->root[1] = (XrXirInstruction){.op = XR_XIR_GO, .type = (XrXirType)256, .args = {0, 1}};
    f->root[2] = (XrXirInstruction){.op = XR_XIR_TASK_AWAIT, .args = {1}, .targets = {1, 3}};
    f->root[3] = (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = XR_XIR_I64, .immediate = 2};
    f->root[4] = (XrXirInstruction){.op = XR_XIR_TASK_AWAIT, .args = {1}, .targets = {2, 4}};
    f->root[5] = (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = XR_XIR_I64, .immediate = 4};
    f->root[6] = (XrXirInstruction){.op = XR_XIR_ADD_INT, .type = XR_XIR_I64, .args = {3, 5}};
    f->root[7] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {6}};
    f->root[8] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR, .immediate = 2};
    f->root[9] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {8}};
    f->root[10] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR, .immediate = 4};
    f->root[11] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {10}};
    f->root_blocks[0] = (XrXirBlock){.count = 3};
    f->root_blocks[1] = (XrXirBlock){.first = 3, .count = 2};
    f->root_blocks[2] = (XrXirBlock){.first = 5, .count = 3};
    f->root_blocks[3] = (XrXirBlock){.first = 8, .count = 2};
    f->root_blocks[4] = (XrXirBlock){.first = 10, .count = 2};
    f->text_root[0] = (XrXirInstruction){.op = XR_XIR_GO, .type = (XrXirType)257, .immediate = 1};
    f->text_root[1] = (XrXirInstruction){.op = XR_XIR_TASK_AWAIT, .args = {0}, .targets = {1, 2}};
    f->text_root[2] = (XrXirInstruction){.op = XR_XIR_INVOKE_RESULT, .type = XR_XIR_STRING, .immediate = 1};
    f->text_root[3] = (XrXirInstruction){.op = XR_XIR_RETURN, .args = {2}};
    f->text_root[4] = (XrXirInstruction){.op = XR_XIR_INVOKE_ERROR, .type = XR_XIR_ERROR, .immediate = 1};
    f->text_root[5] = (XrXirInstruction){.op = XR_XIR_THROW, .args = {4}};
    f->text_blocks[0] = (XrXirBlock){.count = 2};
    f->text_blocks[1] = (XrXirBlock){.first = 2, .count = 2};
    f->text_blocks[2] = (XrXirBlock){.first = 4, .count = 2};
    f->initializer = (XrXirInstruction){.op = XR_XIR_RETURN}; f->init_block = (XrXirBlock){.count = 1};
    f->functions[0] = (XrXirFunction){.name = "worker", .name_length = 6, .parameters = &f->parameter,
        .parameter_count = 1, .result = XR_XIR_I64, .blocks = &f->worker_block, .block_count = 1,
        .instructions = f->worker, .instruction_count = 2};
    f->functions[1] = (XrXirFunction){.name = "text_worker", .name_length = 11, .result = XR_XIR_STRING,
        .blocks = &f->text_worker_block, .block_count = 1, .instructions = f->text_worker, .instruction_count = 3};
    f->functions[2] = (XrXirFunction){.name = "root", .name_length = 4, .result = XR_XIR_I64,
        .blocks = f->root_blocks, .block_count = 5, .instructions = f->root, .instruction_count = 12,
        .operands = &f->operand, .operand_count = 1};
    f->functions[3] = (XrXirFunction){.name = "text", .name_length = 4, .result = XR_XIR_STRING,
        .blocks = f->text_blocks, .block_count = 3, .instructions = f->text_root, .instruction_count = 6};
    f->functions[4] = (XrXirFunction){.name = "init", .name_length = 4, .blocks = &f->init_block,
        .block_count = 1, .instructions = &f->initializer, .instruction_count = 1};
    f->identities[2].exported = f->identities[3].exported = 1;
    f->source = (XrXirSourceModule){"root", 4, NULL, 0, 4};
    f->literal = (XrXirLiteral){"A\0\xe4\xb8\xad", 5};
    f->declarations = (XrXirDeclarations){.modules = &f->source, .module_count = 1,
        .functions = f->identities, .literals = &f->literal, .literal_count = 1, .root_module = 0, .entry_function = 2};
    f->module = (XrXirModule){.stage = XR_XIR_BUILT, .functions = f->functions, .function_count = 5,
        .declarations = &f->declarations, .types = &f->types};
}
#endif
