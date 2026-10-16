/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_public_repl_resources.h - Incremental public ownership fault boundaries
 */
#ifndef SOURCE_PUBLIC_REPL_RESOURCES_H
#define SOURCE_PUBLIC_REPL_RESOURCES_H
typedef struct PublicReplOwners {
    XrSourceRepl *owner;
    XrSourceResult *result;
    XrSourceDiagnostic *diagnostic;
    XrSourceValue *value,*independent;
} PublicReplOwners;
static size_t public_repl_boundaries[11];
static bool public_repl_rooted_case,public_repl_file_case,public_repl_array_case;
static void public_repl_close(PublicReplOwners *owners) {
    XrSourceEvent cleanup={0};
    CHECK(xray_source_repl_drop(&owners->owner,&cleanup)==XR_SOURCE_OK && !owners->owner);
    CHECK(cleanup.state==XR_SOURCE_RUN_READY || (cleanup.state==XR_SOURCE_RUN_CLEANUP_FAILED &&
        (cleanup.engine_status==XR_XIR_CALL_OOM || cleanup.engine_status==XR_XIR_CALL_LIMIT)));
    xray_source_result_free(owners->result);xray_source_diagnostic_free(owners->diagnostic);
    xray_source_value_free(owners->value);xray_source_value_free(owners->independent);
    *owners=(PublicReplOwners){0};CHECK(!runtime_live && !runtime_bytes);
}
static XrSourceStatus public_repl_text(PublicReplOwners *owners,const XrSourceText *text,XrSourceRunState expected) {
    XrSourceStatus status=xray_source_repl_begin(owners->owner,text,&owners->diagnostic);
    if(status!=XR_SOURCE_OK)return status;
    XrSourceEvent event={0};size_t polls=0;
    do {status=xray_source_repl_poll(owners->owner,8,&event);CHECK(++polls<10000);}
    while(status==XR_SOURCE_OK && event.state==XR_SOURCE_RUN_READY);
    if(status!=XR_SOURCE_OK)return status;
    if(event.state==XR_SOURCE_RUN_OUT_OF_MEMORY || (event.state==XR_SOURCE_RUN_CLEANUP_FAILED &&
        event.engine_status==XR_XIR_CALL_OOM))return XR_SOURCE_OUT_OF_MEMORY;
    if(event.state==XR_SOURCE_RUN_LIMIT || (event.state==XR_SOURCE_RUN_CLEANUP_FAILED &&
        event.engine_status==XR_XIR_CALL_LIMIT))return XR_SOURCE_LIMIT;
    CHECK(event.state==expected);
    status=xray_source_repl_take(owners->owner,&owners->result);
    if(status==XR_SOURCE_OK) {
        CHECK(owners->result && owners->result->value.status==(expected==XR_SOURCE_RUN_RETURNED?
            XR_XIR_CALL_RETURNED:XR_XIR_CALL_ASSERTION));
        xray_source_result_free(owners->result);owners->result=NULL;
    }
    return status;
}
static XrSourceStatus public_repl_input(PublicReplOwners *owners,const char *source,XrSourceRunState expected) {
    XrSourceText text={NULL,"public-repl-resources",source,strlen(source)};
    return public_repl_text(owners,&text,expected);
}
static XrSourceStatus public_repl_normal_drive(const XrCompileResourceLimits *limits,PublicReplOwners *owners) {
    memset(public_repl_boundaries,0,sizeof(public_repl_boundaries));
    XrSourceCompilerOptions compiler;CHECK(xray_source_compiler_options_init(&compiler,sizeof(compiler))==XR_SOURCE_OK);
    compiler.stdlib_path=XR_PUBLIC_STDLIB;compiler.allocated_bytes=limits->allocated_bytes;
    compiler.live_bytes=limits->live_bytes;compiler.work=limits->work;
    XrSourceCallOptions runtime;CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);runtime.write=discard;
    XrSourceStatus status=xray_source_repl_new(&compiler,"public-repl-resources",&runtime,&owners->owner);
    if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[0]=runtime_attempts;
    status=public_repl_input(owners,"var state=40\nconst bytes=\"owned\\0中\"\nfn next()->i64{state+=1;return state}\n",XR_SOURCE_RUN_RETURNED);
    if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[1]=runtime_attempts;
    status=public_repl_input(owners,"var state=\"shadow\"\nconst observed=next()\nbytes\n",XR_SOURCE_RUN_RETURNED);
    if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[2]=runtime_attempts;
    XrSourceText rejected={NULL,"public-repl-resources","const absent=missingDefinition\n",0};rejected.length=strlen(rejected.bytes);
    status=xray_source_repl_begin(owners->owner,&rejected,&owners->diagnostic);
    if(status!=XR_SOURCE_REJECTED)return status;
    CHECK(owners->diagnostic);xray_source_diagnostic_free(owners->diagnostic);owners->diagnostic=NULL;
    public_repl_boundaries[3]=runtime_attempts;
    status=public_repl_input(owners,"const abandoned=next()\nassert(false)\n77\n",XR_SOURCE_RUN_PANICKED);
    if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[4]=runtime_attempts;
    status=public_repl_input(owners,"const prior=it\nconst after=next()\nafter\n",XR_SOURCE_RUN_RETURNED);
    if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[5]=runtime_attempts;
    status=xray_source_repl_read(owners->owner,"after",&owners->value);if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[6]=runtime_attempts;
    XrSourceScalar scalar={0};CHECK(xray_source_value_scalar(owners->value,&scalar)==XR_SOURCE_OK &&
        scalar.kind==XR_SOURCE_VALUE_I64 && scalar.value.signed_value==43);
    xray_source_value_free(owners->value);owners->value=NULL;
    status=xray_source_repl_read(owners->owner,"it",&owners->value);if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[7]=runtime_attempts;
    CHECK(xray_source_value_scalar(owners->value,&scalar)==XR_SOURCE_OK && scalar.value.signed_value==43);
    xray_source_value_free(owners->value);owners->value=NULL;
    status=xray_source_repl_read(owners->owner,"bytes",&owners->value);if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[8]=runtime_attempts;
    XrSourceEvent cleanup={0};status=xray_source_repl_drop(&owners->owner,&cleanup);
    if(status!=XR_SOURCE_OK)return status;
    CHECK(!owners->owner && cleanup.state==XR_SOURCE_RUN_READY);
    public_repl_boundaries[9]=runtime_attempts;
    status=xray_source_value_copy(owners->value,&owners->independent);if(status!=XR_SOURCE_OK)return status;
    public_repl_boundaries[10]=runtime_attempts;
    xray_source_value_free(owners->value);owners->value=NULL;
    static const char expected[]="owned\0中";const char *bytes=NULL;size_t length=0;
    CHECK(xray_source_value_bytes(owners->independent,&bytes,&length)==XR_SOURCE_OK &&
        length==sizeof(expected)-1 && !memcmp(bytes,expected,length));
    return XR_SOURCE_OK;
}
#include "source_public_repl_inspection_resources.h"
#include "source_public_repl_rooted_resources.h"
#include "source_public_repl_file_resources.h"
#include "source_public_repl_array_resources.h"
static XrSourceStatus public_repl_drive(const XrCompileResourceLimits *limits,PublicReplOwners *owners) {
    return public_repl_array_case ? public_repl_array_drive(limits,owners) : public_repl_file_case ? public_repl_file_drive(limits,owners) : public_repl_rooted_case ? public_repl_rooted_drive(limits,owners) :
        public_repl_inspection_case ? public_repl_inspection_drive(limits,owners) : public_repl_normal_drive(limits,owners);
}
static void public_repl_sticky(PublicReplOwners *owners) {
    if(!owners->owner)return;
    XrCompileResources *resources=xr_source_compiler_resources(owners->owner->compiler);
    XrCompileResourceStats before={0},after={0};CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t attempts=runtime_attempts;XrSourceText text={NULL,"public-repl-resources","const retry=1\n",14};
    XrSourceStatus retried=xray_source_repl_begin(owners->owner,&text,NULL);
    CHECK(retried==XR_SOURCE_OUT_OF_MEMORY || retried==XR_SOURCE_BUDGET || retried==XR_SOURCE_BAD_STATE || retried==XR_SOURCE_BUSY);
    if(public_repl_file_case) {
        XrSourceInputState state=XR_SOURCE_INPUT_INCOMPLETE;
        XrSourceStatus scan=xray_source_repl_check_input(owners->owner,"42",2,&state);
        XrSourceStatus file=xray_source_repl_begin_file(owners->owner,XR_PUBLIC_REPL_FILE_FIXTURE "/main.xr",NULL);
        CHECK(state==XR_SOURCE_INPUT_INCOMPLETE &&
            (scan==XR_SOURCE_OUT_OF_MEMORY || scan==XR_SOURCE_BUDGET || scan==XR_SOURCE_BAD_STATE || scan==XR_SOURCE_BUSY) &&
            (file==XR_SOURCE_OUT_OF_MEMORY || file==XR_SOURCE_BUDGET || file==XR_SOURCE_BAD_STATE || file==XR_SOURCE_BUSY));
    }
    CHECK(runtime_attempts==attempts && xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK &&
        !memcmp(&before,&after,sizeof(before)));
}
static void public_repl_faults(bool probe) {
    const XrCompileResourceLimits limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};size_t sites=0;
    for(size_t point=0;!point || (!probe && point<=sites);++point) {
        PublicReplOwners owners={0};runtime_attempts=0;runtime_fail_at=point?point-1:SIZE_MAX;
        XrSourceStatus status=public_repl_drive(&limits,&owners);
        if(!point){CHECK(status==XR_SOURCE_OK && owners.independent);sites=runtime_attempts;CHECK(sites);}
        else {
            if(status!=XR_SOURCE_OUT_OF_MEMORY || owners.independent)
                fprintf(stderr,"public REPL FI point=%zu/%zu status=%u\n",point,sites,(unsigned)status);
            CHECK(status==XR_SOURCE_OUT_OF_MEMORY && !owners.independent && runtime_attempts>runtime_fail_at);
            public_repl_sticky(&owners);
        }
        runtime_fail_at=SIZE_MAX;public_repl_close(&owners);
    }
    printf("public Source REPL %s actual allocation sites=%zu swept=%zu; %s; sticky retry and physical=0/0\n",
        public_repl_array_case?"array-birth":public_repl_file_case?"file+input":public_repl_rooted_case?"rooted":public_repl_inspection_case?"inspection":"history",sites,probe?0:sites,
        public_repl_array_case?"actual checked birth push, const array read, panic old effect, owned array after producer death":
        public_repl_file_case?"real lexer/file intake, repeat path and exact old closure, syntax failure and owned bytes":
        public_repl_rooted_case?"real rooted imports, repeated path, invalid nearer authority, temporary type query and owned bytes after owner death":
        public_repl_inspection_case?"real typeName, unchanged published observations/it, state41 and owned result after owner death":
            "four published/aborted inputs, typed tails/it/display, actual old state/closure and owned bytes");
}
static void public_repl_samples(void) {
    const XrCompileResourceLimits limits={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    PublicReplOwners owners={0};runtime_attempts=0;runtime_fail_at=SIZE_MAX;
    CHECK(public_repl_drive(&limits,&owners)==XR_SOURCE_OK);size_t sites=runtime_attempts;
    size_t boundaries[11];memcpy(boundaries,public_repl_boundaries,sizeof(boundaries));public_repl_close(&owners);
    size_t previous=0;unsigned cuts=0;
    for(unsigned i=0;i<11;++i) {
        CHECK(boundaries[i]>0 && boundaries[i]<=sites);
        if(boundaries[i]==previous)continue;
        previous=boundaries[i];runtime_attempts=0;runtime_fail_at=boundaries[i]-1;
        XrSourceStatus status=public_repl_drive(&limits,&owners);
        if(status!=XR_SOURCE_OUT_OF_MEMORY || owners.independent)
            fprintf(stderr,"public REPL measured cut stage=%u point=%zu/%zu status=%u\n",i,boundaries[i],sites,(unsigned)status);
        CHECK(status==XR_SOURCE_OUT_OF_MEMORY && !owners.independent && runtime_attempts>runtime_fail_at);
        public_repl_sticky(&owners);runtime_fail_at=SIZE_MAX;public_repl_close(&owners);++cuts;
    }
    printf("public Source REPL measured operation-end FI cuts=%u original sites=%zu; sticky/no-extra-work/physical=0/0; complete prefix remains separately required\n",cuts,sites);
}
static void public_repl_budgets(void) {
    const XrCompileResourceLimits generous={UINT64_C(64)<<20,UINT64_C(8)<<20,UINT64_C(128000000)};
    PublicReplOwners owners={0};XrCompileResourceStats required={0};runtime_fail_at=SIZE_MAX;
    CHECK(public_repl_drive(&generous,&owners)==XR_SOURCE_OK);
    CHECK(xr_compile_resources_stats(owners.independent->resources,&required)==XR_COMPILE_RESOURCE_OK);public_repl_close(&owners);
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={required.allocated_bytes,required.peak_bytes,required.work};
        uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound>1 && *bound<UINT64_MAX);*bound=delta<0?*bound-1:*bound+(unsigned)delta;
        XrSourceStatus status=public_repl_drive(&limits,&owners);
        if(delta<0) {
            if(status!=XR_SOURCE_BUDGET || owners.independent)
                fprintf(stderr,"public REPL budget axis=%u delta=%d status=%u\n",axis,delta,(unsigned)status);
            CHECK(status==XR_SOURCE_BUDGET && !owners.independent);public_repl_sticky(&owners);
        } else CHECK(status==XR_SOURCE_OK && owners.independent);
        public_repl_close(&owners);
    }
    printf("public Source REPL compiler axes allocated=%llu peak=%llu work=%llu; exact/minus-one/plus-one and physical=0/0\n",
        (unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
}
static void public_repl_display_failure(void) {
    PublicReplOwners owners={0};XrSourceCompilerOptions compiler;XrSourceCallOptions runtime;
    CHECK(xray_source_compiler_options_init(&compiler,sizeof(compiler))==XR_SOURCE_OK);
    CHECK(xray_source_call_options_init(&runtime,sizeof(runtime))==XR_SOURCE_OK);
    compiler.stdlib_path=XR_PUBLIC_STDLIB;runtime.write=discard;runtime.output_byte_limit=2;
    runtime_fail_at=SIZE_MAX;
    CHECK(xray_source_repl_new(&compiler,"public-repl-resources",&runtime,&owners.owner)==XR_SOURCE_OK);
    CHECK(public_repl_input(&owners,"var state=0\n1\n",XR_SOURCE_RUN_RETURNED)==XR_SOURCE_OK);
    XrXirSourceGenerationBinding before={0},after={0};
    CHECK(xr_source_submission_lookup(owners.owner->submission,"it",&before)==XR_SOURCE_OK && before.declaration);
    CHECK(public_repl_input(&owners,"state=7\n2\n",XR_SOURCE_RUN_RETURNED)==XR_SOURCE_LIMIT);
    CHECK(owners.owner->stopped && !owners.owner->active && !owners.owner->result);
    CHECK(xr_source_submission_lookup(owners.owner->submission,"it",&after)==XR_SOURCE_OK &&
        after.generation==before.generation && after.declaration==before.declaration);
    XrXirValue actual={0};
    CHECK(xr_source_submission_read(owners.owner->submission,"it",&actual)==XR_SOURCE_OK && actual.payload==1);
    xr_xir_value_drop(&actual);
    CHECK(xr_source_submission_read(owners.owner->submission,"state",&actual)==XR_SOURCE_OK && actual.payload==7);
    xr_xir_value_drop(&actual);public_repl_close(&owners);
    puts("public REPL display failure: exact old result declaration/reader, old-state write survives, candidate unpublished, owner stopped, physical=0/0");
}
#endif // SOURCE_PUBLIC_REPL_RESOURCES_H
