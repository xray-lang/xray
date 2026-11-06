/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_native_invocation_owner.c - Actual Source C, owned inputs and three MSVC commands
 */
#include "app/toolchain/xtc_xir_invocation.h"
#include "toolchain/xcompiler_session.h"
#include <windows.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "invocation_allocator.h"
#ifdef INVOCATION_INJECTED
#include "invocation_faults.inc.h"
#include "invocation_profile_tests.inc.h"
#endif
static bool cancel_immediately(void *context) { (void)context; return true; }

static void *read_input(const char *path, size_t *length) {
    FILE *file = fopen(path, "rb"); CHECK(file && !fseek(file, 0, SEEK_END));
    long size = ftell(file); CHECK(size > 0 && !fseek(file, 0, SEEK_SET));
    void *bytes = malloc((size_t)size); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t)size, file) == (size_t)size && !fclose(file));
    *length = (size_t)size; return bytes;
}
static XrXirNativeProjection *projection(XrCompileResources *resources, const char *directory,
    const char *source, const char *stdlib) {
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(resources, &session) == XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, directory};
    XrXirSourceProductRequest request = {{session, source, &authority, &context, stdlib, NULL, XR_XIR_PROGRAM, NULL},
        {XR_XIR_ARCH_LE64, XR_XIR_VALUE_ABI_VERSION}};
    XrXirSourceProduct *product = NULL;
    CHECK(xr_xir_compile_source_product_build(&request, &product, NULL) == XR_XIR_OK);
    xr_compile_session_free(session);
    XrXirNativeProjectionRequest prepare = {product, "invocation_source", 16777216};
    XrXirNativeProjection *result = NULL;
    CHECK(xr_compile_native_projection_prepare(&prepare, &result) == XR_XIR_OK);
    xr_xir_compile_source_product_free(product); return result;
}
typedef struct CommandStorage {
    XrProcessSpec spec;
    char source[32768], object[32768], report[32768];
    char object_argument[32768], include[7][32768];
} CommandStorage;
static XrToolchainProcess *compile(XrCompileResources *resources, const char *compiler,
    const char *directory, const char *output_directory, const char *sdk, unsigned stage, CommandStorage *storage,
    const XrXirMsvcRecipe *recipe) {
    static const char *const fixed[] = {"/nologo", "/std:c11", "/utf-8", "/experimental:c11atomics",
        "/MD", "/O2", "/W4", "/WX", "/DNDEBUG", "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/D_CRT_SECURE_NO_WARNINGS"};
    CHECK(snprintf(storage->source, sizeof(storage->source), "%s/%s.c", directory, stage ? "launcher" : "generated") > 0);
    CHECK(snprintf(storage->object, sizeof(storage->object), "%s/%s.obj", output_directory, stage ? "launcher" : "generated") > 0);
    CHECK(snprintf(storage->report, sizeof(storage->report), "%s/%s.json", output_directory, stage ? "launcher" : "generated") > 0);
    CHECK(snprintf(storage->object_argument, sizeof(storage->object_argument), "/Fo%s", storage->object) > 0);
    xtc_process_spec_init(&storage->spec, compiler, 30000);
    storage->spec.completion_policy = XR_PROC_COMPLETE_ROOT;
    storage->spec.environment_source = XTC_PROCESS_ENV_EXPLICIT;
    storage->spec.env_count = 3;
    storage->spec.env_keys[0] = "SystemRoot"; storage->spec.env_values[0] = recipe->system_root;
    storage->spec.env_keys[1] = "TEMP"; storage->spec.env_values[1] = output_directory;
    storage->spec.env_keys[2] = "TMP"; storage->spec.env_values[2] = output_directory;
    storage->spec.cwd = directory; storage->spec.output_limit = 4 * 1024 * 1024;
    storage->spec.argv[0] = "independent-argv-zero";
    for (unsigned i = 0; i < 12; ++i) storage->spec.argv[i + 1] = fixed[i];
    const char *names[] = {"src", "include", "generated"};
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(snprintf(storage->include[i], sizeof(storage->include[i]), "/I%s/%s", sdk, names[i]) > 0);
        storage->spec.argv[14 + i] = storage->include[i];
    }
    storage->spec.argv[13] = "/X";
    const char *external[] = {recipe->vc_include, recipe->ucrt_include, recipe->shared_include, recipe->um_include};
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(snprintf(storage->include[3 + i], sizeof(storage->include[3 + i]), "/I%s", external[i]) > 0);
        storage->spec.argv[17 + i] = storage->include[3 + i];
    }
    storage->spec.argv[21] = "/c"; storage->spec.argv[22] = storage->source;
    storage->spec.argv[23] = storage->object_argument; storage->spec.argv[24] = "/sourceDependencies";
    storage->spec.argv[25] = storage->report;
    if (stage) storage->spec.argv[26] = "/DXIR_SDK_PROGRAM_SYMBOL=invocation_source_program";
    XrToolchainProcess *process = NULL;
    CHECK(xtc_process_prepare(resources, &storage->spec, &process) == XTC_PROCESS_OK);
    return process;
}
/* Only caller-owned flat build directories are reclaimed between attempts. */
static void clear_build_directory(const char *directory) {
    char pattern[32768],path[32768];
    int count=snprintf(pattern,sizeof(pattern),"%s/*",directory);CHECK(count>0 && (size_t)count<sizeof(pattern));
    WIN32_FIND_DATAA entry;HANDLE find=FindFirstFileA(pattern,&entry);
    if(find==INVALID_HANDLE_VALUE) {CHECK(GetLastError()==ERROR_FILE_NOT_FOUND);return;}
    do {
        if(!strcmp(entry.cFileName,".") || !strcmp(entry.cFileName,".."))continue;
        CHECK(!(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY));
        count=snprintf(path,sizeof(path),"%s/%s",directory,entry.cFileName);CHECK(count>0 && (size_t)count<sizeof(path));
        CHECK(DeleteFileA(path));
    } while(FindNextFileA(find,&entry));
    CHECK(GetLastError()==ERROR_NO_MORE_FILES && FindClose(find));
}
static int run_test(int argc, char **argv) {
    CHECK(argc==24);
    XrXirMsvcRecipe recipe={argv[16],argv[17],argv[18],argv[19],argv[20]};
    const char *mode=argv[15];
    bool known=!strcmp(mode,"positive") || !strcmp(mode,"full-program") || !strcmp(mode,"cancel") ||
        !strcmp(mode,"occupied-out") || !strcmp(mode,"source-shape") || !strcmp(mode,"launcher-shape") ||
        !strcmp(mode,"bad-recipe") || !strcmp(mode,"same-directory") || !strcmp(mode,"case-directory") || !strcmp(mode,"ancestor-directory") || !strcmp(mode,"reverse-ancestor") ||
        !strcmp(mode,"nonempty") || !strcmp(mode,"child") || !strcmp(mode,"timeout") ||
        !strcmp(mode,"foreign-sdk") || !strcmp(mode,"foreign-0") || !strcmp(mode,"foreign-1") || !strcmp(mode,"foreign-2");
#ifdef INVOCATION_INJECTED
    known=known || !strncmp(mode,"oom-",4) || !strncmp(mode,"process-oom-",12) ||
        !strcmp(mode,"source-input-mismatch") || !strcmp(mode,"launcher-input-mismatch");
#endif
    CHECK(known);
#ifdef INVOCATION_INJECTED
    if(!strcmp(mode,"positive"))owner_record_boundaries(argv[2]);
#endif
    XrCompileResources *resources=sdk_ledger(&sdk_unlimited);
    XrXirNativeProjection *project=projection(resources,argv[1],argv[2],argv[3]);
    char manifest_path[32768];CHECK(snprintf(manifest_path,sizeof(manifest_path),"%s/sdk_manifest.json",argv[4])>0);
    size_t manifest_length=0,launcher_length=0;
    void *manifest=read_input(manifest_path,&manifest_length),*launcher=read_input(argv[8],&launcher_length);
    XrXirRuntimeSdkRequest load={argv[4],manifest,manifest_length,resources};
    XrXirRuntimeSdk *sdk=NULL;CHECK(xr_xir_runtime_sdk_load(&load,&sdk)==XR_XIR_SDK_OK);free(manifest);
    CommandStorage *commands = calloc(2, sizeof(*commands)); CHECK(commands);
    XrToolchainProcess *processes[3] = {NULL, NULL, NULL};
    for (unsigned i = 0; i < 2; ++i) processes[i] = compile(resources, argv[5], argv[7], argv[14], xr_xir_runtime_sdk_root(sdk), i, &commands[i], &recipe);
    char output[32768], report[32768], output_argument[32768], report_argument[32768];
    CHECK(snprintf(output, sizeof(output), "%s/program.exe", argv[14]) > 0);
    CHECK(snprintf(report, sizeof(report), "%s/actual.rsp", argv[14]) > 0);
    CHECK(snprintf(output_argument, sizeof(output_argument), "/out:%s", output) > 0);
    CHECK(snprintf(report_argument, sizeof(report_argument), "/LINKREPROFULLPATHRSP:%s", report) > 0);
    XrProcessSpec spec; xtc_process_spec_init(&spec, argv[6], 30000);
    spec.completion_policy = XR_PROC_COMPLETE_ROOT; spec.cwd = argv[7]; spec.output_limit = 4 * 1024 * 1024;
    spec.environment_source = XTC_PROCESS_ENV_EXPLICIT; spec.env_count = 3;
    spec.env_keys[0] = "SystemRoot"; spec.env_values[0] = recipe.system_root;
    spec.env_keys[1] = "TEMP"; spec.env_values[1] = argv[14];
    spec.env_keys[2] = "TMP"; spec.env_values[2] = argv[14];
    spec.argv[0] = "independent-link-argv-zero"; spec.argv[1] = "/nologo"; spec.argv[2] = "/incremental:no";
    spec.argv[3] = "/NODEFAULTLIB"; spec.argv[4] = "/verbose:lib"; spec.argv[5] = output_argument; spec.argv[6] = report_argument;
    spec.argv[7] = commands[0].object; spec.argv[8] = commands[1].object;
    static const char *const names[] = {"lib/xray_compile_resources.lib", "lib/xray_xir_admission.lib",
        "lib/xray_xir_declarations.lib", "lib/xray_xir_scalar.lib", "lib/xray_xir_runtime_host.lib"};
    for (unsigned i = 0; i < 5; ++i) CHECK(xr_xir_runtime_sdk_file(sdk, names[i], &spec.argv[9 + i]) == XR_XIR_SDK_OK);
    XrXirInvocationLibrary libraries[8];
    for (unsigned i = 0; i < 5; ++i) {
        libraries[i] = (XrXirInvocationLibrary){argv[9 + i], i == 4 ? XR_XIR_INVOCATION_SYSTEM : XR_XIR_INVOCATION_CRT};
        spec.argv[14 + i] = libraries[i].path;
    }
    for(unsigned i=0;i<3;++i) {
        libraries[5+i]=(XrXirInvocationLibrary){argv[21+i],XR_XIR_INVOCATION_SYSTEM};
        spec.argv[19+i]=libraries[5+i].path;
    }
    CHECK(xtc_process_prepare(resources, &spec, &processes[2]) == XTC_PROCESS_OK);
    XrXirInvocationRequest request = {0}; request.projection = project; request.sdk = sdk; request.msvc = recipe;
    for (unsigned i = 0; i < 2; ++i) request.compile[i] = (XrXirInvocationCompile){processes[i], commands[i].source, commands[i].object, commands[i].report};
    request.link = processes[2]; request.input_directory = argv[7]; request.output_directory = argv[14];
    request.link_report = report; request.output = output;
    request.launcher = launcher; request.launcher_length = launcher_length; request.libraries = libraries; request.library_count = 8;
    request.limits = (XrXirInvocationLimits){{4 * 1024 * 1024, 32768, 4096}, 64 * 1024 * 1024, 4096};
    XrXirInvocationStatus expected=XR_XIR_INVOCATION_OK;
    char collision[32768]={0},alternate_directory[32768]={0};
    XrCompileResources *foreign=NULL;XrXirRuntimeSdk *foreign_sdk=NULL;XrToolchainProcess *foreign_process=NULL;
    bool occupied=!strcmp(mode,"occupied-out"),retry=false;void *large_launcher=NULL;
    if(!strcmp(mode,"source-shape")) {request.limits.artifact_bytes=1;expected=XR_XIR_INVOCATION_BUDGET;}
    if(!strcmp(mode,"launcher-shape")) {
        uint64_t maximum=xr_compile_native_projection_source(project)->length;
        for(unsigned i=0;i<2;++i) {WIN32_FILE_ATTRIBUTE_DATA info;CHECK(GetFileAttributesExA(argv[5+i],GetFileExInfoStandard,&info));uint64_t length=((uint64_t)info.nFileSizeHigh<<32)|info.nFileSizeLow;if(length>maximum)maximum=length;}
        CHECK(maximum<SIZE_MAX);request.limits.artifact_bytes=maximum;
        request.launcher_length=(size_t)maximum+1;large_launcher=malloc(request.launcher_length);CHECK(large_launcher);
        memset(large_launcher,' ',request.launcher_length);request.launcher=large_launcher;expected=XR_XIR_INVOCATION_BUDGET;
    }
    if(!strcmp(mode,"cancel")) {request.cancelled=cancel_immediately;expected=XR_XIR_INVOCATION_CANCELLED;retry=true;}
    if(!strcmp(mode,"bad-recipe")) {request.msvc.vc_include=request.input_directory;expected=XR_XIR_INVOCATION_UNSUPPORTED;}
    if(!strcmp(mode,"same-directory") || !strcmp(mode,"case-directory") || !strcmp(mode,"ancestor-directory") || !strcmp(mode,"reverse-ancestor")) {
        request.output_directory=request.input_directory;
        if(!strcmp(mode,"case-directory")) {size_t length=strlen(request.input_directory);CHECK(length<sizeof(alternate_directory));for(size_t i=0;i<=length;++i){char c=request.input_directory[i];alternate_directory[i]=c>='a' && c<='z'?(char)(c-'a'+'A'):c;}request.output_directory=alternate_directory;}
        if(!strcmp(mode,"ancestor-directory")) {CHECK(snprintf(alternate_directory,sizeof(alternate_directory),"%s/nested",request.input_directory)>0);CHECK(CreateDirectoryA(alternate_directory,NULL));request.output_directory=alternate_directory;}
        if(!strcmp(mode,"reverse-ancestor"))request.output_directory=argv[1];
        expected=XR_XIR_INVOCATION_INVALID;
        for(unsigned i=0;i<3;++i) {
            XrProcessSpec *changed=i<2?&commands[i].spec:&spec;changed->env_values[1]=request.output_directory;changed->env_values[2]=request.output_directory;
            xtc_process_free(processes[i]);processes[i]=NULL;CHECK(xtc_process_prepare(resources,changed,&processes[i])==XTC_PROCESS_OK);
            if(i<2)request.compile[i].process=processes[i];else request.link=processes[i];
        }
    }
    if(!strcmp(mode,"timeout")) {
        commands[0].spec.timeout_ms=1;xtc_process_free(processes[0]);processes[0]=NULL;
        CHECK(xtc_process_prepare(resources,&commands[0].spec,&processes[0])==XTC_PROCESS_OK);request.compile[0].process=processes[0];expected=XR_XIR_INVOCATION_TIMEOUT;
    }
    if(!strcmp(mode,"nonempty")) {
        CHECK(snprintf(collision,sizeof(collision),"%s/preserved",request.input_directory)>0);FILE *file=fopen(collision,"wb");CHECK(file);
        CHECK(fwrite("preserved",1,9,file)==9 && !fclose(file));expected=XR_XIR_INVOCATION_INVALID;
    }
    if(!strcmp(mode,"child")) {
        const char suffix[]="\n#error intentional native invocation child failure\n";CHECK(launcher_length<=SIZE_MAX-sizeof(suffix));
        large_launcher=malloc(launcher_length+sizeof(suffix)-1);CHECK(large_launcher);memcpy(large_launcher,launcher,launcher_length);
        memcpy((char *)large_launcher+launcher_length,suffix,sizeof(suffix)-1);request.launcher=large_launcher;request.launcher_length=launcher_length+sizeof(suffix)-1;
        expected=XR_XIR_INVOCATION_CHILD_FAILED;
    }
    if(!strncmp(mode,"foreign-",8)) {
        foreign=sdk_ledger(&sdk_unlimited);expected=XR_XIR_INVOCATION_INVALID;
        if(!strcmp(mode,"foreign-sdk")) {
            manifest=read_input(manifest_path,&manifest_length);load.resources=foreign;load.manifest=manifest;load.manifest_length=manifest_length;
            CHECK(xr_xir_runtime_sdk_load(&load,&foreign_sdk)==XR_XIR_SDK_OK);free(manifest);request.sdk=foreign_sdk;
        } else {
            unsigned stage=(unsigned)strtoul(mode+8,NULL,10);CHECK(stage<3);
            CHECK(xtc_process_prepare(foreign,stage<2?&commands[stage].spec:&spec,&foreign_process)==XTC_PROCESS_OK);
            if(stage<2)request.compile[stage].process=foreign_process;else request.link=foreign_process;
        }
    }
    XrXirInvocation *owner=occupied?(XrXirInvocation *)(uintptr_t)1:NULL;XrXirInvocationDiagnostic diagnostic={0};
    if(occupied)expected=XR_XIR_INVOCATION_INVALID;
    XrCompileResourceStats before,after;CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t blocks=runtime_live,bytes=runtime_bytes,allocation_start=runtime_attempts;
#ifdef INVOCATION_INJECTED
    invocation_runs=0;owner_writes=0;
    if(!strncmp(mode,"oom-",4)) {runtime_fail_at=runtime_attempts+strtoull(mode+4,NULL,10);expected=XR_XIR_INVOCATION_OUT_OF_MEMORY;retry=true;}
    if(!strncmp(mode,"process-oom-",12)) {invocation_process_fail=(unsigned)strtoul(mode+12,NULL,10);CHECK(invocation_process_fail<3);expected=XR_XIR_INVOCATION_OUT_OF_MEMORY;retry=true;}
    if(!strcmp(mode,"source-input-mismatch") || !strcmp(mode,"launcher-input-mismatch")) {
        owner_tamper_write=!strcmp(mode,"source-input-mismatch")?0u:1u;expected=XR_XIR_INVOCATION_INPUT_MISMATCH;retry=true;
    }
#endif
    XrXirInvocationStatus status=xtc_xir_invocation_run(&request,&owner,&diagnostic);
    size_t allocations=runtime_attempts-allocation_start;runtime_fail_at=SIZE_MAX;
    CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
    if(status!=expected)fprintf(stderr,"mode=%s actual=%u expected=%u stage=%u pass=%u domain=%u code=%d exit=%d\n",mode,status,expected,diagnostic.stage,diagnostic.pass,diagnostic.domain,diagnostic.code,diagnostic.exit_code);
    CHECK(status==expected);
    printf("invocation mode=%s status=%u held=%llu peak=%llu allocations=%zu allocated=%llu\n",mode,status,
        (unsigned long long)after.held_bytes,(unsigned long long)after.peak_bytes,allocations,(unsigned long long)after.allocated_bytes);
    if(status!=XR_XIR_INVOCATION_OK) {
        if(occupied) {CHECK(owner==(XrXirInvocation *)(uintptr_t)1);owner=NULL;CHECK(runtime_attempts==allocation_start);}
        CHECK(!owner && after.held_bytes==before.held_bytes && runtime_live==blocks && runtime_bytes==bytes);
        if(occupied || !strncmp(mode,"foreign-",8))CHECK(diagnostic.pass==XR_XIR_INVOCATION_NO_PASS && diagnostic.domain==XR_XIR_INVOCATION_SELF);
        if(!strcmp(mode,"source-shape"))CHECK(diagnostic.pass==XR_XIR_INVOCATION_NO_PASS && diagnostic.stage==XR_XIR_INVOCATION_GENERATED && diagnostic.domain==XR_XIR_INVOCATION_TARGET && diagnostic.code==XR_XIR_TARGET_BUDGET);
        if(!strcmp(mode,"launcher-shape"))CHECK(diagnostic.pass==XR_XIR_INVOCATION_NO_PASS && diagnostic.stage==XR_XIR_INVOCATION_LAUNCHER && diagnostic.domain==XR_XIR_INVOCATION_SELF);
        if(!strcmp(mode,"cancel") || !strcmp(mode,"timeout"))CHECK(diagnostic.pass==XR_XIR_INVOCATION_EXECUTE && diagnostic.stage==XR_XIR_INVOCATION_GENERATED && diagnostic.domain==XR_XIR_INVOCATION_PROCESS);
        if(!strcmp(mode,"child"))CHECK(diagnostic.pass==XR_XIR_INVOCATION_EXECUTE && diagnostic.stage==XR_XIR_INVOCATION_LAUNCHER && diagnostic.domain==XR_XIR_INVOCATION_PROCESS && diagnostic.exit_code);
        if(!strcmp(mode,"bad-recipe") || !strcmp(mode,"same-directory") || !strcmp(mode,"case-directory") || !strcmp(mode,"ancestor-directory") || !strcmp(mode,"reverse-ancestor") || !strcmp(mode,"nonempty"))CHECK(diagnostic.pass==XR_XIR_INVOCATION_NO_PASS && diagnostic.domain==XR_XIR_INVOCATION_SELF);
        if(!strcmp(mode,"source-input-mismatch") || !strcmp(mode,"launcher-input-mismatch"))CHECK(diagnostic.pass==XR_XIR_INVOCATION_NO_PASS && diagnostic.domain==XR_XIR_INVOCATION_SELF && diagnostic.stage==(!strcmp(mode,"source-input-mismatch")?XR_XIR_INVOCATION_GENERATED:XR_XIR_INVOCATION_LAUNCHER));
#ifdef INVOCATION_INJECTED
        if(!strncmp(mode,"oom-",4))CHECK(allocations==strtoull(mode+4,NULL,10)+1);
        if(!strncmp(mode,"process-oom-",12))CHECK(invocation_runs==invocation_process_fail+1 && diagnostic.pass==XR_XIR_INVOCATION_EXECUTE && diagnostic.stage==(XrXirInvocationStage)invocation_process_fail && diagnostic.domain==XR_XIR_INVOCATION_PROCESS);
        owner_tamper_write=UINT_MAX;invocation_process_fail=UINT_MAX;invocation_runs=0;
#endif
        if(retry) {
            clear_build_directory(argv[7]);clear_build_directory(argv[14]);request.cancelled=NULL;
            CHECK(xtc_xir_invocation_run(&request,&owner,&diagnostic)==XR_XIR_INVOCATION_OK && owner);
            puts("same prepared request and ledger restored: successful three-command retry PASS");
        }
    }
#ifdef INVOCATION_INJECTED
    if(owner)CHECK(invocation_runs==3);
#endif
    xtc_process_free(foreign_process);xr_xir_runtime_sdk_free(foreign_sdk);xr_compile_resources_release(foreign);
    for(unsigned i=0;i<3;++i)xtc_process_free(processes[i]);
    xr_compile_native_projection_owner_free(project);xr_xir_runtime_sdk_free(sdk);free(launcher);free(large_launcher);
    xr_compile_resources_release(resources);memset(&request,0,sizeof(request));memset(commands,0xa5,2*sizeof(*commands));free(commands);
    if(collision[0]) {size_t saved_length=0;void *saved=read_input(collision,&saved_length);CHECK(saved_length==9 && !memcmp(saved,"preserved",9));free(saved);}
    if(!owner) {CHECK(!runtime_live && !runtime_bytes);puts("typed rejection: empty output/held restored/physical zero PASS");return 0;}
    const XrXirInvocationFacts *facts=xtc_xir_invocation_facts(owner);const XrXirInvocationProviderFacts *provider=xtc_xir_invocation_provider(owner);
    CHECK(facts && provider && xtc_xir_invocation_resources(owner)==resources && !strcmp(facts->projection.prefix,"invocation_source"));
    CHECK(provider->compiler.path[0] && provider->linker.path[0] && provider->compiler.version.file_text[0] && provider->linker.version.file_text[0]);
    for(unsigned stage=0;stage<3;++stage) {const XrProcessView *frozen=xtc_xir_invocation_command(owner,(XrXirInvocationStage)stage);CHECK(frozen && frozen->executable[0] && frozen->argc && frozen->argv[0] && strcmp(frozen->executable,frozen->argv[0]));}
    uint64_t output_length=0;unsigned kinds[10]={0};
    for(uint32_t i=0;i<facts->file_count;++i) {
        const XrXirInvocationFile *file=xtc_xir_invocation_file(owner,i);CHECK(file && file->kind<10 && file->path && file->length);++kinds[file->kind];
        if(file->kind==XR_XIR_INVOCATION_OUTPUT)output_length=file->length;
    }
    CHECK(!xtc_xir_invocation_file(owner,facts->file_count));
    CHECK(kinds[XR_XIR_INVOCATION_SOURCE]==2 && kinds[XR_XIR_INVOCATION_OBJECT]==2 && kinds[XR_XIR_INVOCATION_SDK_ARCHIVE]==5 &&
        kinds[XR_XIR_INVOCATION_CRT]==4 && kinds[XR_XIR_INVOCATION_SYSTEM]==4 && kinds[XR_XIR_INVOCATION_REPORT]==3 &&
        kinds[XR_XIR_INVOCATION_OUTPUT]==1 && kinds[XR_XIR_INVOCATION_HEADER] && kinds[XR_XIR_INVOCATION_PROVIDER_EXECUTABLE]==3 && kinds[XR_XIR_INVOCATION_PROVIDER_CONFIG]==2);
    printf("current explicit file table %u; producer retirement; three compiler commands; allocations=%zu PASS\n",facts->file_count,allocations);
    CHECK(output_length && output_length<=SIZE_MAX);size_t expected_length=0;void *expected_output=read_input(output,&expected_length);CHECK(expected_length==output_length);
    HANDLE write=CreateFileA(output,GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);CHECK(write==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION);
    void *result=NULL,*again=NULL;size_t length=0,again_length=0;
    CHECK(xtc_xir_invocation_read_output(owner,output_length,&result,&length)==XR_XIR_INVOCATION_OK && length==output_length && !memcmp(result,expected_output,length));
    CHECK(xtc_xir_invocation_read_output(owner,output_length,&again,&again_length)==XR_XIR_INVOCATION_OK && again_length==length && !memcmp(result,again,length));xr_compile_resources_free(again);
    void *occupied_bytes=(void *)(uintptr_t)1;size_t empty_length=0;
    CHECK(xtc_xir_invocation_read_output(owner,output_length,&occupied_bytes,&empty_length)==XR_XIR_INVOCATION_INVALID && occupied_bytes==(void *)(uintptr_t)1 && !empty_length);
    again=NULL;again_length=1;CHECK(xtc_xir_invocation_read_output(owner,output_length,&again,&again_length)==XR_XIR_INVOCATION_INVALID && !again && again_length==1);
    again_length=0;CHECK(xtc_xir_invocation_read_output(owner,output_length-1,&again,&again_length)==XR_XIR_INVOCATION_BUDGET && !again && !again_length);
    size_t attempts=runtime_attempts;runtime_fail_at=attempts;
    CHECK(xtc_xir_invocation_read_output(owner,output_length,&again,&again_length)==XR_XIR_INVOCATION_OUT_OF_MEMORY && !again && !again_length && runtime_attempts==attempts+1);
    runtime_fail_at=SIZE_MAX;CHECK(xtc_xir_invocation_read_output(owner,output_length,&again,&again_length)==XR_XIR_INVOCATION_OK);xr_compile_resources_free(again);
    xtc_xir_invocation_free(owner);CHECK(runtime_live && length==output_length && !memcmp(result,expected_output,length));free(expected_output);xr_compile_resources_free(result);
    CHECK(!runtime_live && !runtime_bytes);puts("independent output outlives producers and invocation; read OOM/retry; physical zero PASS");return 0;
}
typedef struct InvocationThread { int argc; char **argv; } InvocationThread;
static DWORD WINAPI invocation_thread(void *context) {
    InvocationThread *test = context; return (DWORD)run_test(test->argc, test->argv);
}
int main(int argc, char **argv) {
#ifdef INVOCATION_INJECTED
    if (argc == 4 && !strcmp(argv[1], "--provider-only")) return owner_provider_suite(argv[2], argv[3]);
#endif
    if (argc == 2 && !strcmp(argv[1], "--native-control-child")) return 0;
    /* A pure WinAPI child is an explicit control for process-wide lazy state.
     * Windows also retains its debugger object until the calling thread exits. */
    wchar_t self[32768], command[32768];
    CHECK(GetModuleFileNameW(NULL, self, 32768));
    DWORD initial = 0, first = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &initial));
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
        for (unsigned i = 0; i < 2; ++i) {
            HANDLE read = NULL, write = NULL;
            CHECK(CreatePipe(&read, &write, &security, 0));
            CHECK(CloseHandle(read) && CloseHandle(write));
        }
        CHECK(swprintf(command, 32768, L"\"%ls\" --native-control-child", self) > 0);
        STARTUPINFOW startup = {0}; startup.cb = sizeof(startup); PROCESS_INFORMATION process = {0};
        CHECK(CreateProcessW(self, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
        CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
        CHECK(CloseHandle(process.hThread) && CloseHandle(process.hProcess));
        DWORD current = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &current));
        if (!repeat) first = current; else CHECK(current == first);
    }
    DWORD baseline = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &baseline));
    printf("pure WinAPI handles initial=%lu first=%lu repeated=%lu\n", (unsigned long)initial,
        (unsigned long)first, (unsigned long)baseline); fflush(stdout);
    InvocationThread test = {argc, argv}; HANDLE thread = CreateThread(NULL, 0, invocation_thread, &test, 0, NULL); CHECK(thread);
    CHECK(WaitForSingleObject(thread, 300000) == WAIT_OBJECT_0); DWORD code = 1;
    CHECK(GetExitCodeThread(thread, &code) && !code && CloseHandle(thread));
    DWORD after = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
    printf("thread scope handles baseline=%lu after=%lu\n", (unsigned long)baseline, (unsigned long)after);
    CHECK(after == baseline); return 0;
}
