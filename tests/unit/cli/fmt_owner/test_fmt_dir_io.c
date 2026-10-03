/* Inject failures at the real iterator's Win32 boundary. */
#include "base/xwindows_utf8.h"
bool fmt_dir_open_failure,fmt_dir_next_failure;
static HANDLE WINAPI fixture_first(LPCWSTR path,LPWIN32_FIND_DATAW data) {
    if(fmt_dir_open_failure){SetLastError(ERROR_ACCESS_DENIED);return INVALID_HANDLE_VALUE;}
    return FindFirstFileW(path,data);
}
static BOOL WINAPI fixture_next(HANDLE handle,LPWIN32_FIND_DATAW data) {
    if(fmt_dir_next_failure){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
    return FindNextFileW(handle,data);
}
#define FindFirstFileW fixture_first
#define FindNextFileW fixture_next
#include "os/win/dir_win.c"
