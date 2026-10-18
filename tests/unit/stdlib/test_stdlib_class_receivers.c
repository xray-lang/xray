/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_stdlib_class_receivers.c - Parser-backed class receiver contracts
 *
 * KEY CONCEPT:
 *   Ordinary library classes mutate identity through READ receivers. Value
 *   declarations retain their separate explicit receiver contracts.
 */
#include "frontend/parser/xparse.h"
#include "frontend/parser/xast_api.h"
#include "frontend/parser/xast_walk.h"
#include "base/xfileio.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if(!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);return 1;} } while(0)
typedef struct ReceiverScan {const char *path;unsigned classes,methods,invalid;bool inventory,normalize;char *signature;} ReceiverScan;
static bool receiver_visit(AstNode *node,void *user) {
    if(!node)return true;
    ReceiverScan *scan=user;
    if(node->type==AST_CLASS_DECL) {
        ++scan->classes;const ClassDeclNode *owner=&node->as.class_decl;
        for(int i=0;i<owner->method_count;++i) {
            AstNode *entry=owner->methods[i];if(!entry || entry->type!=AST_METHOD_DECL)return false;
            MethodDeclNode *method=&entry->as.method_decl;++scan->methods;
            if(scan->normalize){method->receiver_mode=XR_PARAM_READ;method->receiver_mode_span=(XrNameSpan){0};}
            if(method->receiver_mode!=XR_PARAM_READ) {
                ++scan->invalid;
                printf("METHOD\t%s\t%s\t%s\t%u\t%d\t%d\n",scan->path,owner->name,method->name,
                    (unsigned)method->receiver_mode,method->receiver_mode_span.line,method->receiver_mode_span.column);
            }
        }
    }
    if(scan->normalize){if(!xr_ast_node_signature(node,scan->signature,16384))return false;puts(scan->signature);}
    return xr_ast_for_each_child(node,receiver_visit,user);
}
static int receiver_self_test(void) {
    const char *cases[]={
        "class Handle {\n value: i64\n set(value: i64) { this.value = value }\n}\n",
        "class Handle {\n ref set() {}\n}\n",
        "class Handle {\n move finish() {}\n}\n",
        "struct Value {\n ref set() {}\n}\n"};
    const unsigned expected[]={0,1,1,0};
    for(unsigned i=0;i<4;++i) {
        XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
        XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;AstNode *program=NULL;
        CHECK(xr_compile_resources_new(&caps,&resources)==XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats initial={0},final={0};CHECK(xr_compile_resources_stats(resources,&initial)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
        CHECK(xr_compile_parse(session,cases[i],&program)==XR_PARSE_OK && program);
        ReceiverScan scan={"receiver contract self-test",0,0,0,true,false,NULL};
        CHECK(receiver_visit(program,&scan) && scan.invalid==expected[i]);
        xr_program_destroy(program);xr_compile_session_free(session);
        CHECK(xr_compile_resources_stats(resources,&final)==XR_COMPILE_RESOURCE_OK && final.live_bytes==initial.live_bytes);
        xr_compile_resources_release(resources);
    }
    puts("READ class, explicit class REF/MOVE refusal and retained value REF contracts PASS");return 0;
}

int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"--self-test"))return receiver_self_test();
    CHECK(argc>=3);bool inventory=!strcmp(argv[1],"--inventory");bool normalize=!strcmp(argv[1],"--signature");CHECK(inventory || normalize || !strcmp(argv[1],"--verify"));unsigned total=0;
    for(int i=2;i<argc;++i) {
        size_t length=0;char *source=xr_file_read_all(argv[i],"rb",&length);CHECK(source && length);
        XrCompileResourceLimits caps={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
        XrCompileResources *resources=NULL;XrCompilerSession *session=NULL;AstNode *program=NULL;
        CHECK(xr_compile_resources_new(&caps,&resources)==XR_COMPILE_RESOURCE_OK);
        XrCompileResourceStats initial={0};CHECK(xr_compile_resources_stats(resources,&initial)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
        CHECK(xr_compile_parse_with_source(session,source,argv[i],&program)==XR_PARSE_OK && program);
        char signature[16384];ReceiverScan scan={argv[i],0,0,0,inventory,normalize,signature};CHECK(receiver_visit(program,&scan));total+=scan.invalid;
        if(!normalize)printf("MODULE\t%s\t%u\t%u\t%u\n",argv[i],scan.classes,scan.methods,scan.invalid);
        xr_program_destroy(program);xr_compile_session_free(session);xr_free(source);XrCompileResourceStats final={0};
        CHECK(xr_compile_resources_stats(resources,&final)==XR_COMPILE_RESOURCE_OK && final.live_bytes==initial.live_bytes);
        xr_compile_resources_release(resources);
    }
    if(!normalize)printf("CLASS_RECEIVERS invalid%u\n",total);return inventory || normalize || !total?0:1;
}
