/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_genuine_sys_path.h - Actual Path dependency for canonical sys fixtures
 *
 * KEY CONCEPT:
 *   The exact source-owned Path class and a real direct import authorize its
 *   read. Fixture text stays owned through the synchronous compiler request.
 */
#ifndef XIR_GENUINE_SYS_PATH_H
#define XIR_GENUINE_SYS_PATH_H
#include "xir/xxir_source.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <string.h>
static XrXirStatus xir_genuine_sys_path_check(const XrXirSourceRequest *request,
    const char *input,size_t length,XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic){
    CHECK(request&&request->stdlib_path&&request->entry_path&&request->authority&&input);
    CHECK(request->authority->kind==XR_MODULE_IDENTITY_STDLIB&&request->authority->namespace_id&&
        !strcmp(request->authority->namespace_id,"sys"));
    const char *root=request->stdlib_path;char locator[2048];
    int size=snprintf(locator,sizeof(locator),"%s/path/path.xr",root);CHECK(size>0&&(size_t)size<sizeof(locator));
    FILE *file=fopen(locator,"rb");CHECK(file&&!fseek(file,0,SEEK_END));long bytes=ftell(file);CHECK(bytes>0&&!fseek(file,0,SEEK_SET));
    char *whole=xr_malloc((size_t)bytes+1);CHECK(whole&&fread(whole,1,(size_t)bytes,file)==(size_t)bytes&&!fclose(file));whole[bytes]=0;
    const char *start=strstr(whole,"export class Path {"),*end=start?strstr(start,"\nexport class PathInfo {"):NULL;
    CHECK(start&&end&&end>start);size_t path_length=(size_t)(end-start);char *path=xr_malloc(path_length+1);CHECK(path);
    memcpy(path,start,path_length);path[path_length]=0;memset(whole,0xa5,(size_t)bytes);xr_free(whole);
    static const char dependency[]="import {Path} from path\n";
    CHECK(length<=SIZE_MAX-sizeof(dependency));size_t total=length+sizeof(dependency)-1;
    char *source=xr_malloc(total+1);CHECK(source);memcpy(source,dependency,sizeof(dependency)-1);memcpy(source+sizeof(dependency)-1,input,length);source[total]=0;
    XrModuleOverlayInput overlays[]={
        {{XR_MODULE_IDENTITY_STDLIB,"path",root},"path/path.xr",locator,path,path_length},
        {*request->authority,"sys/sys.xr",request->entry_path,source,total}};
    XrXirStatus status=xr_xir_compile_source_check_overlay(request,overlays,2,output,diagnostic,NULL);
    memset(path,0xa5,path_length);memset(source,0xa5,total);xr_free(path);xr_free(source);return status;
}
#endif // XIR_GENUINE_SYS_PATH_H
