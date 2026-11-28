/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_array_hold.c - Held Array chains: shape, aliasing and ownership
 */
#include "lir/xlir_array.h"
#include "lir/xlir_builder.h"
#include "lir/xlir_print.h"
#include "lir/xlir_rebuild.h"
#include "lir/xlir_verify.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

typedef enum HoldMode {
    HOLD_VALID,
    HOLD_DROP_ALIAS,        /* a held value is released directly */
    HOLD_EDGE_UNHELD,       /* an unheld owner enters a held parameter */
    HOLD_PARAMETER_UNHELD,  /* a held owner enters an unheld parameter */
    HOLD_UNHELD_OPERATION,  /* a held operation reads an unheld owner */
    HOLD_TWICE,             /* hold takes a held owner */
    HOLD_MISSING_UNHOLD,    /* the held owner is never retired */
    HOLD_PUSH_ELEMENT,      /* a push stores the wrong element type */
    HOLD_STRING_ELEMENT,    /* String elements are not holdable */
    HOLD_MODE_COUNT
} HoldMode;

typedef struct Hold { XrLirModule *m; XrLirFunction f; XrLirStatus emit; } Hold;

static XrLirValue value(Hold *h, XrLirBlock b, XrLirInstruction i) {
    XrLirValue result = {0};
    if (h->emit == XR_LIR_OK) h->emit = xr_lir_emit_value(h->m, b, &i, &result);
    return result;
}
static void effect(Hold *h, XrLirBlock b, XrLirInstruction i) {
    if (h->emit == XR_LIR_OK) h->emit = xr_lir_emit_effect(h->m, b, &i);
}
static XrLirValue number(Hold *h, XrLirBlock b, XrLirType type, uint64_t bits) {
    return value(h, b, (XrLirInstruction){.op=XR_LIR_CONST,.type=type,.bits=bits});
}
static XrLirBlock block(Hold *h, XrLirType element, bool held, XrLirValue out[2], uint32_t count) {
    const XrLirType types[] = {XR_LIR_ARRAY, XR_LIR_I64};
    XrLirBlock b = {0};
    OK(xr_lir_add_block(h->m, h->f, types, count, &b));
    for (uint32_t n = 0; n < count; ++n) OK(xr_lir_block_parameter(h->m, b, n, &out[n]));
    OK(xr_lir_set_array_shape(h->m, out[0], &(XrLirArrayShape){element}));
    if (held) OK(xr_lir_set_array_held(h->m, out[0]));
    return b;
}
static void branch(Hold *h, XrLirBlock from, XrLirBlock to, XrLirValue *arguments, uint32_t count) {
    if (h->emit == XR_LIR_OK)
        h->emit = xr_lir_set_terminator(h->m, from, &(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
            .edge_count=1,.edges={{to,arguments,count}}});
}

/* entry: new, hold; loop: push i while i < n; exit: len + a[0], unhold, drop. */
static XrLirStatus hold_build(HoldMode mode, XrLirModule **output, char reason[192]) {
    Hold h = {0};
    XrLirType element = mode == HOLD_STRING_ELEMENT ? XR_LIR_STRING : XR_LIR_I64;
    OK(xr_lir_array_module_create(NULL, &h.m));
    const XrLirType parameters[] = {XR_LIR_I64};
    OK(xr_lir_declare_function(h.m, &(XrLirFunctionSpec){.name="held",.name_length=4,.parameters=parameters,
        .parameter_count=1,.result=XR_LIR_I64}, &h.f));
    OK(xr_lir_define_function(h.m, h.f));
    XrLirValue n = {0}; OK(xr_lir_function_parameter(h.m, h.f, 0, &n));
    XrLirBlock entry = {0}; OK(xr_lir_add_block(h.m, h.f, NULL, 0, &entry)); OK(xr_lir_set_entry(h.m, h.f, entry));
    bool held_params = mode != HOLD_PARAMETER_UNHELD && mode != HOLD_STRING_ELEMENT;
    XrLirValue head[2], body[2], tail[2];
    XrLirBlock header = block(&h, element, held_params, head, 2);
    XrLirBlock loop = block(&h, element, held_params, body, 2);
    XrLirBlock done = block(&h, element, held_params, tail, 1);

    uint32_t create = 0; OK(xr_lir_array_operation(h.m, element, XR_LIR_ARRAY_OP_NEW, &create));
    XrLirValue array = value(&h, entry, (XrLirInstruction){.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_ARRAY,
        .intrinsic=create});
    XrLirValue chain = mode == HOLD_EDGE_UNHELD ? array : value(&h, entry, (XrLirInstruction){.op=XR_LIR_ARRAY_HOLD,
        .type=XR_LIR_ARRAY,.operands=&array,.operand_count=1});
    XrLirValue start[] = {chain, number(&h, entry, XR_LIR_I64, 0)};
    branch(&h, entry, header, start, 2);

    XrLirValue compare[] = {head[1], n};
    XrLirValue more = value(&h, header, (XrLirInstruction){.op=XR_LIR_ICMP,.type=XR_LIR_I1,.predicate=XR_LIR_LT,
        .operands=compare,.operand_count=2});
    if (h.emit == XR_LIR_OK)
        h.emit = xr_lir_set_terminator(h.m, header, &(XrLirInstruction){.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,
            .operands=&more,.operand_count=1,.edge_count=2,.edges={{loop,head,2},{done,head,1}}});

    XrLirValue item = mode == HOLD_PUSH_ELEMENT ? number(&h, loop, XR_LIR_F64, 0) : body[1];
    XrLirValue push[] = {body[0], item};
    effect(&h, loop, (XrLirInstruction){.op=XR_LIR_ARRAY_HELD_PUSH,.type=XR_LIR_UNIT,.operands=push,.operand_count=2});
    XrLirValue step[] = {body[1], number(&h, loop, XR_LIR_I64, 1)};
    XrLirValue next[] = {body[0], value(&h, loop, (XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,
        .operands=step,.operand_count=2})};
    branch(&h, loop, header, next, 2);

    XrLirValue length = value(&h, done, (XrLirInstruction){.op=XR_LIR_ARRAY_HELD_LEN,.type=XR_LIR_I64,
        .operands=tail,.operand_count=1});
    XrLirValue at[] = {tail[0], number(&h, done, XR_LIR_I64, 0)};
    XrLirValue first = value(&h, done, (XrLirInstruction){.op=XR_LIR_ARRAY_HELD_GET,.type=XR_LIR_I64,
        .operands=at,.operand_count=2});
    if (mode == HOLD_DROP_ALIAS) {
        effect(&h, done, (XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=tail,.operand_count=1});
    } else if (mode != HOLD_MISSING_UNHOLD) {
        XrLirValue owner = value(&h, done, (XrLirInstruction){.op=mode == HOLD_TWICE ? XR_LIR_ARRAY_HOLD :
            XR_LIR_ARRAY_UNHOLD,.type=XR_LIR_ARRAY,.operands=tail,.operand_count=1});
        if (mode == HOLD_UNHELD_OPERATION)
            (void)value(&h, done, (XrLirInstruction){.op=XR_LIR_ARRAY_HELD_LEN,.type=XR_LIR_I64,
                .operands=&owner,.operand_count=1});
        effect(&h, done, (XrLirInstruction){.op=XR_LIR_ARRAY_DROP,.type=XR_LIR_UNIT,.operands=&owner,.operand_count=1});
    }
    XrLirValue sum[] = {length, first};
    XrLirValue result = value(&h, done, (XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,.operands=sum,.operand_count=2});
    if (h.emit == XR_LIR_OK)
        h.emit = xr_lir_set_terminator(h.m, done, &(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
            .operands=&result,.operand_count=1});
    XrLirDiagnostic diagnostic = {0};
    XrLirStatus status = h.emit == XR_LIR_OK ? xr_lir_module_finish(h.m, &diagnostic) : h.emit;
    snprintf(reason, 192, "%s", h.emit == XR_LIR_OK ? diagnostic.message : "construction refused");
    if (status == XR_LIR_OK) *output = h.m;
    else xr_lir_module_destroy(h.m);
    return status;
}

typedef struct Text { char bytes[4096]; size_t length; } Text;
static bool text_write(void *context, const char *bytes, size_t length) {
    Text *t = context;
    if (length > sizeof(t->bytes) - 1 - t->length) return false;
    memcpy(t->bytes + t->length, bytes, length); t->length += length; t->bytes[t->length] = 0;
    return true;
}

int main(void) {
    XrLirModule *module = NULL;
    char reason[192];
    OK(hold_build(HOLD_VALID, &module, reason));
    Text printed = {0};
    OK(xr_lir_print(module, text_write, &printed));
    if (getenv("HOLD_PRINT")) fputs(printed.bytes, stderr);
    CHECK(strstr(printed.bytes, "array_hold") && strstr(printed.bytes, "^b1(%4: array held, %5: i64)") &&
        strstr(printed.bytes, "array_held_push") && strstr(printed.bytes, "array_unhold"));
    XrLirModule *copy = NULL;
    OK(xr_lir_rebuild(module, &copy));
    Text again = {0};
    OK(xr_lir_print(copy, text_write, &again));
    CHECK(!strcmp(printed.bytes, again.bytes));
    xr_lir_module_destroy(copy);
    xr_lir_module_destroy(module);
    static const char *const reasons[HOLD_MODE_COUNT] = {
        [HOLD_DROP_ALIAS] = "held Array has an alias outside its chain",
        [HOLD_EDGE_UNHELD] = "held Array edge does not match its block parameter",
        [HOLD_PARAMETER_UNHELD] = "held Array edge does not match its block parameter",
        [HOLD_UNHELD_OPERATION] = "held Array operation uses an unheld value",
        [HOLD_TWICE] = "held Array has an alias outside its chain",
        [HOLD_MISSING_UNHOLD] = "Managed owner requires release or result transfer",
        [HOLD_PUSH_ELEMENT] = "construction refused",
        [HOLD_STRING_ELEMENT] = "construction refused"};
    for (HoldMode mode = HOLD_DROP_ALIAS; mode < HOLD_MODE_COUNT; ++mode) {
        XrLirModule *rejected = NULL;
        XrLirStatus status = hold_build(mode, &rejected, reason);
        if (status == XR_LIR_OK || strcmp(reason, reasons[mode]))
            fprintf(stderr, "held mode %d: status %d, reason \"%s\"\n", (int)mode, (int)status, reason);
        CHECK(status != XR_LIR_OK && !rejected && !strcmp(reason, reasons[mode]));
    }
    puts("held Array chains: hold, held operations and unhold verify; aliases, mismatched edges and element types refuse");
    return 0;
}
