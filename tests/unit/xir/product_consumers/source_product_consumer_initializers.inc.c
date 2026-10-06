/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_initializers.inc.c - Exact root initializer structure
 */
static bool consumer_initializer_case(void) {
    return !strcmp(XR_CONSUMER_NAME, "canonical_initializer");
}

typedef struct ConsumerInitializerTrace {
    uint32_t begins, ready;
} ConsumerInitializerTrace;

static void consumer_initializer_trace(void *context, XrXirLifecycleEvent event, uint32_t index) {
    ConsumerInitializerTrace *trace = context;
    CHECK(!index);
    if (event == XR_XIR_MODULE_BEGIN) {
        CHECK(!trace->begins && !trace->ready);
        ++trace->begins;
    } else {
        CHECK(event == XR_XIR_MODULE_READY && trace->begins == 1 && !trace->ready);
        ++trace->ready;
    }
}

static void consumer_initializer_shape(const XrXirModule *module) {
    const XrXirDeclarations *d = module->declarations;
    CHECK(d && d->module_count == 1 && d->root_module == 0 && module->function_count == 2);
    CHECK(d->entry_function == 1 && d->modules[0].initializer == 0 && !d->slot_count);
    const XrXirFunction *init = &module->functions[0], *entry = &module->functions[1];
    CHECK(init->result == XR_XIR_UNIT && entry->result == XR_XIR_I64);
    CHECK(!init->parameter_count && !entry->parameter_count);
    CHECK(init->name_length == 5 && !memcmp(init->name, "$init", 5));
    CHECK(entry->name_length == 6 && !memcmp(entry->name, "$entry", 6));
    uint32_t aggregates = 0, fields = 0, prints = 0;
    XrXirType box = 0;
    for (uint32_t i = 0; i < init->instruction_count; ++i) {
        const XrXirInstruction *op = &init->instructions[i];
        if (op->op == XR_XIR_STRUCT_NEW) {
            CHECK(!aggregates++ && op->args[1] == 1 && op->args[0] < init->operand_count);
            uint32_t value = init->operands[op->args[0]];
            CHECK(value < init->instruction_count && init->instructions[value].op == XR_XIR_CONST_INT);
            CHECK(init->instructions[value].type == XR_XIR_I64 && init->instructions[value].immediate == 41);
            box = op->type;
            const XrXirTypeNode *node = xr_xir_type_node(module->types, box);
            CHECK(xr_xir_type_is_struct(module->types, box) && node && node->kind == XR_XIR_TYPE_NOMINAL);
            CHECK(node->nominal.argument_count == 1 && node->nominal.arguments[0] == XR_XIR_I64);
            CHECK(node->nominal.field_count == 1 && node->nominal.fields[0] == XR_XIR_I64);
            const XrXirNominalDeclaration *declaration = &module->types->nominals->declarations[node->nominal.declaration];
            CHECK(declaration->name.length == 3 && !memcmp(declaration->name.bytes, "Box", 3));
            CHECK(declaration->fields[0].name.length == 5 && !memcmp(declaration->fields[0].name.bytes, "value", 5));
        } else if (op->op == XR_XIR_STRUCT_GET) {
            CHECK(!fields++ && box && op->type == XR_XIR_I64 && !op->args[1]);
            CHECK(xr_xir_operand_type(init, op->args[0]) == box);
        } else if (op->op == XR_XIR_PRINT) {
            CHECK(!prints++ && op->args[1] == 1 && op->args[0] < init->operand_count);
            uint32_t value = init->operands[op->args[0]];
            CHECK(value < init->instruction_count && init->instructions[value].op == XR_XIR_ADD_INT);
            const XrXirInstruction *sum = &init->instructions[value];
            CHECK(sum->args[0] < init->instruction_count && init->instructions[sum->args[0]].op == XR_XIR_STRUCT_GET);
            CHECK(sum->args[1] < init->instruction_count && init->instructions[sum->args[1]].op == XR_XIR_CONST_INT);
            CHECK(init->instructions[sum->args[1]].type == XR_XIR_I64 && init->instructions[sum->args[1]].immediate == 1);
        }
    }
    CHECK(aggregates == 1 && fields == 1 && prints == 1);
}
