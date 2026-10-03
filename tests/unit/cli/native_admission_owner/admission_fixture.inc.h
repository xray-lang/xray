/* These objects are constructed through private layouts solely to bound the
 * admission fault matrix. Only OUTPUT has a real lease; no fixture authorizes
 * execution. The production native gate supplies all real producer proofs. */
typedef struct AdmissionFixture {
    XrCompileResources *resources;
    XrXirNativeProjection *projection;
    XrXirRuntimeSdk *sdk;
    XtcXirNativeOperation *operation;
} AdmissionFixture;
static const char *const fixture_archives[5]={"lib/xray_compile_resources.lib","lib/xray_xir_admission.lib",
    "lib/xray_xir_declarations.lib","lib/xray_xir_scalar.lib","lib/xray_xir_runtime_host.lib"};
static const char *const fixture_paths[5]={"C:/SDK/中/xray_compile_resources.lib","C:/SDK/中/xray_xir_admission.lib",
    "C:/SDK/中/xray_xir_declarations.lib","C:/SDK/中/xray_xir_scalar.lib","C:/SDK/中/xray_xir_runtime_host.lib"};
static void put16(uint8_t *b,size_t p,uint16_t n){b[p]=(uint8_t)n;b[p+1]=(uint8_t)(n>>8);}
static void put32(uint8_t *b,size_t p,uint32_t n){for(unsigned i=0;i<4;++i)b[p+i]=(uint8_t)(n>>(8*i));}
static void fixture_pe(uint8_t bytes[1024]) {
    memset(bytes,0,1024);put16(bytes,0,0x5a4d);put32(bytes,60,64);put32(bytes,64,0x4550);
    put16(bytes,68,0x8664);put16(bytes,70,1);put16(bytes,84,240);put16(bytes,86,0x22);
    put16(bytes,88,0x20b);put32(bytes,104,0x1000);put32(bytes,144,0x2000);put32(bytes,148,512);put16(bytes,156,3);
    put32(bytes,196,16);put32(bytes,336,512);put32(bytes,340,0x1000);put32(bytes,344,512);put32(bytes,348,512);
    put32(bytes,364,0x60000020);bytes[512]=0xc3;
}
static void fixture_file(const char *path,unsigned mutation) {
    uint8_t bytes[1024];fixture_pe(bytes);
    if(mutation==1)put16(bytes,86,0x2022);
    if(mutation==2)put16(bytes,156,2);
    if(mutation==3)put32(bytes,364,0x40000020);
    if(mutation==4)put32(bytes,104,0);
    FILE *f=fopen(path,"wb");CHECK(f && fwrite(bytes,1,sizeof(bytes),f)==sizeof(bytes) && !fclose(f));
}
static void *fixture_alloc(XrCompileResources *r,size_t bytes){void *p=NULL;CHECK(xr_compile_resources_calloc(r,1,bytes,&p)==XR_COMPILE_RESOURCE_OK);return p;}
static void fixture_row(AdmissionFixture *f,unsigned stage,XrXirInvocationFileKind kind,const char *path) {
    XrXirInvocation *i=f->operation->invocation;uint32_t n=i->facts.file_count++;
    i->files[n].facts=(XrXirInvocationFile){(XrXirInvocationStage)stage,kind,path,4,{1}};
}
static AdmissionFixture fixture_new(const char *path) {
    AdmissionFixture f={0}; f.resources=sdk_ledger(&sdk_unlimited);
    f.projection=fixture_alloc(f.resources,sizeof(*f.projection));
    f.projection->context=(XrXirCompileContext){f.resources,xr_xir_compile_default_limits()};
    memcpy(f.projection->prefix,"fixture",8);f.projection->facts.prefix=f.projection->prefix;
    f.projection->facts.source=(XrXirSourceProductFacts){{XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},0,1,1,{1},{2},{3}};
    f.projection->facts.codegen_policy_id.bytes[0]=4;f.projection->facts.generated_digest.bytes[0]=1;
    f.projection->source=(XrXirNativeProjectionSource){"abcd",4};
    f.sdk=fixture_alloc(f.resources,sizeof(*f.sdk));f.sdk->resources=f.resources;
    f.sdk->facts=(XrXirRuntimeSdkFacts){2,XR_XIR_CHECKED_SCHEMA,XR_XIR_CHECKED_CONTRACT,XR_XIR_VALUE_ABI_VERSION,
        XR_XIR_CALL_ABI_VERSION,XR_XIR_PROGRAM_ABI_VERSION,1,1,1,11,2,0,1,0,3,1,1,5,20,0,0,{7}};
    f.sdk->manifest.file_count=5;
    for(unsigned i=0;i<5;++i){f.sdk->manifest.files[i].path=fixture_archives[i];f.sdk->manifest.files[i].absolute_path=fixture_paths[i];}
    f.operation=fixture_alloc(f.resources,sizeof(*f.operation));f.operation->resources=f.resources;f.operation->phase=XTC_XIR_NATIVE_READY;
    XrXirInvocation *owner=fixture_alloc(f.resources,sizeof(*owner));f.operation->invocation=owner;
    owner->context=f.projection->context;owner->files=fixture_alloc(f.resources,32*sizeof(*owner->files));
    owner->facts.kind=XR_XIR_INVOCATION_LOCKED_REPLAY_FACTS;owner->facts.completed_runs=6;
    owner->facts.projection=f.projection->facts;owner->facts.sdk=f.sdk->facts;
    owner->prefix=fixture_alloc(f.resources,8);memcpy(owner->prefix,"fixture",8);owner->facts.projection.prefix=owner->prefix;
    static const char *const provider[3]={"C:/tools/cl.exe","C:/tools/cl.exe","C:/tools/link.exe"};
    for(unsigned stage=0;stage<3;++stage) {
        XrProcessSpec spec;xtc_process_spec_init(&spec,provider[stage],30000);spec.environment_source=XTC_PROCESS_ENV_EXPLICIT;
        spec.cwd="C:/fixture";spec.output_limit=4096;spec.env_count=3;
        spec.env_keys[0]="SystemRoot";spec.env_keys[1]="TEMP";spec.env_keys[2]="TMP";
        spec.env_values[0]="C:/Windows";spec.env_values[1]="C:/output";spec.env_values[2]="C:/output";
        unsigned count=stage==0?26:stage==1?27:19;
        for(unsigned j=0;j<count;++j)spec.argv[j]=j==0?"distinct-argv0":j==2?"":"argument";
        CHECK(xtc_process_prepare(f.resources,&spec,&owner->process[stage])==XTC_PROCESS_OK);
        CHECK(xtc_process_view(owner->process[stage],&owner->commands[stage])==XTC_PROCESS_OK);
        if(stage<2){fixture_row(&f,stage,XR_XIR_INVOCATION_SOURCE,stage?"C:/fixture/launcher.c":"C:/fixture/generated.c");
            fixture_row(&f,stage,XR_XIR_INVOCATION_OBJECT,stage?"C:/output/launcher.obj":"C:/output/generated.obj");
            fixture_row(&f,stage,XR_XIR_INVOCATION_HEADER,"C:/SDK/中/header.h");}
        else {for(unsigned j=0;j<5;++j)fixture_row(&f,stage,XR_XIR_INVOCATION_SDK_ARCHIVE,fixture_paths[j]);
            fixture_row(&f,stage,XR_XIR_INVOCATION_CRT,"C:/crt/md.lib");fixture_row(&f,stage,XR_XIR_INVOCATION_SYSTEM,"C:/Windows/system.lib");}
        fixture_row(&f,stage,XR_XIR_INVOCATION_REPORT,"C:/output/report");
        if(stage!=1)fixture_row(&f,stage,XR_XIR_INVOCATION_PROVIDER_CONFIG,stage?"C:/tools/link.exe.config":"C:/tools/cl.exe.config");
        fixture_row(&f,stage,XR_XIR_INVOCATION_PROVIDER_IMAGE,provider[stage]);
    }
    owner->provider.compiler=(XrXirInvocationProviderImageFacts){provider[0],4,{1},{{19,44,35219,0},{14,44,35219,0},"19.44.35219.0","14.44.35219.0"},0};
    owner->provider.linker=(XrXirInvocationProviderImageFacts){provider[2],4,{1},{{14,44,35219,0},{14,44,35219,0},"14.44.35219.0","14.44.35219.0"},0};
    CHECK(xtc_xir_file_lease_open(f.resources,path,&owner->output_lease)==XR_XIR_TARGET_OK);
    const XtcXirFileFacts *output=xtc_xir_file_lease_facts(owner->output_lease);
    fixture_row(&f,2,XR_XIR_INVOCATION_OUTPUT,output->path);
    InvocationFile *row=&owner->files[owner->facts.file_count-1];row->lease=owner->output_lease;
    row->facts.length=output->length;memcpy(row->facts.digest,output->digest,32);
    return f;
}
static void fixture_free(AdmissionFixture *f) {
    close_operation(&f->operation);xr_compile_native_projection_owner_free(f->projection);xr_xir_runtime_sdk_free(f->sdk);
    xr_compile_resources_release(f->resources);CHECK(!runtime_live && !runtime_bytes);memset(f,0,sizeof(*f));
}
static XtcXirNativeAdmissionStatus fixture_admit(AdmissionFixture *f,XrXirNativeArtifact **output,XtcXirNativeAdmissionDiagnostic *d) {
    return xtc_xir_native_admit(f->operation,f->projection,f->sdk,1024,output,d);
}
static void fixture_failure(AdmissionFixture *f,XtcXirNativeAdmissionStatus expected) {
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(f->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirNativeArtifact *artifact=NULL;XtcXirNativeAdmissionDiagnostic d;
    XtcXirNativeAdmissionStatus status=fixture_admit(f,&artifact,&d);
    if(status!=expected)fprintf(stderr,"fixture expected=%u actual=%u domain=%u code=%d\n",expected,status,d.domain,d.code);
    CHECK(status==expected && !artifact);CHECK(xr_compile_resources_stats(f->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.live_bytes==before.live_bytes);
}
static void fixture_rejections(const char *path) {
    for(unsigned scenario=0;scenario<18;++scenario) {
        AdmissionFixture f=fixture_new(path);XrXirInvocation *i=f.operation->invocation;
        XtcXirNativeAdmissionStatus expected=XTC_XIR_ADMISSION_MISMATCH;
        switch(scenario){
        case 0:f.operation->phase=XTC_XIR_NATIVE_NEW;expected=XTC_XIR_ADMISSION_INVALID;break;
        case 1:i->facts.completed_runs=5;expected=XTC_XIR_ADMISSION_INVALID;break;
        case 2:i->facts.projection.source.entry=1;break;
        case 3:i->facts.projection.source.lowered_layout_digest[0]^=1;break;
        case 4:i->facts.projection.generated_digest.bytes[0]^=1;break;
        case 5:i->facts.sdk.identity[0]^=1;break;
        case 6:i->facts.sdk.program_abi--;break;
        case 7:i->provider.compiler.digest[0]^=1;break;
        case 8:i->provider.launcher_compiler_image_index=99;expected=XTC_XIR_ADMISSION_INVALID;break;
        case 9:i->provider.linker.length++;break;
        case 10:i->files[12].facts.path=fixture_paths[0];break;
        case 11:i->files[0].facts.length++;break;
        case 12:i->files[0].facts.kind=(XrXirInvocationFileKind)99;expected=XTC_XIR_ADMISSION_INVALID;break;
        case 13:i->files[i->facts.file_count-1].facts.digest[0]^=1;break;
        case 14:i->files[i->facts.file_count-1].facts.length++;break;
        case 15:f.sdk->facts.sanitizers=1;i->facts.sdk.sanitizers=1;expected=XTC_XIR_ADMISSION_UNSUPPORTED;break;
        case 16:i->files[2].facts.stage=XR_XIR_INVOCATION_LINK;expected=XTC_XIR_ADMISSION_INVALID;break;
        case 17:i->files[12].facts.kind=XR_XIR_INVOCATION_SYSTEM;expected=XTC_XIR_ADMISSION_INVALID;break;
        }
        fixture_failure(&f,expected);fixture_free(&f);
    }
    AdmissionFixture f=fixture_new(path);XrCompileResources *foreign=sdk_ledger(&sdk_unlimited);
    XrXirNativeArtifact *artifact=(XrXirNativeArtifact *)(uintptr_t)1;XtcXirNativeAdmissionDiagnostic d;
    size_t attempts=runtime_attempts;CHECK(fixture_admit(&f,&artifact,&d)==XTC_XIR_ADMISSION_INVALID && artifact==(void *)(uintptr_t)1);
    artifact=NULL;CHECK(xtc_xir_native_admit(f.operation,f.projection,f.sdk,0,&artifact,&d)==XTC_XIR_ADMISSION_INVALID);
    for(unsigned part=0;part<3;++part){if(!part)f.operation->resources=foreign;else if(part==1)f.projection->context.resources=foreign;else f.sdk->resources=foreign;
        CHECK(fixture_admit(&f,&artifact,&d)==XTC_XIR_ADMISSION_INVALID && !artifact);
        f.operation->resources=f.resources;f.projection->context.resources=f.resources;f.sdk->resources=f.resources;}
    CHECK(attempts==runtime_attempts);xr_compile_resources_release(foreign);fixture_free(&f);
}
static void fixture_identities(const char *path) {
    AdmissionFixture f=fixture_new(path); XrXirNativeArtifact *artifact=NULL; XtcXirNativeAdmissionDiagnostic d;
    CHECK(fixture_admit(&f,&artifact,&d)==XTC_XIR_ADMISSION_OK);
    XrToolchainBinding expected=xr_compile_native_artifact_view(artifact)->input.toolchain;
    xr_compile_native_artifact_free(artifact);artifact=NULL;
    XrXirInvocation *i=f.operation->invocation;
    const XrXirInvocationFileKind kinds[]={XR_XIR_INVOCATION_HEADER,XR_XIR_INVOCATION_CRT,XR_XIR_INVOCATION_SYSTEM,
        XR_XIR_INVOCATION_PROVIDER_IMAGE,XR_XIR_INVOCATION_PROVIDER_CONFIG};
    for(unsigned kind=0;kind<5;++kind){uint32_t found=UINT32_MAX;
        for(uint32_t n=0;n<i->facts.file_count;++n)if(i->files[n].facts.kind==kinds[kind]){found=n;break;}
        CHECK(found!=UINT32_MAX); i->files[found].facts.digest[1]^=1;
        if(kinds[kind]==XR_XIR_INVOCATION_PROVIDER_IMAGE){i->provider.compiler.digest[1]^=1;
            for(uint32_t n=found+1;n<i->facts.file_count;++n)if(i->files[n].facts.stage==1 && i->files[n].facts.kind==kinds[kind])i->files[n].facts.digest[1]^=1;}
        CHECK(fixture_admit(&f,&artifact,&d)==XTC_XIR_ADMISSION_OK);
        CHECK(memcmp(expected.sysroot_id.bytes,xr_compile_native_artifact_view(artifact)->input.toolchain.sysroot_id.bytes,32));
        xr_compile_native_artifact_free(artifact);artifact=NULL;i->files[found].facts.digest[1]^=1;
        if(kinds[kind]==XR_XIR_INVOCATION_PROVIDER_IMAGE){i->provider.compiler.digest[1]^=1;
            for(uint32_t n=found+1;n<i->facts.file_count;++n)if(i->files[n].facts.stage==1 && i->files[n].facts.kind==kinds[kind])i->files[n].facts.digest[1]^=1;}
    }
    /* Reverse the table, retaining the per-stage one-image sequence. */
    for(uint32_t n=0;n<i->facts.file_count/2;++n){InvocationFile saved=i->files[n];i->files[n]=i->files[i->facts.file_count-n-1];i->files[i->facts.file_count-n-1]=saved;}
    CHECK(fixture_admit(&f,&artifact,&d)==XTC_XIR_ADMISSION_OK);
    CHECK(!memcmp(expected.id.bytes,xr_compile_native_artifact_view(artifact)->input.toolchain.id.bytes,32));
    xr_compile_native_artifact_free(artifact);artifact=NULL;
    const char *saved=NULL; uint32_t row=0;
    for(;row<i->facts.file_count;++row)if(i->files[row].facts.kind==XR_XIR_INVOCATION_SDK_ARCHIVE && !strcmp(i->files[row].facts.path,fixture_paths[0]))break;
    CHECK(row<i->facts.file_count);saved=i->files[row].facts.path;
    i->files[row].facts.path="c:/sdk/中/XRAY_COMPILE_RESOURCES.LIB";
    CHECK(fixture_admit(&f,&artifact,&d)==XTC_XIR_ADMISSION_OK);xr_compile_native_artifact_free(artifact);artifact=NULL;i->files[row].facts.path=saved;
    fixture_free(&f);
}
static int admission_unit(const char *path,const char *exported,const char *json) {
    fixture_file(path,0);AdmissionFixture f=fixture_new(path);XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(f.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirNativeArtifact *artifact=NULL;XtcXirNativeAdmissionDiagnostic d;size_t attempts=runtime_attempts;
    utf_calls=0;compare_calls=0;capture_work=true;CHECK(fixture_admit(&f,&artifact,&d)==XTC_XIR_ADMISSION_OK);capture_work=false;
    CHECK(xr_compile_resources_stats(f.resources,&after)==XR_COMPILE_RESOURCE_OK);
    uint64_t work=after.work-before.work,allocated=after.allocated_bytes-before.allocated_bytes,peak=after.peak_bytes;
    unsigned utf_count=utf_calls,compare_count=compare_calls;
    size_t calls=runtime_attempts-attempts,recorded=boundary_count;CHECK(recorded<32768);
    uint64_t cuts[32768];for(size_t i=0;i<recorded;++i)cuts[i]=work_boundaries[i]-before.work;
    facts_dump(json,f.operation,artifact);save_artifact(exported,artifact);xr_compile_native_artifact_free(artifact);artifact=NULL;fixture_free(&f);
    for(size_t fail=0;fail<calls;++fail){f=fixture_new(path);runtime_fail_at=runtime_attempts+fail;fixture_failure(&f,XTC_XIR_ADMISSION_OUT_OF_MEMORY);
        runtime_fail_at=SIZE_MAX;fixture_free(&f);}
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus){f=fixture_new(path);
        CHECK(xr_compile_resources_stats(f.resources,&before)==XR_COMPILE_RESOURCE_OK);
        if(!axis)f.resources->limits.allocated_bytes=before.allocated_bytes+allocated-minus;
        else if(axis==1)f.resources->limits.live_bytes=peak-minus;
        else f.resources->limits.work=before.work+work-minus;
        XtcXirNativeAdmissionStatus status=fixture_admit(&f,&artifact,&d);
        CHECK(status==(minus?XTC_XIR_ADMISSION_BUDGET:XTC_XIR_ADMISSION_OK));CHECK(minus?!artifact:!!artifact);
        xr_compile_native_artifact_free(artifact);artifact=NULL;
        if(!minus && axis==2)fixture_failure(&f,XTC_XIR_ADMISSION_BUDGET);
        fixture_free(&f);}
    /* Test actual submitted boundaries, not every integer up to total work. */
    for(size_t c=0;c<recorded;++c){if(c && cuts[c]==cuts[c-1])continue;f=fixture_new(path);
        f.resources->limits.work=f.resources->stats.work+cuts[c]-1;fixture_failure(&f,XTC_XIR_ADMISSION_BUDGET);fixture_free(&f);}
    fixture_rejections(path);fixture_identities(path);
    for(unsigned point=1;point<=compare_count;++point)for(unsigned error=0;error<3;++error){f=fixture_new(path);compare_calls=0;compare_fail=point;
        compare_error=error==0?ERROR_ACCESS_DENIED:error==1?ERROR_OUTOFMEMORY:ERROR_NOT_ENOUGH_MEMORY;
        fixture_failure(&f,error?XTC_XIR_ADMISSION_OUT_OF_MEMORY:XTC_XIR_ADMISSION_IO);compare_fail=0;fixture_free(&f);}
    for(unsigned point=1;point<=utf_count;++point)for(unsigned error=0;error<3;++error){f=fixture_new(path);utf_calls=0;utf_fail=point;
        utf_error=error==0?ERROR_ACCESS_DENIED:error==1?ERROR_OUTOFMEMORY:ERROR_NOT_ENOUGH_MEMORY;
        fixture_failure(&f,error?XTC_XIR_ADMISSION_OUT_OF_MEMORY:XTC_XIR_ADMISSION_IO);utf_fail=0;fixture_free(&f);}
    printf("new UTF calls=%u ordinal calls=%u each IO/two-OOM PASS\n",utf_count,compare_count);
    for(unsigned mutation=1;mutation<=4;++mutation){fixture_file(path,mutation);f=fixture_new(path);
        fixture_failure(&f,mutation==4?XTC_XIR_ADMISSION_INVALID:XTC_XIR_ADMISSION_UNSUPPORTED);fixture_free(&f);}
    CHECK(DeleteFileA(path));printf("synthetic owned composition + real OUTPUT lease: allocations=%zu work-boundaries=%zu work=%llu allocated=%llu peak=%llu all-cutoffs/three-axis/foreign/mismatch/PE/WinIO PASS physical=0\n",
        calls,recorded,(unsigned long long)work,(unsigned long long)allocated,(unsigned long long)peak);return 0;
}