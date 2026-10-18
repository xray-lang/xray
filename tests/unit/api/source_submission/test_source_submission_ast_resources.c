/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_submission_ast_resources.c - Actual retained AST allocation cuts
 */
#define main source_ast_resource_existing_main
#include "../source_public/test_source_public_faults.c"
#undef main
#include "api/xsource_submission.c"
#include "xir/xxir_format.h"

typedef struct AstHistoryOwners {
    XrSourceCompiler *compiler;
    XrSourceAst *first, *next;
    XrSourceSubmission *submission;
    XrCompileResources *resources;
    XrXirValue value;
    unsigned outputs;
} AstHistoryOwners;
static const XrCompileResourceLimits history_limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
static XrXirOutputStatus history_output(void *context,const XrXirOutputGroup *group) {
    AstHistoryOwners *owner=context;
    CHECK(group && group->count==1 && group->values[0].type==XR_XIR_I64 && group->values[0].payload==17);
    ++owner->outputs;CHECK(owner->outputs==1);return XR_XIR_OUTPUT_OK;
}
static XrSourceStatus history_terminal(AstHistoryOwners *owner) {
    XrXirInstanceResult observed={0};
    for(unsigned step=0;step<4096;++step) {
        XrSourceStatus status=xr_source_submission_poll(owner->submission,8,&observed);
        if(status!=XR_SOURCE_OK)return status;
        if(observed.outcome.status==XR_XIR_CALL_READY)continue;
        XrXirCallResult owned={0};status=xr_source_submission_take(owner->submission,&owned);
        if(status!=XR_SOURCE_OK)return status;
        CHECK(owned.status==observed.outcome.status);
        status=submission_call(owned.status);xr_xir_call_result_drop(&owned);
        if(observed.outcome.status!=XR_XIR_CALL_RETURNED) {
            CHECK(observed.outcome.status==XR_XIR_CALL_OOM || observed.outcome.status==XR_XIR_CALL_LIMIT);return status;
        }
        size_t attempts=runtime_attempts;
        status=xr_source_submission_publish(owner->submission);
        CHECK(runtime_attempts==attempts);return status;
    }
    CHECK(0);return XR_SOURCE_LIMIT;
}
static XrSourceStatus history_parse(AstHistoryOwners *owner,const char *source,XrSourceAst **output) {
    const XrSourceText text={NULL,"retained-history-faults",source,strlen(source)};
    XrSourceDiagnostic *diagnostic=NULL;
    XrSourceStatus status=xray_source_parse_text(owner->compiler,&text,output,&diagnostic);
    if(status==XR_SOURCE_OK)CHECK(!diagnostic);
    xray_source_diagnostic_free(diagnostic);return status;
}
static XrSourceStatus history_prepare(AstHistoryOwners *owner,XrSourceAst **input) {
    XrXirSourceAstOwner descriptor={0};
    XrSourceStatus status=xr_source_ast_owned_input(*input,&descriptor);
    if(status==XR_SOURCE_OK)status=xr_source_submission_prepare_ast(owner->submission,&descriptor,NULL);
    if(status==XR_SOURCE_OK){xray_source_ast_free(*input);*input=NULL;}
    return status;
}
static XrSourceStatus history_drive(AstHistoryOwners *owner,const XrCompileResourceLimits *limits) {
    XrSourceCompilerOptions options;CHECK(xray_source_compiler_options_init(&options,sizeof(options))==XR_SOURCE_OK);
    options.stdlib_path=XR_PUBLIC_STDLIB;options.allocated_bytes=limits->allocated_bytes;
    options.live_bytes=limits->live_bytes;options.work=limits->work;
    XrSourceStatus status=xray_source_compiler_new(&options,&owner->compiler);
    if(status!=XR_SOURCE_OK)return status;
    owner->resources=xr_source_compiler_resources(owner->compiler);
    CHECK(xr_compile_resources_retain(owner->resources)==XR_COMPILE_RESOURCE_OK);
    status=history_parse(owner,"var state=40\nfn next()->i64{state+=1;return state}\nprint(17)\n",&owner->first);
    if(status!=XR_SOURCE_OK)return status;
    status=history_parse(owner,"state+=1\nvar values=[next()]\nconst observed=state\n",&owner->next);
    if(status!=XR_SOURCE_OK)return status;
    XrSourceSubmissionConfig config={0};config.state=xr_source_compiler_state(owner->compiler);
    config.source=(XrXirSourceGenerationConfig){*xr_source_compiler_context(owner->compiler),"actual-retained-history",XR_PUBLIC_STDLIB,NULL,NULL};
    config.target=(XrXirTarget){XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_instance_config_init(&config.runtime,sizeof(config.runtime))==XR_XIR_CALL_READY);
    config.runtime.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,history_output,owner};
    status=xr_source_submission_new(&config,&owner->submission);if(status!=XR_SOURCE_OK)return status;
    status=history_prepare(owner,&owner->first);if(status!=XR_SOURCE_OK)return status;
    xray_source_compiler_free(owner->compiler);owner->compiler=NULL;
    status=history_terminal(owner);if(status!=XR_SOURCE_OK)return status;
    status=history_prepare(owner,&owner->next);if(status!=XR_SOURCE_OK)return status;
    status=history_terminal(owner);if(status!=XR_SOURCE_OK)return status;
    status=xr_source_submission_read(owner->submission,"observed",&owner->value);
    if(status==XR_SOURCE_OK) {
        CHECK(owner->value.type==XR_XIR_I64 && owner->value.payload==42 && owner->outputs==1);
        xr_xir_value_drop(&owner->value);
        status=xr_source_submission_read(owner->submission,"values",&owner->value);
    }
    return status;
}
static void history_sticky(AstHistoryOwners *owner,XrSourceStatus failure) {
    if(!owner->resources || (failure!=XR_SOURCE_BUDGET && failure!=XR_SOURCE_OUT_OF_MEMORY))return;
    XrCompileState *state=owner->submission ? owner->submission->state : owner->compiler ? xr_source_compiler_state(owner->compiler) : NULL;
    if(!state || xr_compile_state_status(state)==XR_COMPILE_RESOURCE_OK)return;
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(owner->resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t attempts=runtime_attempts;XrXirSourceAstOwner input={0};
    XrSourceAst *ast=owner->next ? owner->next : owner->first;
    if(ast && owner->submission) {
        CHECK(xr_source_ast_owned_input(ast,&input)==XR_SOURCE_OK);
        CHECK(xr_source_submission_prepare_ast(owner->submission,&input,NULL)==failure);
    }
    if(owner->compiler) {
        XrSourceAst *untouched=NULL;
        CHECK(history_parse(owner,"const answer=42\n",&untouched)==failure && !untouched);
    }
    CHECK(xr_compile_resources_stats(owner->resources,&after)==XR_COMPILE_RESOURCE_OK && runtime_attempts==attempts &&
        before.work==after.work && before.allocated_bytes==after.allocated_bytes && before.live_bytes==after.live_bytes);
}
static void history_close(AstHistoryOwners *owner) {
    runtime_fail_at=SIZE_MAX;size_t attempts=runtime_attempts;
    xray_source_ast_free(owner->first);xray_source_ast_free(owner->next);xray_source_compiler_free(owner->compiler);
    XrXirCallStatus cleanup=XR_XIR_CALL_READY;
    CHECK(xr_source_submission_drop(&owner->submission,&cleanup)!=XR_SOURCE_BUSY && !owner->submission);
    if(owner->value.type) {
        XrXirFormatCursor root={0},child={0};XrXirFormatAggregate aggregate={0};XrXirValue element={0};
        CHECK(xr_xir_value_format_cursor(&owner->value,&root) && xr_xir_value_format_aggregate(&root,&aggregate));
        CHECK(aggregate.array && aggregate.count==1 && xr_xir_value_format_child(&root,0,&child));
        CHECK(xr_xir_value_format_leaf(&child,&element) && element.type==XR_XIR_I64 && element.payload==42);
    }
    xr_xir_value_drop(&owner->value);
    if(owner->resources) {
        XrCompileResourceStats stats={0};
        CHECK(xr_compile_resources_stats(owner->resources,&stats)==XR_COMPILE_RESOURCE_OK && stats.live_bytes==sizeof(XrCompileResources));
    }
    xr_compile_resources_release(owner->resources);*owner=(AstHistoryOwners){0};
    CHECK(runtime_attempts==attempts && !runtime_live && !runtime_bytes);
}
int main(int argc,char **argv) {
    CHECK(argc==1 || (argc==2 && !strcmp(argv[1],"--resources")));
    runtime_fail_at=SIZE_MAX;runtime_attempts=0;AstHistoryOwners owner={0};
    CHECK(history_drive(&owner,&history_limits)==XR_SOURCE_OK);
    size_t observed=runtime_attempts;XrCompileResourceStats required={0};
    CHECK(xr_compile_resources_stats(owner.resources,&required)==XR_COMPILE_RESOURCE_OK);history_close(&owner);
    size_t sites=argc==2 ? observed : 0;
    for(size_t cut=0;cut<sites;++cut) {
        runtime_attempts=0;runtime_fail_at=cut;
        XrSourceStatus status=history_drive(&owner,&history_limits);
        CHECK(status==XR_SOURCE_OK || status==XR_SOURCE_OUT_OF_MEMORY || status==XR_SOURCE_LIMIT);
        history_sticky(&owner,status);history_close(&owner);
    }
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0 ? &limits.allocated_bytes : axis==1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound && *bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        XrSourceStatus status=history_drive(&owner,&limits);
        CHECK(status==(delta<0 ? XR_SOURCE_BUDGET : XR_SOURCE_OK));
        history_sticky(&owner,status);history_close(&owner);
    }
    printf("Retained actual AST history observed=%zu swept=%zu; Compiler allocated=%llu peak=%llu work=%llu; three axes exact/minus1/plus1; sticky retry; escaped Array; physical0\n",
        observed,sites,(unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
    return 0;
}
