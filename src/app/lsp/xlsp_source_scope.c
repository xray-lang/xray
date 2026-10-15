/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_scope.c - Owned editor overlays and closed workspace texts
 *
 * KEY CONCEPT:
 *   Canonical authorities decide physical roots. Directory discovery and regular
 *   reads use the original ledger, and editor overlays win before disk reads.
 */
#include "xlsp_source_scope.h"
#include "xlsp_index_scan.h"
#include "xlsp_index_pool.h"
#include "../../api/xsource_authority.h"
#include "../../api/xsource_compile.h"
#include "../../os/os_fs.h"
#include <limits.h>
#include <string.h>
typedef struct ScopeEntry {XlspSourceDocument document;XlspSourceBuffer *buffer;char *path;bool known;} ScopeEntry;
struct XlspSourceScope {
    XrCompileResources *resources;
    ScopeEntry *entries;
    XlspSourceDocument *documents;
    bool *known;
    size_t count,capacity;
};
typedef struct ScopeScan {XrCompileResources *resources;const XlspConfig *configuration;XrXirStatus status;} ScopeScan;
static XrXirStatus scope_resource(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus scope_io(XrOsIoStatus s) {
    return s==XR_OS_IO_OK?XR_XIR_OK:s==XR_OS_IO_BUDGET?XR_XIR_BUDGET:s==XR_OS_IO_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        s==XR_OS_IO_BAD_ARGUMENT?XR_XIR_BAD_STRUCTURE:s==XR_OS_IO_UNSUPPORTED?XR_XIR_UNSUPPORTED:XR_XIR_IO;
}
static XrXirStatus scope_manifest(XrManifestStatus s) {
    return s==XR_MANIFEST_OK?XR_XIR_OK:s==XR_MANIFEST_BUDGET?XR_XIR_BUDGET:s==XR_MANIFEST_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        s==XR_MANIFEST_IO?XR_XIR_IO:s==XR_MANIFEST_NOT_FOUND?XR_XIR_UNRESOLVED:s==XR_MANIFEST_UNSUPPORTED?XR_XIR_UNSUPPORTED:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus scope_length(XrCompileResources *r,const char *text,size_t *out) {
    if(!text)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;;++i){XrXirStatus s=scope_resource(xr_compile_resources_work(r,1));if(s!=XR_XIR_OK)return s;if(!text[i]){*out=i;return XR_XIR_OK;}if(i==SIZE_MAX-1)return XR_XIR_BUDGET;}
}
static XrXirStatus scope_copy(XrCompileResources *r,const char *text,size_t length,char **out) {
    if(!text||length==SIZE_MAX)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus s=scope_resource(xr_compile_resources_work(r,length));if(s!=XR_XIR_OK)return s;if(memchr(text,0,length))return XR_XIR_BAD_STRUCTURE;
    s=scope_resource(xr_compile_resources_alloc(r,length+1,(void **)out));if(s==XR_XIR_OK)s=scope_resource(xr_compile_resources_work(r,length+1));
    if(s==XR_XIR_OK){memcpy(*out,text,length);(*out)[length]=0;}return s;
}
static XrXirStatus scope_equal(XrCompileResources *r,const char *a,const char *b,bool *out) {
    for(;;){XrXirStatus s=scope_resource(xr_compile_resources_work(r,2));if(s!=XR_XIR_OK)return s;char x=*a++,y=*b++;
#ifdef XR_OS_WINDOWS
        /* TEXT joins a basename with '/', while FILE uses GetFullPathName.
         * Separator equivalence is independent of unproved case/link aliases. */
        if(x=='\\')x='/';if(y=='\\')y='/';
#endif
        if(x!=y||!x){*out=x==y;return XR_XIR_OK;}}
}
static XrXirStatus scope_grow(XrCompileResources *r,void **items,size_t *capacity,size_t count,size_t unit) {
    if(count<*capacity)return XR_XIR_OK;size_t next=*capacity?*capacity*2:16;
    if(next<*capacity||next>SIZE_MAX/unit)return XR_XIR_BUDGET;
    XrXirStatus s=scope_resource(xr_compile_resources_resize(r,items,next*unit));if(s==XR_XIR_OK)*capacity=next;return s;
}
static void scope_entry_free(ScopeEntry *entry) {
    xr_compile_resources_free((char *)entry->document.uri);xlsp_source_buffer_free(entry->buffer);xr_compile_resources_free(entry->path);*entry=(ScopeEntry){0};
}
XR_FUNC void xlsp_source_scope_free(XlspSourceScope *owner) {
    if(!owner)return;for(size_t i=0;i<owner->count;++i)scope_entry_free(&owner->entries[i]);
    xr_compile_resources_free(owner->entries);xr_compile_resources_free(owner->documents);xr_compile_resources_free(owner->known);xr_compile_resources_free(owner);
}
XR_FUNC size_t xlsp_source_scope_count(const XlspSourceScope *owner){return owner?owner->count:0;}
XR_FUNC const XlspSourceDocument *xlsp_source_scope_documents(const XlspSourceScope *owner){return owner?owner->documents:NULL;}
XR_FUNC const bool *xlsp_source_scope_known_versions(const XlspSourceScope *owner){return owner?owner->known:NULL;}
XR_FUNC XrXirStatus xlsp_source_scope_find(const XlspSourceScope *owner,const char *uri,size_t length,size_t *out) {
    if(!owner||!uri||length==SIZE_MAX||!out)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;i<owner->count;++i) {
        XrXirStatus s=scope_resource(xr_compile_resources_work(owner->resources,sizeof(XlspSourceDocument)));if(s!=XR_XIR_OK)return s;
        if(owner->documents[i].uri_length!=length)continue;s=scope_resource(xr_compile_resources_work(owner->resources,length));if(s!=XR_XIR_OK)return s;
        if(!memcmp(owner->documents[i].uri,uri,length)){*out=i;return XR_XIR_OK;}
    }
    return XR_XIR_UNRESOLVED;
}
static XrXirStatus scope_has_path(const XlspSourceScope *owner,const char *path,bool *out) {
    for(size_t i=0;i<owner->count;++i){bool equal=false;XrXirStatus s=scope_equal(owner->resources,path,owner->entries[i].path,&equal);if(s!=XR_XIR_OK)return s;if(equal){*out=true;return XR_XIR_OK;}}
    *out=false;return XR_XIR_OK;
}
static XrXirStatus scope_append(XlspSourceScope *owner,ScopeEntry *entry) {
    XrXirStatus s=scope_grow(owner->resources,(void **)&owner->entries,&owner->capacity,owner->count,sizeof(*entry));if(s!=XR_XIR_OK)return s;
    s=scope_resource(xr_compile_resources_work(owner->resources,sizeof(*entry)));if(s!=XR_XIR_OK)return s;
    owner->entries[owner->count++]=*entry;*entry=(ScopeEntry){0};return XR_XIR_OK;
}
static XrXirStatus scope_root(XrCompileResources *r,XlspIndexPaths *roots,char **path) {
    for(size_t i=0;i<roots->count;++i){bool equal=false;XrXirStatus s=scope_equal(r,roots->values[i],*path,&equal);if(s!=XR_XIR_OK)return s;if(equal)return XR_XIR_OK;}
    XrXirStatus s=scope_grow(r,(void **)&roots->values,&roots->capacity,roots->count,sizeof(*roots->values));if(s!=XR_XIR_OK)return s;
    s=scope_resource(xr_compile_resources_work(r,sizeof(*path)));if(s==XR_XIR_OK){roots->values[roots->count++]=*path;*path=NULL;}return s;
}
static XrXirStatus scope_authority(const XlspSourceScopeOptions *options,const XrSourceEntryInput *input,char **path,char **root) {
    XrSourceAuthority *authority=NULL;XrTomlParseLimits limits=xr_source_compile_default_manifest_limits();
    XrXirStatus s=scope_manifest(xr_source_authority_open_input(options->context,input,NULL,&limits,&authority,NULL));
    XrCompileResources *r=options->context->resources;size_t length=0;
    if(s==XR_XIR_OK)s=scope_length(r,xr_source_authority_source_path(authority),&length);
    if(s==XR_XIR_OK)s=scope_copy(r,xr_source_authority_source_path(authority),length,path);
    if(s==XR_XIR_OK&&root) {
        const XrModuleIdentityAuthority *entry=xr_source_authority_entry(authority);
        if(!entry||!entry->physical_root)s=XR_XIR_UNSUPPORTED;
        if(s==XR_XIR_OK)s=scope_length(r,entry->physical_root,&length);
        if(s==XR_XIR_OK)s=scope_copy(r,entry->physical_root,length,root);
    }
    xr_source_authority_close(authority);return s;
}
static XrXirStatus scope_open(XlspSourceScope *owner,const XlspSourceScopeOptions *options,size_t index,XlspIndexPaths *roots) {
    const XlspSourceDocument *input=&options->documents[index];ScopeEntry entry={0};char *decoded=NULL,*root=NULL,*uri=NULL;
    if(input->version<INT32_MIN||input->version>INT32_MAX)return XR_XIR_BAD_STRUCTURE;if(input->length>options->maximum_file_bytes)return XR_XIR_BUDGET;
    XrXirStatus s=xlsp_source_buffer_new(owner->resources,input->text,input->length,(int)input->version,&entry.buffer);if(s!=XR_XIR_OK)goto done;
    s=xlsp_source_uri_path(owner->resources,input->uri,input->uri_length,&decoded);if(s!=XR_XIR_OK)goto done;
    XrSourceEntryInput text={XR_SOURCE_ENTRY_TEXT,decoded,xlsp_source_buffer_text(entry.buffer),input->length};
    s=scope_authority(options,&text,&entry.path,&root);if(s!=XR_XIR_OK)goto done;
    bool duplicate=false;s=scope_has_path(owner,entry.path,&duplicate);if(s!=XR_XIR_OK)goto done;if(duplicate){s=XR_XIR_BAD_STRUCTURE;goto done;}
    s=scope_copy(owner->resources,input->uri,input->uri_length,&uri);if(s!=XR_XIR_OK)goto done;
    entry.document=(XlspSourceDocument){uri,xlsp_source_buffer_text(entry.buffer),input->uri_length,input->length,input->version};uri=NULL;entry.known=true;
    s=scope_root(owner->resources,roots,&root);if(s==XR_XIR_OK)s=scope_append(owner,&entry);
done:
    scope_entry_free(&entry);xr_compile_resources_free(decoded);xr_compile_resources_free(root);xr_compile_resources_free(uri);return s;
}
static XrXirStatus scope_disk(XlspSourceScope *owner,const XlspSourceScopeOptions *options,const char *path) {
    ScopeEntry entry={0};uint8_t *bytes=NULL;size_t length=0;char *uri=NULL;XrFsStat info={0};XrOsIoPolicy policy=xr_compile_io_policy(owner->resources);
    XrXirStatus s=scope_io(xr_os_io_stat(&policy,path,&info));if(s!=XR_XIR_OK)goto done;
    if(info.kind!=XR_FS_FILE){s=XR_XIR_UNSUPPORTED;goto done;}
    XrSourceEntryInput file={XR_SOURCE_ENTRY_FILE,path,NULL,0};s=scope_authority(options,&file,&entry.path,NULL);if(s!=XR_XIR_OK)goto done;
    bool duplicate=false;s=scope_has_path(owner,entry.path,&duplicate);if(s!=XR_XIR_OK||duplicate)goto done;
    s=scope_io(xr_os_io_read_regular_file(&policy,entry.path,options->maximum_file_bytes,&bytes,&length));if(s!=XR_XIR_OK)goto done;
    s=xlsp_source_buffer_new(owner->resources,(const char *)bytes,length,0,&entry.buffer);if(s!=XR_XIR_OK)goto done;
    s=xlsp_index_file_uri(owner->resources,entry.path,&uri);if(s!=XR_XIR_OK)goto done;size_t uri_length=0;s=scope_length(owner->resources,uri,&uri_length);if(s!=XR_XIR_OK)goto done;
    entry.document=(XlspSourceDocument){uri,xlsp_source_buffer_text(entry.buffer),uri_length,length,0};uri=NULL;
    s=scope_append(owner,&entry);
done:
    scope_entry_free(&entry);xr_compile_resources_free(bytes);xr_compile_resources_free(uri);return s;
}
static bool scope_ignore(const char *name,bool directory,void *context) {
    ScopeScan *scan=context;if(scan->status!=XR_XIR_OK)return true;bool ignored=false;
    scan->status=xlsp_source_ignore(scan->resources,scan->configuration,name,directory,&ignored);return scan->status!=XR_XIR_OK||ignored;
}
static XrXirStatus scope_explicit_roots(const XlspSourceScopeOptions *options,XlspIndexPaths *roots) {
    XrCompileResources *r=options->context->resources;XrOsIoPolicy policy=xr_compile_io_policy(r);
    for(size_t i=0;i<options->root_count;++i) {
        const char *root=options->roots[i];size_t length=0;XrXirStatus s=scope_length(r,root,&length);if(s!=XR_XIR_OK)return s;
        s=scope_resource(xr_compile_resources_work(r,length<3?length:3));if(s!=XR_XIR_OK)return s;
#ifdef XR_OS_WINDOWS
        bool letter=length&&((root[0]>='A'&&root[0]<='Z')||(root[0]>='a'&&root[0]<='z'));
        bool absolute=(length>=3&&letter&&root[1]==':'&&(root[2]=='/'||root[2]=='\\'))||
            (length>=2&&(root[0]=='/'||root[0]=='\\')&&(root[1]=='/'||root[1]=='\\'));
#else
        bool absolute=length&&root[0]=='/';
#endif
        if(!absolute)return XR_XIR_BAD_STRUCTURE;
        XrFsStat info={0};s=scope_io(xr_os_io_stat(&policy,root,&info));if(s!=XR_XIR_OK)return s;
        if(info.kind!=XR_FS_DIR)return XR_XIR_UNSUPPORTED;char *canonical=NULL;s=scope_io(xr_realpath_owned(&policy,options->roots[i],&canonical));
        if(s==XR_XIR_OK)s=scope_root(r,roots,&canonical);xr_compile_resources_free(canonical);if(s!=XR_XIR_OK)return s;
    }
    return XR_XIR_OK;
}
static XrXirStatus scope_publish(XlspSourceScope *owner) {
    if(!owner->count)return XR_XIR_OK;
    XrXirStatus s=scope_resource(xr_compile_resources_calloc(owner->resources,owner->count,sizeof(*owner->documents),(void **)&owner->documents));
    if(s==XR_XIR_OK)s=scope_resource(xr_compile_resources_calloc(owner->resources,owner->count,sizeof(*owner->known),(void **)&owner->known));if(s!=XR_XIR_OK)return s;
    for(size_t i=0;i<owner->count;++i){s=scope_resource(xr_compile_resources_work(owner->resources,sizeof(XlspSourceDocument)+sizeof(bool)));if(s!=XR_XIR_OK)return s;owner->documents[i]=owner->entries[i].document;owner->known[i]=owner->entries[i].known;}
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_source_scope_capture(const XlspSourceScopeOptions *options,XlspSourceScope **out) {
    if(!options||!options->context||!options->context->resources||(!options->documents&&options->count)||(!options->roots&&options->root_count)||
       (!options->count&&!options->root_count)||!options->configuration||!options->maximum_file_bytes||options->maximum_file_bytes>INT32_MAX||
       options->count>SIZE_MAX/sizeof(ScopeEntry)||options->root_count>SIZE_MAX/sizeof(char *)||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XrCompileResources *r=options->context->resources;XlspSourceScope *owner=NULL;XlspIndexPaths roots={0},files={0};
    XrXirStatus s=scope_resource(xr_compile_resources_calloc(r,1,sizeof(*owner),(void **)&owner));if(s!=XR_XIR_OK)goto done;owner->resources=r;
    bool ignored=false;s=xlsp_source_ignore(r,options->configuration,"",false,&ignored);
    for(size_t i=0;i<options->count&&s==XR_XIR_OK;++i)s=scope_open(owner,options,i,&roots);
    if(s==XR_XIR_OK)s=scope_explicit_roots(options,&roots);
    ScopeScan scan={r,options->configuration,XR_XIR_OK};XlspIndexScanOptions discover={r,(const char *const *)roots.values,roots.count,scope_ignore,&scan};
    if(s==XR_XIR_OK)s=xlsp_index_scan(&discover,&files);if(s==XR_XIR_OK)s=scan.status;
    for(size_t i=0;i<files.count&&s==XR_XIR_OK;++i)s=scope_disk(owner,options,files.values[i]);
    if(s==XR_XIR_OK)s=scope_publish(owner);
    if(s==XR_XIR_OK){*out=owner;owner=NULL;}
done:
    xlsp_index_paths_free(&roots);xlsp_index_paths_free(&files);xlsp_source_scope_free(owner);return s;
}
