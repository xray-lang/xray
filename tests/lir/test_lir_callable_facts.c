/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_lir_callable_facts.c - Constructor snapshots and atomic failure cleanup
 */
#include "base/xmalloc.h"
#include "lir/xlir_builder.h"
#include "lir/xlir_callable_facts.h"
#include "lir/xlir_value_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define OK(c) CHECK((c) == XR_LIR_OK)

static size_t attempt, fail_at = SIZE_MAX, facts_live, live_bytes;
typedef union FactsAllocation { long double alignment; void *pointer; size_t bytes; } FactsAllocation;
static void *facts_allocate(size_t bytes) {
    if (attempt++ == fail_at || bytes > SIZE_MAX - sizeof(FactsAllocation)) return NULL;
    FactsAllocation *allocation = xr_malloc(sizeof(*allocation) + bytes);
    if (!allocation) return NULL;
    allocation->bytes = bytes; ++facts_live; live_bytes += bytes;
    return allocation + 1;
}
static void *facts_callocate(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    void *memory = facts_allocate(count * size);
    if (memory) memset(memory, 0, count * size);
    return memory;
}
static void facts_free(void *memory) {
    if (!memory) return;
    FactsAllocation *allocation = (FactsAllocation *)memory - 1;
    CHECK(facts_live && live_bytes >= allocation->bytes);
    --facts_live; live_bytes -= allocation->bytes; xr_free(allocation);
}
static void *facts_reallocate(void *memory,size_t bytes) {
    if(!bytes) {facts_free(memory);return NULL;}
    if(!memory)return facts_allocate(bytes);
    if(attempt++==fail_at || bytes>SIZE_MAX-sizeof(FactsAllocation))return NULL;
    FactsAllocation *old=(FactsAllocation *)memory-1;size_t previous=old->bytes;
    CHECK(facts_live && live_bytes>=previous && bytes<=SIZE_MAX-(live_bytes-previous));
    FactsAllocation *allocation=xr_realloc(old,sizeof(*allocation)+bytes);
    if(!allocation)return NULL;
    allocation->bytes=bytes;live_bytes=live_bytes-previous+bytes;
    return allocation+1;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc(bytes) facts_allocate(bytes)
#define xr_calloc(count, size) facts_callocate(count, size)
#define xr_realloc(memory,bytes) facts_reallocate(memory,bytes)
#define xr_free(memory) facts_free(memory)
/* The constructor, copy and retirement resolve to this same instrumented TU. */
#include "lir/xlir_verify.c"
#include "lir/xlir_rebuild.c"

typedef struct FactsFixture {
    XrLirModule *module;
    XrLirType strong, weak;
    XrLirFunction leaf, contracts, hidden, factory, root;
    XrLirValue frame, owned, outer, inner, last_alias, bound, kept, returned;
    XrLirValue cell_bound, seed, future;
    uint32_t alias_source;
} FactsFixture;

static XrLirFunction facts_function(XrLirModule *m, const XrLirFunctionSpec *spec) {
    XrLirFunction function = {0};
    OK(xr_lir_declare_function(m, spec, &function));
    OK(xr_lir_define_function(m, function));
    return function;
}
static XrLirBlock facts_block(XrLirModule *m, XrLirFunction function) {
    XrLirBlock block = {0};
    OK(xr_lir_add_block(m, function, NULL, 0, &block));
    OK(xr_lir_set_entry(m, function, block));
    return block;
}
static XrLirValue facts_emit(XrLirModule *m, XrLirBlock block, XrLirInstruction op) {
    XrLirValue value = {0}; OK(xr_lir_emit_value(m, block, &op, &value)); return value;
}
static XrLirValue facts_parameter(XrLirModule *m, XrLirFunction function, uint32_t index) {
    XrLirValue value = {0}; OK(xr_lir_function_parameter(m, function, index, &value)); return value;
}
static void facts_return(XrLirModule *m, XrLirBlock block, XrLirValue value) {
    XrLirInstruction ret = {.op=XR_LIR_RET, .type=XR_LIR_UNIT,
        .operands=value.id ? &value : NULL, .operand_count=value.id ? 1u : 0u};
    OK(xr_lir_set_terminator(m, block, &ret));
}
static XrLirValue facts_alias(XrLirModule *m, XrLirBlock block, XrLirType type,
    XrLirValue scope, XrLirValue value, uint64_t identity) {
    XrLirValue operands[] = {scope, value};
    return facts_emit(m, block, (XrLirInstruction){.op=XR_LIR_FUNCTION_BIND, .type=type,
        .operands=operands, .operand_count=2, .bits=identity});
}

static FactsFixture facts_fixture(uint32_t aliases) {
    FactsFixture f = {0}; OK(xr_lir_module_create(NULL, &f.module));
    XrLirCallableParameter integer = {.type=XR_LIR_I64};
    XrLirCallableType signature = {.parameters=&integer, .parameter_count=1,
        .result=XR_LIR_I64, .flags=XR_LIR_CALLABLE_NO_SUSPEND};
    OK(xr_lir_callable_type(f.module, &signature, &f.strong)); signature.flags = 0;
    OK(xr_lir_callable_type(f.module, &signature, &f.weak));
    XrLirType integers[] = {XR_LIR_I64, XR_LIR_I64};
    f.leaf = facts_function(f.module, &(XrLirFunctionSpec){.name="leaf", .name_length=4,
        .parameters=integers, .parameter_count=2, .result=XR_LIR_I64});
    OK(xr_lir_set_closure_target(f.module, f.leaf, (XrLirClosureTarget){f.strong, 1, false}));
    XrLirBlock block = facts_block(f.module, f.leaf);
    facts_return(f.module, block, facts_parameter(f.module, f.leaf, 1));
    XrLirType callbacks[] = {f.weak, f.weak};
    XrLirFunctionCallableParameter contracts[] = {
        {.parameter=0, .contract={.type=f.weak}},
        {.parameter=1, .contract={.type=f.weak, .escaping=true}}};
    f.contracts = facts_function(f.module, &(XrLirFunctionSpec){.name="contracts", .name_length=9,
        .parameters=callbacks, .parameter_count=2, .result=XR_LIR_UNIT,
        .callable_parameters=contracts, .callable_parameter_count=2});
    facts_return(f.module, facts_block(f.module, f.contracts), (XrLirValue){0});
    XrLirType hidden[] = {f.weak, XR_LIR_I64};
    f.hidden = facts_function(f.module, &(XrLirFunctionSpec){.name="hidden", .name_length=6,
        .parameters=hidden, .parameter_count=2, .result=XR_LIR_I64});
    OK(xr_lir_set_closure_target(f.module, f.hidden, (XrLirClosureTarget){f.weak, 1, false}));
    facts_return(f.module, facts_block(f.module, f.hidden), facts_parameter(f.module, f.hidden, 1));
    f.factory = facts_function(f.module, &(XrLirFunctionSpec){.name="factory", .name_length=7,
        .parameters=integers, .parameter_count=1, .result=f.weak});
    block = facts_block(f.module, f.factory);
    XrLirValue seed = facts_parameter(f.module, f.factory, 0);
    XrLirInstruction reference = {.op=XR_LIR_FUNCTION_REF, .type=f.weak, .callee=f.leaf,
        .operands=&seed, .operand_count=1, .bits=XR_LIR_CLOSURE_OWNED};
    facts_return(f.module, block, facts_emit(f.module, block, reference));
    XrLirType cell = 0; OK(xr_lir_cell_type(f.module, (XrLirCellType){XR_LIR_I64, 0}, &cell));
    XrLirType cell_parameters[] = {cell, XR_LIR_I64};
    XrLirFunction cell_target = facts_function(f.module, &(XrLirFunctionSpec){.name="cell_target", .name_length=11,
        .parameters=cell_parameters, .parameter_count=2, .result=XR_LIR_I64});
    OK(xr_lir_set_closure_target(f.module, cell_target, (XrLirClosureTarget){f.strong, 1, false}));
    facts_return(f.module, facts_block(f.module, cell_target), facts_parameter(f.module, cell_target, 1));
    f.root = facts_function(f.module, &(XrLirFunctionSpec){.name="root", .name_length=4, .result=XR_LIR_UNIT});
    block = facts_block(f.module, f.root);
    f.outer = facts_emit(f.module, block, (XrLirInstruction){.op=XR_LIR_FUNCTION_SCOPE_ENTER, .type=XR_LIR_FUNCTION_SCOPE});
    f.inner = facts_emit(f.module, block, (XrLirInstruction){.op=XR_LIR_FUNCTION_SCOPE_ENTER,
        .type=XR_LIR_FUNCTION_SCOPE, .operands=&f.outer, .operand_count=1});
    f.seed = facts_emit(f.module, block, (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_I64, .bits=41});
    reference.type = f.strong; reference.operands = &f.seed; reference.bits = XR_LIR_CLOSURE_FRAME;
    f.frame = facts_emit(f.module, block, reference); reference.bits = XR_LIR_CLOSURE_OWNED;
    f.owned = facts_emit(f.module, block, reference);
    XrLirValue captured = facts_emit(f.module, block, (XrLirInstruction){.op=XR_LIR_CELL_NEW,
        .type=cell, .operands=&f.seed, .operand_count=1});
    reference.callee = cell_target; reference.operands = &captured; reference.bits = XR_LIR_CLOSURE_FRAME;
    XrLirValue cell_frame = facts_emit(f.module, block, reference);
    f.cell_bound = facts_alias(f.module, block, f.strong, f.inner, cell_frame, UINT64_C(0x80000001));
    f.last_alias = f.owned;
    for (uint32_t i = 0; i < aliases; ++i) {
        f.last_alias = facts_emit(f.module, block, (XrLirInstruction){.op=XR_LIR_FUNCTION_WEAKEN,
            .type=f.weak, .operands=&f.last_alias, .operand_count=1});
        f.alias_source = f.last_alias.id;
        f.last_alias = facts_alias(f.module, block, f.weak, f.outer, f.last_alias, i);
    }
    f.bound = facts_alias(f.module, block, f.strong, f.outer, f.frame, UINT64_C(0x80000001));
    XrLirValue operands[] = {f.inner, f.last_alias};
    f.kept = facts_emit(f.module, block, (XrLirInstruction){.op=XR_LIR_FUNCTION_SCOPE_LEAVE,
        .type=f.weak, .operands=operands, .operand_count=2});
    XrLirInstruction leave = {.op=XR_LIR_FUNCTION_SCOPE_LEAVE, .type=XR_LIR_UNIT,
        .operands=&f.outer, .operand_count=1}; OK(xr_lir_emit_effect(f.module, block, &leave));
    f.future = facts_emit(f.module, block, (XrLirInstruction){.op=XR_LIR_CONST, .type=XR_LIR_I64, .bits=42});
    f.returned = facts_emit(f.module, block, (XrLirInstruction){.op=XR_LIR_CALL, .type=f.weak,
        .callee=f.factory, .operands=&f.future, .operand_count=1});
    facts_return(f.module, block, (XrLirValue){0});
    return f;
}

static void facts_boundaries(const FactsFixture *f, const XrLirCallableFacts *copy) {
    XrLirCallableFunctionFacts root, formals, hidden, empty = {.value_count=77};
    OK(xr_lir_callable_facts_function(copy, f->root.id, &root));
    OK(xr_lir_callable_facts_function(copy, f->contracts.id, &formals));
    OK(xr_lir_callable_facts_function(copy, f->hidden.id, &hidden));
    CHECK(root.values[f->frame.id].type == root.values[f->owned.id].type &&
        !root.values[f->frame.id].persistent && root.values[f->owned.id].persistent &&
        root.values[f->owned.id].constructor_storage == XR_LIR_CLOSURE_OWNED &&
        !root.values[f->frame.id].constructor_may_repeat && !root.values[f->owned.id].constructor_may_repeat &&
        !root.values[f->last_alias.id].constructor_may_repeat);
    CHECK(formals.values[1].parameter == 1 && !formals.values[1].persistent &&
        formals.values[2].parameter == 2 && formals.values[2].persistent && !formals.values[2].known_target &&
        hidden.values[1].parameter == 1 && hidden.values[1].persistent && !hidden.values[1].known_target &&
        !formals.values[1].constructor_may_repeat);
    CHECK(root.values[f->returned.id].operation == XR_LIR_CALL && root.values[f->returned.id].persistent &&
        !root.values[f->returned.id].known_target && root.values[f->returned.id].origin == f->returned.id);
    CHECK(root.capture_count == 3 && root.values[f->owned.id].capture_count == 1 &&
        root.captures[root.values[f->owned.id].first_capture] == f->seed.id &&
        root.values[f->last_alias.id].origin == f->owned.id &&
        root.values[f->last_alias.id].alias_source == f->alias_source &&
        root.values[f->last_alias.id].known_target == f->leaf.id &&
        root.values[f->last_alias.id].persistent && !root.values[f->last_alias.id].capture_count &&
        !root.values[f->kept.id].capture_count && root.values[f->kept.id].origin == f->owned.id);
    CHECK(root.values[f->inner.id].receiving_scope == f->outer.id &&
        root.values[f->frame.id].owner_scope == f->inner.id &&
        root.values[f->bound.id].owner_scope == f->outer.id &&
        !root.values[f->bound.id].persistent && root.values[f->bound.id].origin == f->frame.id &&
        root.values[f->bound.id].receiving_scope == f->outer.id && !root.values[f->bound.id].frame_floor &&
        root.values[f->bound.id].binding_identity == UINT64_C(0x80000001) &&
        root.values[f->cell_bound.id].frame_floor == f->inner.id &&
        root.values[f->cell_bound.id].receiving_scope == f->inner.id &&
        root.values[f->kept.id].receiving_scope == f->outer.id &&
        root.values[f->owned.id].frame_floor == UINT32_MAX);
    const XrLirCallableValueFact *cell_frame = &root.values[root.values[f->cell_bound.id].origin];
    CHECK(cell_frame->kind == XR_LIR_SCOPED_VALUE_FUNCTION && cell_frame->capture_count == 1);
    uint32_t identity = root.captures[cell_frame->first_capture];
    CHECK(root.values[identity].kind == XR_LIR_SCOPED_VALUE_CELL &&
        root.values[identity].cell_payload == XR_LIR_I64 && !root.values[identity].persistent &&
        !root.values[identity].transport && root.values[identity].cell_transport == XR_LIR_CELL_TRANSPORT_OWNED &&
        root.values[identity].owner_scope == f->inner.id);
    XrLirCallableFunctionFacts cell_formal;
    OK(xr_lir_callable_facts_function(copy,cell_frame->known_target,&cell_formal));
    CHECK(cell_formal.values[1].kind == XR_LIR_SCOPED_VALUE_CELL && cell_formal.values[1].parameter == 1 &&
        cell_formal.values[1].cell_transport == XR_LIR_CELL_TRANSPORT_BORROW && !cell_formal.values[1].owner_scope &&
        !cell_formal.values[1].transport && !cell_formal.values[1].persistent);
    CHECK(xr_lir_callable_facts_function(copy, 0, &empty) == XR_LIR_INVALID_ARGUMENT && empty.value_count == 77);
    OK(xr_lir_callable_facts_function(copy, f->leaf.id, &empty)); CHECK(!empty.values && !empty.value_count);
}

static void facts_copy_failures(void) {
    FactsFixture f = facts_fixture(512); OK(xr_lir_module_finish(f.module, NULL));
    size_t baseline = facts_live, baseline_bytes = live_bytes;
    CHECK(baseline && baseline_bytes && f.module->callable_facts);
    XrLirCallableFacts *cache = f.module->callable_facts, *copy = NULL;
    attempt = 0; OK(xr_lir_callable_facts_create(f.module, &copy)); size_t allocations = attempt;
    CHECK(allocations > 1 && xr_lir_callable_facts_function_count(copy) == f.module->function_count);
    facts_boundaries(&f, copy); xr_lir_callable_facts_destroy(copy); copy = NULL;
    CHECK(facts_live == baseline && live_bytes == baseline_bytes);
    for (size_t point = 0; point < allocations; ++point) {
        attempt = 0; fail_at = point;
        CHECK(xr_lir_callable_facts_create(f.module, &copy) == XR_LIR_OUT_OF_MEMORY && !copy && attempt > point);
        CHECK(f.module->callable_facts == cache && facts_live == baseline && live_bytes == baseline_bytes);
    }
    fail_at = SIZE_MAX; OK(xr_lir_callable_facts_create(f.module, &copy));
    size_t occupied = facts_live, occupied_bytes = live_bytes; XrLirCallableFacts *saved = copy;
    attempt = 0; fail_at = 0;
    CHECK(xr_lir_callable_facts_create(f.module, &copy) == XR_LIR_INVALID_ARGUMENT && copy == saved && !attempt);
    CHECK(xr_lir_module_finish(f.module, NULL) == XR_LIR_INVALID_STATE && !attempt &&
        f.module->callable_facts == cache && facts_live == occupied && live_bytes == occupied_bytes);
    fail_at = SIZE_MAX;
    XrLirOwnershipPlan *ownership = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=f.module}, &ownership, NULL));
    XrLirOwnershipFunction modeled; OK(xr_lir_ownership_function_at(ownership, f.root.id, &modeled));
    CHECK(modeled.unmodeled & XR_LIR_OWNERSHIP_UNMODELED_CALLABLE); xr_lir_ownership_plan_destroy(ownership);
    XrLirTypeLayout layout = {.size=123};
    CHECK(xr_lir_type_layout(f.module, f.strong, &layout) == XR_LIR_UNSUPPORTED && layout.size == 123);
    xr_lir_module_destroy(f.module); facts_boundaries(&f, copy);
    CHECK(facts_live == occupied - baseline && live_bytes == occupied_bytes - baseline_bytes);
    xr_lir_callable_facts_destroy(copy); CHECK(!facts_live && !live_bytes);
    printf("callable copy failure points: %zu\n", allocations);
}

static void facts_finish_failures(void) {
    FactsFixture initial = facts_fixture(2); attempt = 0;
    OK(xr_lir_module_finish(initial.module, NULL)); size_t allocations = attempt;
    xr_lir_module_destroy(initial.module); CHECK(!facts_live && !live_bytes);
    for (size_t point = 0; point < allocations; ++point) {
        FactsFixture f = facts_fixture(2); attempt = 0; fail_at = point;
        XrLirDiagnostic diagnostic;
        CHECK(xr_lir_module_finish(f.module, &diagnostic) == XR_LIR_OUT_OF_MEMORY &&
            !f.module->finished && !f.module->callable_facts && !facts_live && !live_bytes);
        size_t used = attempt; fail_at = SIZE_MAX;
        CHECK(xr_lir_module_finish(f.module, NULL) == XR_LIR_OUT_OF_MEMORY && attempt == used);
        xr_lir_module_destroy(f.module); CHECK(!facts_live && !live_bytes);
    }
    printf("callable constructor failure points: %zu\n", allocations);
}

static void facts_bad_state(FactsFixture *f) {
    XrLirStateModule module = {0};
    XrLirStateSlot slot = {.payload=XR_LIR_I64, .mutable=true, .size=8, .alignment=8};
    XrLirStateLayout layout = {.modules=&module, .module_count=1, .slots=&slot,
        .slot_count=1, .payload_size=8, .payload_alignment=8};
    OK(xr_lir_set_state_layout(f->module, &layout));
    XrLirType state = XR_LIR_STATE, reference = XR_LIR_STATE_REF;
    XrLirFunction function = facts_function(f->module, &(XrLirFunctionSpec){.name="state", .name_length=5,
        .parameters=&state, .parameter_count=1, .result=XR_LIR_I64});
    OK(xr_lir_set_state_entry(f->module, function));
    XrLirBlock entry = facts_block(f->module, function), next = {0};
    OK(xr_lir_add_block(f->module, function, &reference, 1, &next));
    XrLirValue input = facts_parameter(f->module, function, 0), phi = {0};
    XrLirValue ref = facts_emit(f->module, entry, (XrLirInstruction){.op=XR_LIR_STATE_REFERENCE,
        .type=XR_LIR_STATE_REF, .operands=&input, .operand_count=1});
    OK(xr_lir_block_parameter(f->module, next, 0, &phi));
    OK(xr_lir_set_state_reference(f->module, phi, &(XrLirStateReference){.payload=XR_LIR_I64, .mutable=true}));
    XrLirInstruction jump = {.op=XR_LIR_BR, .type=XR_LIR_UNIT, .edge_count=1,
        .edges={{.target=next, .arguments=&ref, .argument_count=1}}};
    OK(xr_lir_set_terminator(f->module, entry, &jump));
    facts_return(f->module, next, facts_emit(f->module, next, (XrLirInstruction){.op=XR_LIR_STATE_READ,
        .type=XR_LIR_I64, .operands=&phi, .operand_count=1}));
    f->module->functions[function.id - 1].values[phi.id - 1].state_reference.mutable = false;
}

static void facts_rejected_pending(void) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        FactsFixture f = facts_fixture(2);
        if (scenario == 0) f.module->functions[f.root.id - 1].result = XR_LIR_STATE_REF;
        else if (scenario == 1) {
            XrLirInstructionView *reference = &f.module->functions[f.root.id - 1].blocks[0].instructions[3].view;
            CHECK(reference->op == XR_LIR_FUNCTION_REF && reference->result == f.frame.id);
            ((uint32_t *)reference->operands)[0] = f.future.id;
        } else facts_bad_state(&f);
        XrLirDiagnostic diagnostic; attempt = 0;
        CHECK(xr_lir_module_finish(f.module, &diagnostic) == XR_LIR_VERIFY_ERROR && attempt &&
            !f.module->finished && !f.module->callable_facts && !facts_live && !live_bytes);
        if (scenario == 1) CHECK(strstr(diagnostic.message, "dominat") || strstr(diagnostic.message, "before"));
        if (scenario == 2) CHECK(strstr(diagnostic.message, "state reference edge shape mismatch"));
        size_t used = attempt;
        CHECK(xr_lir_module_finish(f.module, NULL) == XR_LIR_VERIFY_ERROR && attempt == used);
        XrLirCallableFacts *copy = NULL;
        CHECK(xr_lir_callable_facts_create(f.module, &copy) == XR_LIR_INVALID_ARGUMENT && !copy);
        xr_lir_module_destroy(f.module); CHECK(!facts_live && !live_bytes);
    }
}

static void facts_seed_origins(void) {
    XrLirModule *module=NULL; OK(xr_lir_module_create(NULL,&module));
    XrLirType callable=0, factory_type=0;
    XrLirCallableType signature={.result=XR_LIR_I64};
    OK(xr_lir_callable_type(module,&signature,&callable));
    signature.result=callable;
    OK(xr_lir_callable_type(module,&signature,&factory_type));
    XrLirFunction leaf=facts_function(module,&(XrLirFunctionSpec){
        .name="code_only",.name_length=9,.result=XR_LIR_I64});
    OK(xr_lir_set_closure_target(module,leaf,(XrLirClosureTarget){.callable=callable}));
    XrLirBlock block=facts_block(module,leaf);
    facts_return(module,block,facts_emit(module,block,(XrLirInstruction){
        .op=XR_LIR_CONST,.type=XR_LIR_I64,.bits=42}));
    XrLirFunction factory=facts_function(module,&(XrLirFunctionSpec){
        .name="indirect_factory",.name_length=16,.result=callable});
    OK(xr_lir_set_closure_target(module,factory,(XrLirClosureTarget){.callable=factory_type}));
    block=facts_block(module,factory);
    facts_return(module,block,facts_emit(module,block,(XrLirInstruction){
        .op=XR_LIR_FUNCTION_REF,.type=callable,.callee=leaf,.bits=XR_LIR_CLOSURE_FRAME}));
    uint32_t bytes=0,alignment=0;
    CHECK(xr_lir_internal_state_payload(module,callable,&bytes,&alignment));
    XrLirStateModule owner={0};
    XrLirStateSlot slot={.payload=callable,.mutable=true,.size=bytes,.alignment=alignment};
    XrLirStateLayout layout={.modules=&owner,.module_count=1,.slots=&slot,.slot_count=1,
        .payload_size=bytes,.payload_alignment=alignment};
    OK(xr_lir_set_state_layout(module,&layout));
    XrLirType state_type=XR_LIR_STATE;
    XrLirFunction root=facts_function(module,&(XrLirFunctionSpec){.name="seed_origins",.name_length=12,
        .parameters=&state_type,.parameter_count=1,.result=XR_LIR_I64});
    OK(xr_lir_set_state_entry(module,root));
    XrLirBlock entry=facts_block(module,root),join={0};
    OK(xr_lir_add_block(module,root,&callable,1,&join));
    XrLirValue state=facts_parameter(module,root,0);
    XrLirInstruction effect={.op=XR_LIR_MODULE_BEGIN,.type=XR_LIR_UNIT,.operands=&state,.operand_count=1};
    OK(xr_lir_emit_effect(module,entry,&effect));
    XrLirValue code_only=facts_emit(module,entry,(XrLirInstruction){
        .op=XR_LIR_FUNCTION_REF,.type=callable,.callee=leaf,.bits=XR_LIR_CLOSURE_FRAME});
    XrLirValue initial[]={state,code_only};
    XrLirValue reference=facts_emit(module,entry,(XrLirInstruction){
        .op=XR_LIR_STATE_CONSTRUCT,.type=XR_LIR_STATE_REF,.operands=initial,.operand_count=2});
    effect=(XrLirInstruction){.op=XR_LIR_STATE_PUBLISH,.type=XR_LIR_UNIT,.operands=&reference,.operand_count=1};
    OK(xr_lir_emit_effect(module,entry,&effect));
    effect=(XrLirInstruction){.op=XR_LIR_MODULE_READY,.type=XR_LIR_UNIT,.operands=&state,.operand_count=1};
    OK(xr_lir_emit_effect(module,entry,&effect));
    XrLirValue state_read=facts_emit(module,entry,(XrLirInstruction){
        .op=XR_LIR_STATE_READ,.type=callable,.operands=&reference,.operand_count=1});
    XrLirValue state_result=facts_emit(module,entry,(XrLirInstruction){
        .op=XR_LIR_CALL_INDIRECT,.type=XR_LIR_I64,.operands=&state_read,.operand_count=1});
    XrLirInstruction branch={.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,
        .edges={{.target=join,.arguments=&code_only,.argument_count=1}}};
    OK(xr_lir_set_terminator(module,entry,&branch));
    XrLirValue phi={0}; OK(xr_lir_block_parameter(module,join,0,&phi));
    XrLirValue phi_result=facts_emit(module,join,(XrLirInstruction){
        .op=XR_LIR_CALL_INDIRECT,.type=XR_LIR_I64,.operands=&phi,.operand_count=1});
    XrLirValue factory_ref=facts_emit(module,join,(XrLirInstruction){
        .op=XR_LIR_FUNCTION_REF,.type=factory_type,.callee=factory,.bits=XR_LIR_CLOSURE_FRAME});
    XrLirValue indirect=facts_emit(module,join,(XrLirInstruction){
        .op=XR_LIR_CALL_INDIRECT,.type=callable,.operands=&factory_ref,.operand_count=1});
    XrLirValue final_result=facts_emit(module,join,(XrLirInstruction){
        .op=XR_LIR_CALL_INDIRECT,.type=XR_LIR_I64,.operands=&indirect,.operand_count=1});
    XrLirValue pieces[]={state_result,phi_result};
    pieces[0]=facts_emit(module,join,(XrLirInstruction){.op=XR_LIR_ADD,.type=XR_LIR_I64,
        .operands=pieces,.operand_count=2});
    pieces[1]=final_result;
    facts_return(module,join,facts_emit(module,join,(XrLirInstruction){.op=XR_LIR_ADD,
        .type=XR_LIR_I64,.operands=pieces,.operand_count=2}));
    OK(xr_lir_module_finish(module,NULL));
    XrLirCallableFacts *copy=NULL; OK(xr_lir_callable_facts_create(module,&copy));
    xr_lir_module_destroy(module);
    XrLirCallableFunctionFacts facts={0}; OK(xr_lir_callable_facts_function(copy,root.id,&facts));
    const XrLirCallableValueFact *selected=&facts.values[phi.id], *seed=&facts.values[code_only.id];
    CHECK(selected->type==callable && selected->block==join.id && selected->block &&
        !selected->instruction && !selected->parameter && selected->operation==XR_LIR_OP_INVALID &&
        !selected->persistent && !selected->known_target && selected->origin==phi.id);
    CHECK(seed->operation==XR_LIR_FUNCTION_REF && seed->block==entry.id && seed->instruction &&
        seed->persistent && seed->known_target==leaf.id && !seed->capture_count &&
        seed->constructor_storage==XR_LIR_CLOSURE_FRAME && !seed->state_capture);
    CHECK(seed->materialize && seed->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED &&
        selected->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED && !selected->materialize);
    const XrLirCallableValueFact *returned=&facts.values[indirect.id], *read=&facts.values[state_read.id];
    CHECK(returned->type==callable && returned->operation==XR_LIR_CALL_INDIRECT &&
        returned->block==join.id && returned->instruction && !returned->parameter &&
        returned->persistent && !returned->known_target && returned->origin==indirect.id);
    CHECK(read->type==callable && read->operation==XR_LIR_STATE_READ && read->block==entry.id &&
        read->instruction && !read->parameter && read->persistent && !read->known_target && read->origin==state_read.id);
    CHECK(read->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED && returned->transport==XR_LIR_CALLABLE_TRANSPORT_OWNED);
    CHECK(facts_live && live_bytes); xr_lir_callable_facts_destroy(copy); CHECK(!facts_live && !live_bytes);
    puts("callable block arguments, code-only seeds, indirect results and State reads retain exact facts");
}

static void facts_scalar_empty(void) {
    XrLirModule *module = NULL; OK(xr_lir_module_create(NULL, &module));
    XrLirFunction function = facts_function(module, &(XrLirFunctionSpec){.name="scalar", .name_length=6, .result=XR_LIR_UNIT});
    facts_return(module, facts_block(module, function), (XrLirValue){0});
    OK(xr_lir_module_finish(module, NULL));
    size_t baseline = facts_live, baseline_bytes = live_bytes;
    CHECK(baseline && baseline_bytes);
    XrLirCallableFacts *copy = NULL; attempt = 0; fail_at = 0;
    OK(xr_lir_callable_facts_create(module, &copy));
    CHECK(!copy && !attempt && facts_live == baseline && live_bytes == baseline_bytes);
    fail_at = SIZE_MAX; xr_lir_module_destroy(module);
    CHECK(!facts_live && !live_bytes);
}

#include "lir_callable_construction_fixture.h"
static void facts_construction_reentry(void) {
    for (unsigned shape = 0; shape < 4; ++shape) {
        XrLirModule *module = NULL; XrLirFunction root = {0}; XrLirValue reference = {0};
        callable_construction_fixture(shape, &module, &root, &reference);
        XrLirCallableFacts *copy = NULL;
        OK(xr_lir_callable_facts_create(module, &copy));
        XrLirCallableFunctionFacts cached = {0}, received = {0};
        OK(xr_lir_callable_facts_function(module->callable_facts, root.id, &cached));
        OK(xr_lir_callable_facts_function(copy, root.id, &received));
        CHECK(received.values != cached.values &&
            !memcmp(received.values, cached.values, ((size_t)received.value_count + 1) * sizeof(*received.values)));
        xr_lir_module_destroy(module);
        const XrLirCallableValueFact *row = &received.values[reference.id];
        CHECK(row->kind == XR_LIR_SCOPED_VALUE_FUNCTION && row->operation == XR_LIR_FUNCTION_REF &&
            row->constructor_storage == XR_LIR_CLOSURE_FRAME && row->capture_count == 1 && row->block != 1 &&
            row->constructor_may_repeat == (shape >= 2));
        xr_lir_callable_facts_destroy(copy); CHECK(!facts_live && !live_bytes);
    }
}

#include "lir_slice_boundary_fixture.h"
#include "lir_slice_callback_loan_fixture.h"
static void facts_callback_pairs(XrLirSliceFactsView view) {
    CHECK(view.callback_loans_complete && view.callback_loan_count==2 && view.callback_loans &&
        view.callback_loans[0].callback_parameter==1 && view.callback_loans[0].loan_kind==XR_LIR_SLICE_EFFECT_PARAMETER &&
        view.callback_loans[0].loan_parameter==0 && view.callback_loans[1].callback_parameter==0 &&
        view.callback_loans[1].loan_kind==XR_LIR_SLICE_EFFECT_PARAMETER && view.callback_loans[1].loan_parameter==1);
}
static void facts_callback_receiving_failures(void) {
    const XrLirType elements[]={XR_LIR_I64,XR_LIR_U8};
    for(size_t family=0;family<2;++family) {
        SliceCallbackLoanFixture fixture=slice_callback_loan_fixture(elements[family],SLICE_CALLBACK_LOAN_DIRECT);
        OK(xr_lir_module_finish(fixture.module,NULL));
        XrLirSliceFactsView source;OK(xr_lir_slice_facts_view(fixture.module->slice_facts,&source));facts_callback_pairs(source);
        size_t baseline=facts_live,bytes=live_bytes;XrLirSliceFacts *copy=NULL;
        attempt=0;OK(xr_lir_slice_facts_create(fixture.module,&copy));size_t copy_sites=attempt;
        XrLirSliceFactsView received;OK(xr_lir_slice_facts_view(copy,&received));facts_callback_pairs(received);
        CHECK(copy_sites>=4 && received.callback_loans!=source.callback_loans);
        xr_lir_slice_facts_destroy(copy);copy=NULL;CHECK(facts_live==baseline && live_bytes==bytes);
        for(size_t point=0;point<copy_sites;++point) {
            attempt=0;fail_at=point;
            CHECK(xr_lir_slice_facts_create(fixture.module,&copy)==XR_LIR_OUT_OF_MEMORY && !copy);
            fail_at=SIZE_MAX;CHECK(facts_live==baseline && live_bytes==bytes);
        }
        XrLirModule *rebuilt=NULL;attempt=0;OK(xr_lir_rebuild(fixture.module,&rebuilt));size_t rebuild_sites=attempt;
        OK(xr_lir_slice_facts_view(rebuilt->slice_facts,&received));facts_callback_pairs(received);
        CHECK(rebuild_sites>copy_sites && received.callback_loans!=source.callback_loans);
        xr_lir_module_destroy(rebuilt);rebuilt=NULL;CHECK(facts_live==baseline && live_bytes==bytes);
        for(size_t point=0;point<rebuild_sites;++point) {
            attempt=0;fail_at=point;XrLirStatus status=xr_lir_rebuild(fixture.module,&rebuilt);
            fail_at=SIZE_MAX;
            if(status!=XR_LIR_OUT_OF_MEMORY || rebuilt || facts_live!=baseline || live_bytes!=bytes)
                fprintf(stderr,"callback relation rebuild family%zu allocation%zu/%zu status%u live%zu/%zu bytes%zu/%zu\n",
                    family,point,rebuild_sites,(unsigned)status,facts_live,baseline,live_bytes,bytes);
            CHECK(status==XR_LIR_OUT_OF_MEMORY && !rebuilt && facts_live==baseline && live_bytes==bytes);
        }
        OK(xr_lir_rebuild(fixture.module,&rebuilt));OK(xr_lir_slice_facts_create(fixture.module,&copy));
        xr_lir_module_destroy(fixture.module);fixture.module=NULL;
        OK(xr_lir_slice_facts_view(copy,&received));facts_callback_pairs(received);
        OK(xr_lir_verify(rebuilt,NULL));OK(xr_lir_slice_facts_view(rebuilt->slice_facts,&received));facts_callback_pairs(received);
        xr_lir_slice_facts_destroy(copy);xr_lir_module_destroy(rebuilt);CHECK(!facts_live && !live_bytes);
        printf("callback relation family%zu: copy %zu and rebuild %zu allocation failures preserve owners\n",
            family,copy_sites,rebuild_sites);
    }
}
static void facts_slice_receiving(void) {
    for (unsigned family = 0; family < 2; ++family) {
        SliceBoundaryFixture fixture = slice_boundary_fixture(family ? XR_LIR_U8 : XR_LIR_I64,false);
        OK(xr_lir_module_finish(fixture.module,NULL));
        size_t baseline = facts_live, bytes = live_bytes;
        XrLirSliceFacts *copy = NULL; attempt = 0;
        OK(xr_lir_slice_facts_create(fixture.module,&copy)); size_t sites = attempt;
        XrLirSliceFactsView cached, received;
        OK(xr_lir_slice_facts_view(fixture.module->slice_facts,&cached));
        OK(xr_lir_slice_facts_view(copy,&received));
        CHECK(sites && received.functions != cached.functions && received.terms != cached.terms &&
            received.function_count == 4 && received.term_count == 4 &&
            !memcmp(received.functions,cached.functions,5 * sizeof(*received.functions)) &&
            !memcmp(received.terms,cached.terms,4 * sizeof(*received.terms)));
        XrLirSliceFacts *preserved = copy;
        CHECK(xr_lir_slice_facts_create(fixture.module,&copy) == XR_LIR_INVALID_ARGUMENT && copy == preserved);
        xr_lir_slice_facts_destroy(copy); copy = NULL;
        CHECK(facts_live == baseline && live_bytes == bytes);
        for (size_t n = 0; n < sites; ++n) {
            attempt = 0; fail_at = n;
            CHECK(xr_lir_slice_facts_create(fixture.module,&copy) == XR_LIR_OUT_OF_MEMORY && !copy &&
                facts_live == baseline && live_bytes == bytes);
        }
        fail_at = SIZE_MAX; OK(xr_lir_slice_facts_create(fixture.module,&copy));
        xr_lir_module_destroy(fixture.module);
        OK(xr_lir_slice_facts_view(copy,&received));
        CHECK(received.functions[fixture.relay.id].result_parameter == 1 &&
            received.terms[received.functions[fixture.relay.id].result_origin.first].kind == XR_LIR_SLICE_EFFECT_PARAMETER &&
            received.terms[received.functions[fixture.read.id].write.first].kind == XR_LIR_SLICE_EFFECT_UNKNOWN &&
            received.terms[received.functions[fixture.read.id].access.first].kind == XR_LIR_SLICE_EFFECT_FIELD);
        xr_lir_slice_facts_destroy(copy); CHECK(!facts_live && !live_bytes);
    }
}
static void facts_slice_finish_failures(void) {
    SliceBoundaryFixture fixture = slice_boundary_fixture(XR_LIR_I64,false); attempt = 0;
    OK(xr_lir_module_finish(fixture.module,NULL)); size_t sites = attempt;
    xr_lir_module_destroy(fixture.module); CHECK(!facts_live && !live_bytes);
    for (size_t n = 0; n < sites; ++n) {
        fixture = slice_boundary_fixture(XR_LIR_I64,false); attempt = 0; fail_at = n;
        XrLirDiagnostic diagnostic = {0};
        CHECK(xr_lir_module_finish(fixture.module,&diagnostic) == XR_LIR_OUT_OF_MEMORY &&
            diagnostic.status == XR_LIR_OUT_OF_MEMORY && !fixture.module->finished && !fixture.module->callable_facts);
        fail_at = SIZE_MAX; xr_lir_module_destroy(fixture.module); CHECK(!facts_live && !live_bytes);
    }
}
int main(void) {
    facts_callback_receiving_failures();
    facts_slice_receiving(); facts_slice_finish_failures();
    facts_scalar_empty(); facts_construction_reentry(); facts_seed_origins();
    facts_copy_failures(); facts_finish_failures(); facts_rejected_pending();
    puts("constructor callable facts, owned copies and failure cleanup passed");
    return 0;
}
