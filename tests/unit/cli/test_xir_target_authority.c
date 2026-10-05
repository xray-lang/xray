/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_target_authority.c - Input leases, identity separation and exact failures
 */
#include "base/xmalloc.h"
#include "app/toolchain/xtc_xir_target.h"
#include "os/os_proc.h"
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s (Windows %lu)\n", __LINE__, #c, GetLastError()); exit(1); } } while (0)
typedef struct Allocation { void *pointer; size_t bytes; } Allocation;
static Allocation allocations[65536];
static size_t attempts, fail_at = SIZE_MAX, live, live_bytes,allocated_bytes,peak_bytes;
static void *target_test_malloc(size_t bytes) {
    if (attempts++ == fail_at) return NULL;
    void *pointer = xr_malloc(bytes); if (!pointer) return NULL;
    for (size_t i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i) {
        if (allocations[i].pointer) continue;
        if(!live)allocated_bytes=peak_bytes=0;
        allocations[i] = (Allocation){pointer, bytes}; ++live; live_bytes += bytes;allocated_bytes+=bytes;
        if(live_bytes>peak_bytes)peak_bytes=live_bytes;return pointer;
    }
    CHECK(false); return NULL;
}
static void target_test_free(void *pointer) {
    if (!pointer) return;
    for (size_t i = 0; i < sizeof(allocations) / sizeof(allocations[0]); ++i) {
        if (allocations[i].pointer != pointer) continue;
        --live; live_bytes -= allocations[i].bytes; allocations[i] = (Allocation){0}; xr_free(pointer); return;
    }
    CHECK(false);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc target_test_malloc
#define xr_free target_test_free
#define xr_compile_resources_work target_real_work
#define xr_compile_resources_calloc target_real_calloc
#include "base/xcompile_resources.c"
#undef xr_compile_resources_work
#undef xr_compile_resources_calloc
static bool record_work;
static uint64_t work_boundaries[32768];static size_t work_count;
static XrCompileResourceStatus remember_work(XrCompileResources *owner,XrCompileResourceStatus status) {
    if(record_work&&status==XR_COMPILE_RESOURCE_OK) {
        XrCompileResourceStats result;CHECK(xr_compile_resources_stats(owner,&result)==XR_COMPILE_RESOURCE_OK);
        if(!work_count||work_boundaries[work_count-1]!=result.work) {
            CHECK(work_count<32768);work_boundaries[work_count++]=result.work;
        }
    }
    return status;
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *r,uint64_t n) {
    return remember_work(r,target_real_work(r,n));
}
XR_FUNC XrCompileResourceStatus xr_compile_resources_calloc(XrCompileResources *r,size_t n,size_t size,void **out) {
    return remember_work(r,target_real_calloc(r,n,size,out));
}
#ifndef XIR_TARGET_PRODUCTION_TEST
static size_t io_attempts, io_fail_at = SIZE_MAX;
static size_t conversion_calls,environment_comparisons,path_comparisons;
static DWORD injected_error = ERROR_READ_FAULT;
static bool short_read;
static bool target_io_fail(void) {
    if (io_attempts++ != io_fail_at) return false;
    SetLastError(injected_error); return true;
}
static BOOL target_read(HANDLE file, LPVOID bytes, DWORD count, LPDWORD actual, LPOVERLAPPED over) {
    if (target_io_fail()) return FALSE;
    if (short_read) { *actual = 0; return TRUE; }
    return ReadFile(file, bytes, count, actual, over);
}
#define CreateFileW(...) (target_io_fail() ? INVALID_HANDLE_VALUE : CreateFileW(__VA_ARGS__))
#define ReadFile target_read
#define GetFileInformationByHandle(...) (target_io_fail() ? FALSE : GetFileInformationByHandle(__VA_ARGS__))
#define GetFileInformationByHandleEx(...) (target_io_fail() ? FALSE : GetFileInformationByHandleEx(__VA_ARGS__))
#define GetFinalPathNameByHandleW(...) (target_io_fail() ? 0u : GetFinalPathNameByHandleW(__VA_ARGS__))
#define GetFileSizeEx(...) (target_io_fail() ? FALSE : GetFileSizeEx(__VA_ARGS__))
#define GetFileType(...) (target_io_fail() ? FILE_TYPE_UNKNOWN : GetFileType(__VA_ARGS__))
#define MultiByteToWideChar(...) (++conversion_calls, target_io_fail() ? 0 : MultiByteToWideChar(__VA_ARGS__))
#define WideCharToMultiByte(...) (target_io_fail() ? 0 : WideCharToMultiByte(__VA_ARGS__))
static int target_ordinal(LPCWCH a,int alen,LPCWCH b,int blen,BOOL ignore_case) {
    if(alen<0)++path_comparisons;else ++environment_comparisons;
    CHECK(ignore_case==TRUE);
    return target_io_fail()?0:CompareStringOrdinal(a,alen,b,blen,ignore_case);
}
#define CompareStringOrdinal target_ordinal
#include "app/toolchain/xtc_xir_target.c"
#include "app/toolchain/xtc_xir_sysroot.c"
#include "app/toolchain/xtc_xir_images.c"
#undef CreateFileW
#undef ReadFile
#undef GetFileInformationByHandle
#undef GetFinalPathNameByHandleW
#undef GetFileSizeEx
#undef GetFileType
#undef MultiByteToWideChar
#undef WideCharToMultiByte
#undef CompareStringOrdinal
#endif

static char directory[4096], source_path[4096], header_path[4096], compiler_path[4096];
static XrXirTargetDependency files[3];
static char argument_text[64] = "/std:c11", environment_text[64] = "captured value";
static const char *arguments[] = {"observed-compiler", argument_text, "source.c"};
static char unicode_key[64]="ÄNAME", unicode_value[64]="原始值";
static XrXirTargetEnvironment environment[] = {
    {"INCLUDE", environment_text},{unicode_key,unicode_value},{"with space","spaced value"},
    {"=C:","C:/captured"},{"=ExitCode","0"},{"中文","值"},{"emoji 😄",""}
};
static XrXirTargetCommandFacts command;
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
static XrXirTargetSnapshotRequest request_for(XrCompileResources *resources) {
    return (XrXirTargetSnapshotRequest){resources, "x86_64-windows-msvc", 3, 2, 11, files, 3, &command, 1, NULL};
}
static void write_bytes(const char *path, const char *bytes) {
    FILE *stream = fopen(path, "wb"); CHECK(stream);
    CHECK(fwrite(bytes, 1, strlen(bytes), stream) == strlen(bytes)); CHECK(!fclose(stream));
}
static void setup(void) {
    char temporary[2048]; CHECK(GetTempPathA(sizeof(temporary), temporary));
    CHECK(snprintf(directory, sizeof(directory), "%sxir-target-%lu", temporary, GetCurrentProcessId()) > 0);
    CHECK(CreateDirectoryA(directory, NULL));
    CHECK(snprintf(source_path, sizeof(source_path), "%s/source.c", directory) > 0);
    CHECK(snprintf(header_path, sizeof(header_path), "%s/header.h", directory) > 0);
    CHECK(snprintf(compiler_path, sizeof(compiler_path), "%s/compiler.bin", directory) > 0);
    write_bytes(source_path, "source"); write_bytes(header_path, "abc"); write_bytes(compiler_path, "compiler");
    files[0] = (XrXirTargetDependency){source_path, XR_XIR_TARGET_SOURCE};
    files[1] = (XrXirTargetDependency){header_path, XR_XIR_TARGET_HEADER};
    files[2] = (XrXirTargetDependency){compiler_path, XR_XIR_TARGET_COMPILER};
    command = (XrXirTargetCommandFacts){compiler_path, directory, arguments, 3, environment,
        (uint32_t)(sizeof(environment)/sizeof(environment[0])), 3000, 1048576, 0, 0};
}
static void teardown(void) {
    CHECK(DeleteFileA(source_path)); CHECK(DeleteFileA(header_path)); CHECK(DeleteFileA(compiler_path));
    CHECK(RemoveDirectoryA(directory)); CHECK(!live && !live_bytes);
}
static XrCompileResources *ledger(const XrCompileResourceLimits *limits) {
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(limits, &resources) == XR_COMPILE_RESOURCE_OK); return resources;
}
static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats result; CHECK(xr_compile_resources_stats(resources, &result) == XR_COMPILE_RESOURCE_OK);
    CHECK(result.live_bytes==live_bytes&&result.allocated_bytes==allocated_bytes&&result.peak_bytes==peak_bytes);return result;
}
static XrXirTargetSnapshot *capture(XrCompileResources *resources) {
    XrXirTargetSnapshotRequest request = request_for(resources); XrXirTargetSnapshot *snapshot = NULL;
    XrXirTargetStatus status = xtc_xir_target_snapshot_capture(&request, &snapshot);
    if (status != XR_XIR_TARGET_OK) fprintf(stderr, "capture status %d\n", (int)status);
    CHECK(status == XR_XIR_TARGET_OK && snapshot); return snapshot;
}
static void identity_and_lifetime(void) {
    XrCompileResources *resources = ledger(&unlimited); uint64_t baseline = stats(resources).live_bytes;
    XrXirTargetSnapshot *snapshot = capture(resources); const XrXirTargetFacts *facts = xtc_xir_target_facts(snapshot);
    CHECK(facts->schema == 3 && facts->file_count == 3 && facts->command_count == 1);
    uint8_t first[32], provider[32], sysroot[32]; memcpy(first, facts->identity, 32);
    memcpy(provider, facts->provider_identity, 32); memcpy(sysroot, facts->sysroot_identity, 32);
    static const uint8_t abc[32] = {0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    bool found = false;
    for (uint32_t i = 0; i < facts->file_count; ++i) {
        const XrXirTargetFile *file = xtc_xir_target_file(snapshot, i);
        if (file->kind == XR_XIR_TARGET_HEADER) { CHECK(file->length == 3 && !memcmp(file->digest, abc, 32)); found = true; }
    }
    CHECK(found && !xtc_xir_target_file(snapshot, 3) && !xtc_xir_target_command_facts(snapshot, 1));
    HANDLE writer = CreateFileA(header_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, 0, NULL); CHECK(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    CHECK(!DeleteFileA(header_path) && GetLastError() == ERROR_SHARING_VIOLATION);
    char renamed[4096]; CHECK(snprintf(renamed, sizeof(renamed), "%s-renamed", directory) > 0);
    CHECK(!MoveFileA(directory, renamed));
    strcpy(argument_text, "/changed"); strcpy(environment_text, "changed");
    strcpy(unicode_key,"changed");strcpy(unicode_value,"changed");
    const XrXirTargetCommandFacts *saved = xtc_xir_target_command_facts(snapshot, 0);
    CHECK(!strcmp(saved->argv[1], "/std:c11") && !strcmp(saved->environment[0].value, "captured value"));
    CHECK(!strcmp(saved->environment[1].key,"ÄNAME")&&!strcmp(saved->environment[1].value,"原始值"));
    strcpy(unicode_key,"ÄNAME");strcpy(unicode_value,"原始值");
    strcpy(argument_text, "/std:c11"); strcpy(environment_text, "captured value");
    xtc_xir_target_free(snapshot); CHECK(stats(resources).live_bytes == baseline);
    XrXirTargetDependency swap = files[0]; files[0] = files[2]; files[2] = swap;
    snapshot = capture(resources); CHECK(!memcmp(first, xtc_xir_target_facts(snapshot)->identity, 32)); xtc_xir_target_free(snapshot);
    swap = files[0]; files[0] = files[2]; files[2] = swap;
    write_bytes(header_path, "abd"); snapshot = capture(resources); facts = xtc_xir_target_facts(snapshot);
    CHECK(memcmp(first, facts->identity, 32) && memcmp(sysroot, facts->sysroot_identity, 32));
    CHECK(!memcmp(provider, facts->provider_identity, 32)); xtc_xir_target_free(snapshot);
    write_bytes(header_path, "abc"); write_bytes(compiler_path, "compileX"); snapshot = capture(resources);
    facts = xtc_xir_target_facts(snapshot); CHECK(memcmp(provider, facts->provider_identity, 32));
    CHECK(!memcmp(sysroot, facts->sysroot_identity, 32)); xtc_xir_target_free(snapshot); write_bytes(compiler_path, "compiler");
    snapshot = capture(resources); xr_compile_resources_release(resources);
    CHECK(!strcmp(xtc_xir_target_command_facts(snapshot, 0)->argv[1], "/std:c11"));
    CHECK(!memcmp(first, xtc_xir_target_facts(snapshot)->identity, 32)); xtc_xir_target_free(snapshot);
    CHECK(!live && !live_bytes);
}
static void expected_failure(XrXirTargetSnapshotRequest *request, XrXirTargetStatus expected) {
    XrXirTargetSnapshot *output = NULL;
    uint64_t before = stats(request->resources).live_bytes;
    DWORD handles_before,handles_after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_before));
    CHECK(xtc_xir_target_snapshot_capture(request, &output) == expected);
    CHECK(!output && stats(request->resources).live_bytes == before);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_after)&&handles_before==handles_after);
}
static void invalid_inputs(void) {
    XrCompileResources *resources = ledger(&unlimited); XrXirTargetSnapshotRequest request = request_for(resources);
    XrXirTargetSnapshot *sentinel = (XrXirTargetSnapshot *)(uintptr_t)0x1234;
    CHECK(xtc_xir_target_snapshot_capture(&request, &sentinel) == XR_XIR_TARGET_INVALID && sentinel == (XrXirTargetSnapshot *)(uintptr_t)0x1234);
    const char *original = files[1].path; char path[4096];
    files[1].path = "relative.h"; expected_failure(&request, XR_XIR_TARGET_UNSUPPORTED);
    snprintf(path, sizeof(path), "%s/header.h.", directory); files[1].path = path; expected_failure(&request, XR_XIR_TARGET_INVALID);
    snprintf(path, sizeof(path), "%s/../header.h", directory); expected_failure(&request, XR_XIR_TARGET_INVALID);
    snprintf(path, sizeof(path), "%s/header.h:stream", directory); expected_failure(&request, XR_XIR_TARGET_INVALID);
    snprintf(path, sizeof(path), "%s/missing.h", directory); expected_failure(&request, XR_XIR_TARGET_UNRESOLVED);
    files[1].path = directory; expected_failure(&request, XR_XIR_TARGET_INVALID);
    files[1].path = compiler_path; expected_failure(&request, XR_XIR_TARGET_INVALID);
    snprintf(path, sizeof(path), "%s/COMPILER.BIN", directory); files[1].path = path;
    expected_failure(&request, XR_XIR_TARGET_INVALID);
    files[1].path = original; files[1].kind = 99; expected_failure(&request, XR_XIR_TARGET_INVALID);
    files[1].kind = XR_XIR_TARGET_HEADER; request.crt = 3; expected_failure(&request, XR_XIR_TARGET_UNSUPPORTED);
    request.crt = 2; request.provider = 2; expected_failure(&request, XR_XIR_TARGET_UNSUPPORTED);
    xr_compile_resources_release(resources); CHECK(!live);
}
static void command_contract(void) {
    XrCompileResources *resources = ledger(&unlimited);
    XrXirTargetSnapshotRequest request = request_for(resources);
    XrXirTargetCommandFacts original = command;
    XrXirTargetSnapshot *snapshot = capture(resources);
    XrXirTargetFacts first = *xtc_xir_target_facts(snapshot);
    const XrXirTargetCommandFacts *saved = xtc_xir_target_command_facts(snapshot, 0);
    CHECK(strcmp(saved->executable, saved->argv[0]) && !strcmp(saved->executable, compiler_path));
    CHECK(saved->timeout_ms == 3000 && saved->output_limit == 1048576 && saved->image_mode == 0);
    xtc_xir_target_free(snapshot);
    for (unsigned field = 0; field < 9; ++field) {
        command = original;
        const char *alternate_args[] = {"different argv0", argument_text, "source.c"};
        XrXirTargetEnvironment alternate_env[] = {{"EMPTY", ""}};
        switch (field) {
        case 0: command.executable = "Z:/not-required-to-exist/compiler.exe"; break;
        case 1: command.cwd = "Z:/not-required-to-exist"; break;
        case 2: command.timeout_ms++; break;
        case 3: command.output_limit++; break;
        case 4: command.image_mode = 1; break;
        case 5: command.argv = alternate_args; break;
        case 6: command.argc--; break;
        case 7: command.completion_policy = XR_PROC_COMPLETE_ROOT; break;
        default: command.environment = alternate_env; command.environment_count = 1; break;
        }
        snapshot = capture(resources);
        const XrXirTargetFacts *facts = xtc_xir_target_facts(snapshot);
        CHECK(memcmp(first.identity, facts->identity, 32));
        CHECK(!memcmp(first.provider_identity, facts->provider_identity, 32));
        CHECK(!memcmp(first.sysroot_identity, facts->sysroot_identity, 32));
        xtc_xir_target_free(snapshot);
    }
    command = original;
    const char *bad_paths[] = {NULL, "", "C", "C:", "C:relative", "/root", "relative", "1:/bad", "C:/\xc3\x28"};
    for (unsigned field = 0; field < 2; ++field) for (size_t i = 0; i < sizeof(bad_paths)/sizeof(bad_paths[0]); ++i) {
        command = original;
        if (field) command.cwd = bad_paths[i]; else command.executable = bad_paths[i];
        expected_failure(&request, XR_XIR_TARGET_INVALID);
    }
    command = original; command.timeout_ms = 0; expected_failure(&request, XR_XIR_TARGET_INVALID);
    command = original; command.output_limit = 0; expected_failure(&request, XR_XIR_TARGET_INVALID);
    command.output_limit = SIZE_MAX; expected_failure(&request, XR_XIR_TARGET_INVALID);
    command.output_limit = UINT64_MAX; expected_failure(&request, XR_XIR_TARGET_INVALID);
    command = original; command.image_mode = 2; expected_failure(&request, XR_XIR_TARGET_INVALID);
    command = original; command.image_mode = UINT32_MAX; expected_failure(&request, XR_XIR_TARGET_INVALID);
    command = original; command.completion_policy = 2; expected_failure(&request, XR_XIR_TARGET_INVALID);
    command = original; command.completion_policy = UINT32_MAX; expected_failure(&request, XR_XIR_TARGET_INVALID);
    char executable_text[] = "Z:/mutable-compiler.exe", cwd_text[] = "Z:/mutable-directory";
    command = original; command.executable = executable_text; command.cwd = cwd_text;
    command.timeout_ms = UINT32_MAX; command.output_limit = (uint64_t)SIZE_MAX - 1; command.image_mode = 1;
    snapshot = capture(resources); memset(executable_text, 'x', sizeof(executable_text)); memset(cwd_text, 'x', sizeof(cwd_text));
    memset(&command, 0xCD, sizeof(command)); xr_compile_resources_release(resources);
    saved = xtc_xir_target_command_facts(snapshot, 0);
    CHECK(!strcmp(saved->executable, "Z:/mutable-compiler.exe") && !strcmp(saved->cwd, "Z:/mutable-directory"));
    CHECK(saved->timeout_ms == UINT32_MAX && saved->output_limit == (uint64_t)SIZE_MAX - 1 && saved->image_mode == 1);
    xtc_xir_target_free(snapshot); command = original; CHECK(!live && !live_bytes);
    puts("command v2: all identity fields, independent executable, exact domains and lifetime PASS");
}
static void reparse_rejection(void) {
    char junction[4096], borrowed[4096];
    CHECK(snprintf(junction, sizeof(junction), "%s/junction", directory) > 0);
    CHECK(snprintf(borrowed, sizeof(borrowed), "%s/junction/header.h", directory) > 0);
    CHECK(CreateDirectoryA(junction, NULL));
    HANDLE handle = CreateFileA(junction, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE);
    struct JunctionData { DWORD tag; WORD length, reserved, substitute_offset, substitute_length,
        print_offset, print_length; WCHAR path[4096]; } data = {0};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT; memcpy(data.path, L"\\??\\", 4 * sizeof(WCHAR));
    int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, directory, -1, data.path + 4, 2048);
    CHECK(converted > 0);
    for (int i = 4; i < converted + 3; ++i) if (data.path[i] == '/') data.path[i] = '\\';
    size_t substitute = wcslen(data.path), print = substitute - 4;
    data.substitute_length = (WORD)(substitute * sizeof(WCHAR));
    data.print_offset = (WORD)((substitute + 1) * sizeof(WCHAR)); data.print_length = (WORD)(print * sizeof(WCHAR));
    memcpy(data.path + substitute + 1, data.path + 4, (print + 1) * sizeof(WCHAR));
    data.length = (WORD)(8 + (substitute + print + 2) * sizeof(WCHAR)); DWORD returned;
    CHECK(DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.length + 8, NULL, 0, &returned, NULL));
    CHECK(CloseHandle(handle));
    XrCompileResources *resources = ledger(&unlimited); XrXirTargetSnapshotRequest request = request_for(resources);
    const char *original = files[1].path; files[1].path = borrowed;
    expected_failure(&request, XR_XIR_TARGET_INVALID); files[1].path = original;
    xr_compile_resources_release(resources); CHECK(RemoveDirectoryA(junction)); CHECK(!live);
}
static void resource_limits(void) {
    XrCompileResources *resources = ledger(&unlimited); XrXirTargetSnapshot *snapshot = capture(resources);
    XrCompileResourceStats used = stats(resources); xtc_xir_target_free(snapshot); xr_compile_resources_release(resources);
    XrCompileResourceLimits exact = {used.allocated_bytes, used.peak_bytes, used.work};
    resources = ledger(&exact); snapshot = capture(resources); xtc_xir_target_free(snapshot); xr_compile_resources_release(resources);
    for (int field = 0; field < 3; ++field) {
        XrCompileResourceLimits limit = exact;
        if (field == 0) --limit.allocated_bytes; else if (field == 1) --limit.live_bytes; else --limit.work;
        resources = ledger(&limit); XrXirTargetSnapshotRequest request = request_for(resources);
        expected_failure(&request, XR_XIR_TARGET_BUDGET); xr_compile_resources_release(resources); CHECK(!live);
    }
}
static void allocation_failures(void) {
    attempts = 0; XrCompileResources *resources = ledger(&unlimited); XrXirTargetSnapshot *snapshot = capture(resources);
    size_t count = attempts; xtc_xir_target_free(snapshot); xr_compile_resources_release(resources);
    for (size_t i = 0; i < count; ++i) {
        DWORD handles_before, handles_after; CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles_before));
        attempts = 0; fail_at = i; resources = NULL;
        XrCompileResourceStatus status = xr_compile_resources_new(&unlimited, &resources);
        if (!i) CHECK(status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY && !resources);
        else {
            CHECK(status == XR_COMPILE_RESOURCE_OK); XrXirTargetSnapshotRequest request = request_for(resources);
            expected_failure(&request, XR_XIR_TARGET_OUT_OF_MEMORY); xr_compile_resources_release(resources);
        }
        fail_at = SIZE_MAX; CHECK(!live && !live_bytes);
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles_after) && handles_before == handles_after);
    }
    printf("allocation failure points: %zu\n", count);
}
#ifndef XIR_TARGET_PRODUCTION_TEST
static void io_failures(void) {
    XrCompileResources *resources = ledger(&unlimited); io_attempts = 0;
    environment_comparisons=path_comparisons=0;
    XrXirTargetSnapshot *snapshot = capture(resources); size_t count = io_attempts; xtc_xir_target_free(snapshot);
    size_t environment_sites=environment_comparisons,path_sites=path_comparisons;
    CHECK(environment_sites&&path_sites);
    for (size_t i = 0; i < count; ++i) {
        for (int mode = 0; mode < 3; ++mode) {
            io_attempts = 0; io_fail_at = i; injected_error = mode==2?ERROR_OUTOFMEMORY:mode?ERROR_NOT_ENOUGH_MEMORY:ERROR_READ_FAULT;
            XrXirTargetSnapshotRequest request = request_for(resources);
            expected_failure(&request, mode ? XR_XIR_TARGET_OUT_OF_MEMORY : XR_XIR_TARGET_IO);
            CHECK(io_attempts==i+1);
        }
    }
    io_fail_at = SIZE_MAX; xr_compile_resources_release(resources); CHECK(!live && !live_bytes);
    printf("typed I/O failure points: %zu, each IO/two OOM codes; environment ordinal %zu, path ordinal %zu\n",count,environment_sites,path_sites);
}
#endif
static void environment_contract(void) {
    XrCompileResources *resources=ledger(&unlimited);XrXirTargetSnapshotRequest request=request_for(resources);
    const XrXirTargetEnvironment *saved=command.environment;uint32_t count=command.environment_count;
    const char *invalid[]={"","=","bad=key","==C:","=C:=bad","\xc3\x28","\xed\xa0\x80"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        XrXirTargetEnvironment entry={invalid[i],"value"};command.environment=&entry;command.environment_count=1;
        expected_failure(&request,XR_XIR_TARGET_INVALID);
    }
    XrXirTargetEnvironment invalid_value={"normal","\xc0\x80"};command.environment=&invalid_value;
    expected_failure(&request,XR_XIR_TARGET_INVALID);
    XrXirTargetEnvironment duplicate[][2]={{{"ÄNAME","a"},{"äname","b"}},{{"=ExitCode","a"},{"=exitcode","b"}},{{"With Space","a"},{"with space","b"}}};
    command.environment_count=2;
    for(size_t i=0;i<sizeof(duplicate)/sizeof(duplicate[0]);++i) {
        command.environment=duplicate[i];expected_failure(&request,XR_XIR_TARGET_INVALID);
    }
    XrXirTargetEnvironment legal[]={{" ",""},{"= ","system fact"},{"\x01","control key"}};
    command.environment=legal;command.environment_count=3;XrXirTargetSnapshot *snapshot=capture(resources);
    CHECK(!strcmp(xtc_xir_target_command_facts(snapshot,0)->environment[1].key,"= "));
    xtc_xir_target_free(snapshot);command.environment=saved;command.environment_count=count;
#ifndef XIR_TARGET_PRODUCTION_TEST
    XrXirTargetSnapshot local={0};local.resources=resources;conversion_calls=environment_comparisons=0;
    CHECK(target_commands(&local,&request));
    CHECK(conversion_calls==count&&environment_comparisons==(size_t)count*(count-1)/2);
    while(local.memory){XtcXirMemory *next=local.memory->next;xr_compile_resources_free(local.memory);local.memory=next;}
#endif
    xr_compile_resources_release(resources);CHECK(!live&&!live_bytes);
#ifdef XIR_TARGET_PRODUCTION_TEST
    puts("production TU: Unicode ordinal duplicates, spaces/reserved keys and UTF8/equals rejection PASS");
#else
    puts("Unicode ordinal duplicates, spaces/reserved keys, UTF8/equals rejection, one conversion per key PASS");
#endif
}
static void work_failures(void) {
    XrCompileResources *resources=ledger(&unlimited);record_work=true;work_count=0;
    XrXirTargetSnapshot *snapshot=capture(resources);record_work=false;
    xtc_xir_target_free(snapshot);xr_compile_resources_release(resources);
    size_t count=work_count;
    for(size_t i=0;i<count;++i) {
        XrCompileResourceLimits limit=unlimited;limit.work=work_boundaries[i]-1;resources=NULL;
        XrCompileResourceStatus status=xr_compile_resources_new(&limit,&resources);
        if(status==XR_COMPILE_RESOURCE_OK) {
            XrXirTargetSnapshotRequest request=request_for(resources);expected_failure(&request,XR_XIR_TARGET_BUDGET);
            xr_compile_resources_release(resources);
        } else CHECK(status==XR_COMPILE_RESOURCE_BUDGET&&!resources);
        CHECK(!live&&!live_bytes);
    }
    printf("actual work cutoff points: %zu; physical/handle zero PASS\n",count);
}
#ifndef XIR_TARGET_PRODUCTION_TEST
static void fixed_operation_work(void) {
    XrCompileResources *resources = ledger(&unlimited);
    XrXirTargetSnapshot snapshot = {0}; snapshot.resources = resources;
    const char *args[] = {"a"};
    XrXirTargetCommandFacts description = {"Z:/x", "Z:/", args, 1, NULL, 0, 1, 1, 1, 0};
    XrXirTargetSnapshotRequest request = {0}; request.commands = &description; request.command_count = 1;
    uint64_t command_before = stats(resources).work;
    CHECK(target_commands(&snapshot, &request));
    /* Five real calloc payloads; three scans, UTF8 checks and copies; six
     * drive-prefix reads; 20 policy-copy bytes; the argv0 nonempty read. */
    CHECK(stats(resources).work - command_before == sizeof(description) + 2 * sizeof(char *) +
        5 * sizeof(XtcXirMemory) + 73);
    while (snapshot.memory) { XtcXirMemory *next = snapshot.memory->next;
        xr_compile_resources_free(snapshot.memory); snapshot.memory = next; }
    snapshot.commands = NULL;
    XrSHA256Context policy_hash; xr_sha256_init(&policy_hash);
    command_before = stats(resources).work;
    CHECK(target_u32(&snapshot, &policy_hash, 1) && target_u64(&snapshot, &policy_hash, 1) &&
        target_u32(&snapshot, &policy_hash, 1));
    CHECK(stats(resources).work - command_before == 32);
    wchar_t scratch[32768]; snapshot.scratch = scratch;
    HANDLE handle = CreateFileA(header_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE); XtcXirLock lock = {0}; lock.handle = handle;
    XrXirTargetFile file = {0};
    for (unsigned mode = 0; mode < 4; ++mode) {
        LARGE_INTEGER zero = {0}; CHECK(SetFilePointerEx(handle, zero, NULL, FILE_BEGIN));
        snapshot.status = XR_XIR_TARGET_OK; io_attempts = 0;
        io_fail_at = mode == 1 || mode == 2 ? 1 : SIZE_MAX;
        injected_error = mode == 2 ? ERROR_NOT_ENOUGH_MEMORY : ERROR_READ_FAULT;
        short_read = mode == 3; uint64_t before = stats(resources).work;
        CHECK(sysroot_hash(&snapshot, &lock, &file) == (mode == 0));
        CHECK(stats(resources).work - before == (mode == 0 ? 4 + 2 * 3 : 3 + 3));
        CHECK(snapshot.status == (mode == 0 ? XR_XIR_TARGET_OK :
            mode == 2 ? XR_XIR_TARGET_OUT_OF_MEMORY : XR_XIR_TARGET_IO));
    }
    CHECK(CloseHandle(handle)); short_read = false; io_fail_at = SIZE_MAX; injected_error = ERROR_READ_FAULT;
    snapshot.status = XR_XIR_TARGET_OK;
    memcpy(scratch, L"\\\\?\\C:\\a", 9 * sizeof(wchar_t)); snapshot.scratch_length = 8;
    uint64_t before = stats(resources).work;
    CHECK(!strcmp(sysroot_canonical_text(&snapshot), "C:/a"));
    CHECK(stats(resources).work - before == 2 * 4 * sizeof(wchar_t) + 3 + sizeof(XtcXirMemory) + 5 + 4);
    while (snapshot.memory) { XtcXirMemory *next = snapshot.memory->next;
        xr_compile_resources_free(snapshot.memory); snapshot.memory = next; }
    XrXirTargetFile sorted[2] = {{"b",0,0,{0}}, {"a",0,0,{0}}}; snapshot.files = sorted;
    before = stats(resources).work; CHECK(sysroot_sort_last(&snapshot, 1));
    CHECK(stats(resources).work - before == 2 + 3 * sizeof(XrXirTargetFile));
    CHECK(!strcmp(sorted[0].path, "a"));
    xr_compile_resources_release(resources); CHECK(!live && !live_bytes);
    puts("fixed work: command-copy formula, 32 policy framing bytes, UTF-16/read/hash/swap PASS");
}
static void bounded_scanning(void) {
    SYSTEM_INFO info; GetSystemInfo(&info); size_t page = info.dwPageSize;
    unsigned char *memory = VirtualAlloc(NULL, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); CHECK(memory);
    DWORD old; CHECK(VirtualProtect(memory + page, page, PAGE_NOACCESS, &old)); memory[page - 1] = 'x';
    XrCompileResourceLimits limit = {UINT64_MAX, UINT64_MAX, 2};
    XrCompileResources *resources = ledger(&limit); XrXirTargetSnapshot snapshot = {0}; snapshot.resources = resources;
    size_t length = 123;
    CHECK(!xtc_xir_target_length(&snapshot, (const char *)memory + page - 1, &length));
    CHECK(snapshot.status == XR_XIR_TARGET_BUDGET && length == 123 && stats(resources).work == 2);
    snapshot.status = XR_XIR_TARGET_OK; snapshot.facts.file_count = 1; snapshot.files = (XrXirTargetFile *)(memory + page);
    CHECK(!target_identity(&snapshot) && snapshot.status == XR_XIR_TARGET_BUDGET);
    xr_compile_resources_release(resources); CHECK(VirtualFree(memory, 0, MEM_RELEASE)); CHECK(!live);
}
#endif
static uint64_t diagnostic_number(const char *text, uint64_t maximum) {
    CHECK(text && *text); uint64_t value = 0;
    for (size_t i = 0; text[i]; ++i) {
        unsigned char c = (unsigned char)text[i]; CHECK(c >= '0' && c <= '9');
        uint64_t digit = (uint64_t)(c - '0'); CHECK(value <= (maximum - digit) / 10);
        value = value * 10 + digit;
    }
    return value;
}
/* Diagnostic transport only: the caller still owns complete dependency tracing
 * and replay auditing. This protocol never constructs executable authority. */
static int diagnostic_lease(const char *manifest) {
    static char storage[4 * 1024 * 1024];
    static XrXirTargetDependency dependencies[4096];
    static XrXirTargetCommandFacts commands[8];
    static const char *argv[8][256];
    static XrXirTargetEnvironment env[8][128];
    FILE *stream = fopen(manifest, "rb"); CHECK(stream);
    size_t size = fread(storage, 1, sizeof(storage) - 1, stream); CHECK(!ferror(stream) && feof(stream)); CHECK(!fclose(stream));
    storage[size] = 0;
    char *lines[16384]; uint32_t count = 0; lines[count++] = storage;
    for (size_t i = 0; i < size; ++i) {
        if (storage[i] != '\n') continue;
        storage[i] = 0; if (i && storage[i - 1] == '\r') storage[i - 1] = 0;
        CHECK(count < 16384); lines[count++] = storage + i + 1;
    }
    uint32_t at = 0; CHECK(count > 3);
    if (strcmp(lines[at++], "xray-target-command-v3")) { puts("STATUS 6"); return 2; }
    uint32_t provider = (uint32_t)strtoul(lines[at++], NULL, 10);
    uint32_t file_count = (uint32_t)strtoul(lines[at++], NULL, 10); CHECK(file_count && file_count <= 4096);
    for (uint32_t i = 0; i < file_count; ++i) {
        CHECK(at < count); char *tab = strchr(lines[at], '\t'); CHECK(tab); *tab = 0;
        dependencies[i] = (XrXirTargetDependency){tab + 1, (uint32_t)strtoul(lines[at++], NULL, 10)};
    }
    CHECK(at < count); uint32_t command_count = (uint32_t)strtoul(lines[at++], NULL, 10); CHECK(command_count && command_count <= 8);
    for (uint32_t i = 0; i < command_count; ++i) {
        CHECK(at + 2 < count); commands[i].executable = lines[at++]; commands[i].cwd = lines[at++];
        CHECK(at < count); commands[i].argc = (uint32_t)diagnostic_number(lines[at++], UINT32_MAX);
        CHECK(commands[i].argc && commands[i].argc < 256); commands[i].argv = argv[i];
        for (uint32_t a = 0; a < commands[i].argc; ++a) { CHECK(at < count); argv[i][a] = lines[at++]; }
        CHECK(at < count); commands[i].environment_count = (uint32_t)strtoul(lines[at++], NULL, 10);
        CHECK(commands[i].environment_count < 128); commands[i].environment = env[i];
        for (uint32_t e = 0; e < commands[i].environment_count; ++e) {
            CHECK(at + 1 < count); env[i][e] = (XrXirTargetEnvironment){lines[at], lines[at + 1]}; at += 2;
        }
        CHECK(at + 3 < count); commands[i].timeout_ms = (uint32_t)diagnostic_number(lines[at++], UINT32_MAX);
        commands[i].output_limit = diagnostic_number(lines[at++], UINT64_MAX);
        commands[i].image_mode = (uint32_t)diagnostic_number(lines[at++], UINT32_MAX);
        commands[i].completion_policy = (uint32_t)diagnostic_number(lines[at++], UINT32_MAX);
    }
    XrCompileResources *resources = ledger(&unlimited);
    XrXirTargetSnapshotRequest request = {resources, "x86_64-windows-msvc", provider, 2, 11, dependencies, file_count, commands, command_count, NULL};
    XrXirTargetSnapshot *snapshot = NULL; XrXirTargetStatus status = xtc_xir_target_snapshot_capture(&request, &snapshot);
    printf("STATUS %u\n", (unsigned)status); if (status != XR_XIR_TARGET_OK) { xr_compile_resources_release(resources); return 2; }
    memset(storage, 0, sizeof(storage)); xr_compile_resources_release(resources);
    const XrXirTargetFacts *facts = xtc_xir_target_facts(snapshot);
    const uint8_t *digests[] = {facts->provider_identity, facts->sysroot_identity, facts->identity};
    for (uint32_t d = 0; d < 3; ++d) { printf("IDENTITY %u ", d); for (uint32_t b = 0; b < 32; ++b) printf("%02x", digests[d][b]); puts(""); }
    for (uint32_t i = 0; i < facts->file_count; ++i) {
        const XrXirTargetFile *file = xtc_xir_target_file(snapshot, i);
        printf("FILE %u %llu ", file->kind, (unsigned long long)file->length);
        for (uint32_t b = 0; b < 32; ++b) printf("%02x", file->digest[b]); printf(" %s\n", file->path);
    }
    puts("READY"); fflush(stdout); CHECK(getchar() == '\n'); xtc_xir_target_free(snapshot);
    CHECK(!live && !live_bytes); puts("RELEASED"); return 0;
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--diagnostic-lease")) return diagnostic_lease(argv[2]);
    CHECK(argc == 1);
    setup(); identity_and_lifetime(); invalid_inputs(); command_contract(); environment_contract(); reparse_rejection(); resource_limits();
    allocation_failures();work_failures();
#ifndef XIR_TARGET_PRODUCTION_TEST
    io_failures();fixed_operation_work();bounded_scanning();
#endif
    teardown();
    puts("xir target snapshots: PASS"); return 0;
}
