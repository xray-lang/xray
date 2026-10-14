/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_stdio_win.inc.c - Windows pipe observation without socket fd conversion
 */
#include <windows.h>
#include <io.h>
#include <fcntl.h>
static XrXirStatus stdio_platform_read(char *bytes,size_t capacity,size_t *length,bool *eof) {
    HANDLE input=(HANDLE)_get_osfhandle(_fileno(stdin));
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
static int stdio_platform_mode(int fd,int mode) { return _setmode(fd,mode); }
