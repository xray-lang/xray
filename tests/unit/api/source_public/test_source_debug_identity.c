/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_debug_identity.c - Interactive debug facts outlive their birth owners
 */
#include "api/xsource_ast_owner.h"
#include "api/xsource_compiler.h"
#include "xir/xxir_source_query_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "../../xir/xir_source_fixture_owner.h"

static const XrXirSourceDeclaration *value_fact(const XrXirSourceSnapshot *snapshot,XrXirType type) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(snapshot);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_snapshot_debug(snapshot);
    CHECK(view && view->complete && debug && debug->global_count && debug->function_count);
    const XrXirSourceDeclaration *found=NULL;
    for(uint32_t g=0;g<debug->global_count;++g) {
        const XrXirSourceDebugGlobal *global=&debug->globals[g];
        CHECK(global->declaration && global->declaration<=view->declaration_count);
        const XrXirSourceDeclaration *declaration=&view->declarations[global->declaration-1];
        if(strcmp(declaration->name,"value"))continue;
        CHECK(!found && declaration->kind==XR_XIR_SOURCE_BINDING && !declaration->parent &&
            declaration->mutable && declaration->type.known && declaration->type.type==type);
        CHECK(declaration->range.module<view->module_count);
        const char *identity=view->modules[declaration->range.module].identity;
        CHECK(identity && !strncmp(identity,"repl-module:",12));found=declaration;
    }
    CHECK(found);return found;
}
static XrXirSourceSnapshot *copy_facts(const XrXirCompileContext *context,const XrXirSourceResult *source) {
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(source->snapshot);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_snapshot_debug(source->snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    CHECK(view && debug && module && module->declarations);
    for(uint32_t g=0;g<debug->global_count;++g) {
        const XrXirSourceDebugGlobal *global=&debug->globals[g];
        const XrXirSourceDeclaration *declaration=&view->declarations[global->declaration-1];
        CHECK(global->slot<module->declarations->slot_count);
        const XrXirSlot *slot=&module->declarations->slots[global->slot];
        CHECK(slot->module<module->declarations->module_count);
        const XrXirSourceModule *owner=&module->declarations->modules[slot->module];
        const char *identity=view->modules[declaration->range.module].identity;
        CHECK(strlen(identity)==owner->name_length && !memcmp(identity,owner->name,owner->name_length));
    }
    XrXirSourceSnapshot *copy=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(context,view,
        xr_xir_compile_source_snapshot_construction(source->snapshot),&copy)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(copy,
        xr_xir_compile_source_snapshot_syntax(source->snapshot))==XR_XIR_OK);
    CHECK(xr_xir_compile_source_snapshot_calls_copy(copy,
        xr_xir_compile_source_snapshot_calls(source->snapshot))==XR_XIR_OK);
    CHECK(xr_xir_compile_source_snapshot_debug_copy(copy,debug,module)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_snapshot_dependencies_copy(copy,
        xr_xir_compile_source_snapshot_dependencies(source->snapshot))==XR_XIR_OK);
    CHECK(xr_xir_compile_source_snapshot_type_facts_copy(copy,
        xr_xir_compile_source_snapshot_type_facts(source->snapshot))==XR_XIR_OK);
    const XrXirSourceView *owned=xr_xir_compile_source_snapshot_view(copy);
    CHECK(owned!=view && owned->modules!=view->modules &&
        owned->modules[0].identity!=view->modules[0].identity);
    CHECK(xr_xir_compile_source_snapshot_debug(copy)->globals!=debug->globals);return copy;
}
static XrXirSourceSnapshot *prepare_copy(XrSourceCompiler *compiler,XrXirSourceGenerations *owner,
    const XrXirCompileContext *receiver,const char *source,uint64_t *serial) {
    const XrSourceText text={NULL,"debug-identity",source,strlen(source)};
    XrSourceAst *ast=NULL;XrSourceDiagnostic *parse_detail=NULL;
    CHECK(xray_source_parse_text(compiler,&text,&ast,&parse_detail)==XR_SOURCE_OK && ast && !parse_detail);
    XrXirSourceAstOwner syntax={0};CHECK(xr_source_ast_owned_input(ast,&syntax)==XR_SOURCE_OK);
    const XrXirSourceReplInput input={.ast=&syntax};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_generation_prepare_repl_ast_bindings(owner,&input,serial,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"prepare %u: %s\n",(unsigned)status,diagnostic.message);
    CHECK(status==XR_XIR_OK && *serial);xray_source_ast_free(ast);
    XrXirSourceGenerationView view={0};
    CHECK(xr_xir_compile_source_generation_view(owner,*serial,&view)==XR_XIR_OK && view.interactive);
    return copy_facts(receiver,view.result);
}
int main(void) {
    SourceFixtureOwner receiver={0};source_fixture_owner_new(&receiver);
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=UINT64_C(64)<<20;
    options.live_bytes=UINT64_C(8)<<20;options.work=UINT64_C(128000000);
    XrSourceCompiler *compiler=NULL;CHECK(xray_source_compiler_new(&options,&compiler)==XR_SOURCE_OK);
    XrXirSourceGenerationConfig config={0};
    CHECK(xr_source_compiler_generation_config(compiler,"debug-identity",&config)==XR_SOURCE_COMPILE_OK);
    XrXirSourceGenerations *owner=NULL;CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    uint64_t first=0,second=0;
    XrXirSourceSnapshot *old=prepare_copy(compiler,owner,&receiver.context,
        "var value=40\nfn next()->i64{value+=1;return value}\nconst old=next\nprint(17)\n",&first);
    CHECK(xr_xir_compile_source_generation_abort(owner,first)==XR_XIR_OK);
    XrXirSourceSnapshot *fresh=prepare_copy(compiler,owner,&receiver.context,
        "var value=\"shadow\"\nassert(true)\n",&second);
    CHECK(second>first);
    xr_xir_compile_source_generations_free(owner);xray_source_compiler_free(compiler);
    const XrXirSourceDeclaration *a=value_fact(old,XR_XIR_I64),*b=value_fact(fresh,XR_XIR_STRING);
    const XrXirSourceView *av=xr_xir_compile_source_snapshot_view(old),*bv=xr_xir_compile_source_snapshot_view(fresh);
    CHECK(strcmp(av->modules[a->range.module].identity,bv->modules[b->range.module].identity));
    bool core=false;
    for(uint32_t d=0;d<bv->declaration_count;++d) {
        const XrXirSourceDeclaration *declaration=&bv->declarations[d];
        if(declaration->kind!=XR_XIR_SOURCE_INTRINSIC || strcmp(declaration->name,"assert"))continue;
        const XrXirSourceQueryModule *module=&bv->modules[declaration->range.module];
        CHECK(!core && !module->path && !strcmp(module->identity,
            "memory-module-v1:id=23:xray-core-assertions-v1"));core=true;
    }
    CHECK(core && xr_xir_compile_source_snapshot_calls(fresh)->count);
    xr_xir_compile_source_snapshot_free(fresh);value_fact(old,XR_XIR_I64);
    xr_xir_compile_source_snapshot_free(old);source_fixture_owner_free(&receiver);
    puts("Interactive debug identity: exact Checked owners, distinct aborted births, governed core and independent snapshot lifetime PASS");return 0;
}
