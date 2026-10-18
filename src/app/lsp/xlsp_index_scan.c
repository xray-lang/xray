/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_index_scan.h"
#include "../../os/os_fs.h"
#include "../../os/os_dir.h"
#include <string.h>
static XrXirStatus scan_resource(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus scan_io(XrOsIoStatus status) {
    return status==XR_OS_IO_OK?XR_XIR_OK:status==XR_OS_IO_BUDGET?XR_XIR_BUDGET:
        status==XR_OS_IO_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:status==XR_OS_IO_BAD_ARGUMENT?XR_XIR_BAD_STRUCTURE:XR_XIR_IO;
}
static XrXirStatus scan_length(XrCompileResources *resources,const char *text,size_t *out) {
    if(!text)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;;++i){XrXirStatus s=scan_resource(xr_compile_resources_work(resources,1));if(s!=XR_XIR_OK)return s;
        if(!text[i]){*out=i;return XR_XIR_OK;}if(i==SIZE_MAX-1)return XR_XIR_BUDGET;}
}
static XrXirStatus scan_join(XrCompileResources *resources,const char *parent,const char *name,char **out) {
    size_t a=0,b=0;XrXirStatus s=scan_length(resources,parent,&a);if(s==XR_XIR_OK&&name)s=scan_length(resources,name,&b);
    if(s!=XR_XIR_OK)return s;size_t separator=name?1:0;
    if(b>SIZE_MAX-separator-1||a>SIZE_MAX-b-separator-1)return XR_XIR_BUDGET;
    s=scan_resource(xr_compile_resources_work(resources,a+b+separator+1));if(s!=XR_XIR_OK)return s;
    char *path=NULL;s=scan_resource(xr_compile_resources_alloc(resources,a+b+separator+1,(void **)&path));
    if(s==XR_XIR_OK){memcpy(path,parent,a);if(name){path[a++]='/';memcpy(path+a,name,b);}path[a+b]=0;*out=path;}return s;
}
static XrXirStatus scan_push(XrCompileResources *resources,XlspIndexPaths *list,char **path) {
    XrXirStatus s=scan_resource(xr_compile_resources_work(resources,sizeof(*path)));if(s!=XR_XIR_OK)return s;
    if(list->count==list->capacity){size_t next=list->capacity?list->capacity*2:16;
        if(next<list->capacity||next>SIZE_MAX/sizeof(*list->values))return XR_XIR_BUDGET;
        s=scan_resource(xr_compile_resources_resize(resources,(void **)&list->values,next*sizeof(*list->values)));
        if(s!=XR_XIR_OK)return s;list->capacity=next;}
    list->values[list->count++]=*path;*path=NULL;return XR_XIR_OK;
}
XR_FUNC void xlsp_index_paths_free(XlspIndexPaths *paths) {
    if(!paths)return;for(size_t i=0;i<paths->count;++i)xr_compile_resources_free(paths->values[i]);
    xr_compile_resources_free(paths->values);*paths=(XlspIndexPaths){0};
}
static XrXirStatus scan_directory(const XlspIndexScanOptions *options,const char *path,XlspIndexPaths *stack,XlspIndexPaths *files) {
    XrOsIoPolicy policy=xr_compile_io_policy(options->resources);XrFsStat info={0};
    XrXirStatus status=scan_io(xr_os_io_stat(&policy,path,&info));if(status!=XR_XIR_OK)return status;
    if(info.kind!=XR_FS_DIR)return info.kind==XR_FS_OTHER?XR_XIR_UNSUPPORTED:XR_XIR_BAD_STRUCTURE;
    XrDirIter *iterator=NULL;status=scan_io(xr_os_io_dir_open(&policy,path,&iterator));if(status!=XR_XIR_OK)return status;
    for(;;){XrDirEntry entry;XrOsIoStatus read=xr_os_io_dir_next(iterator,&entry);if(read==XR_OS_IO_END)break;
        status=scan_io(read);if(status!=XR_XIR_OK)break;
        status=scan_resource(xr_compile_resources_work(options->resources,sizeof(entry)));if(status!=XR_XIR_OK)break;
        if(options->ignore&&options->ignore(entry.name,entry.is_dir,options->context))continue;
        size_t length=0;status=scan_length(options->resources,entry.name,&length);if(status!=XR_XIR_OK)break;
        if(!entry.is_dir&&(length<3||memcmp(entry.name+length-3,".xr",3)))continue;
        char *child=NULL;status=scan_join(options->resources,path,entry.name,&child);
        if(status==XR_XIR_OK)status=scan_push(options->resources,entry.is_dir?stack:files,&child);
        xr_compile_resources_free(child);if(status!=XR_XIR_OK)break;}
    xr_os_io_dir_close(iterator);return status;
}
XR_FUNC XrXirStatus xlsp_index_scan(const XlspIndexScanOptions *options,XlspIndexPaths *out) {
    if(!options||!options->resources||(!options->roots&&options->root_count)||!out||out->values||out->count||out->capacity)return XR_XIR_BAD_STRUCTURE;
    if(options->root_count>SIZE_MAX/sizeof(*options->roots))return XR_XIR_BUDGET;
    XlspIndexPaths stack={0},files={0};XrXirStatus status=XR_XIR_OK;
    for(size_t i=0;i<options->root_count&&status==XR_XIR_OK;++i){char *path=NULL;
        status=scan_join(options->resources,options->roots[i],NULL,&path);
        if(status==XR_XIR_OK)status=scan_push(options->resources,&stack,&path);xr_compile_resources_free(path);}
    while(stack.count&&status==XR_XIR_OK){char *path=stack.values[--stack.count];
        status=scan_directory(options,path,&stack,&files);xr_compile_resources_free(path);}
    xlsp_index_paths_free(&stack);if(status==XR_XIR_OK){*out=files;files=(XlspIndexPaths){0};}xlsp_index_paths_free(&files);return status;
}
