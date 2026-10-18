/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_globals.c - Producer-owned global coordinates survive specialization
 */
#define main reference_source_launch_main
#include "test_launch.c"
#undef main
#include "xir/xxir_source_query_internal.h"
#include "xir/xxir_types.h"
#include "test_source_globals_budgets.inc.c"
static void global_facts(const XrXirSourceView *view,const XrXirSourceDebugView *debug,const XrXirModule *module) {
    CHECK(debug->global_count==6 && debug->globals && module->declarations);
    unsigned shared=0,mutable=0,unit=0,text=0;
    for (uint32_t i=0;i<debug->global_count;++i) {
        const XrXirSourceDebugGlobal *global=&debug->globals[i];
        CHECK(global->declaration && global->declaration<=view->declaration_count && global->slot<module->declarations->slot_count);
        if (i) CHECK(global->slot>debug->globals[i-1].slot && global->declaration>debug->globals[i-1].declaration);
        const XrXirSourceDeclaration *declaration=&view->declarations[global->declaration-1];
        const XrXirSlot *slot=&module->declarations->slots[global->slot];
        CHECK(declaration->kind==XR_XIR_SOURCE_BINDING && !declaration->parent && declaration->type.known);
        CHECK(slot->module==declaration->range.module && !!slot->mutable==declaration->mutable);
        CHECK(declaration->type.type==(slot->mutable ? xr_xir_cell_element(module->types,slot->type) : slot->type));
        if (!strcmp(declaration->name,"shared")) ++shared;
        if (declaration->mutable) ++mutable;
        if (declaration->type.type==XR_XIR_UNIT) ++unit;
        if (!strcmp(declaration->name,"text")) { CHECK(declaration->type.type==XR_XIR_STRING && !declaration->mutable);++text; }
    }
    CHECK(shared==2 && mutable==2 && unit==2 && text==1);
}
typedef struct GlobalPublication { XrXirSourceSnapshot *snapshot;XrCompileResources *resources;uint64_t baseline; } GlobalPublication;
static GlobalPublication global_publication(const XrXirSourceView *view,const XrXirConstruction *construction,
    const XrXirSourceDebugView *debug,const XrXirModule *module,bool inject) {
    XrXirCompileContext context={0};XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);context.limits=xr_xir_compile_default_limits();
    XrCompileResourceStats initial={0};CHECK(xr_compile_resources_stats(context.resources,&initial)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceSnapshot *copy=NULL;CHECK(xr_xir_compile_source_snapshot_copy_v2(&context,view,construction,&copy)==XR_XIR_OK);
    XrXirModule facts=*module;facts.functions=NULL;facts.function_count=0;
    XrXirSourceDebugGlobal globals[6];memcpy(globals,debug->globals,sizeof(globals));
    XrXirSourceDebugView selected={NULL,0,globals,6};
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t blocks=runtime_live,bytes=runtime_bytes;
    if (!inject) for (unsigned bad=0;bad<12;++bad) {
        memcpy(globals,debug->globals,sizeof(globals));selected=(XrXirSourceDebugView){NULL,0,globals,6};
        if (bad==0) globals[0].declaration=0;
        else if (bad==1) globals[0].declaration=view->declaration_count+1;
        else if (bad==2) globals[0].slot=module->declarations->slot_count;
        else if (bad==3) globals[1].slot=globals[0].slot;
        else if (bad==4) globals[1].declaration=globals[0].declaration;
        else if (bad==5) { XrXirSourceDebugGlobal old=globals[0];globals[0]=globals[5];globals[5]=old; }
        else if (bad==6) selected.globals=NULL;
        else if (bad==7) selected.global_count=0;
        else if (bad==8) { globals[0].declaration=globals[1].declaration;selected.global_count=1; }
        else if (bad==9) { globals[0].slot=globals[1].slot;selected.global_count=1; }
        else if (bad==10) {
            selected.global_count=1;globals[0].declaration=0;
            for (uint32_t d=0;d<view->declaration_count;++d)
                if (view->declarations[d].kind==XR_XIR_SOURCE_BINDING && view->declarations[d].parent) { globals[0].declaration=d+1;break; }
            CHECK(globals[0].declaration);
        } else {
            selected.global_count=1;uint32_t original=globals[0].declaration;globals[0].declaration=0;
            for (uint32_t d=0;d<view->declaration_count;++d) {
                const XrXirSourceDeclaration *candidate=&view->declarations[d];
                if (candidate->kind==XR_XIR_SOURCE_BINDING && !candidate->parent && !strcmp(candidate->name,"shared") &&
                    candidate->range.module!=view->declarations[original-1].range.module) { globals[0].declaration=d+1;break; }
            }
            CHECK(globals[0].declaration);
        }
        CHECK(xr_xir_compile_source_snapshot_debug_copy(copy,&selected,&facts)==XR_XIR_BAD_STRUCTURE);
        CHECK(!xr_xir_compile_source_snapshot_debug(copy) && blocks==runtime_live && bytes==runtime_bytes);
    }
    memcpy(globals,debug->globals,sizeof(globals));selected=(XrXirSourceDebugView){NULL,0,globals,6};
    size_t start=runtime_attempts;if (inject) runtime_fail_at=start;
    XrXirStatus status=xr_xir_compile_source_snapshot_debug_copy(copy,&selected,&facts);runtime_fail_at=SIZE_MAX;
    CHECK(status==(inject ? XR_XIR_OUT_OF_MEMORY : XR_XIR_OK) && runtime_attempts==start+1);
    if (inject) CHECK(!xr_xir_compile_source_snapshot_debug(copy) && blocks==runtime_live && bytes==runtime_bytes);
    else {
        const XrXirSourceDebugView *owned=xr_xir_compile_source_snapshot_debug(copy);
        CHECK(owned && owned->globals!=globals && owned->globals!=debug->globals);memset(globals,0,sizeof(globals));
        global_facts(xr_xir_compile_source_snapshot_view(copy),owned,module);
        CHECK(xr_xir_compile_source_snapshot_debug_copy(copy,&selected,&facts)==XR_XIR_BAD_STRUCTURE);
    }
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.work>before.work);
    if (!inject) return (GlobalPublication){copy,context.resources,initial.live_bytes};
    xr_xir_compile_source_snapshot_free(copy);CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==initial.live_bytes);
    xr_compile_resources_release(context.resources);return (GlobalPublication){0};
}
int main(void) {
    xr_test_suppress_dialogs();XrCompileResources *resources=NULL;XrCompileResourceLimits limits={67108864,8388608,128000000};
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats initial={0};CHECK(xr_compile_resources_stats(resources,&initial)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrSourceCompilerConfig config={context,{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},XR_DAP_STDLIB,xr_source_compile_default_manifest_limits()};
    XrSourceCompiler *compiler=NULL;XrSourceCompileDiagnostic diagnostic={0};
    CHECK(xr_source_compiler_new(&config,&compiler,&diagnostic)==XR_SOURCE_COMPILE_OK);
    XrXirSourceProduct *product=NULL;
    XrSourceCompileStatus status=xr_source_compiler_build_file(compiler,XR_DAP_GLOBAL_SLOTS_FIXTURE,&product,&diagnostic);
    if (status!=XR_SOURCE_COMPILE_OK) fprintf(stderr,"global source=%u stage=%u: %s\n",status,diagnostic.source.stage,diagnostic.source.source.message);
    CHECK(status==XR_SOURCE_COMPILE_OK && product);xr_source_compile_diagnostic_free(&diagnostic);xr_source_compiler_free(compiler);
    const XrXirSourceView *view=xr_xir_compile_source_product_view(product);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_product_debug(product);CHECK(view && debug);
    unsigned instances=0;
    for (uint32_t f=0;f<debug->function_count;++f) {
        uint32_t d=debug->functions[f].declaration;
        if (d && !strcmp(view->declarations[d-1].name,"identity")) ++instances;
    }
    CHECK(instances==2);
    GlobalPublication retained={0};
    for (unsigned kind=0;kind<2;++kind) {
        XrXirSourceProductPacketView packet={0};
        CHECK(xr_xir_compile_source_product_packet(product,kind ? XR_XIR_SOURCE_PRODUCT_CLOSED : XR_XIR_SOURCE_PRODUCT_SOURCE,&packet)==XR_XIR_OK);
        XrXirArtifact *checked=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
        const XrXirModule *module=xr_xir_compile_artifact_module(checked);global_facts(view,debug,module);
        CHECK(view->module_count>module->declarations->module_count);
        if (!kind) {
            const XrXirConstruction *construction=xr_xir_compile_source_product_construction(product);
            GlobalWorkInput input={view,construction,module,debug};global_work_budgets(&input);
            retained=global_publication(view,construction,debug,module,false);global_publication(view,construction,debug,module,true);
        }
        xr_xir_compile_artifact_free(checked);
    }
    CHECK(debug==xr_xir_compile_source_product_debug(product) && !strcmp(view->declarations[debug->globals[0].declaration-1].name,"shared"));
    xr_xir_compile_source_product_free(product);XrCompileResourceStats after={0};
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==initial.live_bytes);xr_compile_resources_release(resources);
    view=xr_xir_compile_source_snapshot_view(retained.snapshot);debug=xr_xir_compile_source_snapshot_debug(retained.snapshot);
    CHECK(view && debug && debug->global_count==6);
    for (uint32_t g=0;g<debug->global_count;++g) {
        const XrXirSourceDeclaration *declaration=&view->declarations[debug->globals[g].declaration-1];
        CHECK(declaration->name[0] && view->modules[declaration->range.module].path[0]);
    }
    xr_xir_compile_source_snapshot_free(retained.snapshot);
    CHECK(xr_compile_resources_stats(retained.resources,&after)==XR_COMPILE_RESOURCE_OK && after.live_bytes==retained.baseline);xr_compile_resources_release(retained.resources);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Owned Source global slots: same names across modules, local shadow, unit Cells, specialization, malformed facts and allocation rollback PASS");return 0;
}
