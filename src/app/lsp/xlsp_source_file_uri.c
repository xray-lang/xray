/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_file_uri.c - One metered native-path URI encoder for consumers
 */
#include "xlsp_source_query.h"
#include "../../base/xutf8.h"
#include <string.h>
static XrXirStatus uri_resource(XrCompileResourceStatus s) {
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus uri_work(XrCompileResources *r,size_t n){return uri_resource(xr_compile_resources_work(r,n));}
static XrXirStatus uri_length(XrCompileResources *r,const char *text,size_t *length) {
    if(!text)return XR_XIR_BAD_STRUCTURE;
    for(size_t i=0;;++i){XrXirStatus s=uri_work(r,1);if(s!=XR_XIR_OK)return s;
        if(!text[i]){*length=i;return XR_XIR_OK;}if(i==SIZE_MAX-1)return XR_XIR_BUDGET;}
}
static bool uri_letter(unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z');}
static unsigned char uri_separator(unsigned char c) {
#ifdef XR_OS_WINDOWS
    if(c=='\\')return '/';
#endif
    return c;
}
static bool uri_unreserved(unsigned char c){return uri_letter(c)||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~'||c=='/'||c==':';}
XR_FUNC XrXirStatus xlsp_index_file_uri(XrCompileResources *r,const char *path,char **out) {
    if(!r||!path||!out||*out)return XR_XIR_BAD_STRUCTURE;
    size_t n=0;XrXirStatus s=uri_length(r,path,&n);if(s!=XR_XIR_OK)return s;
    if(!n||n>(SIZE_MAX-9)/3)return XR_XIR_BAD_STRUCTURE;
    bool drive=n>=3&&uri_letter((unsigned char)path[0])&&path[1]==':'&&uri_separator((unsigned char)path[2])=='/';
    bool unc=n>=3&&uri_separator((unsigned char)path[0])=='/'&&uri_separator((unsigned char)path[1])=='/';
    if(!drive&&!unc&&path[0]!='/')return XR_XIR_BAD_STRUCTURE;
    s=uri_work(r,n);if(s!=XR_XIR_OK)return s;if(!xr_utf8_validate(path,n))return XR_XIR_BAD_STRUCTURE;
    const char *prefix=drive?"file:///":unc?"file:":"file://";size_t used=strlen(prefix);char *uri=NULL;
    s=uri_resource(xr_compile_resources_alloc(r,used+n*3+1,(void **)&uri));if(s!=XR_XIR_OK)return s;
    s=uri_work(r,used);if(s==XR_XIR_OK)memcpy(uri,prefix,used);const char hex[]="0123456789ABCDEF";
    for(size_t i=0;i<n&&s==XR_XIR_OK;++i){s=uri_work(r,1);if(s!=XR_XIR_OK)break;
        unsigned char c=uri_separator((unsigned char)path[i]);size_t width=uri_unreserved(c)?1:3;
        s=uri_work(r,width);if(s!=XR_XIR_OK)break;
        if(width==1)uri[used++]=(char)c;else{uri[used++]='%';uri[used++]=hex[c>>4];uri[used++]=hex[c&15];}}
    if(s==XR_XIR_OK)s=uri_work(r,1);
    if(s==XR_XIR_OK){uri[used]=0;*out=uri;}else xr_compile_resources_free(uri);return s;
}
