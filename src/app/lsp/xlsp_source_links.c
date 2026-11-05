/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_source_links.h"
#include "xlsp_index_pool.h"
#include "../../api/xsource_paths.h"
#include "../../api/xsource_authority.h"
#include "../../xir/xxir_stdlib_internal.h"
#include "../../xir/xxir_library_catalog.h"
#include "../../module/xmodule_resolver.h"
#include <limits.h>
#include <string.h>

typedef struct SourceLink {char *target;size_t length;XrLspRange range;} SourceLink;
struct XlspSourceLinks {XrCompileResources *resources;SourceLink *items;size_t count;};
static XrXirStatus links_resource(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus links_module(XrModuleStatus s) {
    return s==XR_MODULE_OK?XR_XIR_OK:s==XR_MODULE_BUDGET?XR_XIR_BUDGET:s==XR_MODULE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        s==XR_MODULE_IO?XR_XIR_IO:s==XR_MODULE_NOT_FOUND?XR_XIR_UNRESOLVED:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus links_manifest(XrManifestStatus s) {
    return s==XR_MANIFEST_OK?XR_XIR_OK:s==XR_MANIFEST_OVERFLOW?XR_XIR_BUDGET:s==XR_MANIFEST_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:
        s==XR_MANIFEST_IO?XR_XIR_IO:s==XR_MANIFEST_NOT_FOUND?XR_XIR_UNRESOLVED:s==XR_MANIFEST_UNSUPPORTED?XR_XIR_UNSUPPORTED:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus links_length(XrCompileResources *r,const char *text,size_t *out) {
    if(!text)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;;++i) {
        XrXirStatus status=links_resource(xr_compile_resources_work(r,1));if(status!=XR_XIR_OK)return status;
        if(!text[i]){*out=i;return XR_XIR_OK;}if(i==SIZE_MAX-1)return XR_XIR_BUDGET;
    }
}
XR_FUNC void xlsp_source_links_free(XlspSourceLinks *owner) {
    if(!owner)return;
    for(size_t i=0;i<owner->count;++i)xr_compile_resources_free(owner->items[i].target);
    xr_compile_resources_free(owner->items);xr_compile_resources_free(owner);
}
typedef struct LinksInputs {
    XrStdlibPath stdlib;
    XrXirLibraryCatalog *catalog;
    XrSourceAuthority **authorities;
    XrModuleOverlayInput *overlays;
    size_t count;
    XrModuleResolver *resolver;
} LinksInputs;
static void links_inputs_free(LinksInputs *inputs) {
    xr_compile_module_resolver_free(inputs->resolver);
    if(inputs->authorities)for(size_t i=0;i<inputs->count;++i)xr_source_authority_close(inputs->authorities[i]);
    xr_compile_resources_free(inputs->authorities);xr_compile_resources_free(inputs->overlays);
    xr_xir_compile_library_catalog_free(inputs->catalog);xr_source_stdlib_path_free(&inputs->stdlib);
}
static XrXirStatus links_inputs_build(const XrXirCompileContext *context,const XlspSourceDocument *documents,size_t count,size_t entry,LinksInputs *inputs) {
    XrCompileResources *r=context->resources;inputs->count=count;
    XrXirStatus status=links_resource(xr_compile_resources_calloc(r,count,sizeof(*inputs->authorities),(void **)&inputs->authorities));
    if(status==XR_XIR_OK)status=links_resource(xr_compile_resources_calloc(r,count,sizeof(*inputs->overlays),(void **)&inputs->overlays));
    if(status!=XR_XIR_OK)return status;
    XrSourceCompileStatus selected=xr_source_stdlib_path(r,&inputs->stdlib,NULL);
    if(selected!=XR_SOURCE_COMPILE_OK)return selected==XR_SOURCE_COMPILE_BUDGET?XR_XIR_BUDGET:
        selected==XR_SOURCE_COMPILE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:selected==XR_SOURCE_COMPILE_NOT_FOUND?XR_XIR_UNRESOLVED:XR_XIR_IO;
    status=xir_stdlib_library_catalog_new(context,inputs->stdlib.path,&inputs->catalog);if(status!=XR_XIR_OK)return status;
    XrTomlParseLimits limits=xr_source_compile_default_manifest_limits();
    for(size_t i=0;i<count;++i) {
        char *path=NULL;status=links_resource(xr_compile_resources_work(r,sizeof(documents[i])));if(status!=XR_XIR_OK)return status;
        status=xlsp_source_uri_path(r,documents[i].uri,documents[i].uri_length,&path);if(status!=XR_XIR_OK)return status;
        XrSourceEntryInput input={XR_SOURCE_ENTRY_TEXT,path,documents[i].text,documents[i].length};
        status=links_manifest(xr_source_authority_open_input(context,&input,inputs->catalog,&limits,&inputs->authorities[i],NULL));
        xr_compile_resources_free(path);if(status!=XR_XIR_OK)return status;
        const XrModuleOverlayInput *overlay=xr_source_authority_overlay(inputs->authorities[i]);
        if(!overlay)return XR_XIR_BAD_STRUCTURE;
        status=links_resource(xr_compile_resources_work(r,sizeof(*overlay)));if(status!=XR_XIR_OK)return status;
        inputs->overlays[i]=*overlay;
    }
    /* Only the selected importer's pins may select this request's packages. */
    XrModuleResolverConfig config={inputs->stdlib.path,xr_source_authority_lockfile(inputs->authorities[entry]),inputs->catalog};
    status=links_module(xr_compile_module_resolver_new(r,&config,&inputs->resolver));
    if(status==XR_XIR_OK)status=links_module(xr_compile_module_resolver_set_overlay(inputs->resolver,inputs->overlays,count));
    return status;
}
XR_FUNC XrXirStatus xlsp_source_links_build(const XrXirCompileContext *context,const XlspSourceDocument *documents,
    size_t count,size_t entry,XlspSourceLinks **out) {
    if(!context||!context->resources||!documents||!count||entry>=count||!out||*out||
       count>SIZE_MAX/sizeof(XrModuleOverlayInput)||count>SIZE_MAX/sizeof(XrSourceAuthority *))return XR_XIR_BAD_STRUCTURE;
    XrCompileResources *r=context->resources;XlspSourceImports *imports=NULL;XlspSourceLinks *owner=NULL;LinksInputs inputs={0};
    XrXirStatus status=xlsp_source_imports_build(r,&documents[entry],&imports);if(status!=XR_XIR_OK)goto done;
    status=links_inputs_build(context,documents,count,entry,&inputs);if(status!=XR_XIR_OK)goto done;
    size_t length=0;const XlspSourceImport *items=xlsp_source_imports_items(imports,&length);
    if(length>SIZE_MAX/sizeof(SourceLink)){status=XR_XIR_BUDGET;goto done;}
    status=links_resource(xr_compile_resources_calloc(r,1,sizeof(*owner),(void **)&owner));if(status!=XR_XIR_OK)goto done;
    owner->resources=r;
    if(length){status=links_resource(xr_compile_resources_calloc(r,length,sizeof(*owner->items),(void **)&owner->items));if(status!=XR_XIR_OK)goto done;}
    owner->count=length;
    for(size_t i=0;i<length;++i) {
        XrModuleId resolved={0};char *target=NULL;
        status=links_resource(xr_compile_resources_work(r,sizeof(SourceLink)));if(status!=XR_XIR_OK)goto done;
        status=links_module(xr_compile_module_resolver_resolve(inputs.resolver,items[i].specifier,
            xr_source_authority_source_path(inputs.authorities[entry]),xr_source_authority_entry(inputs.authorities[entry]),&resolved,NULL));
        if(status==XR_XIR_OK)status=resolved.source_path?xlsp_index_file_uri(r,resolved.source_path,&target):XR_XIR_UNSUPPORTED;
        xr_compile_module_id_cleanup(&resolved);
        if(status!=XR_XIR_OK){xr_compile_resources_free(target);goto done;}
        owner->items[i].target=target;owner->items[i].range=items[i].range;
        status=links_length(r,target,&owner->items[i].length);if(status!=XR_XIR_OK)goto done;
    }
    *out=owner;owner=NULL;
done:
    links_inputs_free(&inputs);xlsp_source_imports_free(imports);xlsp_source_links_free(owner);return status;
}
static bool links_member(XrJsonValue *object,const char *key,XrJsonValue *child) {
    if(!child)return false;int before=object->as.object.count;xjson_object_set_new(object,key,child);
    if(object->as.object.count==before){xjson_free(child);return false;}
    if(object->as.object.count!=before+1)return false;
    XrJsonMember *member=&object->as.object.members[before];
    return member->key&&member->value==child&&member->key_len==strlen(key)&&!memcmp(member->key,key,member->key_len);
}
static XrJsonValue *links_position(XrLspPosition position) {
    XrJsonValue *value=xjson_new_object();if(!value)return NULL;
    if(!links_member(value,"line",xjson_new_number(position.line))||
       !links_member(value,"character",xjson_new_number(position.character))){xjson_free(value);return NULL;}
    return value;
}
static XrJsonValue *links_range(XrLspRange range) {
    XrJsonValue *value=xjson_new_object();if(!value)return NULL;
    if(!links_member(value,"start",links_position(range.start))||!links_member(value,"end",links_position(range.end))){xjson_free(value);return NULL;}
    return value;
}
static XrJsonValue *links_value(const SourceLink *link) {
    XrJsonValue *value=xjson_new_object();if(!value)return NULL;
    if(!links_member(value,"range",links_range(link->range))||
       !links_member(value,"target",xjson_new_string_n(link->target,link->length))){xjson_free(value);return NULL;}
    return value;
}
XR_FUNC XrXirStatus xlsp_source_links_json(const XlspSourceLinks *owner,XrJsonValue **out) {
    if(!owner||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=links_resource(xr_compile_resources_work(owner->resources,sizeof(*owner)));if(status!=XR_XIR_OK)return status;
    XrJsonValue *array=xjson_new_array();if(!array)return XR_XIR_OUT_OF_MEMORY;
    for(size_t i=0;i<owner->count;++i) {
        status=links_resource(xr_compile_resources_work(owner->resources,sizeof(SourceLink)));if(status!=XR_XIR_OK)goto done;
        status=links_resource(xr_compile_resources_work(owner->resources,owner->items[i].length+1));if(status!=XR_XIR_OK)goto done;
        if(array->as.array.count==array->as.array.capacity&&array->as.array.capacity>INT_MAX/2){status=XR_XIR_BUDGET;goto done;}
        XrJsonValue *value=links_value(&owner->items[i]);if(!value){status=XR_XIR_OUT_OF_MEMORY;goto done;}
        int before=array->as.array.count;xjson_array_push(array,value);
        if(array->as.array.count!=before+1){xjson_free(value);status=XR_XIR_OUT_OF_MEMORY;goto done;}
    }
    *out=array;array=NULL;
done:
    xjson_free(array);return status;
}
