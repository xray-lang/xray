/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_array_c_fixture.h - Dynamic typed Array calls, COW cells and module payloads
 */
#ifndef LIR_ARRAY_C_FIXTURE_H
#define LIR_ARRAY_C_FIXTURE_H
typedef struct CArrayFixture { XrLirModule *module; XrLirType element; } CArrayFixture;

static XrLirSource ca_source(void) { return (XrLirSource){"array-c.xr", 10, 3, 1, 3, 9}; }
static XrLirValue ca_constant(CArrayFixture *c, XrLirBlock b, int64_t value) {
    XrLirValue output = {0};
    XrLirInstruction op = {.op=XR_LIR_CONST, .type=XR_LIR_I64, .bits=(uint64_t)value};
    OK(xr_lir_emit_value(c->module, b, &op, &output));
    return output;
}
static XrLirValue ca_value(CArrayFixture *c, XrLirBlock b, XrLirOp op,
    XrLirType type, const XrLirValue *args, uint32_t count) {
    XrLirValue output = {0};
    XrLirInstruction instruction = {.op=op, .type=type, .operands=args, .operand_count=count, .source=ca_source()};
    OK(xr_lir_emit_value(c->module, b, &instruction, &output));
    return output;
}
static void ca_effect(CArrayFixture *c, XrLirBlock b, XrLirOp op, XrLirValue value) {
    XrLirInstruction instruction = {.op=op, .type=XR_LIR_UNIT, .operands=&value,
        .operand_count=1, .source=ca_source()};
    OK(xr_lir_emit_effect(c->module, b, &instruction));
}
static XrLirValue ca_intrinsic(CArrayFixture *c, XrLirBlock b, XrLirArrayOperation operation,
    XrLirType type, const XrLirValue *args, uint32_t count) {
    XrLirValue output = {0};
    uint32_t id = 0;
    OK(xr_lir_array_operation(c->module, c->element, operation, &id));
    XrLirInstruction op = {.op=XR_LIR_CALL_INTRINSIC, .type=type, .intrinsic=id,
        .operands=args, .operand_count=count, .source=ca_source()};
    if (type == XR_LIR_UNIT) OK(xr_lir_emit_effect(c->module, b, &op));
    else OK(xr_lir_emit_value(c->module, b, &op, &output));
    return output;
}
static void ca_return(CArrayFixture *c, XrLirBlock b, XrLirValue value) {
    XrLirInstruction op = {.op=XR_LIR_RET, .type=XR_LIR_UNIT, .operands=&value, .operand_count=1};
    OK(xr_lir_set_terminator(c->module, b, &op));
}
static XrLirValue ca_text(CArrayFixture *c, XrLirBlock b, const char *bytes, size_t length) {
    XrLirLiteral literal = {0};
    XrLirValue output = {0};
    OK(xr_lir_add_literal(c->module, bytes, length, &literal));
    XrLirInstruction op = {.op=XR_LIR_STRING_LITERAL, .type=XR_LIR_STRING,
        .bits=literal.id, .source=ca_source()};
    OK(xr_lir_emit_value(c->module, b, &op, &output));
    return output;
}
static XrLirValue ca_number(CArrayFixture *c, XrLirBlock b, XrLirValue value) {
    if (c->element == XR_LIR_I64) return value;
    XrLirValue length = ca_value(c, b, XR_LIR_STRING_LEN, XR_LIR_I64, &value, 1);
    ca_effect(c, b, XR_LIR_STRING_DROP, value);
    return length;
}
static XrLirValue ca_binary(CArrayFixture *c, XrLirBlock b, XrLirOp op, XrLirValue a, XrLirValue other) {
    XrLirValue operands[] = {a, other};
    return ca_value(c, b, op, XR_LIR_I64, operands, 2);
}

static XrLirFunction ca_helper(CArrayFixture *c, uint32_t count) {
    XrLirFunction function = {0};
    XrLirFunctionSpec spec = {.name="dynamic_array", .name_length=13, .parameters=&c->element,
        .parameter_count=1, .result=XR_LIR_ARRAY};
    OK(xr_lir_declare_function(c->module, &spec, &function));
    XrLirArrayShape shape = {c->element};
    OK(xr_lir_set_array_result(c->module, function, &shape));
    OK(xr_lir_define_function(c->module, function));
    XrLirBlock entry = {0};
    OK(xr_lir_add_block(c->module, function, NULL, 0, &entry));
    OK(xr_lir_set_entry(c->module, function, entry));
    XrLirValue parameter = {0}, values[300];
    OK(xr_lir_function_parameter(c->module, function, 0, &parameter));
    CHECK(count <= 300);
    for (uint32_t p = 0; p < count; ++p)
        values[p] = c->element == XR_LIR_I64 ? ca_binary(c, entry, XR_LIR_ADD,
            parameter, ca_constant(c, entry, p)) : parameter;
    XrLirValue array = ca_intrinsic(c, entry, XR_LIR_ARRAY_OP_NEW, XR_LIR_ARRAY, values, count);
    ca_return(c, entry, array);
    return function;
}

static void ca_module(CArrayFixture *c) {
    XrLirStateModule module = {0};
    XrLirStateSlot slot = {.payload=XR_LIR_ARRAY, .mutable=true, .size=8, .alignment=8,
        .array_element=c->element};
    XrLirStateLayout layout = {.modules=&module, .module_count=1, .slots=&slot, .slot_count=1,
        .payload_size=8, .payload_alignment=8};
    OK(xr_lir_set_state_layout(c->module, &layout));
}

static XrLirFunction ca_array_copy(CArrayFixture *c) {
    XrLirType array=XR_LIR_ARRAY;XrLirFunction function={0};
    XrLirFunctionSpec spec={.name="array_copy",.name_length=10,.parameters=&array,.parameter_count=1,.result=XR_LIR_ARRAY};
    OK(xr_lir_declare_function(c->module,&spec,&function));
    XrLirArrayShape shape={c->element};OK(xr_lir_set_array_result(c->module,function,&shape));
    XrLirValue parameter={0};OK(xr_lir_function_parameter(c->module,function,0,&parameter));
    OK(xr_lir_set_array_shape(c->module,parameter,&shape));
    OK(xr_lir_define_function(c->module,function));XrLirBlock block={0};
    OK(xr_lir_add_block(c->module,function,NULL,0,&block));OK(xr_lir_set_entry(c->module,function,block));
    ca_return(c,block,ca_value(c,block,XR_LIR_ARRAY_RETAIN,XR_LIR_ARRAY,&parameter,1));
    return function;
}

static void ca_repeat(CArrayFixture *c, XrLirBlock entry, const char *name) {
    int64_t length = strstr(name, "empty") ? 0 : strstr(name, "negative") ? -1 : 3;
    XrLirValue fill = c->element == XR_LIR_I64 ? ca_constant(c, entry, 7) : ca_text(c, entry, "old!", 4);
    XrLirValue args[] = {ca_constant(c, entry, length), fill};
    XrLirValue array = ca_intrinsic(c, entry, XR_LIR_ARRAY_OP_REPEAT, XR_LIR_ARRAY, args, 2);
    if (c->element == XR_LIR_STRING) ca_effect(c, entry, XR_LIR_STRING_DROP, fill);
    XrLirValue total = ca_intrinsic(c, entry, XR_LIR_ARRAY_OP_LEN, XR_LIR_I64, &array, 1);
    if (length > 0) {
        XrLirValue saved = ca_value(c, entry, XR_LIR_ARRAY_RETAIN, XR_LIR_ARRAY, &array, 1);
        XrLirValue replacement = c->element == XR_LIR_I64 ? ca_constant(c, entry, 9) : ca_text(c, entry, "new", 3);
        XrLirValue index = ca_constant(c, entry, 0);
        XrLirValue cell = ca_value(c, entry, XR_LIR_CELL_NEW, XR_LIR_ARRAY_CELL, &array, 1);
        ca_effect(c, entry, XR_LIR_ARRAY_DROP, array);
        XrLirValue write[] = {cell, index, replacement};
        (void)ca_intrinsic(c, entry, XR_LIR_ARRAY_OP_SET, XR_LIR_UNIT, write, 3);
        if (c->element == XR_LIR_STRING) ca_effect(c, entry, XR_LIR_STRING_DROP, replacement);
        array = ca_value(c, entry, XR_LIR_CELL_READ, XR_LIR_ARRAY, &cell, 1);
        ca_effect(c, entry, XR_LIR_ARRAY_CELL_DROP, cell);
        XrLirValue read[] = {saved, index};
        XrLirValue before = ca_number(c, entry, ca_intrinsic(c, entry, XR_LIR_ARRAY_OP_GET, c->element, read, 2));
        read[0] = array;
        XrLirValue after = ca_number(c, entry, ca_intrinsic(c, entry, XR_LIR_ARRAY_OP_GET, c->element, read, 2));
        total = ca_binary(c, entry, XR_LIR_ADD, total, ca_binary(c, entry, XR_LIR_ADD, before, after));
        ca_effect(c, entry, XR_LIR_ARRAY_DROP, saved);
    }
    ca_effect(c, entry, XR_LIR_ARRAY_DROP, array);
    ca_return(c, entry, total);
}

static void array_c_fixture(const char *name, XrLirModule **output, XrLirFunction *root) {
    bool string = strstr(name, "string") != NULL, state = !strncmp(name, "state-", 6);
    bool layout_only = !strcmp(name, "state-layout"), returned = !strncmp(name, "return-", 7);
    CArrayFixture c = {.element=string ? XR_LIR_STRING : XR_LIR_I64};
    OK(xr_lir_array_module_create(NULL, &c.module));
    if (state) ca_module(&c);
    XrLirType state_type = XR_LIR_STATE;
    XrLirFunctionSpec spec = {.name="array_root", .name_length=10, .result=returned ? XR_LIR_ARRAY : XR_LIR_I64,
        .parameters=state ? &state_type : NULL, .parameter_count=state ? 1u : 0u};
    OK(xr_lir_declare_function(c.module, &spec, root));
    if (state) OK(xr_lir_set_state_entry(c.module, *root));
    if (returned) {
        XrLirArrayShape shape = {c.element};
        OK(xr_lir_set_array_result(c.module, *root, &shape));
    }
    OK(xr_lir_define_function(c.module, *root));
    XrLirBlock entry = {0};
    OK(xr_lir_add_block(c.module, *root, NULL, 0, &entry));
    OK(xr_lir_set_entry(c.module, *root, entry));
    if (!strncmp(name, "repeat-", 7)) {
        ca_repeat(&c, entry, name);
        OK(xr_lir_module_finish(c.module, NULL)); *output = c.module; return;
    }
    if (layout_only) {
        ca_return(&c, entry, ca_constant(&c, entry, 9));
        OK(xr_lir_module_finish(c.module, NULL)); *output = c.module; return;
    }
    if (!strncmp(name,"array-spills",12)) {
        XrLirValue arrays[300];
        XrLirValue shared=string?ca_text(&c,entry,"value",5):(XrLirValue){0};
        for(uint32_t p=0;p<300;++p) {
            XrLirValue seed=string?shared:ca_constant(&c,entry,p);
            arrays[p]=ca_intrinsic(&c,entry,XR_LIR_ARRAY_OP_NEW,XR_LIR_ARRAY,&seed,1);
        }
        if(string)ca_effect(&c,entry,XR_LIR_STRING_DROP,shared);
        XrLirValue index=ca_constant(&c,entry,0),total=ca_constant(&c,entry,0);
        for(uint32_t p=0;p<300;++p) {
            XrLirValue args[]={arrays[p],index};
            XrLirValue value=ca_number(&c,entry,ca_intrinsic(&c,entry,XR_LIR_ARRAY_OP_GET,c.element,args,2));
            total=ca_binary(&c,entry,XR_LIR_ADD,total,value);
            ca_effect(&c,entry,XR_LIR_ARRAY_DROP,arrays[p]);
        }
        ca_return(&c,entry,total);
        OK(xr_lir_module_finish(c.module,NULL));*output=c.module;return;
    }
    XrLirValue state_value = {0}, reference = {0};
    if (state) {
        OK(xr_lir_function_parameter(c.module, *root, 0, &state_value));
        ca_effect(&c, entry, XR_LIR_MODULE_BEGIN, state_value);
        reference = ca_value(&c, entry, XR_LIR_STATE_REFERENCE, XR_LIR_STATE_REF, &state_value, 1);
        if (!strcmp(name, "state-uninitialized-read")) {
            XrLirValue unread = {0};
            XrLirInstruction op = {.op=XR_LIR_STATE_READ, .type=XR_LIR_ARRAY,
                .operands=&reference, .operand_count=1, .source={"array-c.xr", 10, 37, 9, 37, 15}};
            OK(xr_lir_emit_value(c.module, entry, &op, &unread));
            ca_effect(&c, entry, XR_LIR_ARRAY_DROP, unread);
        }
    }
    XrLirFunction helper = ca_helper(&c, !strcmp(name, "large") ? 300u : 2u);
    XrLirValue seed = string ? ca_text(&c, entry, "ab\0cd", 5) : ca_constant(&c, entry, 11);
    XrLirValue array = {0};
    XrLirInstruction call = {.op=XR_LIR_CALL, .type=XR_LIR_ARRAY, .callee=helper,
        .operands=&seed, .operand_count=1, .source=ca_source()};
    OK(xr_lir_emit_value(c.module, entry, &call, &array));
    if (string) ca_effect(&c, entry, XR_LIR_STRING_DROP, seed);
    XrLirFunction copy=ca_array_copy(&c);XrLirValue copied={0};
    call=(XrLirInstruction){.op=XR_LIR_CALL,.type=XR_LIR_ARRAY,.callee=copy,
        .operands=&array,.operand_count=1,.source=ca_source()};
    OK(xr_lir_emit_value(c.module,entry,&call,&copied));ca_effect(&c,entry,XR_LIR_ARRAY_DROP,array);array=copied;
    if(!strncmp(name,"phi-",4)) {
        XrLirBlock join={0};XrLirType type=XR_LIR_ARRAY;XrLirValue selected={0},condition={0};
        OK(xr_lir_add_block(c.module,*root,&type,1,&join));
        OK(xr_lir_block_parameter(c.module,join,0,&selected));
        XrLirArrayShape shape={c.element};OK(xr_lir_set_array_shape(c.module,selected,&shape));
        XrLirInstruction op={.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1};
        OK(xr_lir_emit_value(c.module,entry,&op,&condition));
        op=(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
            .edge_count=2,.edges={{join,&array,1},{join,&array,1}}};
        OK(xr_lir_set_terminator(c.module,entry,&op));entry=join;array=selected;
    }
    if (returned) {
        ca_return(&c, entry, array);
        OK(xr_lir_module_finish(c.module, NULL)); *output = c.module; return;
    }
    if (state) {
        XrLirValue values[] = {state_value, array};
        if (!strcmp(name, "state-uninitialized-write")) {
            XrLirValue write[] = {reference, array};
            XrLirInstruction op = {.op=XR_LIR_STATE_WRITE, .type=XR_LIR_UNIT,
                .operands=write, .operand_count=2, .source={"array-c.xr", 10, 41, 7, 41, 14}};
            OK(xr_lir_emit_effect(c.module, entry, &op));
        }
        XrLirInstruction op = {.op=XR_LIR_STATE_CONSTRUCT, .type=XR_LIR_STATE_REF,
            .operands=values, .operand_count=2, .source=ca_source()};
        XrLirValue initializer = {0};
        OK(xr_lir_emit_value(c.module, entry, &op, &initializer));
        ca_effect(&c, entry, XR_LIR_STATE_PUBLISH, initializer);
        ca_effect(&c, entry, XR_LIR_MODULE_READY, state_value);
    }
    XrLirValue empty = ca_intrinsic(&c, entry, XR_LIR_ARRAY_OP_NEW, XR_LIR_ARRAY, NULL, 0);
    ca_effect(&c, entry, XR_LIR_ARRAY_DROP, empty);
    XrLirValue saved = ca_value(&c, entry, XR_LIR_ARRAY_RETAIN, XR_LIR_ARRAY, &array, 1);
    XrLirValue cell = ca_value(&c, entry, XR_LIR_CELL_NEW, XR_LIR_ARRAY_CELL, &array, 1);
    ca_effect(&c, entry, XR_LIR_ARRAY_DROP, array);
    XrLirValue tail = string ? ca_text(&c, entry, "tail", 4) : ca_constant(&c, entry, 33);
    XrLirValue args[3] = {cell, tail};
    (void)ca_intrinsic(&c, entry, XR_LIR_ARRAY_OP_PUSH, XR_LIR_UNIT, args, 2);
    if (string) ca_effect(&c, entry, XR_LIR_STRING_DROP, tail);
    XrLirValue snapshot = ca_value(&c, entry, XR_LIR_CELL_READ, XR_LIR_ARRAY, &cell, 1);
    XrLirValue index = ca_constant(&c, entry, 0);
    XrLirValue replacement = string ? ca_text(&c, entry, "changed!", 8) : ca_constant(&c, entry, 99);
    args[0] = cell; args[1] = index; args[2] = replacement;
    (void)ca_intrinsic(&c, entry, XR_LIR_ARRAY_OP_SET, XR_LIR_UNIT, args, 3);
    if (string) ca_effect(&c, entry, XR_LIR_STRING_DROP, replacement);
    XrLirValue current = ca_value(&c, entry, XR_LIR_CELL_READ, XR_LIR_ARRAY, &cell, 1);
    if (state) {
        args[0] = reference; args[1] = current;
        XrLirInstruction op = {.op=XR_LIR_STATE_WRITE, .type=XR_LIR_UNIT, .operands=args,
            .operand_count=2, .source=ca_source()};
        OK(xr_lir_emit_effect(c.module, entry, &op));
        ca_effect(&c, entry, XR_LIR_ARRAY_DROP, current);
        current = ca_value(&c, entry, XR_LIR_STATE_READ, XR_LIR_ARRAY, &reference, 1);
    }
    args[0] = saved; args[1] = index;
    XrLirValue first = ca_number(&c, entry, ca_intrinsic(&c, entry, XR_LIR_ARRAY_OP_GET, c.element, args, 2));
    args[0] = current;
    XrLirValue second = ca_number(&c, entry, ca_intrinsic(&c, entry, XR_LIR_ARRAY_OP_GET, c.element, args, 2));
    XrLirValue length = ca_intrinsic(&c, entry, XR_LIR_ARRAY_OP_LEN, XR_LIR_I64, &current, 1);
    XrLirValue scaled = ca_binary(&c, entry, XR_LIR_MUL, first, ca_constant(&c, entry, 100));
    XrLirValue checksum = ca_binary(&c, entry, XR_LIR_ADD, ca_binary(&c, entry, XR_LIR_ADD, scaled, second), length);
    ca_effect(&c, entry, XR_LIR_ARRAY_DROP, saved);
    ca_effect(&c, entry, XR_LIR_ARRAY_DROP, snapshot);
    ca_effect(&c, entry, XR_LIR_ARRAY_DROP, current);
    ca_effect(&c, entry, XR_LIR_ARRAY_CELL_DROP, cell);
    ca_return(&c, entry, checksum);
    OK(xr_lir_module_finish(c.module, NULL));
    *output = c.module;
}
#endif // LIR_ARRAY_C_FIXTURE_H
