/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_module_source_allocations.c - Source context and graph failure rollback
 *
 * The coordinator allocations are observed independently of frontend arenas.
 * Allocation failures cannot publish an incomplete result or lose graph edges.
 */

#include "base/xmalloc.h"
#include "xray_vm.h"
#include "runtime/xisolate_api.h"
#include "runtime/value/xchunk.h"
#include "module/xmodule.h"
#include "module/xmodule_identity.h"
#include "module/xmodule_graph.h"
#include "module/xmodule_resolver.h"
#include "base/xfileio.h"
#include "os/os_temp.h"
#include "../test_win_compat.h"
#include "toolchain/xcompiler_session.h"
#include <string.h>
#include <stdint.h>
#include <stdio.h>
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[16384];
static size_t live, bytes, calls, fail_at;
static bool injecting;
static size_t index_of(void *pointer) {
    for (size_t i=0;i<live;++i) if (allocations[i].pointer==pointer) return i;
    return live;
}
static bool reject(void) { return injecting && ++calls==fail_at; }
static void retain(void *pointer,size_t size) {
    if (!pointer) return;
    if (live==16384) { fprintf(stderr,"probe capacity exceeded\n"); abort(); }
    allocations[live++]=(Allocation){pointer,size};bytes+=size;
}
XR_FUNC void *xr_module_test_malloc(size_t size) {
    if (reject()) return NULL;
    void *p=xr_malloc(size);retain(p,size);return p;
}
XR_FUNC void *xr_module_test_calloc(size_t count,size_t size) {
    if (count && size>SIZE_MAX/count) return NULL;
    if (reject()) return NULL;
    void *p=xr_calloc(count,size);retain(p,count*size);return p;
}
XR_FUNC void *xr_module_test_realloc(void *pointer,size_t size) {
    if (reject()) return NULL;
    size_t i=index_of(pointer);
    void *p=xr_realloc(pointer,size);
    if (!p) return NULL;
    if (i<live) { bytes-=allocations[i].bytes;allocations[i]=(Allocation){p,size};bytes+=size; }
    else retain(p,size);
    return p;
}
XR_FUNC void xr_module_test_free(void *pointer) {
    size_t i=index_of(pointer);
    if (i<live) { bytes-=allocations[i].bytes;allocations[i]=allocations[--live]; }
    xr_free(pointer);
}
XR_FUNC char *xr_module_test_strdup(const char *source) {
    if (!source || reject()) return NULL;
    size_t length=strlen(source)+1;
    char *p=xr_malloc(length);if(p){memcpy(p,source,length);retain(p,length);}return p;
}
static int attempt(size_t failure,size_t *attempted) {
    XrVMConfig config={0};XrVMRuntime *vm=xray_vm_new_full(&config);if(!vm)return 10;
    XrCompilerSession *session=xr_compiler_session_current_for_isolate(vm);
    XrModuleRegistry *registry=xr_isolate_get_module_registry(vm);
    const XrBytecodeModule *previous_image=registry->embedded_modules;
    size_t previous_count=registry->embedded_module_count;
    XrModule **previous_table=registry->module_table;
    int previous_table_count=registry->module_table_count;
    size_t before_live=live,before_bytes=bytes;
    calls=0;fail_at=failure;injecting=true;
    XrModuleIdentityAuthority authority={.kind=XR_MODULE_IDENTITY_MEMORY,.namespace_id="source-module-allocation"};
    XrModuleSourceCompilation result={0};
    bool compiled=xr_compile_module_source(session,"export fn answer() -> i64 { return 42 }\n",NULL,&authority,&result);
    injecting=false;*attempted=calls;
    if(compiled) {
        if(!result.initializer||!result.context||!result.dispose_context) return 11;
        if(!result.dispose_context(result.context,true))return 12;
        xr_instruction_unit_free(result.initializer);
    } else if(result.initializer||result.context||result.dispose_context)return 13;
    if(xr_compiler_session_module_graph(session)||
       xr_compiler_session_vm_import_binding(session)!=XR_VM_IMPORT_PROGRAM_TABLE||
       registry->embedded_modules!=previous_image||registry->embedded_module_count!=previous_count||
       registry->module_table!=previous_table||registry->module_table_count!=previous_table_count)return 14;
    if(live!=before_live||bytes!=before_bytes) {
        fprintf(stderr,"leak failure=%zu live=%zu/%zu bytes=%zu/%zu compiled=%d calls=%zu\n",
            failure,live,before_live,bytes,before_bytes,compiled,calls);return 15;
    }
    if(failure && compiled)return 16;
    if(failure) {
        XrModuleSourceCompilation recovered={0};
        if(!xr_compile_module_source(session,"export fn answer() -> i64 { return 42 }\n",NULL,&authority,&recovered))return 17;
        if(!recovered.dispose_context(recovered.context,true))return 18;
        xr_instruction_unit_free(recovered.initializer);
        if(live!=before_live||bytes!=before_bytes)return 19;
    }
    xray_vm_delete(vm);
    if(live||bytes){fprintf(stderr,"after vm teardown live=%zu bytes=%zu\n",live,bytes);return 20;}
    return 0;
}

static int graph_attempt(const char *directory,const char *source,size_t failure,bool cycle,
                         size_t *attempted) {
    XrCompilerSessionConfig config={0};XrCompilerSession *session=xr_compiler_session_new(&config);
    if(!session)return 30;
    XrModuleResolverConfig resolver_config={0};XrModuleResolver *resolver=xr_module_resolver_new(&resolver_config);
    XrModuleGraph *graph=resolver?xr_module_graph_new(session,resolver):NULL;
    if(!graph)return 31;
    XrModuleIdentityAuthority authority={.kind=XR_MODULE_IDENTITY_SCRIPT,.physical_root=directory};
    char entry[1024];snprintf(entry,sizeof(entry),"%s/root.xr",directory);
    char *error=NULL;calls=0;fail_at=failure;injecting=true;
    int built=xr_module_graph_build_logical_source(graph,&authority,"root.xr",entry,source,&error);
    int sorted=built==0?xr_module_graph_topological_sort(graph):-1;
    injecting=false;*attempted=calls;
    int code=0;
    if(!failure) {
        if(built!=0||sorted!=(cycle?-1:0)||graph->has_cycle!=cycle||
           graph->spec_count!=(cycle?2:19)||graph->topo_count!=graph->spec_count)code=32;
        if(!cycle && (graph->specs[graph->entry_index].dep_count!=18 || graph->resolution_failed))code=33;
    } else if(built==0&&sorted==0)code=34;
    xr_module_test_free(error);
    xr_module_graph_free(graph);xr_module_resolver_free(resolver);xr_compiler_session_delete(session);
    if(live||bytes) { fprintf(stderr,"graph leak ordinal=%zu live=%zu bytes=%zu\n",failure,live,bytes);code=35; }
    return code;
}
static int graph_cases(void) {
    char reserved[512];if(xr_temp_dir_create("xray-module-graph-allocations",reserved,sizeof(reserved))!=0)return 36;
    char *directory=xr_realpath(reserved);if(!directory)return 37;
    char source[2048]={0};size_t used=0;
    for(int i=0;i<18;++i) {
        used+=(size_t)snprintf(source+used,sizeof(source)-used,"import { value } from \"./leaf%d\"\n",i);
        char path[1024];snprintf(path,sizeof(path),"%s/leaf%d.xr",directory,i);
        FILE *file=fopen(path,"wb");if(!file)return 38;
        fputs("export fn value() -> i64 { return 42 }\n",file);if(fclose(file)!=0)return 39;
    }
    char entry[1024];snprintf(entry,sizeof(entry),"%s/root.xr",directory);
    FILE *file=fopen(entry,"wb");if(!file)return 40;fputs(source,file);if(fclose(file)!=0)return 41;
    size_t total=0,attempted=0;int code=graph_attempt(directory,source,0,false,&total);
    if(code)return code;
    for(size_t i=1;i<=total;++i) {
        code=graph_attempt(directory,source,i,false,&attempted);
        if(code){fprintf(stderr,"graph failure ordinal=%zu code=%d\n",i,code);return code;}
    }
    printf("Graph discovery with 19 nodes/18 edges allocation ordinals=%zu PASS\n",total);
    const char *cyclic="import { value } from \"./leaf0\"\nexport fn root() -> i64 { return 42 }\n";
    char leaf[1024];snprintf(leaf,sizeof(leaf),"%s/leaf0.xr",directory);
    file=fopen(leaf,"wb");if(!file)return 42;
    fputs("import { root } from \"./root\"\nexport fn value() -> i64 { return root() }\n",file);
    if(fclose(file)!=0)return 43;
    file=fopen(entry,"wb");if(!file)return 44;fputs(cyclic,file);if(fclose(file)!=0)return 45;
    code=graph_attempt(directory,cyclic,0,true,&total);if(code)return code;
    for(size_t i=1;i<=total;++i) {
        code=graph_attempt(directory,cyclic,i,true,&attempted);
        if(code){fprintf(stderr,"cycle failure ordinal=%zu code=%d\n",i,code);return code;}
    }
    printf("Cycle proof allocation ordinals=%zu, no successful sort under OOM PASS\n",total);
    for(int i=0;i<18;++i) {char path[1024];snprintf(path,sizeof(path),"%s/leaf%d.xr",directory,i);if(remove(path)!=0)return 46;}
    if(remove(entry)!=0||xr_test_rmdir(directory)!=0)return 47;
    xr_free(directory);return 0;
}
int main(void) {
    size_t total=0,attempted=0;int code=attempt(0,&total);if(code){printf("failure baseline rc=%d\n",code);return code;}
    for(size_t i=1;i<=total;++i){code=attempt(i,&attempted);if(code){printf("failure ordinal=%zu rc=%d\n",i,code);return code;}}
    printf("Source context and graph allocation ordinals=%zu, restoration and physical balance PASS\n",total);
    if(!total)return 21;
    return graph_cases();
}
