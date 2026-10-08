/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_product_module_class_error.c - Preserve class error publication and distinguish admission blockers
 *
 * KEY CONCEPT:
 *   Original class error publication remains a positive expectation.
 *   Unexpected rejection is
 *   reported as failure after full owner cleanup, never blessed as a new oracle.
 */
#include "xir/xxir_checked.h"
#include "xir/xxir_declarations.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_source_program_compile_owner.h"
#include "module_class_error_cases.h"
_Static_assert(XR_XIR_CHECKED_SCHEMA == 25u && XR_XIR_CHECKED_CONTRACT == 67u, "Current Checked identity");
_Static_assert(XR_XIR_UNIT == 0 && XR_XIR_BOOL == 1 && XR_XIR_I64 == 2 && XR_XIR_STRING == 3 &&
    XR_XIR_TYPE_TUPLE == 6 && XR_XIR_TYPE_CELL == 3 && XR_XIR_CONSTRUCTED_TYPE_BASE == 256, "Independent slot representation");
_Static_assert(XR_XIR_THROW == 30 && XR_XIR_SUSPEND == 29 && XR_XIR_CLASS_NEW == 118, "Independent error operations");
_Static_assert(sizeof(module_class_error_cases)/sizeof(module_class_error_cases[0]) == 6, "Complete byte obligation matrix");

static void exact_owner(const XrXirArtifact *owner, const ModuleClassErrorCase *test) {
    CHECK(xr_xir_compile_artifact_verify(owner, NULL) == XR_XIR_OK);
    const XrXirModule *m = xr_xir_compile_artifact_module(owner);
    CHECK(m && m->function_count == 9 && m->declarations && m->types);
    const XrXirDeclarations *d = m->declarations;
    CHECK(d->module_count == 4 && d->slot_count == 4 && d->literal_count == 2);
    CHECK(d->root_module == 3 && d->entry_function == 4 && xr_xir_type_is_class(m->types, (XrXirType)256));
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(d->modules[i].initializer == i && d->slots[i].module == test->owner);
        CHECK(d->slots[i].type == 256 && d->slots[i].mutable == 1);
    }
    const XrXirFunction *f = &m->functions[test->owner];
    CHECK(!f->parameter_count && f->result == XR_XIR_UNIT && f->block_count == 1);
    CHECK(f->instruction_count == 19 + test->delayed && f->operand_count == 3);
    if (test->delayed) CHECK(f->instructions[0].op == XR_XIR_SUSPEND);
    const unsigned slots[] = {1,0,3};
    for (unsigned i = 0; i < 3; ++i) {
        unsigned k = test->delayed + 6*i;
        CHECK(f->instructions[k].op == XR_XIR_CONST_STRING);
        CHECK(f->instructions[k+1].op == XR_XIR_CLASS_NEW && f->instructions[k+1].type == 256);
        CHECK(f->instructions[k+2].op == XR_XIR_COPY && f->instructions[k+2].args[0] == k+1);
        CHECK(f->instructions[k+3].op == XR_XIR_SLOT_INIT && f->instructions[k+3].immediate == slots[i]);
        CHECK(f->instructions[k+5].op == XR_XIR_CLASS_SET && f->instructions[k+5].args[0] == k+1);
    }
    const XrXirInstruction *last = &f->instructions[f->instruction_count-1];
    CHECK(last->op == (test->throws ? XR_XIR_THROW : XR_XIR_RETURN));
    if (test->throws) CHECK(last->args[0] == test->delayed+13);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(owner, &packet, NULL) == XR_XIR_OK);
    CHECK(packet.length == test->length && !memcmp(packet.bytes, test->bytes, test->length));
    xr_xir_compile_checked_packet_free(&packet);
}

int main(void) {
    const XrXirCompileContext *context = source_program_owner(67108864, 128000000);
    XrXirArtifact *owner = NULL;
    CHECK(xr_xir_compile_checked_read(context, module_class_error_cases[0].bytes,
        module_class_error_cases[0].length, &owner, NULL) == XR_XIR_OK);
    exact_owner(owner, &module_class_error_cases[0]);
    unsigned mismatches = 0;
    for (size_t c = 0; c < 6; ++c) for (unsigned occupied = 0; occupied < 2; ++occupied) {
        const ModuleClassErrorCase *test = &module_class_error_cases[c];
        uint8_t *input = xr_malloc(test->length); CHECK(input); memcpy(input, test->bytes, test->length);
        size_t live = source_program_compile_live, bytes = source_program_compile_bytes;
        XrXirArtifact *output = occupied ? owner : NULL, *before = output;
        XrXirDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_compile_checked_read(context, input, test->length, &output, &diagnostic);
        CHECK(!memcmp(input, test->bytes, test->length));
        memset(input, 0xa5, test->length); xr_free(input);
        if (status == XR_XIR_OK) {
            CHECK(output && output != owner);
            exact_owner(output, test);
            xr_xir_compile_artifact_free(output);
        } else CHECK(output == before && diagnostic.status == status);
        CHECK(source_program_compile_live == live && source_program_compile_bytes == bytes);
        exact_owner(owner, &module_class_error_cases[0]);
        bool match = status == XR_XIR_OK;
        mismatches += match ? 0u : 1u;
        printf("module-class-error case=%s occupied=%u expected=%u actual=%u function=%u block=%u instruction=%u reason=%u input-dead=1 owner-preserved=1 physical-refund=1 result=%s\n",
            test->name, occupied, (unsigned)XR_XIR_OK, status, diagnostic.function, diagnostic.block, diagnostic.instruction, diagnostic.reason, match ? "PASS" : "FAIL");
    }
    xr_xir_compile_artifact_free(owner);
    source_program_owners_free();
    CHECK(!source_program_compile_allocations && !source_program_compile_capacity);
    printf("module-class-error reads=12 mismatches=%u compiler-physical=0/0 table=0 result=%s\n",
        mismatches, mismatches ? "FAIL" : "PASS");
    return mismatches ? 1 : 0;
}
