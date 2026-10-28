/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_error_union_legacy.c - Complete syntax and legacy count consumers
 */
#include "frontend/parser/xparse.h"
#include "frontend/parser/xtype_ref.h"
#include "runtime/value/xtype.h"
#include "runtime/value/xtype_pool.h"
#include "toolchain/xcompiler_session.h"
#include "base/xmalloc.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if (!(c)) {fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}} while (0)
static bool fail_next;
static size_t live,attempts;
static void *observed_alloc(size_t bytes) {
    ++attempts;if (fail_next) {fail_next=false;return NULL;}
    void *memory=xr_malloc(bytes);if (memory) ++live;return memory;
}
static void observed_free(void *memory) {if (memory) {CHECK(live);--live;}xr_free(memory);}
#undef xr_malloc
#undef xr_free
#define xr_malloc(bytes) observed_alloc(bytes)
#define xr_free(memory) observed_free(memory)
#include "base/xcompile_resources.c"
#undef xr_malloc
#undef xr_free
static void parser_300(XrCompilerSession *session) {
    char source[8192];size_t length=0;const char start[]="fn value() throws(";
    memcpy(source,start,sizeof(start)-1);length=sizeof(start)-1;
    for (unsigned i=0;i<300;++i) {
        int count=snprintf(source+length,sizeof(source)-length,"%sE%u",i?"|":"",i);
        CHECK(count>0 && (size_t)count<sizeof(source)-length);length+=(size_t)count;
    }
    const char end[]=")->i64{return 0}\n";CHECK(sizeof(end)<sizeof(source)-length);memcpy(source+length,end,sizeof(end));
    AstNode *program=NULL;CHECK(xr_compile_parse(session,source,&program)==XR_PARSE_OK);
    CHECK(program && program->as.program.count==1);AstNode *function=program->as.program.statements[0];
    CHECK(function->type==AST_FUNCTION_DECL);XrTypeRef *type=function->as.function_decl.throws_clause.type;
    CHECK(type && type->kind==XR_TREF_UNION && type->nchildren==300 && !strcmp(type->children[299]->name,"E299"));
    XrCompilerSessionScope scope={0};CHECK(xr_compile_session_push_arena(session,program->as.program.arena,&scope)==XR_COMPILE_RESOURCE_OK);
    XrTypeRef *copy=xr_tref_union(session,type->children,type->nchildren);
    CHECK(copy && copy->nchildren==300 && copy->children!=type->children && !strcmp(copy->children[299]->name,"E299"));
    CHECK(xr_compile_session_pop_arena(&scope)==XR_COMPILE_RESOURCE_OK);xr_program_destroy(program);
}
static bool contains(XrType *set,XrType *member) {
    for (int i=0;i<xr_type_union_count(set);++i) if (xr_type_equals(xr_type_union_member(set,i),member)) return true;
    return false;
}
static void legacy_case(unsigned mode) {
    XrCompileResourceLimits limits={.memory_mode=XR_COMPILE_MEMORY_ORDINARY};XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);XrCompileResourceStats baseline={0},after={0};
    CHECK(xr_compile_resources_stats(resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrCompileState *state=xr_compile_session_compile_state(session);XrTypePool *pool=NULL;
    CHECK(xr_compile_type_pool_open(state,&pool)==XR_COMPILE_RESOURCE_OK);xr_type_set_current_pool(pool,NULL);
    if (!mode) {
        parser_300(session);XrType *members[302]={0};char name[12];
        for (unsigned i=0;i<300;++i) {CHECK(snprintf(name,sizeof(name),"E%u",i)>0);members[i]=xr_type_new_enum(NULL,name);CHECK(members[i]);}
        XrType *all=xr_type_new_union(NULL,members,300);CHECK(all && xr_type_union_count(all)==300 && contains(all,members[299]));
        XrType *prefix=xr_type_new_union(NULL,members,299);CHECK(prefix && xr_type_union_count(prefix)==299 && !contains(prefix,members[299]));
        members[300]=members[0];members[301]=xr_type_new_never(NULL);
        XrType *normalized=xr_type_new_union(NULL,members,302);CHECK(normalized && xr_type_union_count(normalized)==300 && contains(normalized,members[299]));
        XrType *tail=members[299];members[299]=xr_type_new_type_param(NULL,"T",0);CHECK(members[299]);
        XrType *generic=xr_type_new_union(NULL,members,300);const char *names[]={"T"};XrType *arguments[]={tail};
        XrType *substituted=xr_type_substitute(NULL,generic,names,arguments,1);
        CHECK(substituted && xr_type_union_count(substituted)==300 && contains(substituted,tail));
        arguments[0]=members[0];substituted=xr_type_substitute(NULL,generic,names,arguments,1);
        CHECK(substituted && xr_type_union_count(substituted)==299 && !contains(substituted,tail));
        size_t before=live,calls=attempts;fail_next=true;
        CHECK(!xr_type_new_union(NULL,members,300) && !fail_next && attempts==calls+1 && live==before);
        CHECK(xr_compile_state_status(state)==XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    } else if (mode==1) {
        size_t before=attempts;CHECK(!xr_type_union_members_alloc((size_t)INT_MAX+1));
        CHECK(xr_compile_state_status(state)==XR_COMPILE_RESOURCE_OVERFLOW && attempts==before);
    } else {
        size_t before=attempts;CHECK(!xr_type_new_union(NULL,NULL,-1));
        CHECK(xr_compile_state_status(state)==XR_COMPILE_RESOURCE_BAD_ARGUMENT && attempts==before);
    }
    xr_type_set_current_pool(NULL,NULL);xr_type_pool_free(pool);xr_compile_session_free(session);
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK && after.held_bytes==baseline.held_bytes);
    xr_compile_resources_release(resources);CHECK(!live);
}
int main(void) {
    xr_type_global_init();for (unsigned i=0;i<3;++i) legacy_case(i);xr_type_global_shutdown();
    puts("Legacy: parser/tref 300, runtime 300th/substitution, physical OOM, overflow/negative first failure and held0 PASS");return 0;
}
