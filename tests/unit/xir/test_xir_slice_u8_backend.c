/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_slice_u8_backend.c - Real readonly byte views across receivers
 */
#include "xir/xxir_source.h"
#include "xir/xxir_types.h"
#include "xir/xxir_operand_roles.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_generic.h"
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
#include "slice_u8_backend_cases.h"
#include "slice_u8_local_storage_cases.h"
#include "slice_u8_scalar_slot_cases.h"
#include "../test_win_compat.h"
#define SLICE_STRING_BYTES_SOURCE "export fn string_bytes()->i64{var text:string=\"h\\0é\";const v:Slice<u8>=text.bytes();if(len(v)!=4 || v[0]!=104 || v[1]!=0 || v[2]!=195 || v[3]!=169){return -31};var empty=\"\";const e:Slice<u8>=empty.bytes();if(len(e)!=0){return -32};var root=\"x\";const first:Slice<u8>=root.bytes();const byte=first[0];root=\"y\";const second:Slice<u8>=root.bytes();if(byte!=120 || second[0]!=121){return -33};return 1}\n"
#define SLICE_INVOKE_CLEANUP_SOURCE \
    "enum CleanupError{Failed}\n" \
    "fn cleanup_scalar()->i64{return 7}\n" \
    "fn invoke_cleanup(fail:bool)->i64{var root:Array<u8>=[8];const v:Slice<u8>=root[0:1];" \
    "try{const n=cleanup_scalar();if(fail){throw CleanupError.Failed};return len(v)+n}catch(CleanupError.Failed){root=[9];return 11}}\n"
static const char library_text[]=SLICE_STRING_BYTES_SOURCE "export fn read(v:Slice<u8>)->i64{if(v[0]!=8){return -10};return len(v)}\nexport fn readFunction()->fn(Slice<u8>)->i64{return read}\nexport fn ignored(a:Array<u8>){const v:Slice<u8>=a[1:2]}\nexport fn run()->i64{if(string_bytes()!=1){return -23};var a:Array<u8>=[7,8,9];const v:Slice<u8>=a[1:2];const copy=v;const reader=readFunction();const n=reader(copy);ignored(a);a=[40];return n}\n";
static const char source_text[]=SLICE_STRING_BYTES_SOURCE SLICE_INVOKE_CLEANUP_SOURCE "export fn read(v:Slice<u8>)->i64{if(v[0]!=8){return -10};return len(v)}\nexport fn readFunction()->fn(Slice<u8>)->i64{return read}\nexport fn ignored(a:Array<u8>){const v:Slice<u8>=a[1:2]}\nexport fn run()->i64{if(string_bytes()!=1){return -23};if(invoke_cleanup(false)!=8){return -20};if(invoke_cleanup(true)!=11){return -21};var a:Array<u8>=[7,8,9];const v:Slice<u8>=a[1:2];const copy=v;const reader=readFunction();const n=reader(copy);ignored(a);a=[40];return n}\nexport fn receiver()->i64{return run()}\n";
static uint32_t named(const XrXirModule *module,const char *name) {
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==strlen(name) &&
            !memcmp(module->functions[f].name,name,strlen(name))) return f;
    CHECK(false);return UINT32_MAX;
}
static XrXirStatus source_check(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *result,XrXirSourceDiagnostic *diagnostic,XrXirLinkageKind linkage) {
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"readonly-byte-backend",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=linkage};
    XrXirSourceText input={NULL,text,strlen(text)};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,diagnostic,NULL);
    xr_compile_session_free(session);
    return status;
}

static void inspect(const XrXirModule *module) {
    bool view=false,get=false,cleanup=false;
    for (uint32_t f=0;f<module->function_count;++f)
        for (uint32_t i=0;i<module->functions[f].instruction_count;++i) {
            const XrXirInstruction *op=module->functions[f].instructions+i;
            CHECK(op->op!=XR_XIR_CALL_INTRINSIC || op->immediate!=4);
            view|=op->op==XR_XIR_VIEW_NEW;
            get|=op->op==XR_XIR_VIEW_GET;
            cleanup|=op->op==XR_XIR_VIEW_CLEANUP;
        }
    CHECK(view && get && cleanup);
    for (uint32_t t=0;t<module->types->count;++t) {
        const XrXirTypeNode *node=module->types->nodes+t;
        if (node->kind==XR_XIR_TYPE_CELL) {
            const XrXirTypeNode *element=xr_xir_type_node(module->types,node->element);
            CHECK(!element || (element->kind!=XR_XIR_TYPE_SLICE && element->kind!=XR_XIR_TYPE_MUT_SLICE));
        }
    }
}
static void snapshot_debug(const XrXirSourceResult *source,bool local_source,bool invocation_fixture) {
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    const XrXirSourceDebugView *debug=xr_xir_compile_source_snapshot_debug(source->snapshot);
    CHECK(debug && debug->function_count==module->function_count);
    uint32_t f=named(module,"ignored");const XrXirFunction *function=module->functions+f;
    const XrXirSourceDebugFunction *row=debug->functions+f;
    CHECK(row->instruction_count==function->instruction_count);
    if (local_source) CHECK(row->points);
    if (row->points) for (uint32_t i=0;i<function->instruction_count;++i) {
        if (function->instructions[i].op==XR_XIR_VIEW_CLEANUP)
            CHECK(!row->points[i].statement && !row->points[i].flags);
        if (function->instructions[i].op==XR_XIR_VIEW_NEW)
            CHECK(row->points[i].statement && row->points[i].range.line>0);
    }
    if (invocation_fixture) {
        uint32_t invoked=named(module,"invoke_cleanup"),scalar=named(module,"cleanup_scalar");
        const XrXirFunction *body=module->functions+invoked;
        const XrXirSourceDebugFunction *observed=debug->functions+invoked;
        CHECK(observed->route_count && observed->points);
        bool invocation=false,local_throw=false;
        for (uint32_t r=0;r<observed->route_count;++r) {
            const XrXirSourceDebugRoute *route=observed->routes+r;
            CHECK(route->instruction<body->instruction_count);
            const XrXirInstruction *call=body->instructions+route->instruction;
            if (call->op!=XR_XIR_INVOKE) {
                bool thrown=false;
                for (uint32_t t=0;t<observed->throw_count;++t)
                    thrown|=observed->throw_sites[t].instruction==route->instruction &&
                        observed->throw_sites[t].operand==route->error;
                CHECK(thrown);local_throw=true;continue;
            }
            CHECK(call->immediate==scalar && call->targets[1]<body->block_count);
            const XrXirBlock *error=body->blocks+call->targets[1];
            CHECK(error->count>2 && error->first==route->error-body->parameter_count &&
                (uint64_t)error->first+error->count-1==route->edge);
            CHECK(body->instructions[error->first].op==XR_XIR_INVOKE_ERROR &&
                body->instructions[error->first].immediate==route->instruction &&
                body->instructions[route->edge].op==XR_XIR_JUMP);
            for (uint32_t i=error->first+1;i<route->edge;++i)
                CHECK(body->instructions[i].op==XR_XIR_VIEW_CLEANUP &&
                    !observed->points[i].statement && !observed->points[i].flags);
            invocation=true;
        }
        CHECK(invocation && local_throw);
    }
}

static void local_storage_query_check(const XrXirCompileContext *context,const XrXirSourceResult *source,
    bool local_probes) {
    const XrXirModule *storage_module=xr_xir_compile_artifact_module(source->checked);
    const XrXirSourceView *storage_query=xr_xir_compile_source_snapshot_view(source->snapshot);
    const char *const mutable_names[]={"localView","alias"};
    for(uint32_t n=0;n<sizeof(mutable_names)/sizeof(mutable_names[0]);++n) {
        CHECK(local_probes);
        bool found=false;
        for(uint32_t q=0;q<storage_query->declaration_count;++q) {
            const XrXirSourceDeclaration *row=storage_query->declarations+q;
            if(strcmp(row->name,mutable_names[n]))continue;
            CHECK(row->kind==XR_XIR_SOURCE_BINDING && row->mutable && row->type.known);
            uint32_t flags=0;CHECK(xr_xir_compile_view_type_flags(context,storage_module->types,row->type.type,&flags)==XR_XIR_OK && flags);
            found=true;
        }
        CHECK(found);
    }
    for(uint32_t t=0;t<storage_module->types->count;++t) {
        const XrXirTypeNode *node=storage_module->types->nodes+t;
        if(node->kind!=XR_XIR_TYPE_CELL)continue;
        uint32_t flags=0;CHECK(xr_xir_compile_view_type_flags(context,storage_module->types,node->element,&flags)==XR_XIR_OK && !flags);
    }
}

static XrXirArtifact *replay_source(const XrXirCompileContext *context,
    XrXirSourceResult *source,uint8_t identity[32],bool local_source) {
    /* Imported Checked libraries do not invent local Source query rows. */
    inspect(xr_xir_compile_artifact_module(source->checked));snapshot_debug(source,local_source,true);
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
    const XrXirSourceView *query=xr_xir_compile_source_snapshot_view(source.snapshot);
    bool bytes=false;
    for (uint32_t i=0;i<query->declaration_count;++i) {
        const XrXirSourceDeclaration *row=query->declarations+i;
        if (row->native_identity!=18 || row->kind!=XR_XIR_SOURCE_MEMBER) continue;
        CHECK(row->parent && row->parent<=query->declaration_count);
        const XrXirSourceDeclaration *parent=query->declarations+row->parent-1;
        if (parent->native_identity!=2) continue;
        const XrXirTypeNode *type=xr_xir_type_node(xr_xir_compile_artifact_module(source.checked)->types,row->type.type);
        CHECK(!strcmp(row->name,"bytes") && !strcmp(row->signature,"() -> Slice<u8>") &&
            !row->parameter_count && row->type.known && type && type->kind==XR_XIR_TYPE_SLICE && type->element==XR_XIR_U8);
        bytes=true;
    }
    CHECK(bytes);
    return replay_source(context,&source,identity,true);
}

static void cold_source(const XrXirSourceRequest *request,const char *path,char *text,XrXirSourceResult *source) {
    XrXirSourceText input={path,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(request,&input,source,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"cold %s status=%u %d:%d %s\n",path,status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source->checked && source->snapshot);snapshot_debug(source,request->linkage_kind==XR_XIR_LIBRARY,request->linkage_kind==XR_XIR_PROGRAM);
    memset(text,0xa5,input.length);
}
static XrXirArtifact *cold_fixture(const XrXirCompileContext *context,uint8_t identity[32]) {
    char directory[XR_TEST_PATH_MAX]="slice-u8-XXXXXX",absolute[XR_TEST_PATH_MAX];
    char library[XR_TEST_PATH_MAX],root[XR_TEST_PATH_MAX];
    CHECK(xr_test_mkdtemp(directory) && xr_test_realpath_buf(directory,absolute,sizeof(absolute)));
    CHECK(snprintf(library,sizeof(library),"%s/lib.xr",absolute)>0);
    CHECK(snprintf(root,sizeof(root),"%s/root.xr",absolute)>0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,absolute};
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceRequest request={.session=session,.entry_path=library,.authority=&authority,
        .context=context,.linkage_kind=XR_XIR_LIBRARY};
    char input_text[sizeof(library_text)];
    strcpy(input_text,library_text);
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
    if (status!=XR_XIR_OK) fprintf(stderr,"slice Catalog status=%u\n",status);
    CHECK(status==XR_XIR_OK && catalog);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    request.session=session;request.entry_path=root;request.linkage_kind=XR_XIR_PROGRAM;request.libraries=catalog;
    const char consumer[]= "import {run} from \"./lib\"\n" SLICE_INVOKE_CLEANUP_SOURCE
        "export fn receiver()->i64{if(invoke_cleanup(false)!=8){return -20};if(invoke_cleanup(true)!=11){return -21};return run()}";
    char consumer_text[sizeof(consumer)];
    strcpy(consumer_text,consumer);
    XrXirSourceResult received={0};cold_source(&request,"root.xr",consumer_text,&received);
    xr_compile_session_free(session);xr_xir_compile_library_catalog_free(catalog);
    CHECK(!xr_test_rmdir(directory));return replay_source(context,&received,identity,false);
}

static XrXirArtifact *lower(const XrXirArtifact *closed) {
    XrXirArtifact *code=NULL;XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,&code,NULL)==XR_XIR_OK);return code;
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

static SliceHostCases host_cases(const XrXirModule *module) {
    uint32_t read=named(module,"read");
    return (SliceHostCases){read,named(module,"readFunction"),module->functions[read].parameters[0]};
}
static void emit(const XrXirArtifact *closed,const char *path) {
    XrXirArtifact *code=lower(closed);XrXirCSource output={0};
    const XrXirModule *module=xr_xir_compile_artifact_module(code);
    uint32_t run=named(module,"receiver");SliceHostCases host=host_cases(module);
    CHECK(xr_xir_compile_emit_c(code,"slice_u8",(size_t)UINT64_C(16)<<20,&output)==XR_XIR_OK);
    CHECK(strstr(output.text,"xr_xir_call_view_operation") && !strstr(output.text,"({"));
    FILE *file=fopen(path,"wb");CHECK(file);
    CHECK(fwrite(output.text,1,output.length,file)==output.length);
    CHECK(fprintf(file,"\nconst uint32_t slice_u8_run=%u;\n",run)>0);
    CHECK(fprintf(file,"const uint32_t slice_u8_host[3]={%u,%u,%u};\n",host.read,host.read_function,(uint32_t)host.view)>0);
    CHECK(!fclose(file));xr_xir_compile_c_source_free(&output);xr_xir_compile_artifact_free(code);
}

static void observed_view(const XrXirCompileContext *context,const char *text,const char *entry,
    const char *thrower,uint32_t selected,int64_t expected) {
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=source_check(context,text,&source,&diagnostic,XR_XIR_PROGRAM);
    if (status!=XR_XIR_OK) fprintf(stderr,"observed view Source %u %d:%d %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    const XrXirSourceDebugView *map=xr_xir_compile_source_snapshot_debug(source.snapshot);
    const XrXirModule *original=xr_xir_compile_artifact_module(source.checked);
    uint32_t target=named(original,entry),throwing=named(original,thrower);
    const XrXirSourceDebugFunction *row=map->functions+throwing;
    CHECK(row->handler_count==1 && selected<row->handlers[0].selector_count);
    uint32_t accepted=row->handlers[0].selectors[selected].accepted;
    uint32_t view=UINT32_MAX,root=UINT32_MAX;
    for (uint32_t i=0;i<original->functions[throwing].instruction_count;++i)
        if (original->functions[throwing].instructions[i].op==XR_XIR_VIEW_NEW) {
            view=original->functions[throwing].parameter_count+i;
            const XrXirFunction *body=original->functions+throwing;
            const XrXirInstruction *op=body->instructions+i;
            CHECK(xr_xir_op_uses_operand_table(op->op) && op->args[1] && body->operands &&
                op->args[0]<body->operand_count && op->args[1]<=body->operand_count-op->args[0]);
            root=body->operands[op->args[0]];
            CHECK(root<(uint64_t)body->parameter_count+body->instruction_count);
        }
    CHECK(view!=UINT32_MAX && root!=UINT32_MAX);
    XrXirType root_type=xr_xir_operand_type(original->functions+throwing,root);
    CHECK(xr_xir_type_is_cell(original->types,root_type));
    XrXirType array_type=xr_xir_cell_element(original->types,root_type);
    const XrXirTypeNode *array=xr_xir_type_node(original->types,array_type);
    CHECK(array && array->kind==XR_XIR_TYPE_ARRAY && array->element==XR_XIR_U8);
    XrXirCheckedPacket packet={0};XrXirArtifact *received=NULL,*closed=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source.checked);source.checked=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    XrXirArtifact *code=lower(closed);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_layout(code,throwing)->offsets[view]!=UINT32_MAX &&
        xr_xir_compile_artifact_layout(code,throwing)->offsets[root]!=UINT32_MAX);
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
        /* Debug stops are observed between execution quanta. */
        XrXirInstanceResult poll=xr_xir_instance_poll_bounded(instance,1);
        CHECK(xr_xir_vm_debug_observation_status(debug,poll.outcome.status==XR_XIR_CALL_RETURNED)==XR_XIR_VALUE_OK);
        if (xr_xir_vm_debug_paused(debug)) {
            CHECK(!escaped && !stops);CHECK(xr_xir_vm_debug_snapshot(debug,&escaped)==XR_XIR_VALUE_OK);
            XirVmDebugExceptionLocal local={0};XirVmDebugExceptionRoute route={0};
            CHECK(xir_vm_debug_snapshot_exception_local(escaped,&local)==XR_XIR_VALUE_OK);
            CHECK(local.kind==XIR_VM_DEBUG_EXCEPTION_LOCAL_MATCH && local.function==throwing && local.handler==1 && local.accepted==accepted);
            CHECK(xir_vm_debug_snapshot_exception_route(escaped,&route)==XR_XIR_VALUE_OK);
            CHECK(route.kind==XIR_VM_DEBUG_EXCEPTION_ROUTE_SELECTED && route.function==throwing && route.accepted==accepted);
            XrXirValue empty={0},zero=empty;
            CHECK(xr_xir_vm_debug_snapshot_value(escaped,0,view,&empty)==XR_XIR_VALUE_BAD_ARGUMENT &&
                !memcmp(&empty,&zero,sizeof(zero)));
            CHECK(xr_xir_vm_debug_snapshot_value(escaped,0,root,&empty)==XR_XIR_VALUE_OK &&
                empty.type==(uint32_t)root_type && empty.payload);
            xr_xir_value_drop(&empty);
            XrXirValue unavailable={XR_XIR_I64,0,123},sentinel=unavailable;
            CHECK(xr_xir_vm_debug_snapshot_value(escaped,0,view,&unavailable)==XR_XIR_VALUE_BAD_ARGUMENT &&
                !memcmp(&unavailable,&sentinel,sizeof(sentinel)));
            ++stops;CHECK(xr_xir_vm_debug_continue(debug,xr_xir_vm_debug_epoch(debug),XR_XIR_VM_DEBUG_CONTINUE)==XR_XIR_VALUE_OK);
        } else if (poll.outcome.status==XR_XIR_CALL_RETURNED) break;
        else if (poll.outcome.status!=XR_XIR_CALL_READY) {
            fprintf(stderr,"observed view poll=%u entry=%s\n",poll.outcome.status,entry);CHECK(false);
        }
    }
    CHECK(stops==1 && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_RETURNED);
    CHECK(result.type==XR_XIR_I64 && (int64_t)result.payload==expected);xr_xir_value_drop(&result);
    xr_xir_vm_debug_disable(debug);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_vm_debug_drop(debug);xr_xir_domain_drop(domain);
    XirVmDebugExceptionLocal retired={0};
    CHECK(xir_vm_debug_snapshot_exception_local(escaped,&retired)==XR_XIR_VALUE_OK && retired.accepted==accepted);
    XrXirValue empty={0},zero=empty;
    CHECK(xr_xir_vm_debug_snapshot_value(escaped,0,view,&empty)==XR_XIR_VALUE_BAD_ARGUMENT &&
        !memcmp(&empty,&zero,sizeof(zero)));
    CHECK(xr_xir_vm_debug_snapshot_value(escaped,0,root,&empty)==XR_XIR_VALUE_OK &&
        empty.type==(uint32_t)root_type && empty.payload);
    xr_xir_value_drop(&empty);
    XrXirValue unavailable={XR_XIR_I64,0,321},sentinel=unavailable;
    CHECK(xr_xir_vm_debug_snapshot_value(escaped,0,view,&unavailable)==XR_XIR_VALUE_BAD_ARGUMENT &&
        !memcmp(&unavailable,&sentinel,sizeof(sentinel)));
    xr_xir_vm_debug_snapshot_drop(escaped);
}

static void observed_cases(const XrXirCompileContext *context) {
    static const char *const cases[]={
        "enum E{A,B}\nfn scalar()->i64{return 7}\n"
        "fn two(second:bool)->i64{var root:Array<u8>=[11,22,33];const v:Slice<u8>=root[0:3];"
        "try{const n=scalar();if(second){throw E.B};throw E.A}"
        "catch(E.A){const n=len(v);root=[44];return n}catch(E.B){root=[55];return 17}}\n"
        "export fn first()->i64{return two(false)}\nexport fn second()->i64{return two(true)}\n",
        "enum E{A,B}\nfn scalar()->i64{return 7}\n"
        "fn two(second:bool)->i64{var root:Array<u8>=[11,22,33];const v:Slice<u8>=root[0:3];"
        "try{const n=scalar();if(second){throw E.B};throw E.A}"
        "catch(E.A){const n=len(v);root=[44];return n}catch(E.B){const n=len(v);root=[55];return n+1}}\n"
        "export fn first()->i64{return two(false)}\nexport fn second()->i64{return two(true)}\n"
    };
    for (uint32_t live=0;live<2;++live) {
        observed_view(context,cases[live],"first","two",0,3);
        observed_view(context,cases[live],"second","two",1,live?4:17);
    }
}
static void string_bytes_rejections(const XrXirCompileContext *context) {
    static const char *const rejected[]={
        "export fn run()->i64{var s=\"a\";const v:Slice<u8>=s.bytes(1);return len(v)}",
        "export fn run()->i64{var s=\"a\";const v:MutSlice<u8>=s.bytes();return len(v)}",
        "export fn run()->i64{var s=\"a\";const v:Slice<u8>=s.bytes();v[0]=0;return len(v)}",
        "export fn run()->i64{var s=\"a\";const v:Slice<u8>=s.bytes();s=\"b\";return v[0] as i64}",
        "export fn run(s:string)->Slice<u8>{return s.bytes()}"
    };
    for (uint32_t i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) {
        XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=source_check(context,rejected[i],&source,&diagnostic,XR_XIR_PROGRAM);
        fprintf(stderr,"bytes refusal%u status=%u: %s\n",i,status,diagnostic.message);
        CHECK(status==(i==3 ? XR_XIR_BAD_VALUE : XR_XIR_BAD_TYPE) && diagnostic.status==status && !source.checked && !source.snapshot);
        CHECK(strstr(diagnostic.message,i==0 ? "bytes requires its exact lowered readonly String declaration" :
            i==1 ? "expression cannot satisfy its declared type" : i==2 ? "value mutation requires a mutable named root" : "view lifetime normalization"));
        xr_xir_compile_source_result_free(&source);
    }
}
static XrXirArtifact *local_storage_positive(const XrXirCompileContext *context) {
    const char text[]=SLICE_LOCAL_STORAGE_SOURCE
        "export fn storage_receiver()->i64{if(local_storage_probes(true)!=1 || local_storage_probes(false)!=1){return 0};return 1}";
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=source_check(context,text,&source,&diagnostic,XR_XIR_PROGRAM);
    if(status!=XR_XIR_OK)fprintf(stderr,"local storage positive %u: %s\n",status,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    local_storage_query_check(context,&source,true);
    XrXirCheckedPacket packet={0};XrXirArtifact *checked=NULL,*closed=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    return closed;
}
static void local_storage_emit(const XrXirArtifact *closed,const char *path) {
    XrXirArtifact *code=lower(closed);XrXirCSource output={0};
    uint32_t target=named(xr_xir_compile_artifact_module(code),"storage_receiver");
    CHECK(xr_xir_compile_emit_c(code,"local_storage",(size_t)UINT64_C(16)<<20,&output)==XR_XIR_OK);
    CHECK(strstr(output.text,"xr_xir_call_view_operation") && !strstr(output.text,"({"));
    FILE *file=fopen(path,"wb");CHECK(file && fwrite(output.text,1,output.length,file)==output.length);
    CHECK(fprintf(file,"\nconst uint32_t local_storage_run=%u;\n",target)>0);
    CHECK(!fclose(file));xr_xir_compile_c_source_free(&output);xr_xir_compile_artifact_free(code);
}
static void local_storage_execution(const XrXirCompileContext *context) {
    XrXirArtifact *closed=local_storage_positive(context),*code=lower(closed);xr_xir_compile_artifact_free(closed);
    uint32_t target=named(xr_xir_compile_artifact_module(code),"storage_receiver");
    XrXirProgram *program=take_vm(&code);slice_program_case(program,target);
}
static void scalar_slot_ordinary_guards(const XrXirCompileContext *context,const XrXirSourceResult *source) {
    const XrXirModule *module=xr_xir_compile_artifact_module(source->checked);
    const XrXirConstruction *construction=xr_xir_compile_artifact_construction(source->checked);
    XrXirDiagnostic diagnostic={0};CHECK(xir_compile_view_prepare_ordinary(context,module,construction,&diagnostic)==XR_XIR_OK);
    uint32_t init=module->declarations->modules[0].initializer,other=named(module,"other_initializer"),site=UINT32_MAX;
    const XrXirFunction *body=module->functions+init;
    for(uint32_t i=0;i<body->instruction_count;++i) if(body->instructions[i].op==XR_XIR_SLOT_INIT)site=i;
    CHECK(site!=UINT32_MAX && other!=init);
    XrXirSourceModule modules[2]={module->declarations->modules[0],module->declarations->modules[0]};
    XrXirSlot slot=module->declarations->slots[0];XrXirDeclarations declarations=*module->declarations;
    XrXirModule changed=*module;changed.declarations=&declarations;declarations.modules=modules;declarations.slots=&slot;
    slot.mutable=0;
    CHECK(xir_compile_view_prepare_ordinary(context,&changed,construction,&diagnostic)==XR_XIR_BAD_TYPE);
    slot=module->declarations->slots[0];
    modules[0].initializer=other;
    XrXirStatus status=xir_compile_view_prepare_ordinary(context,&changed,construction,&diagnostic);
    fprintf(stderr,"scalar slot nonowning initializer status=%u at %u/%u/%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_BAD_STRUCTURE && diagnostic.function==init && diagnostic.instruction==site);
    modules[0]=module->declarations->modules[0];
    XrXirFunctionIdentity *identities=NULL;XrXirFunction *functions=NULL;XrXirInstruction *ops=NULL;
    CHECK(xr_compile_resources_calloc(context->resources,module->function_count,sizeof(*identities),(void **)&identities)==XR_COMPILE_RESOURCE_OK);
    memcpy(identities,module->declarations->functions,(size_t)module->function_count*sizeof(*identities));declarations.functions=identities;
    modules[1].name="scalar-other";modules[1].name_length=12;modules[1].initializer=other;modules[1].dependencies=NULL;modules[1].dependency_count=0;
    declarations.module_count=2;identities[other].module=1;
    CHECK(xir_compile_view_prepare_ordinary(context,&changed,construction,&diagnostic)==XR_XIR_OK);
    slot.module=1;
    status=xir_compile_view_prepare_ordinary(context,&changed,construction,&diagnostic);
    fprintf(stderr,"scalar slot cross module status=%u at %u/%u/%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_BAD_STRUCTURE && diagnostic.function==init && diagnostic.instruction==site);
    slot=module->declarations->slots[0];declarations=*module->declarations;changed=*module;
    CHECK(xr_compile_resources_calloc(context->resources,module->function_count,sizeof(*functions),(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_calloc(context->resources,body->instruction_count,sizeof(*ops),(void **)&ops)==XR_COMPILE_RESOURCE_OK);
    memcpy(functions,module->functions,(size_t)module->function_count*sizeof(*functions));memcpy(ops,body->instructions,(size_t)body->instruction_count*sizeof(*ops));
    changed.functions=functions;functions[init].instructions=ops;
    uint32_t primitive=UINT32_MAX;
    for(uint32_t i=0;i<site;++i)if(ops[i].type==XR_XIR_I64)primitive=body->parameter_count+i;
    CHECK(primitive!=UINT32_MAX);ops[site].args[0]=primitive;
    status=xir_compile_view_prepare_ordinary(context,&changed,construction,&diagnostic);
    fprintf(stderr,"scalar slot wrong payload status=%u at %u/%u/%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_BAD_TYPE && diagnostic.function==init && diagnostic.instruction==site);
    xr_compile_resources_free(ops);xr_compile_resources_free(functions);xr_compile_resources_free(identities);
}
static XrXirArtifact *scalar_slot_source(const XrXirCompileContext *context,uint32_t kind) {
    CHECK(kind<sizeof(scalar_slot_cases)/sizeof(scalar_slot_cases[0]));
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=source_check(context,scalar_slot_cases[kind].source,&source,&diagnostic,XR_XIR_PROGRAM);
    if(status!=XR_XIR_OK)fprintf(stderr,"scalar slot Source%u status=%u: %s\n",kind,status,diagnostic.message);
    CHECK(status==XR_XIR_OK && source.checked && source.snapshot);
    const XrXirModule *module=xr_xir_compile_artifact_module(source.checked);
    const XrXirDeclarations *d=module->declarations;CHECK(d->slot_count==1 && d->module_count==1);
    const XrXirSlot *slot=d->slots;
    CHECK(slot->mutable && slot->module==0 && xr_xir_type_is_cell(module->types,slot->type) && xr_xir_cell_element(module->types,slot->type)==scalar_slot_cases[kind].element);
    const XrXirSlotFacts *facts=xr_xir_compile_construction_slots(xr_xir_compile_artifact_construction(source.checked));
    CHECK(facts && facts->count==1 && facts->rows[0].slot==0 && facts->rows[0].name_length==6 && !memcmp(facts->rows[0].name,"scalar",6));
    const XrXirFunction *init=module->functions+d->modules[0].initializer;bool publication=false,view=false;
    for(uint32_t i=0;i<init->instruction_count;++i) {
        const XrXirInstruction *op=init->instructions+i;view|=op->op==XR_XIR_VIEW_NEW;
        if(op->op==XR_XIR_SLOT_INIT){CHECK(!op->immediate && xr_xir_operand_type(init,op->args[0])==slot->type);publication=true;}
    }
    CHECK(publication && view);
    if(!kind)scalar_slot_ordinary_guards(context,&source);
    XrXirCheckedPacket packet={0};XrXirArtifact *checked=NULL,*closed=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);return closed;
}
static void scalar_slot_non_scalar_refusals(const XrXirCompileContext *context) {
    const char *const sources[]={
        "var scalar:string=\"abc\"\n" SCALAR_SLOT_VIEW_LOOP "export fn storage_receiver()->i64{return 1}",
        "var scalar:Array<u8>=[1]\n" SCALAR_SLOT_VIEW_LOOP "export fn storage_receiver()->i64{return 1}"
    };
    for(uint32_t k=0;k<sizeof(sources)/sizeof(sources[0]);++k) {
        XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=source_check(context,sources[k],&source,&diagnostic,XR_XIR_PROGRAM);
        fprintf(stderr,"non scalar publication Source%u status=%u: %s\n",k,status,diagnostic.message);
        CHECK(status==XR_XIR_UNRESOLVED && diagnostic.status==status && strstr(diagnostic.message,"(SLOT_INIT)"));
        CHECK(!source.checked && !source.snapshot);xr_xir_compile_source_result_free(&source);
    }
}
static void scalar_slot_execution(const XrXirCompileContext *context) {
    scalar_slot_non_scalar_refusals(context);
    for(uint32_t k=0;k<sizeof(scalar_slot_cases)/sizeof(scalar_slot_cases[0]);++k) {
        XrXirArtifact *closed=scalar_slot_source(context,k),*code=lower(closed);xr_xir_compile_artifact_free(closed);
        uint32_t target=named(xr_xir_compile_artifact_module(code),"storage_receiver");
        XrXirProgram *program=take_vm(&code);slice_program_case(program,target);
    }
}
static void local_storage_owned_capture(const XrXirCompileContext *context) {
    XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
    CHECK(source_check(context,local_storage_owned_capture_text,&source,&diagnostic,XR_XIR_PROGRAM)==XR_XIR_OK);
    const XrXirSourceView *query=xr_xir_compile_source_snapshot_view(source.snapshot);bool found=false;
    for(uint32_t q=0;q<query->declaration_count;++q) if(!strcmp(query->declarations[q].name,"owned")) {
        CHECK(query->declarations[q].mutable && query->declarations[q].type.type==XR_XIR_STRING);found=true;
    }
    CHECK(found);XrXirCheckedPacket packet={0};XrXirArtifact *checked=NULL,*closed=NULL;
    CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(checked);
    XrXirArtifact *code=lower(closed);xr_xir_compile_artifact_free(closed);
    uint32_t target=named(xr_xir_compile_artifact_module(code),"capture_probe");
    XrXirProgram *program=take_vm(&code);slice_program_case(program,target);
}
static void local_storage_rejections(const XrXirCompileContext *context) {
    for(uint32_t i=0;i<sizeof(local_storage_refusals)/sizeof(local_storage_refusals[0]);++i) {
        const LocalStorageRefusal *row=local_storage_refusals+i;
        XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=source_check(context,row->source,&result,&diagnostic,XR_XIR_PROGRAM);
        fprintf(stderr,"local storage refusal%u status=%u: %s\n",i,status,diagnostic.message);
        CHECK(status==row->status && diagnostic.status==status && strstr(diagnostic.message,row->message));
        CHECK(!result.checked && !result.snapshot);xr_xir_compile_source_result_free(&result);
    }
}
static XrXirStatus local_storage_attempt(const XrXirCompileContext *context,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;
    XrCompileResourceStatus created=xr_compile_session_new(context->resources,&session);
    if(created!=XR_COMPILE_RESOURCE_OK) {
        CHECK(created==XR_COMPILE_RESOURCE_OUT_OF_MEMORY || created==XR_COMPILE_RESOURCE_MEMORY_LIMIT);
        return created==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_MEMORY_LIMIT;
    }
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"local-storage-resource",NULL};
    XrXirSourceRequest request={.session=session,.authority=&authority,.context=context,.linkage_kind=XR_XIR_PROGRAM};
    XrXirSourceText input={NULL,local_storage_resource_text,sizeof(local_storage_resource_text)-1};
    XrXirSourceDiagnostic diagnostic={0};XrXirSourceFailure failure={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,&failure);
    xr_compile_session_free(session);xr_xir_compile_source_failure_free(&failure);return status;
}
static void local_storage_resources(void) {
    XrCompileResourceLimits caps={.memory_mode=XR_COMPILE_MEMORY_ORDINARY,.profile_allocations=true};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats baseline={0},stats={0};CHECK(xr_compile_resources_stats(context.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceResult result={0};size_t start=effects_compile_attempts;
    CHECK(local_storage_attempt(&context,&result)==XR_XIR_OK);
    size_t sites=effects_compile_attempts-start;
    CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK);uint64_t peak=stats.peak_bytes;
    xr_xir_compile_source_result_free(&result);
    for(size_t i=0;i<sites;++i) {
        effects_compile_fail_at=effects_compile_attempts+i;effects_compile_injected=false;
        XrXirStatus status=local_storage_attempt(&context,&result);effects_compile_fail_at=SIZE_MAX;
        CHECK(effects_compile_injected && status==XR_XIR_OUT_OF_MEMORY && !result.checked && !result.snapshot);
        xr_xir_compile_source_result_free(&result);
        CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.held_bytes==baseline.held_bytes);
    }
    CHECK(local_storage_attempt(&context,&result)==XR_XIR_OK);xr_xir_compile_source_result_free(&result);
    context.limits.frame_bytes=1;
    CHECK(local_storage_attempt(&context,&result)==XR_XIR_BUDGET && !result.checked && !result.snapshot);
    xr_xir_compile_source_result_free(&result);xr_compile_resources_release(context.resources);
    for(unsigned minus=0;minus<2;++minus) {
        caps.memory_mode=XR_COMPILE_MEMORY_FINITE;caps.memory_limit=peak-minus;context.limits=xr_xir_compile_default_limits();context.resources=NULL;
        CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(local_storage_attempt(&context,&result)==(minus ? XR_XIR_MEMORY_LIMIT : XR_XIR_OK));
        xr_xir_compile_source_result_free(&result);CHECK(xr_compile_resources_stats(context.resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.held_bytes==baseline.held_bytes);
        xr_compile_resources_release(context.resources);
    }
    CHECK(!effects_compile_live && !effects_compile_bytes);
    fprintf(stderr,"local borrowed storage: physical OOM sites=%zu held peak=%llu exact/minus1 MEMORY_LIMIT, independent frame BUDGET, same-owner retry and physical0\n",sites,(unsigned long long)peak);
}
int main(int argc,char **argv) {
    XrXirCompileContext context={0};const XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();uint8_t identity[32];XrXirArtifact *closed=NULL;
    if (argc==2 && !strcmp(argv[1],"--local-storage-resources")) {xr_compile_resources_release(context.resources);local_storage_resources();return 0;}
    if(argc==3 && !strcmp(argv[1],"--emit-scalar-slot")) {closed=scalar_slot_source(&context,0);local_storage_emit(closed,argv[2]);}
    else if(argc==3 && !strcmp(argv[1],"--emit-local-storage")) {closed=local_storage_positive(&context);local_storage_emit(closed,argv[2]);}
    else if (argc==3 && !strcmp(argv[1],"--emit")) {closed=cold_fixture(&context,identity);emit(closed,argv[2]);}
    else {
        CHECK(argc==1);string_bytes_rejections(&context);local_storage_rejections(&context);local_storage_owned_capture(&context);local_storage_execution(&context);scalar_slot_execution(&context);observed_cases(&context);
        closed=fixture(&context,identity);strict_normalization(&context,closed);
        XrXirArtifact *code=lower(closed);const XrXirModule *module=xr_xir_compile_artifact_module(code);
        uint32_t run=named(module,"receiver");SliceHostCases host=host_cases(module);
        xr_xir_compile_artifact_free(closed);closed=NULL;XrXirProgram *program=take_vm(&code);
        slice_host_rejections(program,&host);slice_program_case(program,run);
        closed=cold_fixture(&context,identity);code=lower(closed);module=xr_xir_compile_artifact_module(code);
        run=named(module,"receiver");host=host_cases(module);
        xr_xir_compile_artifact_free(closed);closed=NULL;program=take_vm(&code);
        slice_host_rejections(program,&host);slice_program_case(program,run);
    }
    xr_xir_compile_artifact_free(closed);xr_compile_resources_release(context.resources);
    CHECK(!effects_compile_live && !effects_compile_bytes);
    puts("Readonly byte views: real Source, VM, snapshot and cold receiving passed");return 0;
}
