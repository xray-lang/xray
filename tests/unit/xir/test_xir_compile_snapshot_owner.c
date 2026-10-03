/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_snapshot_owner.c - Snapshot copies retain the shared resource owner
 */
#include "base/xmalloc.h"
#include "xir/xxir_compile_memory.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_struct.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_implementation.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_program_internal.h"
#include <stdio.h>
#include <limits.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[4096];
static size_t attempts, fail_at = SIZE_MAX, live, physical, peak, total;
static void *observe_alloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory);
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer) ++i;
    CHECK(i < 4096);
    allocations[i] = (Allocation){memory, bytes};
    ++live; physical += bytes; total += bytes;
    if (physical > peak) peak = physical;
    return memory;
}
static void observe_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < 4096 && allocations[i].pointer != memory) ++i;
    CHECK(i < 4096 && live);
    size_t bytes = allocations[i].bytes;
    xr_free(memory);
    allocations[i] = (Allocation){0}; --live; physical -= bytes;
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observe_alloc(bytes)
#define xr_free(memory) observe_free(memory)
#include "base/xcompile_resources.c"

static void reset_observer(void) {
    CHECK(!live && !physical);
    attempts = peak = total = 0; fail_at = SIZE_MAX;
}
static XrXirCompileContext context_new(uint64_t work) {
    XrXirCompileContext context = {0};
    const XrCompileResourceLimits limits = {UINT64_MAX, UINT64_MAX, work};
    CHECK(xr_compile_resources_new(&limits, &context.resources) == XR_COMPILE_RESOURCE_OK);
    context.limits = xr_xir_compile_default_limits();
    return context;
}
static XrCompileResourceStats stats(const XrXirCompileContext *context) {
    XrCompileResourceStats value;
    CHECK(xr_compile_resources_stats(context->resources, &value) == XR_COMPILE_RESOURCE_OK);
    CHECK(value.live_bytes == physical && value.peak_bytes == peak && value.allocated_bytes == total);
    return value;
}

static XrXirStatus snapshot_fixture(const XrXirCompileContext *context, XrXirSourceSnapshot **out) {
    char name[]="compute", signature[]="compute(i64) -> string";
    char identity[]="root.identity", path[]="root.xr";
    XrXirSourceQueryModule module={identity,path,{0}};
    XrXirSourceType parameter={XR_XIR_I64,0,true};
    XrXirSourceDeclaration declaration={0};
    declaration.name=name; declaration.signature=signature;
    declaration.parameters=&parameter; declaration.parameter_count=1;
    declaration.type=(XrXirSourceType){XR_XIR_STRING,0,true};
    XrXirSourceReference reference={0}; reference.declaration=17;
    XrXirSourceExpression expression={0}; expression.node=31; expression.type=parameter;
    XrXirCallableParameter parameter_type={XR_XIR_I64,0};
    XrXirTypeNode node={0}; node.kind=XR_XIR_TYPE_CALLABLE;
    node.parameters=&parameter_type; node.parameter_count=1; node.result=XR_XIR_STRING;
    XrXirType argument=XR_XIR_I64;
    XrXirInterfaceApplication application={0,&argument,1};
    XrXirConstraint constraint={0,&application,1};
    uint32_t binder=XR_XIR_BINDER_TYPE;
    declaration.generic_constraints=&constraint; declaration.generic_parameter_count=1;
    declaration.type_parameter_kinds=&binder;
    XrXirNominalField field={{"value",5},XR_XIR_I64,0};
    XrXirNominalDeclaration nominal={0};
    nominal.module=(XrXirLiteral){identity,13}; nominal.name=(XrXirLiteral){name,7};
    nominal.constraints=&constraint; nominal.parameter_count=1;
    nominal.fields=&field; nominal.field_count=1;
    XrXirNominalVariant variant={{"Some",4},0,1};
    nominal.kind=XR_XIR_NOMINAL_ENUM; nominal.variants=&variant; nominal.variant_count=1;
    XrXirNominalTable nominals={&nominal,1,NULL};
    XrXirInterfaceMethod method={{name,7},(XrXirType)256,0,1,&constraint};
    XrXirInterfaceDeclaration interface={0};
    interface.module=(XrXirLiteral){identity,13};interface.name=(XrXirLiteral){"Show",4};
    interface.constraints=&constraint;interface.parameter_count=1;
    interface.methods=&method;interface.method_count=1;
    XrXirInterfaceTable interfaces={&interface,1};
    XrXirImplementationBinding binding={application,0,2};
    XrXirImplementation implementation={0,application,&binding,1};
    XrXirImplementationTable implementations={&implementation,1};
    XrXirTypes types={&node,1,&nominals,&interfaces};
    XrXirSourceView view={0}; view.complete=true;
    view.modules=&module; view.module_count=1;
    view.declarations=&declaration; view.declaration_count=1;
    view.references=&reference; view.reference_count=1;
    view.expressions=&expression; view.expression_count=1; view.types=&types;
    view.implementations=&implementations;
    return xr_xir_compile_source_snapshot_copy(context,&view,out);
}
static void snapshot_lifetime_and_failures(void) {
    size_t count=0; uint64_t needed=0;
    for (size_t failure=SIZE_MAX;;) {
        reset_observer();
        XrXirCompileContext context=context_new(UINT64_MAX);
        fail_at=failure;
        XrXirSourceSnapshot *snapshot=NULL;
        XrXirStatus status=snapshot_fixture(&context,&snapshot);
        if (failure==SIZE_MAX) {
            CHECK(status==XR_XIR_OK && snapshot); count=attempts; needed=stats(&context).work;
            xr_compile_resources_release(context.resources);
            const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(snapshot);
            CHECK(view && view->complete && view->module_count==1 && view->declaration_count==1);
            CHECK(!strcmp(view->modules[0].identity,"root.identity") && !strcmp(view->modules[0].path,"root.xr"));
            CHECK(!strcmp(view->declarations[0].name,"compute"));
            CHECK(!strcmp(view->declarations[0].signature,"compute(i64) -> string"));
            CHECK(view->declarations[0].parameters[0].type==XR_XIR_I64);
            CHECK(view->types->nodes[0].parameters[0].type==XR_XIR_I64 && view->types->nodes[0].result==XR_XIR_STRING);
            CHECK(view->references[0].declaration==17 && view->expressions[0].node==31);
            CHECK(view->declarations[0].generic_constraints[0].interfaces[0].arguments[0]==XR_XIR_I64);
            CHECK(view->declarations[0].type_parameter_kinds[0]==XR_XIR_BINDER_TYPE);
            CHECK(!memcmp(view->types->nominals->declarations[0].fields[0].name.bytes,"value",5));
            CHECK(!memcmp(view->types->nominals->declarations[0].variants[0].name.bytes,"Some",4));
            CHECK(!memcmp(view->types->interfaces->declarations[0].methods[0].name.bytes,"compute",7));
            CHECK(view->types->interfaces->declarations[0].methods[0].constraints[0].interfaces[0].arguments[0]==XR_XIR_I64);
            CHECK(view->implementations->records[0].bindings[0].requirement.arguments[0]==XR_XIR_I64);
            xr_xir_compile_source_snapshot_free(snapshot);
        } else {
            CHECK(status==XR_XIR_OUT_OF_MEMORY && !snapshot);
            CHECK(stats(&context).allocation_count>=1);
            xr_compile_resources_release(context.resources);
        }
        CHECK(!physical && !live);
        if (failure==SIZE_MAX) failure=1;
        else if (++failure==count) break;
    }
    for (uint64_t limit=1;limit<=needed;limit+=limit<needed && needed-limit>37 ? 37:1) {
        reset_observer(); XrXirCompileContext context=context_new(limit);
        XrXirSourceSnapshot *snapshot=NULL;
        XrXirStatus status=snapshot_fixture(&context,&snapshot);
        CHECK(status==(limit==needed?XR_XIR_OK:XR_XIR_BUDGET));
        xr_xir_compile_source_snapshot_free(snapshot); xr_compile_resources_release(context.resources);
        CHECK(!physical && !live);
    }
    printf("Source snapshot: %zu actual OOM points, work through %llu, physical zero\n",count-1,(unsigned long long)needed);
}
static void snapshot_output(void) {
    reset_observer(); XrXirCompileContext context=context_new(UINT64_MAX);
    XrXirSourceSnapshot *untouched=(XrXirSourceSnapshot *)(uintptr_t)1;
    CHECK(snapshot_fixture(&context,&untouched)==XR_XIR_BAD_STRUCTURE);
    CHECK(untouched==(XrXirSourceSnapshot *)(uintptr_t)1 && attempts==1);
    XrXirSourceView bad={0};bad.module_count=1;
    XrXirSourceSnapshot *snapshot=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy(&context,&bad,&snapshot)==XR_XIR_BAD_STRUCTURE && !snapshot);
    xr_compile_resources_release(context.resources);CHECK(!physical && !live);
}
static void snapshot_missing_nested_storage(void) {
    for (unsigned kind=0;kind<9;++kind) {
        reset_observer(); XrXirCompileContext context=context_new(UINT64_MAX);
        XrXirSourceView view={0}; XrXirSourceSnapshot *snapshot=NULL;
        XrXirSourceDeclaration declaration={0};
        XrXirTypeNode node={0};
        XrXirNominalDeclaration nominal={0};
        XrXirNominalTable nominals={&nominal,1,NULL};
        XrXirInterfaceDeclaration interface={0};
        XrXirInterfaceTable interfaces={&interface,1};
        XrXirTypes types={0}; view.types=&types;
        switch (kind) {
        case 0: declaration.parameter_count=1;view.declarations=&declaration;view.declaration_count=1;break;
        case 1: types.nodes=&node;types.count=1;node.parameter_count=1;break;
        case 2: types.nodes=&node;types.count=1;node.nominal.argument_count=1;break;
        case 3: types.nodes=&node;types.count=1;node.nominal.field_count=1;break;
        case 4: types.nominals=&nominals;nominals.declarations=NULL;break;
        case 5: types.nominals=&nominals;nominal.field_count=1;break;
        case 6: types.nominals=&nominals;nominal.variant_count=1;break;
        case 7: types.interfaces=&interfaces;interfaces.declarations=NULL;break;
        case 8: types.interfaces=&interfaces;interface.method_count=1;break;
        }
        CHECK(xr_xir_compile_source_snapshot_copy(&context,&view,&snapshot)==XR_XIR_BAD_STRUCTURE);
        CHECK(!snapshot && stats(&context).allocation_count>=1);
        xr_compile_resources_release(context.resources);CHECK(!physical && !live);
    }
}
static void snapshot_fixed_work(void) {
    _Static_assert(sizeof(XrXirSourceView)==296,"Independent source fact layout");
    /* Ledger allocation, shape admission, snapshot allocation, zero 336 bytes,
     * and copy the 296-byte public view. Empty arrays perform no traversal. */
    for (uint64_t work=634;work<=635;++work) {
        reset_observer(); XrXirCompileContext context=context_new(work);
        XrXirSourceView view={0}; XrXirSourceSnapshot *snapshot=NULL;
        CHECK(xr_xir_compile_source_snapshot_copy(&context,&view,&snapshot)==
            (work==635?XR_XIR_OK:XR_XIR_BUDGET));
        if (snapshot) CHECK(stats(&context).work==635 && attempts==2);
        xr_xir_compile_source_snapshot_free(snapshot);xr_compile_resources_release(context.resources);
        CHECK(!physical && !live);
    }
}
int main(void) { snapshot_lifetime_and_failures(); snapshot_output(); snapshot_missing_nested_storage(); snapshot_fixed_work(); return 0; }
