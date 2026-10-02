/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_namespace.c - Real directory changes and asynchronous ownership
 */
#include "app/toolchain/xtc_xir_namespace.h"
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s error=%lu\n", __LINE__, #c, GetLastError()); exit(1); } } while (0)
#include "namespace_allocator.h"
#ifndef NAMESPACE_PRODUCTION
static bool io_active, hold_cancel, fail_cancel, fail_close, fail_wait, spoof_wait, unknown_result, wrong_identity;
static size_t io_attempts, io_fail_at = SIZE_MAX;
static unsigned identity_calls, false_wakeups;
static bool create_before_grant;
static wchar_t raced_directory[4096];
static DWORD io_error = ERROR_READ_FAULT;
static HANDLE owned_handles[4096];
static size_t handle_count;
static bool fail_io(void) {
    if (!io_active || io_attempts++ != io_fail_at) return false;
    SetLastError(io_error); return true;
}
static HANDLE record_handle(HANDLE handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) {
        for (size_t i = 0; i < 4096; ++i) if (!owned_handles[i]) {
            owned_handles[i] = handle; ++handle_count; return handle;
        }
        CHECK(false);
    }
    return handle;
}
static void forget_handle(HANDLE handle) {
    for (size_t i = 0; i < 4096; ++i) if (owned_handles[i] == handle) {
        owned_handles[i] = NULL; --handle_count; return;
    }
    CHECK(false);
}
static BOOL namespace_test_close(HANDLE handle) {
    if (fail_close) { fail_close = false; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    BOOL result = CloseHandle(handle); if (result) forget_handle(handle); return result;
}
static BOOL namespace_test_find_close(HANDLE handle) {
    BOOL result = FindClose(handle); if (result) forget_handle(handle); return result;
}
static BOOL namespace_test_cancel(HANDLE handle, LPOVERLAPPED over) {
    if (hold_cancel || fail_cancel) {
        SetLastError(hold_cancel ? ERROR_NOT_FOUND : ERROR_ACCESS_DENIED); fail_cancel = false; return FALSE;
    }
    return CancelIoEx(handle, over);
}
static BOOL namespace_test_identity(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID buffer, DWORD size) {
    if (fail_io()) return FALSE;
    BOOL result = GetFileInformationByHandleEx(handle, kind, buffer, size);
    if (result && wrong_identity && kind == FileIdInfo && ++identity_calls == 2)
        ((FILE_ID_INFO *)buffer)->FileId.Identifier[0] ^= 1;
    return result;
}
static BOOL namespace_test_ioctl(HANDLE handle,DWORD code,LPVOID input,DWORD input_size,
    LPVOID output,DWORD output_size,LPDWORD transferred,LPOVERLAPPED over) {
    if(fail_io())return FALSE;
    if(create_before_grant){create_before_grant=false;CHECK(CreateDirectoryW(raced_directory,NULL));}
    return DeviceIoControl(handle,code,input,input_size,output,output_size,transferred,over);
}
static DWORD namespace_test_wait(HANDLE handle, DWORD milliseconds) {
    if (fail_io()) return WAIT_FAILED;
    if (fail_wait) { fail_wait = false; SetLastError(ERROR_ACCESS_DENIED); return WAIT_FAILED; }
    if (spoof_wait) { ++false_wakeups; return WAIT_OBJECT_0; }
    return WaitForSingleObject(handle, milliseconds);
}
static BOOL namespace_test_result(HANDLE handle, LPOVERLAPPED over, LPDWORD bytes, BOOL wait) {
    if (fail_io()) return FALSE;
    if (unknown_result) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return GetOverlappedResult(handle, over, bytes, wait);
}
#define CreateFileW(...) (fail_io() ? INVALID_HANDLE_VALUE : record_handle(CreateFileW(__VA_ARGS__)))
#define CreateEventW(...) (fail_io() ? NULL : record_handle(CreateEventW(__VA_ARGS__)))
#define CloseHandle namespace_test_close
#define FindClose namespace_test_find_close
#define FindFirstFileW(...) (fail_io() ? INVALID_HANDLE_VALUE : record_handle(FindFirstFileW(__VA_ARGS__)))
#define FindNextFileW(...) (fail_io() ? FALSE : FindNextFileW(__VA_ARGS__))
#define GetFileInformationByHandle(...) (fail_io() ? FALSE : GetFileInformationByHandle(__VA_ARGS__))
#define GetFileInformationByHandleEx namespace_test_identity
#define GetFinalPathNameByHandleW(...) (fail_io() ? 0u : GetFinalPathNameByHandleW(__VA_ARGS__))
#define GetFileType(...) (fail_io() ? FILE_TYPE_UNKNOWN : GetFileType(__VA_ARGS__))
#define MultiByteToWideChar(...) (fail_io() ? 0 : MultiByteToWideChar(__VA_ARGS__))
#define WideCharToMultiByte(...) (fail_io() ? 0 : WideCharToMultiByte(__VA_ARGS__))
#define CompareStringOrdinal(...) (fail_io() ? 0 : CompareStringOrdinal(__VA_ARGS__))
#define GetVolumeInformationByHandleW(...) (fail_io() ? FALSE : GetVolumeInformationByHandleW(__VA_ARGS__))
#define DeviceIoControl namespace_test_ioctl
#define CancelIoEx namespace_test_cancel
#define WaitForSingleObject namespace_test_wait
#define GetOverlappedResult namespace_test_result
#include "base/xio_policy.c"
#include "base/xfileio.c"
#include "os/win/dir_win.c"
#include "app/toolchain/xtc_xir_target.c"
#include "app/toolchain/xtc_xir_sysroot.c"
#include "app/toolchain/xtc_xir_images.c"
#include "app/toolchain/xtc_xir_namespace.c"
#undef CreateFileW
#undef CreateEventW
#undef CloseHandle
#undef FindClose
#undef FindFirstFileW
#undef FindNextFileW
#undef GetFileInformationByHandle
#undef GetFileInformationByHandleEx
#undef GetFinalPathNameByHandleW
#undef GetFileType
#undef MultiByteToWideChar
#undef WideCharToMultiByte
#undef CompareStringOrdinal
#undef GetVolumeInformationByHandleW
#undef DeviceIoControl
#undef CancelIoEx
#undef WaitForSingleObject
#undef GetOverlappedResult
#endif
static const XrXirNamespaceLimits shape = {16, 64, 8, 4096};
static char root_path[4096];
static DWORD handles(void) { DWORD n = 0; CHECK(GetProcessHandleCount(GetCurrentProcess(), &n)); return n; }
static void physical_zero(void) {
    CHECK(!runtime_live && !runtime_bytes);
#ifndef NAMESPACE_PRODUCTION
    CHECK(!handle_count);
#endif
}
static void path(char *out, const char *base, const char *leaf) { CHECK(snprintf(out, 4096, "%s/%s", base, leaf) > 0); }
static void wide_path(wchar_t *out, const char *text) { CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, 4096)); }
static void touch(const char *name) {
    wchar_t wide[4096]; wide_path(wide, name);
    HANDLE file = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE); CHECK(CloseHandle(file));
}
static void remove_file(const char *name) { wchar_t wide[4096]; wide_path(wide, name); CHECK(DeleteFileW(wide)); }
static void prepare_fixture_directory(const char *directory) {
    char pattern[4096];wchar_t wide[4096];path(pattern,directory,"*");wide_path(wide,pattern);
    WIN32_FIND_DATAW entry;HANDLE iterator=FindFirstFileW(wide,&entry);CHECK(iterator!=INVALID_HANDLE_VALUE);
    do {
        if(!(entry.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||!wcscmp(entry.cFileName,L".")||!wcscmp(entry.cFileName,L".."))continue;
        char leaf[1024],child[4096];CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,entry.cFileName,-1,leaf,sizeof(leaf),NULL,NULL));
        path(child,directory,leaf);prepare_fixture_directory(child);
    }while(FindNextFileW(iterator,&entry));
    CHECK(GetLastError()==ERROR_NO_MORE_FILES);CHECK(FindClose(iterator));
}
static void close_owner(XrXirNamespace **owner) {
    for (unsigned i = 0; i < 100 && *owner; ++i) {
        XrXirNamespaceStatus status = xtc_xir_namespace_close(owner, 10);
        CHECK(status == XR_XIR_NAMESPACE_OK || status == XR_XIR_NAMESPACE_PENDING);
    }
    CHECK(!*owner);
}
static XrXirNamespace *guard(XrCompileResources *r, const char *name, XrXirNamespaceScope scope) {
    static unsigned guard_number;
    ++guard_number;
    XrXirNamespaceRoot root = {name, scope};
    XrXirNamespaceRequest request = {&root, 1, shape}; XrXirNamespace *owner = NULL;
    CHECK(xtc_xir_namespace_new(r, &request, &owner) == XR_XIR_NAMESPACE_OK);
    XrXirNamespaceStatus armed = xtc_xir_namespace_arm(owner);
    if (armed != XR_XIR_NAMESPACE_OK) fprintf(stderr,"arm number=%u scope=%u status=%d os=%lu dirs=%u path=%s\n",guard_number,scope,armed,
        (unsigned long)xtc_xir_namespace_diagnostic(owner)->os_error,xtc_xir_namespace_facts(owner)->directory_count,name);
    CHECK(armed == XR_XIR_NAMESPACE_OK);
    CHECK(xtc_xir_namespace_check(owner) == XR_XIR_NAMESPACE_OK); return owner;
}
static XrXirNamespaceDiagnostic directory_failure(XrXirNamespace *owner, XrXirNamespaceFailureKind kind) {
    XrCompileResourceStats before=sdk_stats(xtc_xir_namespace_resources(owner));
    XrXirNamespaceDiagnostic saved=*xtc_xir_namespace_diagnostic(owner);
    CHECK(saved.status==XR_XIR_NAMESPACE_BROKEN&&saved.kind==kind);
    const XrXirNamespaceDirectoryFacts *directory=xtc_xir_namespace_directory(owner,saved.directory_index);
    CHECK(directory&&directory->path&&directory->volume==saved.volume&&!memcmp(directory->file_id,saved.file_id,16));
    CHECK(xtc_xir_namespace_check(owner)==XR_XIR_NAMESPACE_BROKEN);
    CHECK(!memcmp(&saved,xtc_xir_namespace_diagnostic(owner),sizeof(saved)));
    CHECK(sdk_stats(xtc_xir_namespace_resources(owner)).work==before.work);
    return saved;
}
static void semantics(void) {
    char tree[4096],nested[4096],file[4096],missing[4096],parent[4096],renamed[4096];
    path(tree,root_path,"tree"); path(nested,tree,"child/deep"); path(file,nested,"toggle.h");
    XrCompileResources *r = sdk_ledger(&sdk_unlimited);
    XrXirNamespace *owner = guard(r, tree, XR_XIR_NAMESPACE_TREE);
    CHECK(xtc_xir_namespace_facts(owner)->directory_count == 3);
    touch(file); remove_file(file);
    CHECK(xtc_xir_namespace_check(owner) == XR_XIR_NAMESPACE_BROKEN);
    XrXirNamespaceDiagnostic known=directory_failure(owner,XR_XIR_NAMESPACE_OPLOCK_COMPLETED);
#ifndef NAMESPACE_PRODUCTION
    fail_close=true;
    CHECK(xtc_xir_namespace_close(&owner,100)==XR_XIR_NAMESPACE_PENDING&&owner);
    CHECK(!memcmp(&known,xtc_xir_namespace_diagnostic(owner),sizeof(known)));
#endif
    CHECK(known.kind==XR_XIR_NAMESPACE_OPLOCK_COMPLETED);
    CHECK(xtc_xir_namespace_check(owner) == XR_XIR_NAMESPACE_BROKEN);
    CHECK(!xtc_xir_namespace_facts(owner)->armed); close_owner(&owner);
    path(tree,root_path,"direct");path(nested,tree,"child/deep");path(file,nested,"toggle.h");
    owner = guard(r, tree, XR_XIR_NAMESPACE_DIRECTORY);
    touch(file); remove_file(file);
    CHECK(xtc_xir_namespace_check(owner) == XR_XIR_NAMESPACE_OK);
    path(file, tree, "direct.h"); touch(file); remove_file(file);
    CHECK(xtc_xir_namespace_check(owner) == XR_XIR_NAMESPACE_BROKEN); close_owner(&owner);
    path(tree,root_path,"readonly");
    owner = guard(r, tree, XR_XIR_NAMESPACE_TREE);
    uint64_t prior_work=sdk_stats(r).work;
    CHECK(xtc_xir_namespace_check(owner)==XR_XIR_NAMESPACE_OK);
    CHECK(sdk_stats(r).work-prior_work==3);
    wchar_t pattern[4096]; char match[4096]; path(match,tree,"*"); wide_path(pattern,match);
    WIN32_FIND_DATAW entry; HANDLE iterator = FindFirstFileW(pattern,&entry); CHECK(iterator!=INVALID_HANDLE_VALUE);
    while(FindNextFileW(iterator,&entry)) {} CHECK(GetLastError()==ERROR_NO_MORE_FILES); CHECK(FindClose(iterator));
    path(file,tree,"existing.h"); wide_path(pattern,file);
    HANDLE read = CreateFileW(pattern,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    CHECK(read!=INVALID_HANDLE_VALUE && CloseHandle(read));
    CHECK(xtc_xir_namespace_check(owner)==XR_XIR_NAMESPACE_OK);
    path(renamed,root_path,"renamed"); wchar_t from[4096],to[4096]; wide_path(from,tree);wide_path(to,renamed);
    CHECK(!MoveFileW(from,to)); CHECK(xtc_xir_namespace_check(owner)==XR_XIR_NAMESPACE_OK); close_owner(&owner);
    path(parent,root_path,"missing"); path(missing,parent,"absent/further");
    owner=guard(r,missing,XR_XIR_NAMESPACE_TREE);
    const XrXirNamespaceRootFacts *facts=xtc_xir_namespace_root(owner,0);
    CHECK(facts->initially_missing && facts->scope==XR_XIR_NAMESPACE_TREE);
    CHECK(xtc_xir_namespace_facts(owner)->directory_count==1&&!xtc_xir_namespace_directory(owner,0)->recursive);
    path(file,parent,"absent");wide_path(pattern,file);CHECK(CreateDirectoryW(pattern,NULL));
    CHECK(xtc_xir_namespace_check(owner)==XR_XIR_NAMESPACE_BROKEN);close_owner(&owner);CHECK(RemoveDirectoryW(pattern));
    path(tree,root_path,"overlap");
    XrXirNamespaceRoot roots[]={{tree,XR_XIR_NAMESPACE_DIRECTORY},{tree,XR_XIR_NAMESPACE_TREE}};
    XrXirNamespaceRequest request={roots,2,shape};CHECK(xtc_xir_namespace_new(r,&request,&owner)==XR_XIR_NAMESPACE_OK);
    CHECK(xtc_xir_namespace_arm(owner)==XR_XIR_NAMESPACE_OK&&xtc_xir_namespace_facts(owner)->directory_count==3);
    close_owner(&owner);
    path(nested,tree,"child");roots[0]=(XrXirNamespaceRoot){nested,XR_XIR_NAMESPACE_DIRECTORY};
    CHECK(xtc_xir_namespace_new(r,&request,&owner)==XR_XIR_NAMESPACE_OK);
    memset(nested,'?',strlen(nested));roots[1].scope=XR_XIR_NAMESPACE_DIRECTORY;
    CHECK(xtc_xir_namespace_arm(owner)==XR_XIR_NAMESPACE_OK&&xtc_xir_namespace_facts(owner)->directory_count==3);
    xr_compile_resources_release(r);
    CHECK(xtc_xir_namespace_resources(owner)==r&&xtc_xir_namespace_check(owner)==XR_XIR_NAMESPACE_OK);
    CHECK(xtc_xir_namespace_directory(owner,2)->path&&xtc_xir_namespace_root(owner,1)->requested_path);
    close_owner(&owner);physical_zero();
    puts("TREE nested transient changes; DIRECTORY direct-only; read-only; missing root; overlapping scopes; dead producer PASS");
}
static XrXirNamespaceStatus transaction_on(XrCompileResources *r, XrCompileResourceStats *stats) {
    XrXirNamespace *owner=NULL;
    char tree[4096];path(tree,root_path,"matrix");XrXirNamespaceRoot root={tree,XR_XIR_NAMESPACE_TREE};
    XrXirNamespaceRequest request={&root,1,shape};
#ifndef NAMESPACE_PRODUCTION
    io_active=true;
#endif
    XrXirNamespaceStatus status=xtc_xir_namespace_new(r,&request,&owner);
    if(status==XR_XIR_NAMESPACE_OK)status=xtc_xir_namespace_arm(owner);
    if(status==XR_XIR_NAMESPACE_OK)status=xtc_xir_namespace_check(owner);
#ifndef NAMESPACE_PRODUCTION
    io_active=false;
#endif
    *stats=sdk_stats(r);close_owner(&owner);return status;
}
static XrXirNamespaceStatus transaction(const XrCompileResourceLimits *limits, XrCompileResourceStats *stats) {
    memset(stats,0,sizeof(*stats));XrCompileResources *r=NULL;
    XrCompileResourceStatus made=xr_compile_resources_new(limits,&r);
    if(made!=XR_COMPILE_RESOURCE_OK)return made==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_NAMESPACE_BUDGET:XR_XIR_NAMESPACE_OUT_OF_MEMORY;
    XrXirNamespaceStatus status=transaction_on(r,stats);
    xr_compile_resources_release(r);physical_zero();return status;
}
static void unprepared_interval(void) {
    char name[4096];wchar_t native[4096];
    const char *parts[]={"cold","cold/child","cold/child/deep"};
    for(size_t i=0;i<3;++i){path(name,root_path,parts[i]);wide_path(native,name);CHECK(CreateDirectoryW(native,NULL));}
    path(name,root_path,"cold");XrXirNamespaceRoot root={name,XR_XIR_NAMESPACE_TREE};
    XrXirNamespaceRequest request={&root,1,shape};XrCompileResources *r=sdk_ledger(&sdk_unlimited);
    XrXirNamespace *owner=NULL;CHECK(xtc_xir_namespace_new(r,&request,&owner)==XR_XIR_NAMESPACE_OK);
    XrXirNamespaceStatus status=xtc_xir_namespace_arm(owner);
    CHECK(status==XR_XIR_NAMESPACE_OK||status==XR_XIR_NAMESPACE_BROKEN);
    XrXirNamespaceStatus checked=xtc_xir_namespace_check(owner);
    CHECK(checked==XR_XIR_NAMESPACE_BROKEN||(status==XR_XIR_NAMESPACE_OK&&checked==XR_XIR_NAMESPACE_OK));
    printf("fresh unprepared TREE observed %s; no internal retry or rearm\n",checked==XR_XIR_NAMESPACE_BROKEN?"BROKEN":"OK");
    close_owner(&owner);xr_compile_resources_release(r);physical_zero();
}
static void shape_boundaries(void) {
    char name[4096];path(name,root_path,"matrix");
    XrXirNamespaceRoot roots[]={{name,XR_XIR_NAMESPACE_TREE},{name,XR_XIR_NAMESPACE_DIRECTORY}};
    XrXirNamespaceRequest request={roots,2,{2,2,1,(uint32_t)(strlen(name)+strlen("/child"))}};
    for(unsigned axis=0;axis<4;++axis)for(unsigned below=0;below<2;++below){
        XrCompileResources *r=sdk_ledger(&sdk_unlimited);XrXirNamespace *owner=NULL;
        XrXirNamespaceRequest current=request;
        if(axis==0)current.limits.roots-=below;
        if(axis==1)current.limits.directories-=below;
        if(axis==2)current.limits.depth-=below;
        if(axis==3)current.limits.path_bytes-=below;
#ifndef NAMESPACE_PRODUCTION
        io_attempts=0;io_active=true;
#endif
        XrXirNamespaceStatus status=xtc_xir_namespace_new(r,&current,&owner);
#ifndef NAMESPACE_PRODUCTION
        io_active=false;CHECK(io_attempts==0);
#endif
        if(status==XR_XIR_NAMESPACE_OK)status=xtc_xir_namespace_arm(owner);
        CHECK(status==(below?XR_XIR_NAMESPACE_BUDGET:XR_XIR_NAMESPACE_OK));
        if(!below)CHECK(xtc_xir_namespace_facts(owner)->directory_count==2&&xtc_xir_namespace_check(owner)==XR_XIR_NAMESPACE_OK);
        close_owner(&owner);xr_compile_resources_release(r);physical_zero();
    }
    puts("root/directory/depth/path exact and minus1; new performs no I/O PASS");
}
static void arguments(void) {
    XrCompileResources *r=sdk_ledger(&sdk_unlimited);XrXirNamespace *owner=NULL;
    char tree[4096];path(tree,root_path,"tree");XrXirNamespaceRoot root={tree,XR_XIR_NAMESPACE_TREE};
    XrXirNamespaceRequest request={&root,1,shape};
    CHECK(xtc_xir_namespace_new(NULL,&request,&owner)==XR_XIR_NAMESPACE_INVALID&&!owner);
    XrXirNamespace *canary=(void *)(uintptr_t)0x1234;
    CHECK(xtc_xir_namespace_new(r,&request,&canary)==XR_XIR_NAMESPACE_INVALID&&canary==(void *)(uintptr_t)0x1234);
    request.limits.path_bytes=1;CHECK(xtc_xir_namespace_new(r,&request,&owner)==XR_XIR_NAMESPACE_BUDGET&&!owner);
    request.limits=shape;request.limits.depth=0;
    CHECK(xtc_xir_namespace_new(r,&request,&owner)==XR_XIR_NAMESPACE_OK);
    CHECK(xtc_xir_namespace_check(owner)==XR_XIR_NAMESPACE_INVALID);
    CHECK(xtc_xir_namespace_arm(owner)==XR_XIR_NAMESPACE_BUDGET);close_owner(&owner);
    request.limits=shape;request.limits.directories=1;
    CHECK(xtc_xir_namespace_new(r,&request,&owner)==XR_XIR_NAMESPACE_OK);
    CHECK(xtc_xir_namespace_arm(owner)==XR_XIR_NAMESPACE_BUDGET);close_owner(&owner);
    root.path="relative";request.limits=shape;
    CHECK(xtc_xir_namespace_new(r,&request,&owner)==XR_XIR_NAMESPACE_OK);
    CHECK(xtc_xir_namespace_arm(owner)==XR_XIR_NAMESPACE_UNSUPPORTED);close_owner(&owner);
    CHECK(xtc_xir_namespace_close(&owner,UINT32_MAX)==XR_XIR_NAMESPACE_INVALID);
    CHECK(xtc_xir_namespace_close(&owner,0)==XR_XIR_NAMESPACE_OK);xr_compile_resources_release(r);physical_zero();
}
#ifndef NAMESPACE_PRODUCTION
static void pending_lifetime(void) {
    char tree[4096];path(tree,root_path,"matrix");XrCompileResources *r=sdk_ledger(&sdk_unlimited);
    XrXirNamespace *owner=guard(r,tree,XR_XIR_NAMESPACE_DIRECTORY),*original=owner;
    xr_compile_resources_release(r);size_t live=runtime_live,bytes=runtime_bytes,owned=handle_count;
    XrCompileResourceStats before=sdk_stats(r);hold_cancel=true;
    CHECK(xtc_xir_namespace_close(&owner,0)==XR_XIR_NAMESPACE_PENDING&&owner==original);
    CHECK(runtime_live==live&&runtime_bytes==bytes&&handle_count==owned);
    CHECK(xtc_xir_namespace_phase(owner)==XR_XIR_NAMESPACE_CLOSING&&!xtc_xir_namespace_facts(owner)->armed);
    CHECK(sdk_stats(r).work==before.work);
    spoof_wait=true;unknown_result=true;false_wakeups=0;
    CHECK(xtc_xir_namespace_close(&owner,100)==XR_XIR_NAMESPACE_PENDING&&owner==original&&false_wakeups==1);
    CHECK(runtime_live==live&&runtime_bytes==bytes&&handle_count==owned);
    CHECK(xtc_xir_namespace_diagnostic(owner)->status==XR_XIR_NAMESPACE_IO);
    hold_cancel=false;spoof_wait=false;unknown_result=false;close_owner(&owner);physical_zero();
    r=sdk_ledger(&sdk_unlimited);owner=guard(r,tree,XR_XIR_NAMESPACE_DIRECTORY);original=owner;
    live=runtime_live;bytes=runtime_bytes;owned=handle_count;before=sdk_stats(r);
    hold_cancel=true;fail_wait=true;
    CHECK(xtc_xir_namespace_close(&owner,100)==XR_XIR_NAMESPACE_PENDING&&owner==original&&!fail_wait);
    CHECK(runtime_live==live&&runtime_bytes==bytes&&handle_count==owned&&sdk_stats(r).work==before.work);
    CHECK(xtc_xir_namespace_diagnostic(owner)->status==XR_XIR_NAMESPACE_IO&&
        xtc_xir_namespace_diagnostic(owner)->os_error==ERROR_ACCESS_DENIED);
    hold_cancel=false;close_owner(&owner);xr_compile_resources_release(r);physical_zero();
    r=sdk_ledger(&sdk_unlimited);owner=guard(r,tree,XR_XIR_NAMESPACE_DIRECTORY);fail_cancel=true;
    CHECK(xtc_xir_namespace_close(&owner,0)==XR_XIR_NAMESPACE_PENDING);
    CHECK(xtc_xir_namespace_diagnostic(owner)->status==XR_XIR_NAMESPACE_IO);close_owner(&owner);xr_compile_resources_release(r);physical_zero();
    r=sdk_ledger(&sdk_unlimited);owner=guard(r,tree,XR_XIR_NAMESPACE_DIRECTORY);fail_close=true;
    CHECK(xtc_xir_namespace_close(&owner,100)==XR_XIR_NAMESPACE_PENDING&&owner);
    CHECK(xtc_xir_namespace_diagnostic(owner)->status==XR_XIR_NAMESPACE_IO);close_owner(&owner);xr_compile_resources_release(r);physical_zero();
    for (unsigned mode=0;mode<2;++mode) {
        r=sdk_ledger(&sdk_unlimited);owner=guard(r,tree,XR_XIR_NAMESPACE_DIRECTORY);original=owner;
        char changed[4096];path(changed,tree,"completed-error.h");touch(changed);
        CHECK(WaitForSingleObject(owner->directories[0]->overlapped.hEvent,1000)==WAIT_OBJECT_0);
        CHECK(HasOverlappedIoCompleted(&owner->directories[0]->overlapped));
        before=sdk_stats(r);xr_compile_resources_release(r);
        fail_cancel=mode==0;unknown_result=mode==1;
        CHECK(xtc_xir_namespace_close(&owner,0)==XR_XIR_NAMESPACE_PENDING&&owner==original);
        CHECK(xtc_xir_namespace_diagnostic(owner)->kind==XR_XIR_NAMESPACE_FAILURE_UNKNOWN);
        CHECK(xtc_xir_namespace_diagnostic(owner)->status==XR_XIR_NAMESPACE_IO&&
            xtc_xir_namespace_diagnostic(owner)->os_error==ERROR_ACCESS_DENIED);
        CHECK(sdk_stats(r).work==before.work);
        unknown_result=false;close_owner(&owner);remove_file(changed);physical_zero();
    }
    puts("completed requests retain cancellation and observation errors until a later close PASS");
    puts("real pending request survives cancellation/false wake/unknown completion/WAIT_FAILED/cleanup failures; later bounded close frees all storage PASS");
}
static void identity_and_missing_race(void) {
    XrCompileResources *r=sdk_ledger(&sdk_unlimited);XrXirNamespace *owner=NULL;
    char name[4096],missing[4096];path(name,root_path,"matrix");
    XrXirNamespaceRoot root={name,XR_XIR_NAMESPACE_DIRECTORY};XrXirNamespaceRequest request={&root,1,shape};
    CHECK(xtc_xir_namespace_new(r,&request,&owner)==XR_XIR_NAMESPACE_OK);wrong_identity=true;identity_calls=0;
    CHECK(xtc_xir_namespace_arm(owner)==XR_XIR_NAMESPACE_BROKEN);wrong_identity=false;
    (void)directory_failure(owner,XR_XIR_NAMESPACE_DIRECTORY_IDENTITY);
    CHECK(!xtc_xir_namespace_facts(owner)->armed);close_owner(&owner);
    path(name,root_path,"race");path(missing,name,"appeared/still-missing");root.path=missing;
    path(name,root_path,"race/appeared");wide_path(raced_directory,name);
    CHECK(xtc_xir_namespace_new(r,&request,&owner)==XR_XIR_NAMESPACE_OK);create_before_grant=true;
    CHECK(xtc_xir_namespace_arm(owner)==XR_XIR_NAMESPACE_BROKEN&&!create_before_grant);
    CHECK(!xtc_xir_namespace_facts(owner)->armed);close_owner(&owner);CHECK(RemoveDirectoryW(raced_directory));
    xr_compile_resources_release(r);physical_zero();puts("complete directory ID mismatch and first missing component appearing before arm both reject PASS");
}
#endif
int main(int argc,char **argv) {
    CHECK(argc==2&&strlen(argv[1])<sizeof(root_path));strcpy(root_path,argv[1]);DWORD initial=handles();
    prepare_fixture_directory(root_path);puts("native Find enumeration preparation completed before all guarded intervals");
    unprepared_interval();semantics();arguments();shape_boundaries();CHECK(handles()==initial);
#ifndef NAMESPACE_PRODUCTION
    pending_lifetime();identity_and_missing_race();CHECK(handles()==initial);
#endif
    XrCompileResourceStats baseline,current;runtime_attempts=0;
#ifndef NAMESPACE_PRODUCTION
    io_attempts=0;
#endif
    record_work=true;record_physical=true;CHECK(transaction(&sdk_unlimited,&baseline)==XR_XIR_NAMESPACE_OK);record_work=false;record_physical=false;
    CHECK(physical_total==baseline.allocated_bytes&&physical_peak==baseline.peak_bytes);
    size_t allocations=runtime_attempts;
#ifndef NAMESPACE_PRODUCTION
    size_t calls=io_attempts;
#endif
    for(size_t i=0;i<allocations;++i) {
        runtime_attempts=0;runtime_fail_at=i;CHECK(transaction(&sdk_unlimited,&current)==XR_XIR_NAMESPACE_OUT_OF_MEMORY);CHECK(handles()==initial);
    }
    runtime_fail_at=SIZE_MAX;
    for(size_t i=0;i<work_count;++i) {
        XrCompileResourceLimits limits=sdk_unlimited;limits.work=work_edges[i]-1;
        CHECK(transaction(&limits,&current)==XR_XIR_NAMESPACE_BUDGET);CHECK(handles()==initial);
    }
    for(unsigned axis=0;axis<3;++axis)for(unsigned below=0;below<2;++below) {
        XrCompileResourceLimits limits=sdk_unlimited;
        if(axis==0)limits.allocated_bytes=baseline.allocated_bytes-below;
        if(axis==1)limits.live_bytes=baseline.peak_bytes-below;
        if(axis==2)limits.work=baseline.work-below;
        CHECK(transaction(&limits,&current)==(below?XR_XIR_NAMESPACE_BUDGET:XR_XIR_NAMESPACE_OK));CHECK(handles()==initial);
    }
    XrCompileResourceLimits cumulative=sdk_unlimited;cumulative.work=2*baseline.work-2;
    XrCompileResources *shared=sdk_ledger(&cumulative);
    CHECK(transaction_on(shared,&current)==XR_XIR_NAMESPACE_OK&&current.work==baseline.work);
    CHECK(transaction_on(shared,&current)==XR_XIR_NAMESPACE_BUDGET);
    xr_compile_resources_release(shared);physical_zero();CHECK(handles()==initial);
#ifndef NAMESPACE_PRODUCTION
    DWORD errors[]={ERROR_READ_FAULT,ERROR_NOT_ENOUGH_MEMORY,ERROR_OUTOFMEMORY};
    for(size_t e=0;e<3;++e)for(size_t i=0;i<calls;++i) {
        io_attempts=0;io_fail_at=i;io_error=errors[e];
        CHECK(transaction(&sdk_unlimited,&current)==(e?XR_XIR_NAMESPACE_OUT_OF_MEMORY:XR_XIR_NAMESPACE_IO));CHECK(handles()==initial);
    }
    io_fail_at=SIZE_MAX;printf("Win32 %zu points x 3 statuses\n",calls);
#endif
    CHECK(handles()==initial);physical_zero();
    printf("OOM %zu; work %zu edges; exact/minus1 three axes; physical heap and handles zero PASS\n",allocations,work_count);
    return 0;
}
