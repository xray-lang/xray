/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_specialize_instance_index_cases.h - Exact lookup and genuine Source scale
 *
 * KEY CONCEPT:
 *   Whole-operation samples receive their complete origins and typed keys
 *   from the public Source path; production lookup is deliberately unchanged.
 */
#ifndef XIR_SPECIALIZE_INSTANCE_INDEX_CASES_H
#define XIR_SPECIALIZE_INSTANCE_INDEX_CASES_H

typedef struct SpecIndexSourceCase { uint32_t count,ordinary,effects;bool report; } SpecIndexSourceCase;
static const char *const spec_index_type_names[]={"i8","i16","i32","i64","u8","u16","u32","u64"};
static const XrXirType spec_index_type_ids[]={XR_XIR_I8,XR_XIR_I16,XR_XIR_I32,XR_XIR_I64,
    XR_XIR_U8,XR_XIR_U16,XR_XIR_U32,XR_XIR_U64};

static void spec_index_text_append(char *text,size_t capacity,size_t *length,const char *piece) {
    size_t count=strlen(piece);CHECK(*length<capacity && count<capacity-*length);
    memcpy(text+*length,piece,count+1);*length+=count;
}

static void spec_index_source_text(const SpecIndexSourceCase *input,char *text,size_t capacity) {
    CHECK(input->count && input->count<=64 && (input->ordinary==3 || input->ordinary==6) &&
        (input->effects==1 || input->effects==4));size_t length=0;text[0]=0;
    spec_index_text_append(text,capacity,&length,"fn stamp<A,B,C");
    if (input->ordinary==6) spec_index_text_append(text,capacity,&length,",D,E,F");
    spec_index_text_append(text,capacity,&length,">(v0:i64");
    if (input->effects==4) spec_index_text_append(text,capacity,&length,",v1:i64,v2:i64,v3:i64");
    spec_index_text_append(text,capacity,&length,")->i64{return v0}\nexport fn run()->i64{var sum=0;");
    for (uint32_t repeat=0;repeat<2;++repeat) for (uint32_t i=0;i<input->count;++i) {
        spec_index_text_append(text,capacity,&length,"sum=sum+stamp<");
        for (uint32_t a=0;a<input->ordinary;++a) {
            if (a) spec_index_text_append(text,capacity,&length,",");
            uint32_t scalar=a<3?(i>>(a*3))&7:3;
            spec_index_text_append(text,capacity,&length,spec_index_type_names[scalar]);
        }
        spec_index_text_append(text,capacity,&length,">(7");
        if (input->effects==4) spec_index_text_append(text,capacity,&length,",7,7,7");
        spec_index_text_append(text,capacity,&length,");");
    }
    spec_index_text_append(text,capacity,&length,"return sum}\nprint(run())\n");
}

static uint32_t spec_index_source_function(const XrXirModule *module,const char *name) {
    size_t length=strlen(name);uint32_t output=UINT32_MAX;
    for (uint32_t f=0;f<module->function_count;++f)
        if (module->functions[f].name_length==length && !memcmp(module->functions[f].name,name,length)) {
            CHECK(output==UINT32_MAX);output=f;
        }
    CHECK(output!=UINT32_MAX);return output;
}

static void spec_index_source_origins(const SpecIndexSourceCase *input,const XrXirModule *source,
    const XrXirModule *instance,uint64_t *elements) {
    uint32_t stamp=spec_index_source_function(source,"stamp"),run=spec_index_source_function(source,"run");
    const XrXirProvenance *proof=instance->provenance;CHECK(proof && proof->kind==XR_XIR_EVIDENCE_INSTANCE);
    uint32_t actual_run=UINT32_MAX,targets[64],seen=0,calls=0;
    for (uint32_t f=0;f<proof->count;++f) if (proof->origins[f].function==run) {
        CHECK(actual_run==UINT32_MAX);actual_run=f;
    }
    CHECK(actual_run!=UINT32_MAX && instance->functions[actual_run].instruction_count==source->functions[run].instruction_count);
    const XrXirGeneric *generic=&source->generics[run];
    for (uint32_t i=0;i<source->functions[run].instruction_count;++i) {
        const XrXirInstruction *before=&source->functions[run].instructions[i];
        if (before->op!=XR_XIR_CALL || before->immediate!=(int64_t)stamp) continue;
        const XrXirInstruction *after=&instance->functions[actual_run].instructions[i];
        CHECK(after->op==XR_XIR_CALL && after->immediate>=0 && (uint64_t)after->immediate<proof->count &&
            before->type_arguments[1]==input->ordinary);
        uint32_t key=calls%input->count,id=(uint32_t)after->immediate;const XrXirOrigin *origin=&proof->origins[id];
        CHECK(origin->function==stamp && origin->argument_count==input->ordinary && origin->effect_argument_count==input->effects);
        for (uint32_t a=0;a<input->ordinary;++a) {
            XrXirType expected=spec_index_type_ids[a<3?(key>>(a*3))&7:3];
            CHECK(origin->arguments[a]==expected && generic->arguments[before->type_arguments[0]+a]==expected);
        }
        for (uint32_t a=0;a<input->effects;++a)
            CHECK(origin->effect_arguments[a].parameter==a && origin->effect_arguments[a].type==XR_XIR_I64);
        CHECK(!instance->declarations->functions[id].cleanup_owner);
        if (calls<input->count) { targets[key]=id;if (key) CHECK(targets[key-1]<id);++seen; }
        else CHECK(targets[key]==id);
        ++calls;
    }
    CHECK(seen==input->count && calls==2*input->count);uint32_t stamp_instances=0;*elements=0;
    for (uint32_t f=0;f<proof->count;++f) if (proof->origins[f].function==stamp) {
        ++stamp_instances;*elements+=4+proof->origins[f].argument_count+2*(uint64_t)proof->origins[f].effect_argument_count;
    }
    CHECK(stamp_instances==input->count);
}

static XrXirStatus spec_index_source_operation(const XrXirCompileContext *context,void *opaque) {
    XrCompileResourceStats start=library_compile_stats(context),entry={0},after={0};size_t first_sites=source_program_compile_attempts;
    SpecIndexSourceCase *input=opaque;char text[32768];spec_index_source_text(input,text,sizeof(text));
    XrCompilerSession *session=NULL;XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if (opened!=XR_COMPILER_SESSION_OK) return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"spec-index-scale",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText source_text={NULL,text,strlen(text)};XrXirSourceResult source={0};
    XrXirSourceDiagnostic source_diagnostic={0};XrXirSourceFailure failure={0};XrXirDiagnostic diagnostic={0};
    XrXirArtifact *read=NULL,*instance=NULL,*lowered=NULL;XrXirCheckedPacket packet={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&source_text,&source,&source_diagnostic,&failure);
    xr_compile_session_free(session);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(source.checked,&packet,&diagnostic);
    xr_xir_compile_source_result_free(&source);
    if (status==XR_XIR_OK) status=xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,&diagnostic);
    xr_xir_compile_checked_packet_free(&packet);uint64_t elements=0;
    if (status==XR_XIR_OK) {
        entry=library_compile_stats(context);status=xr_xir_compile_specialize(read,&instance,&diagnostic);
        after=library_compile_stats(context);
    }
    if (status==XR_XIR_OK) spec_index_source_origins(input,&read->module,&instance->module,&elements);
    uint32_t functions=instance?instance->module.function_count:0,origins=instance?instance->module.provenance->count:0;
    uint32_t source_functions=read?read->module.function_count:0,source_types=read&&read->module.types?read->module.types->count:0;
    uint64_t rows=0,blocks=0,total_key_elements=0;
    if (instance) for (uint32_t f=0;f<instance->module.function_count;++f) {
        rows+=instance->module.functions[f].instruction_count;blocks+=instance->module.functions[f].block_count;
        const XrXirOrigin *origin=&instance->module.provenance->origins[f];
        total_key_elements+=4+origin->argument_count+2*(uint64_t)origin->effect_argument_count;
    }
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    xr_xir_compile_artifact_free(read);read=NULL;
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(instance,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_lower_take(&instance,&target, NULL,&lowered,&diagnostic);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(lowered,&diagnostic);
    xr_xir_compile_artifact_free(instance);xr_xir_compile_artifact_free(lowered);xr_xir_compile_source_failure_free(&failure);
    XrCompileResourceStats final=library_compile_stats(context);CHECK(final.live_bytes==start.live_bytes);
    if (status==XR_XIR_OK && input->report) fprintf(stderr,
        "SPEC_INDEX_SCALE N%u ordinary%u effects%u stampKeyElements%llu totalKeyElements%llu origins%u functions%u rows%llu blocks%llu sourceFunctions%u sourceTypes%u sourceBytes%zu specializeWork%llu specializeAllocated%llu specializeSites%llu wholeWork%llu wholeAllocated%llu wholeSites%zu live%llu peak%llu\n",
        input->count,input->ordinary,input->effects,(unsigned long long)elements,(unsigned long long)total_key_elements,
        origins,functions,(unsigned long long)rows,(unsigned long long)blocks,source_functions,source_types,strlen(text),
        (unsigned long long)(after.work-entry.work),(unsigned long long)(after.allocated_bytes-entry.allocated_bytes),
        (unsigned long long)(after.allocation_count-entry.allocation_count),(unsigned long long)(final.work-start.work),
        (unsigned long long)(final.allocated_bytes-start.allocated_bytes),source_program_compile_attempts-first_sites,
        (unsigned long long)final.live_bytes,(unsigned long long)final.peak_bytes);
    if (status!=XR_XIR_OK && input->report) fprintf(stderr,"SPEC_INDEX_SCALE refused status%u source=%d:%d %s f%u b%u i%u\n",
        (unsigned)status,source_diagnostic.line,source_diagnostic.column,source_diagnostic.message,
        diagnostic.function,diagnostic.block,diagnostic.instruction);
    return status;
}

static void spec_index_source_scales(void) {
    for (uint32_t ordinary=3;ordinary<=6;ordinary+=3) for (uint32_t effects=1;effects<=4;effects+=3)
        for (uint32_t count=8;count<=32;count*=2) {
            LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
            SpecIndexSourceCase input={count,ordinary,effects,true};
            CHECK(spec_index_source_operation(&owner.context,&input)==XR_XIR_OK);library_compile_owner_drop(&owner);
        }
}

static void spec_index_source_resources(void) {
    SpecIndexSourceCase input={8,3,4,false};LibraryCompileOwner owner={0};
    CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);source_program_compile_attempts=0;
    CHECK(spec_index_source_operation(&owner.context,&input)==XR_XIR_OK);size_t sites=source_program_compile_attempts;
    XrCompileResourceStats required=library_compile_stats(&owner.context);CHECK(sites);library_compile_owner_drop(&owner);
    for (size_t site=0;site<sites;++site) {
        CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);source_program_compile_attempts=0;
        source_program_compile_fail_at=site;source_program_compile_injected=false;
        XrXirStatus status=spec_index_source_operation(&owner.context,&input);source_program_compile_fail_at=SIZE_MAX;
        CHECK(status==XR_XIR_OUT_OF_MEMORY && source_program_compile_injected);
        XrCompileResourceStats failed=library_compile_stats(&owner.context);CHECK(failed.live_bytes==owner.baseline.live_bytes);
        CHECK(spec_index_source_operation(&owner.context,&input)==XR_XIR_OK);
        CHECK(library_compile_stats(&owner.context).work>failed.work);library_compile_owner_drop(&owner);
    }
    for (unsigned axis=0;axis<3;++axis) for (unsigned less=0;less<2;++less) {
        XrCompileResourceLimits limits=library_compile_limits;
        uint64_t exact=axis==0?required.allocated_bytes:axis==1?required.peak_bytes:required.work;
        uint64_t *cap=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;CHECK(exact);*cap=exact-less;
        XrXirStatus status=library_compile_owner_new(&owner,&limits);
        if (status==XR_XIR_OK) status=spec_index_source_operation(&owner.context,&input);
        CHECK(status==(less?XR_XIR_BUDGET:XR_XIR_OK));library_compile_owner_drop(&owner);
    }
    fprintf(stderr,"spec-index genuine Source compilerOOM=%zu sameownerretry=%zu allocated%llu peak%llu work%llu threeaxes/physical0 PASS\n",
        sites,sites,(unsigned long long)required.allocated_bytes,(unsigned long long)required.peak_bytes,(unsigned long long)required.work);
}
#endif // XIR_SPECIALIZE_INSTANCE_INDEX_CASES_H
