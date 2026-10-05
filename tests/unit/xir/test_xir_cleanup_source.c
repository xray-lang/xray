/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_cleanup_source.c - Source cleanup through packets, specialization and VM execution
 */
#include "xir/xxir_source.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_generic.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir_cleanup_source_cases.h"
static void cleanup_member_queries(const XrXirSourceView *view) {
    unsigned fields = 0, root = 0;
    CHECK(view && view->complete);
    for (uint32_t i = 0; i < view->reference_count; ++i) {
        const XrXirSourceReference *ref = &view->references[i];
        CHECK(ref->target && ref->target <= view->declaration_count);
        if (ref->access != XR_XIR_SOURCE_READ_WRITE) continue;
        const XrXirSourceDeclaration *decl = &view->declarations[ref->target - 1];
        if (decl->kind == XR_XIR_SOURCE_MEMBER && !strcmp(decl->name, "value")) ++fields;
        if (decl->kind == XR_XIR_SOURCE_BINDING && !strcmp(decl->name, "compoundState")) ++root;
    }
    CHECK(fields >= 7 && root >= 5);
}
static XrXirStatus cleanup_source_lower(const XrXirCompileContext *context,XrXirArtifact **output) {
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus setup=xr_compile_session_new(context->resources,&session);
    if(setup!=XR_COMPILER_SESSION_OK)return setup==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BUDGET;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_CLEANUP_FIXTURES};
    XrXirSourceRequest request={session,XR_CLEANUP_FIXTURES "/root.xr",&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};XrXirCheckedPacket packet={0};
    XrXirArtifact *checked=NULL,*closed=NULL;
    XrXirStatus status=xr_xir_compile_source_check(&request,&result,&diagnostic,NULL);
    if(status!=XR_XIR_OK) {
        CHECK(!result.checked && !result.snapshot);
        if(status!=XR_XIR_OUT_OF_MEMORY && status!=XR_XIR_BUDGET)
            fprintf(stderr,"cleanup source %u at %u:%d:%d %s\n",status,diagnostic.module,diagnostic.line,diagnostic.column,diagnostic.message);
    }
    if(status==XR_XIR_OK) {
        cleanup_member_queries(xr_xir_compile_source_snapshot_view(result.snapshot));
        status=xr_xir_compile_checked_write(result.checked,&packet,NULL);
    }
    xr_xir_compile_source_result_free(&result);xr_compile_session_free(session);
    if(status==XR_XIR_OK)status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL);
    if(packet.bytes)memset(packet.bytes,0xCC,packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    if(status==XR_XIR_OK)status=xr_xir_compile_specialize(checked,&closed,NULL);
    xr_xir_compile_artifact_free(checked);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(closed,&target,output,NULL);
    xr_xir_compile_artifact_free(closed);return status;
}
static void cleanup_constructor_storage(const XrXirModule *module) {
    unsigned ordinary = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length != 11 || memcmp(function->name, "constructor", 11)) continue;
        const XrXirTypeNode *type = xr_xir_type_node(module->types, function->result);
        CHECK(type && type->kind == XR_XIR_TYPE_NOMINAL);
        CHECK(module->types->nominals->identities && !module->types->nominals->declarations);
        XrXirLiteral name = module->types->nominals->identities[type->nominal.declaration].name;
        bool plain = name.length == 16 && !memcmp(name.bytes, "PlainConstructor", 16);
        bool unrelated = name.length == 21 && !memcmp(name.bytes, "UncapturedConstructor", 21);
        if (!plain && !unrelated) continue;
        ++ordinary;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            const XrXirInstruction *op = &function->instructions[i];
            CHECK(op->op != XR_XIR_CELL_NEW && op->op != XR_XIR_CELL_LOCAL_WRITE);
        }
    }
    CHECK(ordinary == 2);
}
static const char *cleanup_source_output_path;
static uint32_t cleanup_source_ids[CLEANUP_SOURCE_FUNCTIONS+4];
static void cleanup_source_find(const XrXirModule *module) {
    const char *names[]={"scopes","loops","errors","panic","cancelled","generics","snapshot","constructorLate","constructorNested","constructorBare","constructorUncaptured","memberCompound","memberOperators","memberResume","memberCancel","memberFailure","fatalReturn","fatalError","fatalPanic","fatalCancel"};
    for(unsigned n=0;n<CLEANUP_SOURCE_FUNCTIONS+4;++n) {
        cleanup_source_ids[n]=UINT32_MAX;
        for(uint32_t f=0;f<module->function_count;++f)
            if(module->functions[f].name_length==strlen(names[n]) && !memcmp(module->functions[f].name,names[n],strlen(names[n]))) {
                CHECK(cleanup_source_ids[n]==UINT32_MAX);cleanup_source_ids[n]=f;
            }
        CHECK(cleanup_source_ids[n]!=UINT32_MAX);
    }
}
static XrXirStatus cleanup_source_build(const XrXirCompileContext *context,unsigned variant,XrXirProgram **output) {
    (void)variant;XrXirArtifact *lowered=NULL;XrXirCSource source={0};
    XrXirStatus status=cleanup_source_lower(context,&lowered);
    if(status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        cleanup_constructor_storage(module);cleanup_source_find(module);
        status=xr_xir_compile_emit_c(lowered,"cleanup_source",4194304,&source);
    }
    if(status==XR_XIR_OK) {
        CHECK(source.text[source.length]==0 && !strstr(source.text,"({") && !strstr(source.text,"xr_xir_vm"));
        if(cleanup_source_output_path) {
            FILE *file=fopen(cleanup_source_output_path,"wb");CHECK(file);
            CHECK(fwrite(source.text,1,source.length,file)==source.length);
            CHECK(fprintf(file,"\nconst uint32_t cleanup_source_functions[%u] = {",CLEANUP_SOURCE_FUNCTIONS)>0);
            for(unsigned i=0;i<CLEANUP_SOURCE_FUNCTIONS;++i)CHECK(fprintf(file,"%s%uu",i?",":"",cleanup_source_ids[i])>0);
            CHECK(fputs("};\nconst uint32_t cleanup_source_fatal_functions[4] = {",file)>=0);
            for(unsigned i=0;i<4;++i)CHECK(fprintf(file,"%s%uu",i?",":"",cleanup_source_ids[CLEANUP_SOURCE_FUNCTIONS+i])>0);
            CHECK(fputs("};\n",file)>=0 && fclose(file)==0);
        }
    }
    xr_xir_compile_c_source_free(&source);
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&lowered,output);
    if(status!=XR_XIR_OK)CHECK(!*output);
    xr_xir_compile_artifact_free(lowered);return status;
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2 || (argc==3 && !strcmp(argv[1],"--fatal")));
    if(argc==2)cleanup_source_output_path=argv[1];
    XrXirCompileContext context=cleanup_source_context((XrCompileResourceLimits){67108864,8388608,128000000});
    XrCompileResourceStats baseline=cleanup_source_stats(&context);XrXirProgram *program=NULL;
    XrXirStatus status=cleanup_source_build(&context,0,&program);
    if(status!=XR_XIR_OK)fprintf(stderr,"cleanup whole pipeline status%u\n",status);
    CHECK(status==XR_XIR_OK && program);
    if(argc==3) {
        unsigned mode=(unsigned)atoi(argv[2]);CHECK(mode<4);
        cleanup_source_fatal(program,cleanup_source_ids[CLEANUP_SOURCE_FUNCTIONS+mode]);
    }
    if(argc==1) {
        cleanup_source_primary_stats(&context,0);
        cleanup_source_allocations(program,cleanup_source_ids);cleanup_source_cases(program,cleanup_source_ids);program=NULL;
    }
    xr_xir_compile_program_drop(program);cleanup_source_owner_free(&context,baseline);
    if(argc==1)cleanup_source_compiler(cleanup_source_build,0);
    puts("Source cleanup VM: lexical exits, late reads, nested bodies and independent results passed");return 0;
}
