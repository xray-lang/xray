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
#include "../test_win_compat.h"
#define SLICE_INVOKE_CLEANUP_SOURCE \
    "enum CleanupError{Failed}\n" \
    "fn cleanup_scalar()->i64{return 7}\n" \
    "fn invoke_cleanup(fail:bool)->i64{var root:Array<u8>=[8];const v:Slice<u8>=root[0:1];" \
    "try{const n=cleanup_scalar();if(fail){throw CleanupError.Failed};return len(v)+n}catch(CleanupError.Failed){root=[9];return 11}}\n"
static const char library_text[]="export fn read(v:Slice<u8>)->i64{if(v[0]!=8){return -10};return len(v)}\nexport fn readFunction()->fn(Slice<u8>)->i64{return read}\nexport fn ignored(a:Array<u8>){const v:Slice<u8>=a[1:2]}\nexport fn run()->i64{var a:Array<u8>=[7,8,9];const v:Slice<u8>=a[1:2];const copy=v;const reader=readFunction();const n=reader(copy);ignored(a);a=[40];return n}\n";
static const char source_text[]=SLICE_INVOKE_CLEANUP_SOURCE "export fn read(v:Slice<u8>)->i64{if(v[0]!=8){return -10};return len(v)}\nexport fn readFunction()->fn(Slice<u8>)->i64{return read}\nexport fn ignored(a:Array<u8>){const v:Slice<u8>=a[1:2]}\nexport fn run()->i64{if(invoke_cleanup(false)!=8){return -20};if(invoke_cleanup(true)!=11){return -21};var a:Array<u8>=[7,8,9];const v:Slice<u8>=a[1:2];const copy=v;const reader=readFunction();const n=reader(copy);ignored(a);a=[40];return n}\nexport fn receiver()->i64{return run()}\n";
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

static XrXirArtifact *replay_source(const XrXirCompileContext *context,
    XrXirSourceResult *source,uint8_t identity[32],bool local_source) {
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
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
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
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
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
int main(int argc,char **argv) {
    XrXirCompileContext context={0};const XrCompileResourceLimits limits=XR_RESOURCE_DEFAULT_COMPILER_LIMITS;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();uint8_t identity[32];XrXirArtifact *closed=NULL;
    if (argc==3 && !strcmp(argv[1],"--emit")) {closed=cold_fixture(&context,identity);emit(closed,argv[2]);}
    else {
        CHECK(argc==1);observed_cases(&context);
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
    puts("Readonly byte views: real Source, VM, snapshot and cold receiving passed");return 0;
}
