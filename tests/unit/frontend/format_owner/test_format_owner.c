/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_format_owner.c - Physical allocation and failure tests for formatting
 */
#include "base/xmalloc.h"
#include "frontend/format/xfmt.h"
#include "frontend/parser/xparse.h"
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct Block { void *pointer; size_t bytes; } Block;
static Block blocks[16384];
static uint64_t physical, peak, total;
static size_t attempts, failure = SIZE_MAX;
static void *observe_malloc(size_t bytes) {
    if (attempts++ == failure) return NULL;
    void *memory = xr_malloc(bytes);
    CHECK(memory);
    size_t i = 0;
    while (i < 16384 && blocks[i].pointer) ++i;
    CHECK(i < 16384);
    blocks[i] = (Block){memory,bytes};
    physical += bytes; total += bytes;
    if (physical > peak) peak = physical;
    return memory;
}
static void observe_free(void *memory) {
    if (!memory) return;
    size_t i = 0;
    while (i < 16384 && blocks[i].pointer != memory) ++i;
    CHECK(i < 16384);
    physical -= blocks[i].bytes; blocks[i] = (Block){0};
    xr_free(memory);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(n) observe_malloc(n)
#define xr_free(p) observe_free(p)
#include "base/xcompile_resources.c"

static const XrCompileResourceLimits limits = {UINT64_C(1073741824),UINT64_C(268435456),UINT64_C(8589934592)};
static const char source[] =
    "// retained\nfn value(n: i64) -> i64 {\nreturn match (n) {\n0 -> 1\n12 -> 2\n_ -> 3\n}\n}\n"
    "const short_name = 1 // alpha\nconst long_name = 2 // beta\n";
static const char expected[] =
    "// retained\nfn value(n: i64) -> i64 {\n    return match (n) {\n        0  -> 1\n        12 -> 2\n        _  -> 3\n    }\n}\n\n"
    "const short_name = 1  // alpha\nconst long_name = 2   // beta\n";

static void reset(void) { CHECK(!physical); total = peak = attempts = 0; failure = SIZE_MAX; }

static XrFmtStatus pipeline(const XrCompileResourceLimits *bound, XrCompileResourceStats *stats,
    size_t fail_format, uint64_t work_after_parse) {
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    AstNode *ast = NULL;
    XrFmtOutput output = {0};
    XrCompileResourceStatus created = xr_compile_resources_new(bound, &resources);
    if (created != XR_COMPILE_RESOURCE_OK)
        return created == XR_COMPILE_RESOURCE_BUDGET ? XR_FMT_BUDGET : XR_FMT_OUT_OF_MEMORY;
    XrFmtStatus status = XR_FMT_OK;
    XrCompilerSessionStatus opened = xr_compile_session_new(resources, &session);
    if (opened != XR_COMPILER_SESSION_OK) {
        status = opened == XR_COMPILER_SESSION_BUDGET ? XR_FMT_BUDGET : XR_FMT_OUT_OF_MEMORY;
        goto cleanup;
    }
    XrParseStatus parsed = xr_compile_parse_with_trivia(session, source, "retained.xr", NULL, &ast);
    if (parsed != XR_PARSE_OK) {
        CHECK(parsed == XR_PARSE_BUDGET || parsed == XR_PARSE_OUT_OF_MEMORY);
        status = parsed == XR_PARSE_BUDGET ? XR_FMT_BUDGET : XR_FMT_OUT_OF_MEMORY;
        goto cleanup;
    }
    size_t start = attempts;
    if (fail_format != SIZE_MAX) failure = start + fail_format;
    if (work_after_parse != UINT64_MAX) resources->limits.work = resources->stats.work + work_after_parse;
    XrFmtConfig config = xfmt_default_config;
    config.align_trailing_comments = 1;
    status = xr_compile_format_ast(xr_compile_session_compile_state(session), ast, &config, &output);
    if (fail_format == SIZE_MAX && work_after_parse == UINT64_MAX && status == XR_FMT_OK)
        printf("format allocations=%zu work=%llu\n", attempts-start, (unsigned long long)resources->stats.work);
    CHECK(xr_compile_resources_stats(resources, stats) == XR_COMPILE_RESOURCE_OK);
    CHECK(stats->live_bytes == physical && stats->allocated_bytes == total && stats->peak_bytes == peak);
    if (status == XR_FMT_OK) {
        if (output.length != sizeof(expected)-1 || memcmp(output.text, expected, sizeof(expected))) {
            fprintf(stderr, "actual[%zu]=%s\n", output.length, output.text); exit(1);
        }
    } else {
        CHECK(!output.text && !output.length);
        uint64_t before = stats->work;
        CHECK(xr_compile_format_ast(xr_compile_session_compile_state(session), ast, &config, &output) == status);
        CHECK(resources->stats.work == before);
    }
cleanup:
    xr_program_destroy(ast);
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    if (status == XR_FMT_OK) CHECK(!memcmp(output.text, expected, sizeof(expected)));
    xr_compile_format_output_free(&output);
    CHECK(!physical && !output.text && !output.length);
    return status;
}

static void output_and_foreign(void) {
    XrCompileResources *a=NULL,*b=NULL;
    XrCompilerSession *sa=NULL,*sb=NULL;
    AstNode *ast=NULL;
    CHECK(xr_compile_resources_new(&limits,&a)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_resources_new(&limits,&b)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(a,&sa)==XR_COMPILER_SESSION_OK);
    CHECK(xr_compile_session_new(b,&sb)==XR_COMPILER_SESSION_OK);
    CHECK(xr_compile_parse_with_trivia(sa,"const x = 1\n","a.xr",NULL,&ast)==XR_PARSE_OK);
    XrFmtOutput output={(char *)(uintptr_t)1,99}, empty={0};
    CHECK(xr_compile_format_ast(xr_compile_session_compile_state(sa),ast,NULL,&output)==XR_FMT_BAD_ARGUMENT);
    CHECK(output.text==(char *)(uintptr_t)1 && output.length==99);
    uint64_t before=b->stats.work;
    CHECK(xr_compile_format_ast(xr_compile_session_compile_state(sb),ast,NULL,&empty)==XR_FMT_BAD_ARGUMENT);
    CHECK(!empty.text && !empty.length && b->stats.work==before);
    xr_program_destroy(ast); xr_compile_session_free(sa); xr_compile_session_free(sb);
    xr_compile_resources_release(a); xr_compile_resources_release(b); CHECK(!physical);
}

static void growth_and_types(void) {
    XrCompileResources *resources=NULL;
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    char text[8500]; size_t length=0;
    memcpy(text,"const text = \"",14); length=14;
    memset(text+length,'z',6000); length+=6000;
    memcpy(text+length,"\"\ntype T = { ",13); length+=13;
    for(unsigned i=0;i<70;++i) length+=(size_t)snprintf(text+length,sizeof(text)-length,"field%u: i64%s",i,i==69?" }\n":", ");
    AstNode *ast=NULL;
    CHECK(xr_compile_parse_with_trivia(session,text,"growth.xr",NULL,&ast)==XR_PARSE_OK);
    XrFmtOutput out={0};
    CHECK(xr_compile_format_ast(xr_compile_session_compile_state(session),ast,NULL,&out)==XR_FMT_OK);
    CHECK(out.length>6500 && strstr(out.text,"field69: i64"));
    AstNode *again=NULL;
    CHECK(xr_compile_parse_with_trivia(session,out.text,"again.xr",NULL,&again)==XR_PARSE_OK);
    CHECK(again->as.program.statements[0]->as.var_decl.initializer->as.literal.string_length==6000);
    xr_program_destroy(ast); xr_program_destroy(again); xr_compile_session_free(session);
    xr_compile_resources_release(resources); CHECK(out.text[14]=='z');
    xr_compile_format_output_free(&out); CHECK(!physical);
}


static size_t growth_fixture(const char *input,size_t fail_index,bool expect_success) {
    reset(); XrCompileResources *resources=NULL; XrCompilerSession *session=NULL; AstNode *ast=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
    /* This separately owned source dies before the AST/trivia are consumed. */
    char *copy=NULL; CHECK(xr_compile_state_strdup(xr_compile_session_compile_state(session),input,&copy)==XR_COMPILE_RESOURCE_OK);
    CHECK(xr_compile_parse_with_trivia(session,copy,"grown.xr",NULL,&ast)==XR_PARSE_OK);
    xr_compile_resources_free(copy);
    size_t start=attempts; if(fail_index!=SIZE_MAX)failure=start+fail_index;
    XrFmtOutput out={0}; XrFmtStatus status=xr_compile_format_ast(xr_compile_session_compile_state(session),ast,&xfmt_default_config,&out);
    size_t used=attempts-start;
    CHECK(status==(expect_success?XR_FMT_OK:XR_FMT_OUT_OF_MEMORY));
    if(!expect_success)CHECK(!out.text && !out.length && used==fail_index+1);
    xr_program_destroy(ast); xr_compile_session_free(session); xr_compile_resources_release(resources);
    if(expect_success)CHECK(out.length && out.text[out.length]==0);
    xr_compile_format_output_free(&out); CHECK(!physical); return used;
}
static void scratch_faults_and_depth(void) {
    char text[10000]; size_t used=0;
    memcpy(text,"// trivia\nconst s=\"",19); used=19;
    memset(text+used,'x',6000); used+=6000;
    used+=(size_t)snprintf(text+used,sizeof(text)-used,"\"\ntype T={");
    for(unsigned i=0;i<70;++i)used+=(size_t)snprintf(text+used,sizeof(text)-used,"field%u:i64%s",i,i==69?"}\n":",");
    static const char select[]="fn main(){select{v from ch1->{print(v)}\n100 to ch2->{print(1)}\nafter 10->{print(2)}\n_->{print(3)}\n}}\n";
    const char *fixtures[]={text,select};
    for(unsigned f=0;f<2;++f) {
        size_t count=growth_fixture(fixtures[f],SIZE_MAX,true);
        for(size_t i=0;i<count;++i)growth_fixture(fixtures[f],i,false);
        printf("growth/select fixture=%u formatter malloc points=%zu\n",f,count);
    }
    /* Unary nesting reaches the parser's exact recursive expression ceiling. */
    for(unsigned extra=0;extra<2;++extra) {
        reset(); XrCompileResources *resources=NULL; XrCompilerSession *session=NULL; AstNode *ast=NULL;
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_session_new(resources,&session)==XR_COMPILER_SESSION_OK);
        size_t depth=XR_PARSER_MAX_DEPTH-1u+extra;
        memcpy(text,"const value=",12); memset(text+12,'!',depth); memcpy(text+12+depth,"true\n",6);
        XrParseStatus status=xr_compile_parse_with_trivia(session,text,"depth.xr",NULL,&ast);
        CHECK(status==(extra?XR_PARSE_SYNTAX:XR_PARSE_OK));
        XrFmtOutput out={0};
        if(!extra)CHECK(xr_compile_format_ast(xr_compile_session_compile_state(session),ast,&xfmt_default_config,&out)==XR_FMT_OK);
        xr_program_destroy(ast); xr_compile_session_free(session); xr_compile_resources_release(resources);
        xr_compile_format_output_free(&out); CHECK(!physical);
    }
    puts("maximum legal unary parser depth and next rejected; producer/source death PASS");
}


static void scalar_formula(void) {
    /* Config copy, one reserve + initial terminator, four walker entries,
     * five spelling reads, four byte reads/copies + terminator, newline span. */
    const uint64_t work=sizeof(XrFmtConfig)+1+1+4+5+(4+4+1)+(1+1+1);
    for(unsigned minus=0;minus<2;++minus) {
        reset();XrCompileResources *resources=NULL;XrCompileState *state=NULL;
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_compile_state_new(resources,&state)==XR_COMPILE_RESOURCE_OK);
        uint64_t before=resources->stats.work;
        resources->limits.work=before+work-minus;
        AstNode literal={.type=AST_LITERAL_TRUE},statement={.type=AST_EXPR_STMT};statement.as.expr_stmt=&literal;
        XrFmtOutput out={0};
        CHECK(xr_compile_format_ast(state,&statement,NULL,&out)==(minus?XR_FMT_BUDGET:XR_FMT_OK));
        if(!minus)CHECK(resources->stats.work-before==work && out.length==5 && !memcmp(out.text,"true\n",6));
        else CHECK(!out.text && !out.length);
        xr_compile_state_release(state);xr_compile_resources_release(resources);
        xr_compile_format_output_free(&out);CHECK(!physical);
    }
    printf("independent scalar work formula=%llu exact/minus1 PASS\n",(unsigned long long)work);
}

int main(void) {
    scalar_formula();
    scratch_faults_and_depth();
    reset(); output_and_foreign(); reset(); growth_and_types();
    XrCompileResourceStats baseline={0}; reset(); CHECK(pipeline(&limits,&baseline,SIZE_MAX,UINT64_MAX)==XR_FMT_OK);
    size_t count=attempts;
    for(size_t i=0;i<count;++i) {
        reset(); failure=i; XrCompileResourceStats ignored={0};
        CHECK(pipeline(&limits,&ignored,SIZE_MAX,UINT64_MAX)==XR_FMT_OUT_OF_MEMORY);
        CHECK(!physical);
    }
    for(unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits exact=limits;
        if(axis==0) exact.allocated_bytes=baseline.allocated_bytes;
        if(axis==1) exact.live_bytes=baseline.peak_bytes;
        if(axis==2) exact.work=baseline.work;
        reset(); XrCompileResourceStats observed={0}; CHECK(pipeline(&exact,&observed,SIZE_MAX,UINT64_MAX)==XR_FMT_OK);
        if(axis==0)--exact.allocated_bytes;
        if(axis==1)--exact.live_bytes;
        if(axis==2)--exact.work;
        reset(); CHECK(pipeline(&exact,&observed,SIZE_MAX,UINT64_MAX)==XR_FMT_BUDGET);
    }
    /* Enumerate every formatter work cutoff after the same successful parse.
     * The retained original ledger is bounded, never replaced between stages. */
    uint64_t cutoff=0;
    for(;cutoff<=baseline.work;++cutoff) {
        reset(); XrCompileResourceStats observed={0};
        XrFmtStatus status=pipeline(&limits,&observed,SIZE_MAX,cutoff);
        if(status==XR_FMT_OK)break;
        CHECK(status==XR_FMT_BUDGET);
    }
    CHECK(cutoff<=baseline.work);
    printf("PASS allocations=%zu formatter_work_cutoffs=%llu physical=0\n",count,(unsigned long long)cutoff);
    return 0;
}
