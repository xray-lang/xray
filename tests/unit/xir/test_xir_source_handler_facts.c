/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_handler_facts.c - Owned selectors and lexical error routes
 */
#include "xir/xxir_internal.h"
#include "xir/xxir_source.h"
#include "xir/xxir_source_query_internal.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir_library_compile_owner.h"

typedef struct HandlerFactsCase { uint32_t which;bool inspect; } HandlerFactsCase;
typedef struct HandlerFactsSource { const char *text;uint32_t handlers,selectors,routes; } HandlerFactsSource;
static const HandlerFactsSource handler_sources[]={
    {"enum E{Bad}\nfn leaf()->i64{throw E.Bad}\nfn run()->i64{try{\n"
     "try{return leaf()}catch(e:E){throw e}\n}catch(e:E){return 7}}\nfn main()->i64{return run()}\n",2,2,2},
    {"enum I{X{code:i64},Y}\nenum E{A{inner:I,text:string,flag:bool,r:rune,f:f64},B{code:i64},C{code:i64}}\n"
     "fn run()->i64{try{throw E.B{code:4}}\n"
     "catch(E.A{inner:I.X{code:1..=5},text:\"x\\0中\",flag:true,r:'中',f:1.25}){return 1}\n"
     "catch(E.B{code},E.C{code}){return code}}\nfn main()->i64{return run()}\n",1,2,1},
    {"enum E<T>{Bad{value:T}}\nfn run<T>(v:T)->T{try{throw E<T>.Bad{value:v}}"
     "catch(E<T>.Bad{value}){return value}}\nfn main()->i64{return run<i64>(13)}\n",1,1,1},
    {"enum E{Bad}\nfn run()->i64{try{try{throw E.Bad}catch(e){return 3}}catch(e:E){return 4}}\n"
     "fn dead()->i64{try{}catch(e:E){throw E.Bad};return 5}\n"
     "fn panic_only()->i64{try{return 1/0}catch panic{return 6}}\nfn main()->i64{return run()}\n",1,1,1},
    {"enum E{Bad}\nfn leaf()->i64{throw E.Bad}\nfn run()->i64{try{return await go leaf()}catch(e:E){return 7}}\n"
     "fn main()->i64{return run()}\n",1,1,1},
    {"enum E{Bad}\nfn run()->i64{try{defer{const value=9};throw E.Bad}catch(e:E){return 7}}\n"
     "fn main()->i64{return run()}\n",1,1,1}
};
static uint32_t handler_function(const XrXirModule *module,const char *name) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==strlen(name) && !memcmp(module->functions[f].name,name,strlen(name))) return f;
    CHECK(false);return UINT32_MAX;
}
static XrXirStatus handler_source(const XrXirCompileContext *context,uint32_t which,XrXirSourceResult *output) {
    char text[1024];size_t length=strlen(handler_sources[which].text);CHECK(length<sizeof(text));memcpy(text,handler_sources[which].text,length+1);
    XrCompilerSession *session=NULL;XrCompilerSessionStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILER_SESSION_OK) return created==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"handler-facts",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,length};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,&diagnostic,NULL);
    xr_compile_session_free(session);memset(text,0xff,sizeof(text));
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"handler source%u status%u line%d %s\n",which,status,diagnostic.line,diagnostic.message);
    return status;
}
static void handler_check(const XrXirSourceDebugView *view,uint32_t run,uint32_t which) {
    CHECK(view && run<view->function_count);const XrXirSourceDebugFunction *function=&view->functions[run];
    CHECK(function->handler_count==handler_sources[which].handlers && function->route_count==handler_sources[which].routes);
    uint32_t selectors=0;
    for (uint32_t h=0;h<function->handler_count;++h) {
        const XrXirSourceDebugHandler *handler=&function->handlers[h];selectors+=handler->selector_count;
        CHECK(handler->error>=function->parameter_count && handler->error-function->parameter_count<function->instruction_count);
        CHECK(!handler->parent || (handler->parent>h+1 && handler->parent<=function->handler_count));
        for (uint32_t s=0;s<handler->selector_count;++s) {
            const XrXirSourceDebugSelector *selector=&handler->selectors[s];
            CHECK(selector->first<=selector->accepted && selector->accepted<function->instruction_count);
            CHECK(function->points[selector->accepted].flags&XR_XIR_SOURCE_DEBUG_HANDLER_ACCEPT);
            CHECK(!s || handler->selectors[s-1].rejected==selector->first);
        }
        CHECK(handler->selectors[handler->selector_count-1].rejected==handler->propagation);
    }
    CHECK(selectors==handler_sources[which].selectors);
    for (uint32_t r=0;r<function->route_count;++r) {
        CHECK(function->routes[r].handler && function->routes[r].handler<=function->handler_count);
        CHECK(function->routes[r].instruction<function->instruction_count);
        CHECK(!r || function->routes[r-1].instruction<function->routes[r].instruction);
    }
    if (!which) {
        CHECK(function->handlers[0].parent==2 && !function->handlers[1].parent);
        CHECK(function->routes[0].handler==1 && function->routes[1].handler==2);
    } else if (which==3) {
        CHECK(function->handlers[0].propagation==UINT32_MAX && !function->handlers[0].parent);
        CHECK(function->handlers[0].selectors[0].first==function->handlers[0].selectors[0].accepted);
    }
}
#include "xir_source_handler_mutations.h"
static XrXirStatus handler_operation(const XrXirCompileContext *context,void *opaque) {
    const HandlerFactsCase *fixture=opaque;XrXirSourceResult source={0};XrXirSourceSnapshot *copy=NULL;
    XrXirStatus status=handler_source(context,fixture->which,&source);uint32_t run=UINT32_MAX;
    if (status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);run=handler_function(module,"run");
        const XrXirSourceDebugView *debug=xr_xir_compile_source_snapshot_debug(source.snapshot);handler_check(debug,run,fixture->which);
        if (fixture->which==3) {
            CHECK(!debug->functions[handler_function(module,"dead")].handler_count);
            CHECK(!debug->functions[handler_function(module,"panic_only")].handler_count);
        }
        if (fixture->which==4) CHECK(module->functions[run].instructions[debug->functions[run].routes[0].instruction].op==XR_XIR_TASK_AWAIT);
        if (fixture->which==5) CHECK(module->functions[run].instructions[debug->functions[run].routes[0].instruction].op==XR_XIR_CLEANUP_ERROR);
        if (fixture->inspect && !fixture->which) handler_mutations(context,&source,run);
        status=xr_xir_compile_source_snapshot_copy_v2(context,xr_xir_compile_source_snapshot_view(source.snapshot),
            xr_xir_compile_source_snapshot_construction(source.snapshot),&copy);
        if (status==XR_XIR_OK) status=handler_copy_poison(context,copy,debug,module,run);
    }
    xr_xir_compile_source_result_free(&source);
    if (status==XR_XIR_OK) handler_check(xr_xir_compile_source_snapshot_debug(copy),run,fixture->which);
    else if (copy) CHECK(!xr_xir_compile_source_snapshot_debug(copy));
    xr_xir_compile_source_snapshot_free(copy);return status;
}
static XrXirStatus handler_retry(const XrXirCompileContext *context,void *opaque) {
    size_t blocks=source_program_compile_live,bytes=source_program_compile_bytes;XrXirStatus status=handler_operation(context,opaque);
    CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);
    if (status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected && source_program_compile_fail_at!=SIZE_MAX) {
        size_t failed=source_program_compile_fail_at;XrCompileResourceStats paid=library_compile_stats(context);
        source_program_compile_fail_at=SIZE_MAX;CHECK(handler_operation(context,opaque)==XR_XIR_OK);
        XrCompileResourceStats retried=library_compile_stats(context);
        CHECK(retried.live_bytes==paid.live_bytes && retried.allocated_bytes>paid.allocated_bytes && retried.work>paid.work);
        CHECK(source_program_compile_live==blocks && source_program_compile_bytes==bytes);source_program_compile_fail_at=failed;
    }
    return status;
}
int main(int argc,char **argv) {
    if (argc==3 && !strcmp(argv[1],"--compiler")) {
        char *end=NULL;unsigned long which=strtoul(argv[2],&end,10);CHECK(end && end!=argv[2] && !*end && which<6);
        HandlerFactsCase fixture={(uint32_t)which,false};
        library_compile_operation_cases("Source handler selectors, routes, producer death and same-owner retry",handler_retry,&fixture);
    } else if (argc==1 || (argc==2 && !strcmp(argv[1],"--census"))) {
        for (uint32_t which=0;which<6;++which) {
            LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
            HandlerFactsCase fixture={which,argc==1};source_program_compile_attempts=0;
            CHECK(handler_operation(&owner.context,&fixture)==XR_XIR_OK);
            XrCompileResourceStats stats=library_compile_stats(&owner.context);
            printf("Source handler case%u census=%zu allocated=%llu peak=%llu work=%llu\n",which,source_program_compile_attempts,
                (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);
            library_compile_owner_drop(&owner);
        }
    } else return 2;
    library_compile_observer_free();return 0;
}
