/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_local_errors.c - Declared error checks through owned Source execution
 */
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "base/xcompile_resources.h"
#include "base/xmalloc.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_types.h"
#include "xir/xxir_generic.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c);exit(1);} } while (0)

typedef struct LocalErrorCase {
    const char *name, *text, *diagnostic;
    int64_t expected;
} LocalErrorCase;
static uint32_t local_answer(XrXirSourceProduct *product) {
    XrXirSourceProductPacketView packet={0};XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(packet.length>64 && packet.bytes[8]==38 && !packet.bytes[9] && !packet.bytes[10] && !packet.bytes[11]);
    uint8_t *old=xr_malloc(packet.length);CHECK(old);memcpy(old,packet.bytes,packet.length);old[8]=37;
    CHECK(xr_xir_compile_checked_read(xr_xir_compile_source_product_context(product),old,packet.length,&checked,NULL)==XR_XIR_BAD_STRUCTURE && !checked);
    xr_free(old);
    CHECK(xr_xir_compile_checked_read(xr_xir_compile_source_product_context(product),packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);uint32_t answer=UINT32_MAX;
    if (module->types && module->types->count) {
        size_t bytes=(size_t)module->types->count*sizeof(*module->types->nodes);
        XrXirTypeNode *nodes=xr_malloc(bytes);CHECK(nodes);memcpy(nodes,module->types->nodes,bytes);
        XrXirTypes raw=*module->types;raw.nodes=nodes;
        for (uint32_t t=0;t<raw.count;++t) {
            if (nodes[t].kind!=XR_XIR_TYPE_TASK && nodes[t].kind!=XR_XIR_TYPE_ITERATOR) continue;
            XrXirType error=nodes[t].result;CHECK(error!=XR_XIR_UNIT);
            nodes[t].result=XR_XIR_UNIT;
            CHECK(xr_xir_compile_types_structure_verify(xr_xir_compile_source_product_context(product),&raw)==XR_XIR_BAD_TYPE);
            nodes[t].result=XR_XIR_I64;
            CHECK(xr_xir_compile_types_structure_verify(xr_xir_compile_source_product_context(product),&raw)==XR_XIR_BAD_TYPE);
            nodes[t].result=error;
        }
        xr_free(nodes);
    }
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *function=&module->functions[f];
        if (function->name_length==6 && !memcmp(function->name,"answer",6)) {
            CHECK(answer==UINT32_MAX);answer=f;
        }
        if (function->name_length==4 && !memcmp(function->name,"pure",4))
            CHECK(function->error_type==XR_XIR_ERROR);
        if (function->name_length==6 && !memcmp(function->name,"values",6)) {
            CHECK(function->error_type==XR_XIR_NEVER && module->types);
            uint32_t index=(uint32_t)function->result-XR_XIR_CONSTRUCTED_TYPE_BASE;
            CHECK(index<module->types->count);
            const XrXirTypeNode *iterator=&module->types->nodes[index];
            CHECK(iterator->kind==XR_XIR_TYPE_ITERATOR && iterator->result!=XR_XIR_UNIT);
        }
    }
    CHECK(answer!=UINT32_MAX);xr_xir_compile_artifact_free(checked);return answer;
}
static void local_execute(XrXirSourceProduct *product,const char *name,int64_t expected) {
    uint32_t answer=local_answer(product),entry=xr_xir_compile_source_product_facts(product)->entry;
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
    xr_xir_compile_source_product_free(product);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    for (unsigned n=0;n<2;++n) {
        CHECK(xr_xir_instance_start(instance,n?answer:entry,NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
        if (value.type!=XR_XIR_I64 || (int64_t)value.payload!=(n?expected:0))
            fprintf(stderr,"local execution %s activation%u type%u value%lld expected%lld\n",name,n,
                value.type,(long long)value.payload,(long long)(n?expected:0));
        CHECK(value.type==XR_XIR_I64 && (int64_t)value.payload==(n?expected:0));xr_xir_value_drop(&value);
    }
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
}
static void local_case(const LocalErrorCase *test) {
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};XrXirCompileContext context={0};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"local-errors",NULL};
    XrXirSourceProductRequest request={{session,NULL,&authority,&context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL},
        {XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    size_t length=strlen(test->text);char *text=xr_malloc(length+1);CHECK(text);memcpy(text,test->text,length+1);
    XrXirSourceText input={NULL,text,length};XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build_text(&request,&input,&product,&diagnostic);
    memset(text,0xa5,length+1);xr_free(text);xr_compile_session_free(session);
    if (test->diagnostic) {
        if (status!=XR_XIR_BAD_TYPE || !strstr(diagnostic.source.message,test->diagnostic) ||
            diagnostic.source.line<=0 || diagnostic.source.column<=0)
            fprintf(stderr,"local rejection %s status%u stage%u %d:%d %s\n",test->name,(unsigned)status,
                (unsigned)diagnostic.stage,diagnostic.source.line,diagnostic.source.column,diagnostic.source.message);
        CHECK(status==XR_XIR_BAD_TYPE && !product && diagnostic.source.line>0 && diagnostic.source.column>0);
        CHECK(strstr(diagnostic.source.message,test->diagnostic));
    } else {
        if (status!=XR_XIR_OK) fprintf(stderr,"local product %s status%u stage%u %d:%d %s (function%u block%u instruction%u reason%u)\n",test->name,
            (unsigned)status,(unsigned)diagnostic.stage,diagnostic.source.line,diagnostic.source.column,diagnostic.source.message,
            diagnostic.xir.function,diagnostic.xir.block,diagnostic.xir.instruction,(unsigned)diagnostic.xir.reason);
        CHECK(status==XR_XIR_OK && product);local_execute(product,test->name,test->expected);
    }
    xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes);xr_compile_resources_release(context.resources);
}
int main(void) {
    xr_test_suppress_dialogs();
    static const LocalErrorCase cases[]={
        {"typed propagation","enum Failure{Bad}\nfn value() throws(Failure)->i64{throw Failure.Bad}\n"
            "export fn answer()->i64{try{return value()}catch(e:Failure){return 42}}\n",NULL,42},
        {"declared pure","enum Failure{Bad}\nfn pure() throws(Error)->i64{return 42}\n"
            "export fn answer()->i64{try{return pure()}catch(e){return 7}}\n",NULL,42},
        {"never","fn certain() throws(Never)->i64{return 42}\nexport fn answer()->i64{return certain()}\n",NULL,42},
        {"selected default","enum Failure{Bad}\nfn value() throws(Failure)->i64{throw Failure.Bad}\n"
            "fn selected(x:i64=try value())->i64{return x}\n"
            "fn exact() throws(Failure)->i64{return try selected()}\n"
            "export fn answer()->i64{try{return exact()}catch(e){return selected(42)}}\n",NULL,42},
        {"implicit constructor","enum Failure{Bad}\nfn value() throws(Failure)->i64{throw Failure.Bad}\n"
            "struct Box{value:i64=try value()}\nfn exact() throws(Failure)->Box{return try Box()}\n"
            "export fn answer()->i64{try{const box=exact();return box.value}catch(e){return 42}}\n",NULL,42},
        {"explicit constructor","enum Failure{Bad}\nfn value() throws(Failure)->i64{throw Failure.Bad}\n"
            "struct Box{value:i64=try value()\ninit() throws(Failure){}}\n"
            "export fn answer()->i64{try{const box=Box();return box.value}catch(e){return 42}}\n",NULL,42},
        {"catch patterns","enum Failure{One,Two}\nfn value() throws(Failure)->i64{throw Failure.Two}\n"
            "export fn answer()->i64{try{return value()}catch(Failure.One){return 1}catch(Failure.Two){return 42}}\n",NULL,42},
        {"missing marker","enum Failure{Bad}\nfn value() throws(Failure)->i64{return 1}\n"
            "export fn answer()->i64{return value()}\n","E0330",0},
        {"undeclared throw","enum Failure{Bad}\nexport fn answer()->i64{throw Failure.Bad}\n","E0331",0},
        {"propagation bound","enum Failure{Bad}\nfn value() throws(Failure)->i64{return 1}\n"
            "export fn answer()->i64{return try value()}\n","E0331",0},
        {"default own marker","enum Failure{Bad}\nfn value() throws(Failure)->i64{return 1}\n"
            "fn selected(x:i64=value())->i64{return x}\nexport fn answer()->i64{return selected(42)}\n","E0330",0},
        {"default caller marker","enum Failure{Bad}\nfn value() throws(Failure)->i64{return 1}\n"
            "fn selected(x:i64=try value())->i64{return x}\nexport fn answer()->i64{return selected()}\n","E0330",0},
        {"constructor bound","enum Failure{Bad}\nfn value() throws(Failure)->i64{return 1}\n"
            "struct Box{value:i64=try value()\ninit(){}}\nexport fn answer()->i64{return 42}\n","E0331",0},
        {"closure region","enum Failure{Bad}\nfn value() throws(Failure)->i64{return 1}\n"
            "export fn answer()->i64{try{const f=fn()->i64{return value()};return f()}catch(e){return 42}}\n","E0330",0},
        {"handler region","enum Failure{Bad}\nexport fn answer()->i64{try{throw Failure.Bad}catch(e){throw e}}\n","E0331",0},
        {"incomplete catch","enum Failure{One,Two}\nfn value() throws(Failure)->i64{return 1}\n"
            "export fn answer()->i64{try{return value()}catch(Failure.One){return 42}}\n","E0331",0},
        {"explicit closure","enum Failure{Bad}\nexport fn answer()->i64{"
            "const f:fn() throws(Error)->i64=fn() throws(Never)->i64{throw Failure.Bad};return 42}\n","E0331",0},
        {"contextual closure","enum Failure{Bad}\nfn accept(f:fn()->i64)->i64{return f()}\n"
            "export fn answer()->i64{return accept(fn()->i64{throw Failure.Bad})}\n","E0331",0},
        {"typed generator next","enum Failure{Bad}\nfn values()->Iterator<i64,Failure>{yield 1\nthrow Failure.Bad}\n"
            "fn create()->Iterator<i64,Failure>{return values()}\n"
            "export fn answer()->i64{const it=create();try{const a=it.next();return a+it.next()}catch(e:Failure){return 42}}\n",NULL,42},
        {"typed generator hasNext","enum Failure{Bad}\nfn values()->Iterator<i64,Failure>{throw Failure.Bad\nyield 1}\n"
            "export fn answer()->i64{const it=values();try{if(it.hasNext()){return 1};return 2}catch(e:Failure){return 42}}\n",NULL,42},
        {"never generator","fn values()->Iterator<i64>{yield 42}\n"
            "export fn answer()->i64{const it=values();return it.next()}\n",NULL,42},
        {"task declared error outcome","enum Failure{Bad}\nfn worker() throws(Failure)->i64{throw Failure.Bad}\n"
            "fn create()->Task<i64,Failure>{return go worker()}\n"
            "export fn answer()->i64{try{const task=create();return await task}catch(e:Failure){return 42}catch(e){return -1}}\n",NULL,42},
        {"iterator missing marker","enum Failure{Bad}\nfn values()->Iterator<i64,Failure>{yield 1}\n"
            "export fn answer() throws(Failure)->i64{const it=values();return it.next()}\n","E0330",0},
        {"generator missing bound","enum Failure{Bad}\nfn values()->Iterator<i64>{yield 1\nthrow Failure.Bad}\n"
            "export fn answer()->i64{return 42}\n","E0331",0},
        {"generator duplicate bound","enum Failure{Bad}\nfn values() throws(Failure)->Iterator<i64,Failure>{yield 1}\n"
            "export fn answer()->i64{return 42}\n","generator errors belong",0},
        {"defer declaration","enum Failure{Bad}\nfn value() throws(Failure)->i64{return 1}\n"
            "export fn answer()->i64{defer{const x=try value()};return 42}\n","E0387",0},
        {"generic iterator errors","enum Failure{Bad}\nfn values()->Iterator<i64,Failure>{throw Failure.Bad\nyield 1}\n"
            "fn consume<E:Error>(it:Iterator<i64,E>) throws(E)->i64{return try it.next()}\n"
            "export fn answer()->i64{try{return consume(values())}catch(e:Failure){return 42}}\n",NULL,42},
        {"generic task errors","enum Failure{Bad}\nfn worker() throws(Failure)->i64{throw Failure.Bad}\n"
            "fn identity<E:Error & Sendable>(task:Task<i64,E>)->Task<i64,E>{return task}\n"
            "export fn answer()->i64{try{return await identity(go worker())}catch(e){return 42}}\n",NULL,42},
        {"invalid iterator error","fn consume(it:Iterator<i64,i64>){}\nexport fn answer()->i64{return 42}\n",
            "error type must satisfy Error",0},
        {"unbounded iterator error","fn consume<E>(it:Iterator<i64,E>){}\nexport fn answer()->i64{return 42}\n",
            "error type must satisfy Error",0},
        {"nonSendable task error","enum Failure{Bad{payload:fn()->i64}}\n"
            "fn worker() throws(Failure)->i64{return 1}\n"
            "export fn answer()->i64{const task=go worker();return 42}\n",
            "Task error type must satisfy Error and Sendable",0},
        {"unknown generic task error","fn worker<E:Error>(error:E) throws(E)->i64{throw error}\n"
            "fn spawn<E:Error & Sendable>(error:E)->Task<i64,E>{return go worker<E>(error)}\n"
            "export fn answer()->i64{return 42}\n","Task worker declared error type must satisfy Sendable",0},
        {"declared generic task error","enum Failure{Bad}\n"
            "fn worker<E:Error & Sendable>(error:E) throws(E)->i64{throw error}\n"
            "fn spawn<E:Error & Sendable>(error:E)->Task<i64,E>{return go worker<E>(error)}\n"
            "export fn answer()->i64{try{return await spawn(Failure.Bad)}catch(e){return 42}}\n",NULL,42},
        {"Task receiver unknown error","fn receive<E:Error>(task:Task<i64,E>){}\nexport fn answer()->i64{return 42}\n",
            "Task error type must satisfy Error and Sendable",0},
        {"synchronous iterator nonSendable error","enum Failure{Bad{payload:fn()->i64}}\n"
            "fn values()->Iterator<i64,Failure>{yield 42}\n"
            "export fn answer()->i64{const it=values();try{return it.next()}catch(e){return -1}}\n",NULL,42},
        {"pure generated initializer cycle","struct Loop{value:i64=Loop().value}\n"
            "export fn answer()->i64{return 42}\n","generated initialization error cycles are not yet supported",0},
        {"throwing generated initializer cycle","enum Failure{Bad}\nfn read() throws(Failure)->i64{throw Failure.Bad}\n"
            "struct Loop{first:i64=try read()\nsecond:i64=try Loop().second}\n"
            "export fn answer()->i64{return 42}\n","generated initialization error cycles are not yet supported",0},
        {"explicit initializer boundary","enum Failure{Bad}\nvar depth=0\n"
            "fn read() throws(Failure)->i64{if(depth==0){depth=1;const nested=try Box();return nested.value};return 42}\n"
            "struct Box{value:i64=try read()\ninit() throws(Failure){}}\n"
            "export fn answer()->i64{try{const box=Box();return box.value}catch(e){return -1}}\n",NULL,42},
        {"nested iterator error name","enum Failure{Bad}\nstruct Box<T>{value:T}\n"
            "fn values()->Iterator<i64,Failure>{yield 42}\n"
            "export fn answer()->i64{const box=Box<Array<Iterator<i64,Failure>>>{value:[values()]};"
            "if(typeName(box)==\"Box<Array<iterator<i64, Failure>>>\"){return 42};return 0}\n",NULL,42},
        {"nested iterator Never name","struct Box<T>{value:T}\nfn values()->Iterator<i64>{yield 42}\n"
            "export fn answer()->i64{const box=Box<Iterator<i64>>{value:values()};"
            "if(typeName(box)==\"Box<iterator<i64, Never>>\"){return 42};return 0}\n",NULL,42},
        {"nested iterator Error name","struct Box<T>{value:T}\nfn values()->Iterator<i64,Error>{yield 42}\n"
            "export fn answer()->i64{const box=Box<Iterator<i64,Error>>{value:values()};"
            "if(typeName(box)==\"Box<iterator<i64, Error>>\"){return 42};return 0}\n",NULL,42},
        {"nested Task error name","enum Failure{Bad}\nstruct Box<T>{value:T}\n"
            "fn worker() throws(Failure)->i64{return 1}\n"
            "export fn answer()->i64{const box=Box<Task<i64,Failure>>{value:go worker()};"
            "if(typeName(box)==\"Box<Task<i64, Failure>>\"){return 42};return 0}\n",NULL,42}
    };
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) local_case(&cases[i]);
    puts("Local declared errors, markers, defaults, constructors and independent VM execution passed");return 0;
}
