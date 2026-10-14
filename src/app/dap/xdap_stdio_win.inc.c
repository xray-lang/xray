/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_stdio_win.inc.c - Windows pipe observation without socket fd conversion
 */
#include <windows.h>
static HANDLE stdio_platform_handle(DWORD stream) {
    HANDLE handle=GetStdHandle(stream);
    return handle && handle!=INVALID_HANDLE_VALUE ? handle : INVALID_HANDLE_VALUE;
}
static XrXirStatus stdio_platform_open(void) {
    HANDLE input=stdio_platform_handle(STD_INPUT_HANDLE),output=stdio_platform_handle(STD_OUTPUT_HANDLE);
    if (input==INVALID_HANDLE_VALUE || output==INVALID_HANDLE_VALUE ||
        GetFileType(input)==FILE_TYPE_UNKNOWN || GetFileType(output)==FILE_TYPE_UNKNOWN) return XR_XIR_IO;
    DWORD kind=GetFileType(input);return kind==FILE_TYPE_PIPE || kind==FILE_TYPE_DISK ? XR_XIR_OK : XR_XIR_UNSUPPORTED;
}
static XrXirStatus stdio_platform_read(char *bytes,size_t capacity,size_t *length,bool *eof) {
    HANDLE input=stdio_platform_handle(STD_INPUT_HANDLE);
    if (input==INVALID_HANDLE_VALUE) return XR_XIR_IO;
    DWORD available=0,kind=GetFileType(input);
    if (kind==FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(input,NULL,0,NULL,&available,NULL)) {
            DWORD error=GetLastError();
            if (error==ERROR_BROKEN_PIPE || error==ERROR_PIPE_NOT_CONNECTED) { *eof=true;return XR_XIR_OK; }
            return XR_XIR_IO;
        }
        if (!available) return XR_XIR_OK;
        if (capacity>available) capacity=available;
    } else if (kind!=FILE_TYPE_DISK) return XR_XIR_UNSUPPORTED;
    DWORD received=0;
    if (!ReadFile(input,bytes,(DWORD)capacity,&received,NULL)) {
        if (GetLastError()==ERROR_BROKEN_PIPE) { *eof=true;return XR_XIR_OK; }
        return XR_XIR_IO;
    }
    *length=received;*eof=received==0;return XR_XIR_OK;
}
static XrXirStatus stdio_platform_wait(uint32_t milliseconds) { Sleep(milliseconds);return XR_XIR_OK; }
/* Native writes preserve protocol bytes without changing a CRT text mode. */
static XrXirStatus stdio_platform_write(const char *bytes,size_t length) {
    HANDLE output=stdio_platform_handle(STD_OUTPUT_HANDLE);if (output==INVALID_HANDLE_VALUE) return XR_XIR_IO;
    while (length) {
        DWORD written=0;
        if (!WriteFile(output,bytes,(DWORD)length,&written,NULL) || !written) return XR_XIR_IO;
        bytes+=written;length-=written;
    }
    return XR_XIR_OK;
}
