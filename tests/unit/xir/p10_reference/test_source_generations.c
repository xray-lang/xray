/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_generations.c - Exact Source records over real Instance transactions
 */
#include "xir/xxir_source_generation.h"
#include "xir/xxir_instance_generation.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_generic.h"
#include "module/xmodule_fingerprint.h"
#include "frontend/parser/xast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"

static XrXirOutputStatus generation_output(void *context,const XrXirOutputGroup *group) {
    unsigned *calls=context;
    CHECK(group && group->count==1 && group->values[0].type==XR_XIR_I64 && group->values[0].payload==17);
    CHECK(!*calls);++*calls;return XR_XIR_OUTPUT_OK;
}
static uint64_t prepare(XrXirSourceGenerations *owner,const char *source) {
    const XrXirSourceText text={NULL,source,strlen(source)};
    uint64_t serial=0;XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_generation_prepare(owner,&text,&serial,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"prepare=%u line=%d %s\n",(unsigned)status,diagnostic.line,diagnostic.message);
    CHECK(status==XR_XIR_OK && serial && diagnostic.status==XR_XIR_OK);return serial;
}
static XrXirProgram *program(const XrXirSourceGenerationView *view,const char *name,uint32_t *entry) {
    XrXirArtifact *closed=NULL,*lowered=NULL;XrXirProgram *output=NULL;
    CHECK(xr_xir_compile_specialize(view->result->checked,&closed,NULL)==XR_XIR_OK);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);bool found=false;
    for (uint32_t i=0;i<module->function_count;++i)
        if (module->functions[i].name_length==strlen(name) && !memcmp(module->functions[i].name,name,strlen(name))) {
            CHECK(!found);*entry=i;found=true;
        }
    CHECK(found && xr_xir_compile_vm_program_take(&lowered,&output)==XR_XIR_OK && !lowered);return output;
}
static XrXirCallStatus generation_poll(XrXirInstance *instance) {
    for (unsigned i=0;i<1024;++i) {
        XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,256).outcome.status;
        if (status!=XR_XIR_CALL_READY) return status;
    }
    CHECK(false);return XR_XIR_CALL_LIMIT;
}
static void old_function(XrXirInstance *instance,const XrXirValue *function) {
    CHECK(xr_xir_instance_start_function(instance,function,NULL,0)==XR_XIR_CALL_READY);
    CHECK(generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && result.payload==5);xr_xir_value_drop(&result);
}
static void source_publish(SourceFixtureOwner *compiler,XrXirSourceGenerations *owner,uint64_t serial) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(compiler->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_xir_compile_source_generation_publish(owner,serial)==XR_XIR_OK);
    CHECK(xr_compile_resources_stats(compiler->context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.work==after.work && before.allocated_bytes==after.allocated_bytes && after.live_bytes<=before.live_bytes);
}
static void compile_failure(XrXirSourceGenerations *owner,uint64_t first) {
    const char source[]="const value=missing";
    const XrXirSourceText text={NULL,source,sizeof(source)-1};
    uint64_t serial=0;XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_generation_prepare(owner,&text,&serial,&diagnostic);
    CHECK(status==XR_XIR_BAD_VALUE);
    CHECK(!serial && diagnostic.status==XR_XIR_BAD_VALUE && diagnostic.line==1 &&
        strstr(diagnostic.message,"name is not an initialized value"));
    XrXirSourceGenerationBinding binding={0};
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK);
    CHECK(binding.generation==first && binding.declaration->type.type==XR_XIR_I64);
}
static void aborted_string(XrXirSourceGenerations *owner,XrXirInstance *instance,uint64_t first) {
    uint64_t serial=prepare(owner,"const value=\"new\"\nexport fn answer()->string{return value}");
    XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK);
    CHECK(!view.published && view.parent==first);
    XrXirSourceGenerationBinding binding={0};
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK && binding.generation==first);
    uint32_t entry=0;XrXirProgram *candidate=program(&view,"answer",&entry);uint64_t runtime=0;
    CHECK(xr_xir_instance_generation_prepare(instance,candidate,&runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&result,&bytes,&length) && length==3 && !memcmp(bytes,"new",3));
    CHECK(xr_xir_instance_generation_abort(instance,runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_compile_source_generation_abort(owner,serial)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_BAD_STAGE);
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK && binding.generation==first);
    xr_xir_compile_program_drop(candidate);xr_xir_value_drop(&result);
}
static void failed_runtime(XrXirSourceGenerations *owner,XrXirInstance *instance,uint64_t first,XrXirCallResult *failure) {
    uint64_t serial=prepare(owner,"const value=9\nassert(false,\"new-fault\")\nexport fn answer()->i64{return value}");
    XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,serial,&view)==XR_XIR_OK);
    uint32_t entry=0;XrXirProgram *candidate=program(&view,"answer",&entry);uint64_t runtime=0;
    CHECK(xr_xir_instance_generation_prepare(instance,candidate,&runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_ASSERTION);
    CHECK(xr_xir_instance_generation_publish(instance,runtime)==XR_XIR_CALL_BAD_STATE);
    CHECK(xr_xir_instance_copy_failure(instance,failure)==XR_XIR_CALL_ASSERTION);
    CHECK(xr_xir_instance_generation_abort(instance,runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_compile_source_generation_abort(owner,serial)==XR_XIR_OK);
    XrXirSourceGenerationBinding binding={0};
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK && binding.generation==first);
    xr_xir_compile_program_drop(candidate);
}
int main(void) {
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    char namespace_id[]="source-generation-test";
    XrXirSourceGenerationConfig config={compiler.context,namespace_id,NULL,NULL,NULL};
    XrXirSourceGenerations *owner=NULL;CHECK(xr_xir_compile_source_generations_new(&config,&owner)==XR_XIR_OK);
    memset(namespace_id,'!',sizeof(namespace_id)-1);
    char source[]="const value=5\nprint(17)\nfn read()->i64{const local=1;return value}\nexport fn make()->fn()->i64{return read}";
    uint64_t first=prepare(owner,source);memset(source,'?',sizeof(source)-1);
    XrXirSourceGenerationView view={0};CHECK(xr_xir_compile_source_generation_view(owner,first,&view)==XR_XIR_OK);
    CHECK(!view.published && !view.parent && view.ast && view.ast->type==AST_PROGRAM);
    CHECK(view.ast->as.program.count==4 && !strcmp(view.ast->as.program.statements[0]->as.var_decl.name,"value") &&
        !strcmp(view.ast->as.program.statements[2]->as.function_decl.name,"read"));
    CHECK(strstr(view.source,"const value=5") && strstr(view.identity,"gen-22-source-generation-test-1"));
    XrFingerprint fingerprint={0};xr_module_source_fingerprint(view.source,&fingerprint);
    CHECK(!memcmp(&fingerprint,&view.fingerprint,sizeof(fingerprint)));
    XrXirSourceGenerationBinding sentinel={99,NULL},binding=sentinel;
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_BAD_STRUCTURE && !memcmp(&binding,&sentinel,sizeof(binding)));
    uint64_t occupied=0;const XrXirSourceText empty={NULL,"",0};
    CHECK(xr_xir_compile_source_generation_prepare(owner,&empty,&occupied,NULL)==XR_XIR_BAD_STAGE && !occupied);
    CHECK(xr_xir_compile_source_generation_publish(owner,first+1)==XR_XIR_BAD_STAGE);
    uint32_t entry=0;XrXirProgram *initial=program(&view,"make",&entry);
    XrXirInstanceConfig runtime_config;CHECK(xr_xir_instance_config_init(&runtime_config,sizeof(runtime_config))==XR_XIR_CALL_READY);
    unsigned output_calls=0;runtime_config.output=(XrXirOutputProvider){.abi_version=XR_XIR_CALL_ABI_VERSION,.write=generation_output,.context=&output_calls};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_generation_new(&runtime_config,&instance)==XR_XIR_CALL_READY);
    uint64_t runtime=0;CHECK(xr_xir_instance_generation_prepare(instance,initial,&runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue function={0};CHECK(xr_xir_instance_take_result(instance,&function)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_generation_publish(instance,runtime)==XR_XIR_CALL_READY);source_publish(&compiler,owner,first);
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK && binding.generation==first && binding.declaration->type.type==XR_XIR_I64);
    binding=sentinel;CHECK(xr_xir_compile_source_generation_lookup(owner,"local",&binding)==XR_XIR_BAD_STRUCTURE && !memcmp(&binding,&sentinel,sizeof(binding)));
    old_function(instance,&function);aborted_string(owner,instance,first);old_function(instance,&function);
    compile_failure(owner,first);old_function(instance,&function);
    XrXirCallResult failure={0};failed_runtime(owner,instance,first,&failure);old_function(instance,&function);
    uint64_t next=prepare(owner,"const value=\"published\"\nexport fn answer()->string{return value}");
    CHECK(next>first+2 && xr_xir_compile_source_generation_view(owner,next,&view)==XR_XIR_OK);
    uint32_t next_entry=0;XrXirProgram *newer=program(&view,"answer",&next_entry);uint64_t next_runtime=0;
    CHECK(xr_xir_instance_generation_prepare(instance,newer,&next_runtime)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,next_entry,NULL,0)==XR_XIR_CALL_READY && generation_poll(instance)==XR_XIR_CALL_RETURNED);
    XrXirValue result={0};CHECK(xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(xr_xir_instance_generation_publish(instance,next_runtime)==XR_XIR_CALL_READY);source_publish(&compiler,owner,next);
    CHECK(xr_xir_compile_source_generation_lookup(owner,"value",&binding)==XR_XIR_OK && binding.generation==next && binding.declaration->type.type==XR_XIR_STRING);
    CHECK(xr_xir_compile_source_generation_view(owner,first,&view)==XR_XIR_OK && view.published && strstr(view.source,"const value=5"));
    xr_xir_compile_source_generations_free(owner);xr_xir_compile_program_drop(initial);xr_xir_compile_program_drop(newer);
    old_function(instance,&function);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(&result,&bytes,&length) && length==9 && !memcmp(bytes,"published",9));
    CHECK(xr_xir_string_view(&failure.panic.message,&bytes,&length) && length==9 && !memcmp(bytes,"new-fault",9));
    xr_xir_value_drop(&result);xr_xir_call_result_drop(&failure);xr_xir_value_drop(&function);
    CHECK(output_calls==1);source_fixture_owner_free(&compiler);
    puts("Owned Source generations: exact bytes/AST/identity, staged names, compile/runtime abort, no-allocation publish and old function5 after producer death passed");
    return 0;
}
