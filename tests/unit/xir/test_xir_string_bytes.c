/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_string_bytes.c - Authentic borrowed String calls and independent receivers
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_intrinsic.h"
#include "xir/xxir_ctfe.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_vm_debug_internal.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_view_normalize_internal.h"
#include "toolchain/xcompiler_session.h"
#include "base/xresource_limits.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "string_bytes_cases.h"
#include "../test_win_compat.h"
#define BYTES_ERROR_SOURCE \
    "enum E{Bad}\n" \
    "fn scalar()->i64{return 7}\n" \
    "export fn bytesCatch()->i64{const text=\"abc\";const v=text.bytes();try{const n=scalar();throw E.Bad}catch(e:E){return len(v)}}\n"
static const char error_text[]=BYTES_ERROR_SOURCE;
static const char source_text[]=
    BYTES_ERROR_SOURCE
    "export fn read(v:Slice<u8>)->i64{return len(v)}\n"
    "export fn readFunction()->fn(Slice<u8>)->i64{return read}\n"
    "fn marked(count:ref i64)->string{count+=1;return \"abc\"}\n"
    "export fn unused(text:string){const ignored=text.bytes()}\n"
    "export fn probe(text:string)->i64{const view=text.bytes();return read(view)}\n"
    "export fn loan(text:ref string)->Slice<u8>{return text.bytes()}\n"
    "export fn loanFunction()->fn(ref string)->Slice<u8>{return loan}\n"
    "fn callback()->i64{const text=\"hi\";const view=text.bytes();const visit=fn()->i64{return len(view)};return visit()}\n"
    "export fn exact(text:string)->bool{const view:Slice<u8>=text.bytes();return view[0]==195 && view[1]==169 && view[2]==0 && view[3]==65}\n"
    "export fn run()->i64{const empty=\"\";const ascii=\"abc\";var text=\"\xc3\xa9\\0A\";"
    "const first=probe(empty);const second=probe(ascii);var count=0;const once=probe(marked(ref count));"
    "if(count!=1 || once!=3){return -4};if(bytesCatch()!=3){return -5};"
    "const view=text.bytes();const copy=view;const middle:Slice<u8>=copy[1:3];"
    "const part=read(middle);const size=read(copy);text=\"other\";unused(text);"
    "const returned=loan(ref text);if(read(returned)!=5){return -2};text=\"x\";if(callback()!=2){return -3};"
    "const original=\"\xc3\xa9\\0A\";if(!exact(original)){return -1};"
    "return first*1000+second*100+size*10+part}\n";
static uint32_t named(const XrXirModule *module,const char *name) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==strlen(name) &&
            !memcmp(module->functions[f].name,name,strlen(name))) return f;
    CHECK(false);return UINT32_MAX;
}
static XrXirStatus source_check(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *result,XrXirSourceDiagnostic *diagnostic,XrXirLinkageKind linkage) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"string-byte-borrows",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=linkage};
    XrXirSourceText input={NULL,text,strlen(text)};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,diagnostic,NULL);
    xr_compile_session_free(session);
    return status;
}
static void registry(void) {
    const XrNativeIntrinsicDeclaration *call=xr_xir_intrinsic(4);
    CHECK(call && xr_xir_intrinsic_is_string_bytes(call) && !call->ctfe);
    CHECK(call->member->operation==XR_NATIVE_OPERATION_STRING_BYTES && call->member->parameter_count==0);
    CHECK(call->member->allocation==XR_NATIVE_ALLOCATION_MAY_HEAP &&
        call->member->ownership==XR_NATIVE_RESULT_OWNERSHIP_BORROWED &&
        !strcmp(call->member->failures,"allocation,retain,limit"));
    for (uint32_t id=1;id<4;++id) {
        call=xr_xir_intrinsic(id);
        CHECK(call && call->ctfe && call->member->allocation==XR_NATIVE_ALLOCATION_NO_HEAP &&
            call->member->ownership==XR_NATIVE_RESULT_OWNERSHIP_OWNED && !strcmp(call->member->failures,"none"));
    }
    CHECK(!xr_xir_intrinsic(0) && !xr_xir_intrinsic(UINT32_MAX));
}
static void inspect(const XrXirModule *module) {
    const XrXirFunction *function=module->functions+named(module,"unused");bool found=false,cleanup=false;
    for (uint32_t i=0;i<function->instruction_count;++i) {
        const XrXirInstruction *op=function->instructions+i;
        if (op->op==XR_XIR_CALL_INTRINSIC) {
            CHECK(xr_xir_instruction_is_string_bytes(op) && op->args[1]==1);
            CHECK(xr_xir_intrinsic_result_matches(xr_xir_intrinsic(op->immediate),module->types,op->type));
            found=true;
        }
        CHECK(op->op!=XR_XIR_VIEW_LEN && op->op!=XR_XIR_VIEW_GET && op->op!=XR_XIR_CALL);
        if (op->op==XR_XIR_VIEW_CLEANUP) {
            CHECK(op->args[1]==1);cleanup=true;
            printf("unused bytes ordinary cleanup instruction=%u owners=%u\n",i,op->args[1]);
        }
    }
    CHECK(found && cleanup);
}
static void snapshot_debug(const XrXirSourceResult *source,bool local_source) {
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_snapshot_debug(source->snapshot);
    CHECK(debug && debug->function_count==module->function_count);
    uint32_t f=named(module,"unused");const XrXirFunction *function=module->functions+f;
    const XrXirSourceDebugFunction *row=debug->functions+f;
    CHECK(row->instruction_count==function->instruction_count);
    if (local_source) CHECK(row->points);
    if (!row->points) return;
    for (uint32_t i=0;i<function->instruction_count;++i) {
        if (function->instructions[i].op==XR_XIR_VIEW_CLEANUP)
            CHECK(!row->points[i].statement && !row->points[i].flags);
        if (xr_xir_instruction_is_string_bytes(function->instructions+i))
            CHECK(row->points[i].statement && row->points[i].range.line>0);
    }
}
static XrXirArtifact *replay_source(const XrXirCompileContext *context,
    XrXirSourceResult *source,uint8_t identity[32],bool local_source) {
    inspect(xr_xir_compile_artifact_module(source->checked));snapshot_debug(source,local_source);
    XrXirCheckedPacket packet={0};XrXirArtifact *read=NULL,*closed=NULL;
    CHECK(xr_xir_compile_checked_write(source->checked,&packet,NULL)==XR_XIR_OK);
    xr_sha256(packet.bytes,packet.length,identity);
    xr_xir_compile_source_result_free(source);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(read,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(read);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    xr_sha256(packet.bytes,packet.length,identity);xr_xir_compile_artifact_free(closed);closed=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&closed,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    inspect(xr_xir_compile_artifact_module(closed));return closed;
}
static XrXirArtifact *fixture(const XrXirCompileContext *context,uint8_t identity[32]) {
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=source_check(context,source_text,&source,&diagnostic,XR_XIR_PROGRAM);
    if (status!=XR_XIR_OK) fprintf(stderr,"Source %u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    return replay_source(context,&source,identity,true);
}
static void cold_source(const XrXirSourceRequest *request,const char *path,char *text,XrXirSourceResult *source) {
    XrXirSourceText input={path,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(request,&input,source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"cold %s status=%u %d:%d %s\n",path,status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source->checked && source->snapshot);snapshot_debug(source,request->linkage_kind==XR_XIR_LIBRARY);
    memset(text,0xa5,input.length);
}
static XrXirArtifact *cold_fixture(const XrXirCompileContext *context,uint8_t identity[32],bool minimal) {
    char directory[XR_TEST_PATH_MAX]="string-bytes-XXXXXX",absolute[XR_TEST_PATH_MAX];
    char library[XR_TEST_PATH_MAX],root[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(library,sizeof(library),"%s/lib.xr",absolute)>0);
    CHECK(snprintf(root,sizeof(root),"%s/root.xr",absolute)>0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest request={.session=session,.entry_path=library,.authority=&authority,
        .context=context,.linkage_kind=XR_XIR_LIBRARY};
    const char minimal_text[]="export fn unused(text:string){const ignored=text.bytes()}";
    char input_text[sizeof(source_text)];
    strcpy(input_text,minimal?minimal_text:source_text);
    XrXirSourceResult produced={0};cold_source(&request,"lib.xr",input_text,&produced);
    inspect(xr_xir_compile_artifact_module(produced.checked));
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(produced.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&produced);xr_compile_session_free(session);session=NULL;
    XrXirLibraryModuleInput binding={authority,"lib.xr"};
    XrXirLibraryInput input={packet.bytes,packet.length,{0},&binding,1};
    xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    if (status!=XR_XIR_OK) fprintf(stderr,"bytes Catalog status=%u\n",status);
    CHECK(status==XR_XIR_OK && catalog);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    request.session=session;request.entry_path=root;request.linkage_kind=XR_XIR_PROGRAM;request.libraries=catalog;
    const char consumer[]= "import {run} from \"./lib\"\nexport fn receiver()->i64{return run()}";
    const char minimal_consumer[]= "import {unused} from \"./lib\"\nexport fn receiver()->i64{const text=\"a\";unused(text);return 342}";
    char consumer_text[sizeof(minimal_consumer)+sizeof(consumer)];
    strcpy(consumer_text,minimal?minimal_consumer:consumer);
    XrXirSourceResult received={0};cold_source(&request,"root.xr",consumer_text,&received);
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    CHECK(!xr_test_rmdir(directory));return replay_source(context,&received,identity,false);
}
static XrXirArtifact *lower(const XrXirArtifact *closed) {
    XrXirArtifact *lowered=NULL;
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==6 && !memcmp(module->functions[f].name,"unused",6))
            CHECK(xr_xir_compile_artifact_layout(lowered,f)->outgoing_count>=2);
    return lowered;
}
static XrXirStatus foreign_source_owner(void *owner,const XrXirModule *module,uint32_t function,bool *preserved) {
    (void)module;(void)function;*(bool *)owner=true;*preserved=true;return XR_XIR_OK;
}
static void strict_normalization(const XrXirCompileContext *context,const XrXirArtifact *closed) {
    const XrXirModule *module=xr_xir_compile_artifact_module(closed);XirViewNormalization *normalized=NULL;
    CHECK(xir_compile_view_normalize_verified(context,module,&normalized,NULL)==XR_XIR_BAD_STAGE && !normalized);
    bool called=false;XrXirModule foreign=*module;
    XirViewNormalizationSource source={module,&called,foreign_source_owner};
    CHECK(xir_compile_view_normalize_source_verified(context,&foreign,&source,&normalized,NULL)==XR_XIR_BAD_STRUCTURE &&
        !normalized && !called);
    foreign.functions=module->functions+module->function_count;
    CHECK(xir_compile_view_normalize_source_verified(context,&foreign,&source,&normalized,NULL)==XR_XIR_BAD_STRUCTURE &&
        !normalized && !called);
    source.owner=NULL;
    CHECK(xir_compile_view_normalize_source_verified(context,module,&source,&normalized,NULL)==XR_XIR_BAD_STRUCTURE && !normalized);
}
static XrXirProgram *take_vm(XrXirArtifact **code) {
    XrXirProgram *program=NULL;XrXirStatus status=xr_xir_compile_vm_program_take(code,&program);
    if (status!=XR_XIR_OK) fprintf(stderr,"VM Program take status=%u\n",status);
    CHECK(status==XR_XIR_OK && program && !*code);return program;
}
static BytesHostCases host_cases(const XrXirModule *module) {
    uint32_t read=named(module,"read");
    return (BytesHostCases){read,named(module,"loan"),named(module,"readFunction"),named(module,"loanFunction"),
        module->functions[read].parameters[0]};
}
static void bytes_put32(uint8_t *bytes,uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[i]=(uint8_t)(value>>(i*8));
}
static void invalid_packets(const XrXirCompileContext *context,const XrXirArtifact *closed) {
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(closed);
    const XrXirFunction *function=module->functions+named(module,"unused");
    const XrXirInstruction *call=NULL;
    for (uint32_t i=0;i<function->instruction_count;++i)
        if (xr_xir_instruction_is_string_bytes(function->instructions+i)) {call=function->instructions+i;break;}
    CHECK(call);uint8_t pattern[40]={0};
    bytes_put32(pattern,XR_XIR_CALL_INTRINSIC);bytes_put32(pattern+4,call->type);
    bytes_put32(pattern+8,call->args[0]);bytes_put32(pattern+12,call->args[1]);bytes_put32(pattern+24,4);
    size_t at=64;
    while (at+sizeof(pattern)<=packet.length && memcmp(packet.bytes+at,pattern,sizeof(pattern))) ++at;
    CHECK(at+sizeof(pattern)<=packet.length);
    const struct {uint32_t offset,value;XrXirStatus expected;} attacks[]={
        {24,0,XR_XIR_BAD_STRUCTURE},{24,UINT32_MAX,XR_XIR_BAD_STRUCTURE},
        {24,1,XR_XIR_BAD_STRUCTURE},{12,2,XR_XIR_BAD_STRUCTURE},
        {4,XR_XIR_STRING,XR_XIR_BAD_TYPE},{16,1,XR_XIR_BAD_STRUCTURE}
    };
    for (size_t i=0;i<sizeof(attacks)/sizeof(attacks[0]);++i) {
        uint8_t original[4];memcpy(original,packet.bytes+at+attacks[i].offset,sizeof(original));
        bytes_put32(packet.bytes+at+attacks[i].offset,attacks[i].value);
        XrSHA256Context hash;xr_sha256_init(&hash);xr_sha256_update(&hash,packet.bytes,32);
        xr_sha256_update(&hash,packet.bytes+64,packet.length-64);xr_sha256_final(&hash,packet.bytes+32);
        XrXirArtifact *received=NULL;
        XrXirStatus status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL);
        if (status!=attacks[i].expected) fprintf(stderr,"bytes packet attack %zu status%u expected%u\n",
            i,status,attacks[i].expected);
        CHECK(status==attacks[i].expected && !received);
        memcpy(packet.bytes+at+attacks[i].offset,original,sizeof(original));
    }
    xr_xir_compile_checked_packet_free(&packet);
}
static void rejected(const XrXirCompileContext *context) {
    static const char *const inputs[]={
        "fn bad(){const v=\"temporary\".bytes()}",
        "fn bad(text:string){const v=text.bytes(1)}",
        "fn bad(text:string)->Slice<u8>{return text.bytes()}",
        "fn bad()->i64{var text=\"abc\";const v=text.bytes();text=\"z\";return len(v)}",
        "fn bad(text:string)->fn()->i64{const v=text.bytes();return fn()->i64{return len(v)}}"
    };
    for (size_t i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) {
        XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=source_check(context,inputs[i],&source,&diagnostic,XR_XIR_PROGRAM);
        if (status==XR_XIR_OK) fprintf(stderr,"unexpected accepted rejection %zu\n",i);
        CHECK(status!=XR_XIR_OK && !source.checked && !source.snapshot && diagnostic.line>0);
        xr_xir_compile_source_result_free(&source);
    }
}
static void ctfe_rejected(const XrXirArtifact *closed,const uint8_t identity[32]) {
    XrXirArtifact *code=lower(closed);
    const XrXirModule *module=xr_xir_compile_artifact_module(code);
    XrXirValue argument={.type=XR_XIR_STRING,.payload=1};
    XrXirCtfeRequest request={.artifact=code,.function=named(module,"unused"),
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},.arguments=&argument,.argument_count=1,
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}};
    memcpy(request.checked_identity,identity,32);XrXirValue result={0};
    CHECK(xr_xir_compile_ctfe_leaf(&request,&result)==XR_XIR_CTFE_NOT_LEAF);
    CHECK(!result.type && !result.payload);xr_xir_compile_artifact_free(code);
}
static void emit(const XrXirArtifact *closed,const char *path) {
    XrXirArtifact *code=lower(closed);XrXirCSource output={0};
    const XrXirModule *module=xr_xir_compile_artifact_module(code);
    uint32_t run=named(module,"receiver"),probe=named(module,"probe");BytesHostCases host=host_cases(module);
    CHECK(xr_xir_compile_emit_c(code,"string_bytes",(size_t)UINT64_C(16)<<20,&output)==XR_XIR_OK);
    xr_xir_compile_artifact_free(code);
    CHECK(strstr(output.text,"xr_xir_call_view_operation") && !strstr(output.text,"({"));
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(output.text,1,output.length,file)==output.length);
    CHECK(fprintf(file,"\nconst uint32_t string_bytes_run=%u;\nconst uint32_t string_bytes_probe=%u;\n",run,probe)>0);
    CHECK(fprintf(file,"const uint32_t string_bytes_host[5]={%u,%u,%u,%u,%u};\n",
        host.read,host.loan,host.read_function,host.loan_function,(uint32_t)host.view)>0);
    CHECK(!fclose(file));xr_xir_compile_c_source_free(&output);
}
static void observed_bytes(const XrXirCompileContext *context,const char *text,const char *entry,
    const char *thrower,uint32_t selected,int64_t expected) {
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=source_check(context,text,&source,&diagnostic,XR_XIR_PROGRAM);
    if (status!=XR_XIR_OK) fprintf(stderr,"observed bytes Source %u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    const XrXirSourceDebugView *map=xr_xir_compile_source_snapshot_debug(source.snapshot);
    const XrXirModule *original=xr_xir_compile_artifact_module(source.checked);
    uint32_t target=named(original,entry),throwing=named(original,thrower);
    const XrXirSourceDebugFunction *row=map->functions+throwing;
    CHECK(row->handler_count==1 && selected<row->handlers[0].selector_count);
    uint32_t accepted=row->handlers[0].selectors[selected].accepted;
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source.checked);source.checked=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    XrXirArtifact *code=lower(closed);xr_xir_compile_artifact_free(closed);
    XrXirDomain *domain=NULL;XrXirVmDebug *debug=NULL;XrXirProgram *program=NULL;
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirDomainBudgetControls controls={config.requested_value_limit,config.requested_call_limit,
        config.work_limit,config.metadata_limit,config.call_limit};
    CHECK(xr_xir_domain_new_budgeted(config.value_limit,&controls,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_vm_debug_new(domain,32,&debug)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_compile_vm_debug_program_take(&code,debug,&program)==XR_XIR_OK && !code);
    size_t bytes=(size_t)map->function_count*sizeof(*map->functions);XrXirSourceDebugFunction *functions=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,bytes,(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    for (uint32_t variant=0;variant<5;++variant) {
        memcpy(functions,map->functions,bytes);XrXirSourceDebugHandler handler=row->handlers[0];
        XrXirSourceDebugRoute route=row->routes[0];functions[throwing].handlers=&handler;
        XrXirSourceDebugView bad=*map;bad.functions=functions;
        if (!variant) handler.error=UINT32_MAX;
        else if (variant==1) handler.parent=1;
        else { functions[throwing].routes=&route;functions[throwing].route_count=1;
            if (variant==2) route.edge=UINT32_MAX;
            else if (variant==3) route.input=UINT32_MAX;
            else route.error=UINT32_MAX; }
        CHECK(xir_vm_debug_source_map(debug,context,&bad)==XR_XIR_BAD_STRUCTURE);
    }
    xr_compile_resources_free(functions);
    CHECK(xir_vm_debug_source_map(debug,context,map)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_vm_debug_break_on_throw(debug,XR_XIR_VM_DEBUG_EXCEPTION_CAUGHT)==XR_XIR_VALUE_OK);
    XrXirInstance *instance=NULL;
    CHECK(xr_xir_vm_debug_instance_new(debug,program,&config,&instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(program);
    CHECK(xr_xir_instance_start(instance,target,NULL,0)==XR_XIR_CALL_READY);
    XrXirVmDebugSnapshot *escaped=NULL;XrXirValue result={0};uint32_t stops=0;
    for (;;) {
        XrXirInstanceResult poll=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
        CHECK(xr_xir_vm_debug_observation_status(debug,poll.outcome.status==XR_XIR_CALL_RETURNED)==XR_XIR_VALUE_OK);
        if (xr_xir_vm_debug_paused(debug)) {
            CHECK(!escaped && !stops);CHECK(xr_xir_vm_debug_snapshot(debug,&escaped)==XR_XIR_VALUE_OK);
            XirVmDebugExceptionLocal local={0};XirVmDebugExceptionRoute route={0};
            CHECK(xir_vm_debug_snapshot_exception_local(escaped,&local)==XR_XIR_VALUE_OK);
            CHECK(local.kind==XIR_VM_DEBUG_EXCEPTION_LOCAL_MATCH && local.function==throwing && local.handler==1 && local.accepted==accepted);
            CHECK(xir_vm_debug_snapshot_exception_route(escaped,&route)==XR_XIR_VALUE_OK);
            CHECK(route.kind==XIR_VM_DEBUG_EXCEPTION_ROUTE_SELECTED && route.function==throwing && route.accepted==accepted);
            ++stops;CHECK(xr_xir_vm_debug_continue(debug,xr_xir_vm_debug_epoch(debug),XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
        } else if (poll.outcome.status==XR_XIR_CALL_RETURNED) break;
        else if (poll.outcome.status!=XR_XIR_CALL_READY) {
            fprintf(stderr,"observed bytes poll=%u entry=%s\n",poll.outcome.status,entry);CHECK(false);
        }
    }
    CHECK(stops==1 && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && (int64_t)result.payload==expected);xr_xir_value_drop(&result);
    xr_xir_vm_debug_disable(debug);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_vm_debug_drop(debug);xr_xir_domain_drop(domain);
    XirVmDebugExceptionLocal retired={0};
    CHECK(xir_vm_debug_snapshot_exception_local(escaped,&retired)==XR_XIR_VALUE_OK && retired.accepted==accepted);
    xr_xir_vm_debug_snapshot_drop(escaped);
}
static void observed_cases(const XrXirCompileContext *context) {
    observed_bytes(context,error_text,"bytesCatch","bytesCatch",0,3);
    static const char *const cases[]={
        "enum E{A,B}\nfn scalar()->i64{return 7}\n"
        "fn two(second:bool)->i64{const text=\"abcd\";const b=text.bytes();try{const n=scalar();if(second){throw E.B};throw E.A}"
        "catch(E.A){return len(b)}catch(E.B){return 17}}\n"
        "export fn first()->i64{return two(false)}\nexport fn second()->i64{return two(true)}\n",
        "enum E{A,B}\nfn scalar()->i64{return 7}\n"
        "fn two(second:bool)->i64{const text=\"abcd\";const b=text.bytes();try{const n=scalar();if(second){throw E.B};throw E.A}"
        "catch(E.A){return len(b)}catch(E.B){return len(b)+1}}\n"
        "export fn first()->i64{return two(false)}\nexport fn second()->i64{return two(true)}\n"
    };
    for (uint32_t live=0;live<2;++live) {
        observed_bytes(context,cases[live],"first","two",0,4);
        observed_bytes(context,cases[live],"second","two",1,live?5:17);
    }
}
int main(int argc,char **argv) {
    XrXirCompileContext context={0};const XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();registry();uint8_t identity[32];
    if (argc==2 && !strcmp(argv[1],"--debug")) {
        observed_cases(&context);xr_compile_resources_release(context.resources);return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--error-source")) {
        XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=source_check(&context,error_text,&source,&diagnostic,XR_XIR_PROGRAM);
        fprintf(stderr,"bytes exception Source status=%u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
        CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
        const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
        const XrXirSourceDebugView *debug=xr_xir_compile_source_snapshot_debug(source.snapshot);
        const XrXirSourceDebugFunction *function=debug->functions+named(module,"bytesCatch");
        CHECK(function->points && function->throw_count && function->handler_count && function->route_count);
        xr_xir_compile_source_result_free(&source);xr_compile_resources_release(context.resources);return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--cold-minimal")) {
        XrXirArtifact *closed=cold_fixture(&context,identity,true);strict_normalization(&context,closed);
        XrXirArtifact *code=lower(closed);
        uint32_t run=named(xr_xir_compile_artifact_module(code),"receiver");XrXirProgram *program=NULL;
        program=take_vm(&code);
        xr_xir_compile_artifact_free(closed);bytes_program_cases(program,run);
        xr_compile_resources_release(context.resources);return 0;
    }
    XrXirArtifact *closed=fixture(&context,identity);
    if (argc==3 && !strcmp(argv[1],"--emit")) {
        xr_xir_compile_artifact_free(closed);closed=cold_fixture(&context,identity,false);emit(closed,argv[2]);
    }
    else {
        CHECK(argc==1);strict_normalization(&context,closed);invalid_packets(&context,closed);rejected(&context);ctfe_rejected(closed,identity);
        XrXirArtifact *code=lower(closed);const XrXirModule *module=xr_xir_compile_artifact_module(code);
        uint32_t run=named(module,"run");XrXirProgram *program=NULL;BytesHostCases host=host_cases(module);
        xr_xir_compile_artifact_free(closed);closed=NULL;
        program=take_vm(&code);
        bytes_host_rejections(program,&host);bytes_program_cases(program,run);
        closed=cold_fixture(&context,identity,false);code=lower(closed);
        run=named(xr_xir_compile_artifact_module(code),"receiver");host=host_cases(xr_xir_compile_artifact_module(code));
        xr_xir_compile_artifact_free(closed);closed=NULL;
        program=take_vm(&code);
        bytes_host_rejections(program,&host);bytes_program_cases(program,run);
    }
    xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
    puts("String byte views: fixed semantic results and owned receiving PASS");return 0;
}
