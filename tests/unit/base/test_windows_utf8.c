/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_windows_utf8.c - Owned Windows argument conversion and failure cleanup
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"failed %d: %s\n",__LINE__,#c); exit(1); } } while(0)
static size_t attempts, fail_at, live;
static void *probe_malloc(size_t size) {
    if (++attempts == fail_at) return NULL;
    void *p = xr_malloc(size); CHECK(p); ++live; return p;
}
static void *probe_calloc(size_t count, size_t size) {
    if (++attempts == fail_at) return NULL;
    void *p = xr_calloc(count,size); CHECK(p); ++live; return p;
}
static void probe_free(void *p) { if(p) { CHECK(live); --live; xr_free(p); } }
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#define xr_malloc probe_malloc
#define xr_calloc probe_calloc
#define xr_free probe_free
#include "base/xio_policy.c"
#include "base/xwindows_utf8.h"
int main(void) {
    wchar_t *args[] = {L"xray",L"\x5de5\x7a0b",L"\xd83d\xde00"};
    XrWinPathStatus status;
    char **result = xr_win_utf16_arguments(3,args,&status);
    CHECK(result && status==XR_WIN_PATH_OK && result[3]==NULL);
    CHECK(!strcmp(result[1],"\xe5\xb7\xa5\xe7\xa8\x8b"));
    CHECK(!strcmp(result[2],"\xf0\x9f\x98\x80"));
    XrOsIoPolicy policy = xr_os_io_system_policy();
    wchar_t *round = NULL;
    CHECK(xr_win_utf8_text_owned(&policy,result[2],&round)==XR_OS_IO_OK);
    CHECK(round && round[0]==0xd83d && round[1]==0xde00 && !round[2]);
    probe_free(round); xr_win_utf8_arguments_free(3,result); CHECK(!live);
    for (size_t fail=1; fail<=4; ++fail) {
        attempts=0; fail_at=fail;
        CHECK(!xr_win_utf16_arguments(3,args,&status));
        CHECK(status==XR_WIN_PATH_OOM && !live);
    }
    fail_at=0;
    wchar_t invalid[] = {0xd800,0}; args[1]=invalid;
    CHECK(!xr_win_utf16_arguments(3,args,&status)); CHECK(status==XR_WIN_PATH_INVALID && !live);
    round=NULL;
    CHECK(xr_win_utf8_text_owned(&policy,"\xc0\xaf",&round)==XR_OS_IO_BAD_ARGUMENT);
    CHECK(!round && !live);
    puts("Unicode arguments: Chinese, non-BMP, malformed input, four OOM points, zero owners");
    return 0;
}
