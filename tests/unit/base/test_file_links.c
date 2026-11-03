/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_file_links.c - Actual link contents, following paths and close owners
 *
 * KEY CONCEPT:
 *   Filesystem-visible results are checked independently of allocation order.
 *   A refused symlink remains a real failure, never a junction substitute.
 */
#include "os/os_file_links.h"
#include <stdio.h>
#if defined(XR_OS_WINDOWS)
#include "base/xwindows_utf8.h"
static BOOL WINAPI links_directory(LPCWSTR path,LPSECURITY_ATTRIBUTES security) {
    BOOL made=CreateDirectoryW(path,security);
    if (!made) {
        DWORD error=GetLastError();
        fwprintf(stderr,L"file links setup CreateDirectoryW error=%lu path=%ls\n",(unsigned long)error,path);
        SetLastError(error);
    }
    return made;
}
#define CreateDirectoryW links_directory
#endif
#define main metadata_fixture_main
#include "test_file_metadata.c"
#undef main
#if defined(XR_OS_WINDOWS)
#undef CreateDirectoryW
#endif

static XrOsPathSpan links_span(const char *text) {return (XrOsPathSpan){text,strlen(text)};}
static void links_text_free(const XrOsIoPolicy *policy,XrOsFileLinkText *text) {
    if (text->text) policy->free(policy->context,text->text);
    *text=(XrOsFileLinkText){0};
}
#if defined(XR_OS_WINDOWS)
static DWORD links_create_error;
static bool links_cancel_after_create;
static BOOLEAN WINAPI links_create(LPCWSTR name,LPCWSTR target,DWORD flags) {
    BOOLEAN made=CreateSymbolicLinkW(name,target,flags);
    links_create_error=made?ERROR_SUCCESS:GetLastError();
    if(made&&links_cancel_after_create)metadata_memory.cancelled=true;
    return made;
}
#define CreateSymbolicLinkW links_create
#define CreateFileW observed_create
#include "os/win/file_links_win.c"
#undef CreateFileW
#undef CreateSymbolicLinkW

typedef struct LinksPaths {
    wchar_t file[32768],directory[32768],dangling[32768],absolute[32768];
    wchar_t file_locator[32768],directory_locator[32768],dangling_locator[32768],absolute_locator[32768];
    char file_utf8[65536],directory_utf8[65536],dangling_utf8[65536],absolute_utf8[65536];
    bool file_created,directory_created,dangling_created,absolute_created;
} LinksPaths;
static LinksPaths links_paths;
static int links_name(wchar_t *wide,wchar_t *locator,char *utf8,const wchar_t *tail) {
    CHECK(wide_join(wide,paths.child,tail));
    int count=swprintf(locator,32768,L"\\\\?\\%ls",wide);CHECK(count>0&&count<32768);
    CHECK(xr_win_utf16_to_utf8(wide,wcslen(wide),utf8,65536));return 1;
}
static int links_setup(void) {
    wchar_t name[220];
    for (size_t n=0;n<185;++n) name[n]=L'l';
    memcpy(name+185,L"-\x4e2d-\xd83d\xde00.sym",10*sizeof(wchar_t));
    CHECK(links_name(links_paths.file,links_paths.file_locator,links_paths.file_utf8,name));
    CHECK(wcslen(links_paths.file)>260);
    CHECK(links_name(links_paths.directory,links_paths.directory_locator,links_paths.directory_utf8,L"directory-link"));
    CHECK(links_name(links_paths.dangling,links_paths.dangling_locator,links_paths.dangling_utf8,L"dangling-link"));
    CHECK(links_name(links_paths.absolute,links_paths.absolute_locator,links_paths.absolute_utf8,L"outside-link"));
    return 1;
}
static int links_create_checked(const XrOsIoPolicy *policy,const char *target,const char *name,bool *created) {
    bool changed=false;XrOsIoStatus status=xr_os_io_path_symlink(policy,links_span(target),links_span(name),test_current,&changed);
    if (status==XR_OS_IO_OK&&changed) *created=true;
    if (status!=XR_OS_IO_OK||!changed)
        fprintf(stderr,"real symlink creation refused: status=%u Win32=%lu (no skip or replacement)\n",
            (unsigned)status,(unsigned long)links_create_error);
    CHECK(status==XR_OS_IO_OK&&changed&&!metadata_memory.live&&!metadata_memory.held);
    return 1;
}
static int links_reference_final(const wchar_t *name,const XrOsFileLinkText *text) {
    HANDLE handle=CreateFileW(name,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);CHECK(handle!=INVALID_HANDLE_VALUE);
    wchar_t reference[32768],actual[32768];
    DWORD count=GetFinalPathNameByHandleW(handle,reference,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    bool closed=CloseHandle(handle)!=0;CHECK(closed&&count&&count<32768);
    CHECK(xr_win_utf8_to_utf16(text->text,text->length,actual,32768));
    CHECK(!wcscmp(reference,actual));return 1;
}
static int links_visible_results(void) {
    XrOsIoPolicy policy=test_policy();XrOsFileMetadataOwner owner={0};XrOsFileLinkText text={0};
    const wchar_t *base=wcsrchr(paths.file,L'\\');CHECK(base);++base;
    char relative[1024],outside[65536],junction[65536];
    CHECK(xr_win_utf16_to_utf8(base,wcslen(base),relative,sizeof(relative)));
    CHECK(xr_win_utf16_to_utf8(paths.marker,wcslen(paths.marker),outside,sizeof(outside)));
    CHECK(xr_win_utf16_to_utf8(paths.junction,wcslen(paths.junction),junction,sizeof(junction)));
    CHECK(links_create_checked(&policy,relative,links_paths.file_utf8,&links_paths.file_created));
    CHECK(links_create_checked(&policy,".",links_paths.directory_utf8,&links_paths.directory_created));
    const char missing[]="missing-\xe4\xb8\xad.bin";
    CHECK(links_create_checked(&policy,missing,links_paths.dangling_utf8,&links_paths.dangling_created));
    CHECK(links_create_checked(&policy,outside,links_paths.absolute_utf8,&links_paths.absolute_created));
    bool changed=true;
    CHECK(xr_os_io_path_symlink(&policy,links_span(relative),links_span(links_paths.file_utf8),NULL,&changed)==XR_OS_IO_OK&&!changed);
    CHECK(xr_os_io_path_readlink(&policy,links_span(links_paths.file_utf8),test_current,&owner,&text)==XR_OS_IO_OK&&
        text.text&&text.length==strlen(relative)&&!strcmp(text.text,relative)&&!owner.handle&&!kernel_handles);
    /* Returned text remains owned when its named source disappears. */
    CHECK(DeleteFileW(links_paths.file_locator));links_paths.file_created=false;
    CHECK(!strcmp(text.text,relative));links_text_free(&policy,&text);
    CHECK(xr_os_io_path_readlink(&policy,links_span(links_paths.directory_utf8),NULL,&owner,&text)==XR_OS_IO_OK&&
        text.text&&!strcmp(text.text,"."));links_text_free(&policy,&text);
    CHECK(xr_os_io_path_readlink(&policy,links_span(links_paths.dangling_utf8),NULL,&owner,&text)==XR_OS_IO_OK&&
        text.text&&!strcmp(text.text,missing));links_text_free(&policy,&text);
    CHECK(xr_os_io_path_realpath(&policy,links_span(links_paths.dangling_utf8),NULL,&owner,&text)==XR_OS_IO_OK&&!text.text);
    CHECK(xr_os_io_path_readlink(&policy,links_span(paths.utf8),NULL,&owner,&text)==XR_OS_IO_OK&&!text.text);
    CHECK(xr_os_io_path_realpath(&policy,links_span(paths.utf8),test_current,&owner,&text)==XR_OS_IO_OK&&text.text);
    CHECK(links_reference_final(paths.extended,&text));links_text_free(&policy,&text);
    CHECK(xr_os_io_path_realpath(&policy,links_span(links_paths.directory_utf8),NULL,&owner,&text)==XR_OS_IO_OK&&text.text);
    CHECK(links_reference_final(paths.child,&text));links_text_free(&policy,&text);
    CHECK(xr_os_io_path_readlink(&policy,links_span(junction),NULL,&owner,&text)==XR_OS_IO_OK&&text.text);
    wchar_t expected[32768],decoded[32768];int n=swprintf(expected,32768,L"\\\\?\\%ls",paths.outside);
    CHECK(n>0&&n<32768&&xr_win_utf8_to_utf16(text.text,text.length,decoded,32768)&&!wcscmp(expected,decoded));
    links_text_free(&policy,&text);
    CHECK(xr_os_io_path_readlink(&policy,links_span(links_paths.absolute_utf8),NULL,&owner,&text)==XR_OS_IO_OK&&text.text);
    n=swprintf(expected,32768,L"\\\\?\\%ls",paths.marker);
    CHECK(n>0&&n<32768&&xr_win_utf8_to_utf16(text.text,text.length,decoded,32768)&&!wcscmp(expected,decoded));
    links_text_free(&policy,&text);
    CHECK(xr_os_io_path_realpath(&policy,links_span(links_paths.absolute_utf8),NULL,&owner,&text)==XR_OS_IO_OK&&text.text);
    CHECK(links_reference_final(paths.marker,&text));links_text_free(&policy,&text);
    HANDLE protected_file=CreateFileW(paths.marker,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(protected_file!=INVALID_HANDLE_VALUE);unsigned char bytes[6];DWORD read=0;
    bool unchanged=ReadFile(protected_file,bytes,sizeof(bytes),&read,NULL)&&read==sizeof(bytes)&&!memcmp(bytes,"m\0\xff" "eta",6);
    CHECK(CloseHandle(protected_file)&&unchanged&&!owner.handle&&!kernel_handles&&!metadata_memory.live&&!metadata_memory.held);
    wchar_t previous[32768];DWORD previous_count=GetCurrentDirectoryW(32768,previous);
    CHECK(previous_count&&previous_count<32768&&SetCurrentDirectoryW(paths.child));
    XrOsIoStatus relative_status=xr_os_io_path_realpath(&policy,links_span(relative),NULL,&owner,&text);
    bool restored=SetCurrentDirectoryW(previous)!=0;
    CHECK(restored&&relative_status==XR_OS_IO_OK&&text.text&&links_reference_final(paths.extended,&text));
    links_text_free(&policy,&text);
    puts("file links: real file/directory/relative/dangling links, long Unicode, substitute targets and following paths");
    return 1;
}
static int links_faults(void) {
    XrOsIoPolicy policy=test_policy();XrOsFileMetadataOwner owner={0};XrOsFileLinkText text={0};bool changed=true;
    const char bad[]={'\xff'},nul[]={'a',0,'b'};
    CHECK(xr_os_io_path_symlink(&policy,(XrOsPathSpan){bad,sizeof(bad)},links_span(links_paths.file_utf8),NULL,&changed)==
        XR_OS_IO_BAD_ARGUMENT&&changed);
    CHECK(xr_os_io_path_symlink(&policy,links_span("missing"),(XrOsPathSpan){nul,sizeof(nul)},NULL,&changed)==
        XR_OS_IO_BAD_ARGUMENT&&changed&&GetFileAttributesW(links_paths.file_locator)==INVALID_FILE_ATTRIBUTES);
    reset_memory();metadata_memory.cancelled=true;
    CHECK(xr_os_io_path_symlink(&policy,links_span("missing"),links_span(links_paths.file_utf8),test_current,&changed)==
        XR_OS_IO_CANCELLED&&changed&&GetFileAttributesW(links_paths.file_locator)==INVALID_FILE_ATTRIBUTES);
    reset_memory();metadata_memory.fail_at=0;
    CHECK(xr_os_io_path_realpath(&policy,links_span(paths.utf8),test_current,&owner,&text)==XR_OS_IO_OUT_OF_MEMORY&&
        !text.text&&!owner.handle&&!metadata_memory.live&&!metadata_memory.held);
    reset_memory();metadata_memory.fail_at=0;metadata_memory.failure=XR_OS_IO_MEMORY_LIMIT;
    CHECK(xr_os_io_path_readlink(&policy,links_span(links_paths.dangling_utf8),test_current,&owner,&text)==XR_OS_IO_MEMORY_LIMIT&&
        !text.text&&!owner.handle&&!metadata_memory.live&&!metadata_memory.held);
    reset_memory();metadata_memory.cancelled=true;
    CHECK(xr_os_io_path_readlink(&policy,links_span(links_paths.dangling_utf8),test_current,&owner,&text)==XR_OS_IO_CANCELLED&&
        !text.text&&!owner.handle&&!metadata_memory.live&&!metadata_memory.held);
    reset_memory();close_failures=1;
    CHECK(xr_os_io_path_readlink(&policy,links_span(links_paths.dangling_utf8),NULL,&owner,&text)==XR_OS_IO_IO&&
        !text.text&&owner.handle&&!metadata_memory.live&&!metadata_memory.held);
    CHECK(xr_os_io_file_metadata_close(&owner)==XR_OS_IO_OK&&!owner.handle&&!kernel_handles);
    CHECK(xr_os_io_path_readlink(&policy,links_span(links_paths.dangling_utf8),NULL,&owner,&text)==XR_OS_IO_OK&&text.text);
    links_text_free(&policy,&text);close_failures=1;
    CHECK(xr_os_io_path_realpath(&policy,links_span(paths.utf8),NULL,&owner,&text)==XR_OS_IO_IO&&
        !text.text&&owner.handle&&!metadata_memory.live&&!metadata_memory.held);
    CHECK(xr_os_io_file_metadata_close(&owner)==XR_OS_IO_OK&&!owner.handle&&!kernel_handles);
    reset_memory();links_cancel_after_create=true;changed=false;
    XrOsIoStatus after_write=xr_os_io_path_symlink(&policy,links_span("still-dangling"),
        links_span(links_paths.file_utf8),test_current,&changed);
    links_cancel_after_create=false;
    links_paths.file_created=GetFileAttributesW(links_paths.file_locator)!=INVALID_FILE_ATTRIBUTES;
    CHECK(after_write==XR_OS_IO_CANCELLED&&!changed&&links_paths.file_created&&!metadata_memory.live&&!metadata_memory.held);
    CHECK(DeleteFileW(links_paths.file_locator));links_paths.file_created=false;
    puts("file links: typed UTF/NUL/OOM/cancel refusals, no half output and retained close retry");return 1;
}
typedef XrOsIoStatus (*LinksQuery)(const XrOsIoPolicy *,XrOsPathSpan,bool (*)(void *),
    XrOsFileMetadataOwner *,XrOsFileLinkText *);
static int links_query_failures(LinksQuery query,const char *name) {
    XrOsIoPolicy policy=test_policy();XrOsFileMetadataOwner owner={0};XrOsFileLinkText text={0};
    const char bad[]={'\xff'},nul[]={'a',0,'b'};
    reset_memory();
    CHECK(query(&policy,(XrOsPathSpan){bad,sizeof(bad)},NULL,&owner,&text)==XR_OS_IO_BAD_ARGUMENT&&!text.text&&!owner.handle);
    CHECK(query(&policy,(XrOsPathSpan){nul,sizeof(nul)},NULL,&owner,&text)==XR_OS_IO_BAD_ARGUMENT&&!text.text&&!owner.handle);
    reset_memory();metadata_memory.fail_at=0;
    CHECK(query(&policy,links_span(name),test_current,&owner,&text)==XR_OS_IO_OUT_OF_MEMORY&&
        !text.text&&!owner.handle&&!kernel_handles&&!metadata_memory.live&&!metadata_memory.held);
    reset_memory();metadata_memory.fail_at=0;metadata_memory.failure=XR_OS_IO_MEMORY_LIMIT;
    CHECK(query(&policy,links_span(name),test_current,&owner,&text)==XR_OS_IO_MEMORY_LIMIT&&
        !text.text&&!owner.handle&&!kernel_handles&&!metadata_memory.live&&!metadata_memory.held);
    reset_memory();metadata_memory.cancelled=true;
    CHECK(query(&policy,links_span(name),test_current,&owner,&text)==XR_OS_IO_CANCELLED&&
        !text.text&&!owner.handle&&!kernel_handles&&!metadata_memory.live&&!metadata_memory.held);
    reset_memory();cancel_after_open=true;
    XrOsIoStatus cancelled=query(&policy,links_span(name),test_current,&owner,&text);
    cancel_after_open=false;
    CHECK(cancelled==XR_OS_IO_CANCELLED&&!text.text&&!owner.handle&&!kernel_handles&&
        !metadata_memory.live&&!metadata_memory.held);
    reset_memory();close_failures=1;
    CHECK(query(&policy,links_span(name),NULL,&owner,&text)==XR_OS_IO_IO&&
        !text.text&&owner.handle&&!metadata_memory.live&&!metadata_memory.held);
    CHECK(xr_os_io_file_metadata_close(&owner)==XR_OS_IO_OK&&!owner.handle&&!kernel_handles);
    CHECK(query(&policy,links_span(name),test_current,&owner,&text)==XR_OS_IO_OK&&text.text&&!owner.handle&&!kernel_handles);
    links_text_free(&policy,&text);
    CHECK(!metadata_memory.live&&!metadata_memory.held);return 1;
}
static int links_following_paths(void) {
    XrOsIoPolicy policy=test_policy();XrOsFileMetadataOwner owner={0};XrOsFileLinkText text={0},other={0};
    char marker[65536],junction[65536],route[65536];wchar_t dotted[32768];
    CHECK(xr_win_utf16_to_utf8(paths.marker,wcslen(paths.marker),marker,sizeof(marker)));
    CHECK(xr_win_utf16_to_utf8(paths.junction,wcslen(paths.junction),junction,sizeof(junction)));
    CHECK(xr_os_io_path_realpath(&policy,links_span(marker),test_current,&owner,&text)==XR_OS_IO_OK&&text.text);
    CHECK(links_reference_final(paths.marker,&text));links_text_free(&policy,&text);
    CHECK(xr_os_io_path_realpath(&policy,links_span(paths.utf8),test_current,&owner,&text)==XR_OS_IO_OK&&text.text);
    CHECK(links_reference_final(paths.extended,&text));links_text_free(&policy,&text);
    const wchar_t *child=wcsrchr(paths.child,L'\\'),*file=wcsrchr(paths.file,L'\\');CHECK(child&&file);
    int n=swprintf(dotted,32768,L"%ls\\.\\..\\%ls\\%ls",paths.child,child+1,file+1);
    CHECK(n>0&&n<32768&&xr_win_utf16_to_utf8(dotted,wcslen(dotted),route,sizeof(route)));
    CHECK(xr_os_io_path_realpath(&policy,links_span(route),test_current,&owner,&text)==XR_OS_IO_OK&&text.text);
    CHECK(links_reference_final(paths.extended,&text));links_text_free(&policy,&text);
    CHECK(xr_os_io_path_realpath(&policy,links_span(junction),test_current,&owner,&text)==XR_OS_IO_OK&&text.text);
    CHECK(links_reference_final(paths.outside,&text));links_text_free(&policy,&text);
    CHECK(xr_os_io_path_realpath(&policy,links_span(links_paths.dangling_utf8),NULL,&owner,&text)==XR_OS_IO_OK&&!text.text);
    CHECK(links_query_failures(xr_os_io_path_realpath,paths.utf8));
    /* An unrelated query and its retirement cannot invalidate owned text. */
    CHECK(xr_os_io_path_realpath(&policy,links_span(paths.utf8),NULL,&owner,&text)==XR_OS_IO_OK&&text.text);
    CHECK(xr_os_io_path_realpath(&policy,links_span(junction),NULL,&owner,&other)==XR_OS_IO_OK&&other.text);
    links_text_free(&policy,&other);CHECK(links_reference_final(paths.extended,&text));links_text_free(&policy,&text);
    CHECK(!owner.handle&&!kernel_handles&&!metadata_memory.live&&!metadata_memory.held);
    puts("file links following-path: regular/Unicode/long/dot/junction paths, owned text and typed cleanup");return 1;
}
static int links_mount_point_readlink(void) {
    XrOsIoPolicy policy=test_policy();XrOsFileMetadataOwner owner={0};XrOsFileLinkText text={0};
    char junction[65536];wchar_t expected[32768],decoded[32768];
    CHECK(xr_win_utf16_to_utf8(paths.junction,wcslen(paths.junction),junction,sizeof(junction)));
    CHECK(xr_os_io_path_readlink(&policy,links_span(paths.utf8),NULL,&owner,&text)==XR_OS_IO_OK&&!text.text);
    CHECK(links_query_failures(xr_os_io_path_readlink,junction));
    CHECK(xr_os_io_path_readlink(&policy,links_span(junction),test_current,&owner,&text)==XR_OS_IO_OK&&text.text);
    int n=swprintf(expected,32768,L"\\\\?\\%ls",paths.outside);
    CHECK(n>0&&n<32768&&xr_win_utf8_to_utf16(text.text,text.length,decoded,32768)&&!wcscmp(expected,decoded));
    CHECK(RemoveDirectoryW(paths.junction));paths.junction_created=false;
    CHECK(xr_win_utf8_to_utf16(text.text,text.length,decoded,32768)&&!wcscmp(expected,decoded));
    links_text_free(&policy,&text);
    HANDLE protected_file=CreateFileW(paths.marker,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(protected_file!=INVALID_HANDLE_VALUE);unsigned char bytes[6];DWORD read=0;
    bool unchanged=ReadFile(protected_file,bytes,sizeof(bytes),&read,NULL)&&read==sizeof(bytes)&&!memcmp(bytes,"m\0\xff" "eta",6);
    CHECK(CloseHandle(protected_file)&&unchanged&&!owner.handle&&!kernel_handles&&!metadata_memory.live&&!metadata_memory.held);
    puts("file links MOUNT_POINT readlink: actual substitute text, independent owner and typed cleanup");return 1;
}
static void links_cleanup(void) {
    close_failures=0;
    if (links_paths.file_created) (void)DeleteFileW(links_paths.file_locator);
    if (links_paths.directory_created) (void)RemoveDirectoryW(links_paths.directory_locator);
    if (links_paths.dangling_created) (void)DeleteFileW(links_paths.dangling_locator);
    if (links_paths.absolute_created) (void)DeleteFileW(links_paths.absolute_locator);
    cleanup_paths();
}
int main(int argc,char **argv) {
    bool following=argc==2&&!strcmp(argv[1],"--following-path");
    bool mount_point=argc==2&&!strcmp(argv[1],"--mount-point-readlink");
    if (argc!=1&&!following&&!mount_point) return 2;
    reset_memory();int passed=setup_paths()&&links_setup();
    if (passed) passed=following?links_following_paths():mount_point?links_mount_point_readlink():
        links_visible_results()&&links_faults();
    links_cleanup();if(metadata_memory.live||metadata_memory.held||kernel_handles)return 1;
    return passed?0:1;
}
#else
#include "os/unix/file_links_unix.c"
int main(int argc,char **argv) {
    (void)argv;
    if (argc!=1) return 2;
    char root[]="/tmp/xray-file-links-XXXXXX",file[1024],name[1024],dangling[1024],directory[1024];
    if(!mkdtemp(root))return 1;
    int a=snprintf(file,sizeof(file),"%s/\xe4\xb8\xad.bin",root),b=snprintf(name,sizeof(name),"%s/link",root);
    int c=snprintf(dangling,sizeof(dangling),"%s/dangling",root),d=snprintf(directory,sizeof(directory),"%s/directory",root);
    if(a<0||b<0||c<0||d<0||(size_t)a>=sizeof(file)||(size_t)b>=sizeof(name)||(size_t)c>=sizeof(dangling)||(size_t)d>=sizeof(directory))return 1;
    FILE *stream=fopen(file,"wb");if(!stream)return 1;
    int written=fputs("protected",stream);if(fclose(stream)||written<0)return 1;
    reset_memory();XrOsIoPolicy policy=test_policy();XrOsFileMetadataOwner owner={0};XrOsFileLinkText text={0};bool made=false;
    const char target[]="\xe4\xb8\xad.bin";
    int passed=xr_os_io_path_symlink(&policy,links_span(target),links_span(name),test_current,&made)==XR_OS_IO_OK&&made&&
        xr_os_io_path_readlink(&policy,links_span(name),NULL,&owner,&text)==XR_OS_IO_OK&&text.text&&!strcmp(text.text,target);
    links_text_free(&policy,&text);
    passed=passed&&xr_os_io_path_symlink(&policy,links_span("absent"),links_span(dangling),NULL,&made)==XR_OS_IO_OK&&made&&
        xr_os_io_path_readlink(&policy,links_span(dangling),NULL,&owner,&text)==XR_OS_IO_OK&&text.text&&!strcmp(text.text,"absent");
    links_text_free(&policy,&text);
    passed=passed&&xr_os_io_path_symlink(&policy,links_span("."),links_span(directory),NULL,&made)==XR_OS_IO_OK&&made;
#if defined(PATH_MAX) && PATH_MAX > 0 && (defined(XR_OS_LINUX) || defined(XR_OS_MACOS))
    passed=passed&&xr_os_io_path_realpath(&policy,links_span(name),test_current,&owner,&text)==XR_OS_IO_OK&&text.text;
    if(text.text){struct stat actual,expected;passed=passed&&!stat(text.text,&actual)&&!stat(file,&expected)&&
        actual.st_dev==expected.st_dev&&actual.st_ino==expected.st_ino;}
    links_text_free(&policy,&text);
    passed=passed&&xr_os_io_path_realpath(&policy,links_span(dangling),NULL,&owner,&text)==XR_OS_IO_OK&&!text.text;
    passed=passed&&xr_os_io_path_realpath(&policy,links_span(directory),NULL,&owner,&text)==XR_OS_IO_OK&&text.text;
    if(text.text){struct stat actual,expected;passed=passed&&!stat(text.text,&actual)&&!stat(root,&expected)&&
        actual.st_dev==expected.st_dev&&actual.st_ino==expected.st_ino;}
    links_text_free(&policy,&text);
#else
    passed=passed&&xr_os_io_path_realpath(&policy,links_span(name),NULL,&owner,&text)==XR_OS_IO_UNSUPPORTED&&!text.text;
#endif
    reset_memory();metadata_memory.cancelled=true;
    passed=passed&&xr_os_io_path_readlink(&policy,links_span(name),test_current,&owner,&text)==XR_OS_IO_CANCELLED&&!text.text;
    reset_memory();metadata_memory.fail_at=0;
    passed=passed&&xr_os_io_path_readlink(&policy,links_span(name),test_current,&owner,&text)==XR_OS_IO_OUT_OF_MEMORY&&!text.text;
    reset_memory();stream=fopen(file,"rb");char protected_bytes[9]={0};
    if(!stream)passed=0;
    else {size_t n=fread(protected_bytes,1,8,stream);if(fclose(stream)||n!=8||memcmp(protected_bytes,"protected",8))passed=0;}
    if(unlink(name)||unlink(dangling)||unlink(directory)||unlink(file)||rmdir(root))passed=0;
    if(owner.handle||metadata_memory.live||metadata_memory.held)passed=0;
    return passed?0:1;
}
#endif
