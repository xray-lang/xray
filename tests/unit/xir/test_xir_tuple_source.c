/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_tuple_source.c - Owned source, Checked specialization and VM results
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"FAIL %d %s\n",__LINE__,#c);exit(1);}} while (0)
#include "xir_source_program_compile_owner.h"
#include "xir_runtime_allocations.h"
#include "program/xr_xir_source_product.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_tuple.h"
#include "tuple_runtime_cases.h"
static void string_is(const XrXirValue *value,const char *expected) {
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(value,&bytes,&length) && length==strlen(expected) && !memcmp(bytes,expected,length));
}
static void vm_oracle(XrXirProgram *program,uint32_t entry) {
    tuple_runtime_faults(program,entry);
    XrXirValue escaped[2]={{0},{0}};
    for (uint32_t i=0;i<2;++i) {
        XrXirInstanceConfig config;XrXirInstance *instance=NULL;unsigned groups=0;
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,tuple_output,&groups};
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&escaped[i])==XR_XIR_CALL_RETURNED);
        CHECK(groups==1);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);
    for (uint32_t i=0;i<2;++i) {
        XrXirValue inner={0},generic={0},number={0},text={0},unit={0};
        CHECK(xr_xir_tuple_get(&escaped[i],2,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I64 && number.payload==1323);
        CHECK(xr_xir_tuple_get(&escaped[i],0,&inner)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_tuple_get(&inner,0,&number)==XR_XIR_VALUE_BAD_ARGUMENT && number.payload==1323);
        xr_xir_value_drop(&number);
        CHECK(xr_xir_tuple_get(&inner,0,&number)==XR_XIR_VALUE_OK && number.type==XR_XIR_I64 && number.payload==11);
        CHECK(xr_xir_tuple_get(&inner,1,&unit)==XR_XIR_VALUE_OK && !unit.payload);
        CHECK(xr_xir_tuple_get(&inner,3,&unit)==XR_XIR_VALUE_OK && !unit.payload);
        CHECK(xr_xir_tuple_get(&inner,2,&text)==XR_XIR_VALUE_OK);string_is(&text,"tuple-owner");
        CHECK(xr_xir_tuple_get(&escaped[i],1,&generic)==XR_XIR_VALUE_OK);
        xr_xir_value_drop(&escaped[i]);xr_xir_value_drop(&inner);string_is(&text,"tuple-owner");xr_xir_value_drop(&text);
        xr_xir_value_drop(&number);
        CHECK(xr_xir_tuple_get(&generic,0,&number)==XR_XIR_VALUE_OK && number.payload==7);
        CHECK(xr_xir_tuple_get(&generic,1,&text)==XR_XIR_VALUE_OK);string_is(&text,"generic-owner");
        xr_xir_value_drop(&generic);string_is(&text,"generic-owner");xr_xir_value_drop(&text);xr_xir_value_drop(&number);
    }
    CHECK(!runtime_live && !runtime_bytes);
    puts("VM independent Tuple oracle order1323/i64=11/generic7/strings/Unit and Program/Instance first-drop physical0 PASS");
}
int main(int argc,char **argv) {
    CHECK(argc==1 || argc==2 || (argc==3 && !strcmp(argv[1],"--reject")));
    const XrXirCompileContext *context=source_program_owner(UINT64_C(64)*1024*1024,UINT64_C(128000000));
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TUPLE_FIXTURES};
    XrXirSourceProductRequest request={{session,XR_TUPLE_FIXTURES "/root.xr",&authority,context,
        XR_TUPLE_STDLIB,NULL,XR_XIR_PROGRAM,NULL},{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION}};
    char rejected_path[1024]={0};
    bool accept_destructure=argc==2 && !strcmp(argv[1],"--accept-destructure");
    if (argc==3 || accept_destructure) {
        int bytes=snprintf(rejected_path,sizeof(rejected_path),"%s/%s/root.xr",XR_TUPLE_CLOSED_ROOT,
            accept_destructure ? "destructure" : argv[2]);
        CHECK(bytes>0 && (size_t)bytes<sizeof(rejected_path));
        request.source.entry_path=rejected_path;authority.physical_root=XR_TUPLE_CLOSED_ROOT;
    }
    XrXirSourceProduct *product=NULL;XrXirSourceProductDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_product_build(&request,&product,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"source status=%u stage=%u line=%d: %s\n",status,
        diagnostic.stage,diagnostic.source.line,diagnostic.source.message);
    if (argc==3) {
        CHECK(status==XR_XIR_BAD_TYPE && !product);
        CHECK(diagnostic.stage==XR_XIR_SOURCE_PRODUCT_CHECK && diagnostic.source.line>0);
        xr_xir_compile_source_product_diagnostic_free(&diagnostic);xr_compile_session_free(session);
        source_program_owners_free();printf("Tuple Source %s closed at actual check status=%u PASS\n",argv[2],status);return 0;
    }
    CHECK(status==XR_XIR_OK && product);xr_xir_compile_source_product_diagnostic_free(&diagnostic);
    xr_compile_session_free(session);memset(&request,0xcc,sizeof(request));memset(&authority,0xcc,sizeof(authority));
    if(accept_destructure) {
        XrXirCSource generated={0};XrXirProgram *program=NULL;XrXirInstance *instance=NULL;XrXirInstanceConfig config;
        CHECK(xr_xir_compile_source_product_emit(product,"tuple_original_destructure",1048576,&generated)==XR_XIR_OK);
        CHECK(strstr(generated.text,"xr_xir_instance_slot_group_init(view,0u,2u,") && !strstr(generated.text,"({"));
        xr_xir_compile_c_source_free(&generated);
        CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK);
        xr_xir_compile_source_product_free(product);product=NULL;
        CHECK(program->declarations->slot_count==2 && program->declarations->slots[0].type==XR_XIR_I64 &&
            program->declarations->slots[1].type==XR_XIR_I64);
        CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,program->declarations->entry_function,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,1000000).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(instance->publication_count==2 && instance->published[0] && instance->published[1] &&
            instance->slots[0].type==XR_XIR_I64 && instance->slots[0].payload==1 &&
            instance->slots[1].type==XR_XIR_I64 && instance->slots[1].payload==2);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
        CHECK(!runtime_live && !runtime_bytes);source_program_owners_free();
        puts("Original module tuple declaration accepted, emitted actual group, VM slots1/2, physical0 PASS");return 0;
    }
    XrXirSourceProductPacketView packet={0};XrXirArtifact *checked=NULL;
    CHECK(xr_xir_compile_source_product_packet(product,XR_XIR_SOURCE_PRODUCT_CLOSED,&packet)==XR_XIR_OK);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(checked);uint32_t entry=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *fn=&module->functions[f];
        if (fn->name_length==4 && !memcmp(fn->name,"make",4)) entry=f;
    }
    CHECK(entry!=UINT32_MAX);xr_xir_compile_artifact_free(checked);
    XrXirCSource source={0};CHECK(xr_xir_compile_source_product_emit(product,"tuple_source",UINT64_C(8)*1024*1024,&source)==XR_XIR_OK);
    if (argc==2) {FILE *file=fopen(argv[1],"wb");CHECK(file);CHECK(fwrite(source.text,1,source.length,file)==source.length);
        CHECK(fprintf(file,"\nconst uint32_t tuple_source_make=%uu;\n",entry)>0);CHECK(!fclose(file));}
    xr_xir_compile_c_source_free(&source);
    if (argc==1) {XrXirProgram *program=NULL;CHECK(xr_xir_compile_source_product_vm_take(product,&program)==XR_XIR_OK && program);
        xr_xir_compile_source_product_free(product);vm_oracle(program,entry);}
    else xr_xir_compile_source_product_free(product);
    source_program_owners_free();return 0;
}
