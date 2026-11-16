/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_block_facts_faults.c - Unpublished CFG fact allocation refunds
 */
#include "base/xmalloc.h"
#include "lir/xlir_builder.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
typedef union FactAllocation { long double alignment;void *pointer;size_t bytes; } FactAllocation;
static size_t fact_attempts,fact_fail_at=SIZE_MAX,fact_live,fact_bytes;
static void *fact_allocate(size_t bytes) {
    if (fact_attempts++==fact_fail_at || bytes>SIZE_MAX-sizeof(FactAllocation)) return NULL;
    FactAllocation *owned=xr_malloc(sizeof(*owned)+bytes);if (!owned) return NULL;
    owned->bytes=bytes;++fact_live;fact_bytes+=bytes;return owned+1;
}
static void *fact_zero(size_t count,size_t size) {
    if (size && count>SIZE_MAX/size) return NULL;
    size_t bytes=count*size;void *result=fact_allocate(bytes);if (result) memset(result,0,bytes);return result;
}
static void fact_release(void *pointer) {
    if (!pointer) return;
    FactAllocation *owned=(FactAllocation *)pointer-1;
    REQUIRE(fact_live && fact_bytes>=owned->bytes);--fact_live;fact_bytes-=owned->bytes;xr_free(owned);
}
/* The production builder keeps its ordinary provider. Only the actual CFG
 * fact producer and its matching retirement are injected into this unit. */
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc(bytes) fact_allocate(bytes)
#define xr_calloc(count,size) fact_zero(count,size)
#define xr_free(pointer) fact_release(pointer)
#include "lir/xlir_verify.c"
#define OK(c) REQUIRE((c)==XR_LIR_OK)
#define CHECK(c) REQUIRE(c)
#include "lir_slice_cleanup_fixture.h"
#undef CHECK
#undef OK

static XrLirModule *fact_module(XrLirBlock *late) {
    XrLirModule *module=NULL;REQUIRE(xr_lir_module_create(NULL,&module)==XR_LIR_OK);
    for (uint32_t f=0;f<2;++f) {
        XrLirFunction function={0};XrLirBlock entry={0};
        REQUIRE(xr_lir_declare_function(module,&(XrLirFunctionSpec){.name=f ? "second" : "actual",.name_length=6,
            .result=XR_LIR_I64},&function)==XR_LIR_OK);REQUIRE(xr_lir_define_function(module,function)==XR_LIR_OK);
        REQUIRE(xr_lir_add_block(module,function,NULL,0,&entry)==XR_LIR_OK);
        REQUIRE(xr_lir_set_entry(module,function,entry)==XR_LIR_OK);
        REQUIRE(xr_lir_add_block(module,function,NULL,0,&late[f])==XR_LIR_OK);
        REQUIRE(xr_lir_set_terminator(module,entry,&(XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,
            .edge_count=1,.edges={{late[f],NULL,0}}})==XR_LIR_OK);
        XrLirValue seed={0},cell={0},value={0};
        REQUIRE(xr_lir_emit_value(module,late[f],&(XrLirInstruction){.op=XR_LIR_CONST,.type=XR_LIR_I64,
            .bits=f+7},&seed)==XR_LIR_OK);
        REQUIRE(xr_lir_emit_value(module,late[f],&(XrLirInstruction){.op=XR_LIR_CELL_NEW,.type=XR_LIR_I64_CELL,
            .operands=&seed,.operand_count=1},&cell)==XR_LIR_OK);
        REQUIRE(xr_lir_emit_value(module,late[f],&(XrLirInstruction){.op=XR_LIR_CELL_READ,.type=XR_LIR_I64,
            .operands=&cell,.operand_count=1},&value)==XR_LIR_OK);
        REQUIRE(xr_lir_set_terminator(module,late[f],&(XrLirInstruction){.op=XR_LIR_RET,.type=XR_LIR_UNIT,
            .operands=&value,.operand_count=1})==XR_LIR_OK);
    }
    return module;
}
static void cleanup_rank_failures(void) {
    size_t sites=0;
    for (size_t point=0;!point || point<=sites;++point) {
        fact_fail_at=SIZE_MAX;
        SliceCleanupFixture c=slice_cleanup_fixture(XR_LIR_I64,SLICE_CLEANUP_BRANCH,false);
        fact_attempts=0;fact_fail_at=point?point-1:SIZE_MAX;
        XrLirStatus status=xr_lir_module_finish(c.module,NULL);
        if (!point) {
            sites=fact_attempts;REQUIRE(status==XR_LIR_OK && sites);
            REQUIRE(!c.module->functions[c.root.id-1].block_cleanup_rank);
            const uint32_t *rank=c.module->functions[c.worker.id-1].block_cleanup_rank;
            REQUIRE(rank && rank[c.definition.id-1]<rank[c.cleanup.id-1]);
        } else {
            REQUIRE(status==XR_LIR_OUT_OF_MEMORY);
            for (uint32_t f=0;f<c.module->function_count;++f)
                REQUIRE(!c.module->functions[f].block_multiplicity &&
                    !c.module->functions[f].block_cleanup_rank && !c.module->functions[f].block_multiplicity_count);
            bool sentinel=true;
            REQUIRE(xr_lir_block_may_repeat(c.module,c.definition,&sentinel)!=XR_LIR_OK && sentinel);
        }
        fact_fail_at=SIZE_MAX;xr_lir_module_destroy(c.module);REQUIRE(!fact_live && !fact_bytes);
    }
    printf("Cleanup rank publication covers %zu real allocation failures with exact refunds\n",sites);
}
int main(void) {
    size_t sites=0;
    for (size_t point=0;!point || point<=sites;++point) {
        fact_fail_at=SIZE_MAX;XrLirBlock late[2];XrLirModule *module=fact_module(late);
        fact_attempts=0;fact_fail_at=point?point-1:SIZE_MAX;
        XrLirStatus status=xr_lir_module_finish(module,NULL);
        if (!point) {
            sites=fact_attempts;REQUIRE(status==XR_LIR_OK && sites);
            for (uint32_t f=0;f<2;++f) {bool repeat=true;REQUIRE(xr_lir_block_may_repeat(module,late[f],&repeat)==XR_LIR_OK && !repeat);}
        } else {
            REQUIRE(status==XR_LIR_OUT_OF_MEMORY);
            for (uint32_t f=0;f<2;++f) {
                bool sentinel=true;REQUIRE(xr_lir_block_may_repeat(module,late[f],&sentinel)!=XR_LIR_OK && sentinel);
                REQUIRE(!module->functions[f].block_multiplicity && !module->functions[f].block_cleanup_rank &&
                    !module->functions[f].block_multiplicity_count);
            }
        }
        fact_fail_at=SIZE_MAX;xr_lir_module_destroy(module);REQUIRE(!fact_live && !fact_bytes);
    }
    cleanup_rank_failures();
    printf("CFG fact publication covers %zu real allocation failures with exact refunds\n",sites);return 0;
}
