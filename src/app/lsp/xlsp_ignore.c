/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_ignore.c - Shared bounded workspace ignore matching
 *
 * KEY CONCEPT:
 *   Match the existing byte-oriented basename policy without recursive glob
 *   expansion. Source callers charge every comparison to their original ledger.
 */
#include "xlsp_ignore.h"
static XrXirStatus ignore_work(XrCompileResources *r,uint64_t amount) {
    if(!r)return XR_XIR_OK;XrCompileResourceStatus s=xr_compile_resources_work(r,amount);
    return s==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:s==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        s==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus ignore_match(XrCompileResources *r,const char *pattern,const char *name,bool glob,bool *out) {
    const char *star=NULL,*retry=NULL;
    for(;;) {
        XrXirStatus s=ignore_work(r,2);if(s!=XR_XIR_OK)return s;
        if(!*name)break;
        if(glob&&*pattern=='*'){star=pattern++;retry=name;}
        else if((glob&&*pattern=='?')||(*pattern&&*pattern==*name)){++pattern;++name;}
        else if(star){pattern=star+1;name=++retry;}
        else {*out=false;return XR_XIR_OK;}
    }
    if(glob)for(;;){XrXirStatus s=ignore_work(r,1);if(s!=XR_XIR_OK)return s;if(*pattern!='*')break;++pattern;}
    XrXirStatus s=ignore_work(r,1);if(s==XR_XIR_OK)*out=!*pattern;return s;
}
static XrXirStatus ignore_name(XrCompileResources *r,const XlspConfig *config,const char *name,bool directory,bool *out) {
    if(!config||!name||!out||config->ignore_pattern_count<0||
        (config->ignore_pattern_count&&!config->ignore_patterns))return XR_XIR_BAD_STRUCTURE;
    XrXirStatus s=ignore_work(r,1);if(s!=XR_XIR_OK)return s;
    if(name[0]=='.'){*out=true;return XR_XIR_OK;}
    for(int i=0;i<config->ignore_pattern_count;++i) {
        s=ignore_work(r,sizeof(XlspIgnorePattern));if(s!=XR_XIR_OK)return s;
        const XlspIgnorePattern *p=&config->ignore_patterns[i];if(!p->pattern)return XR_XIR_BAD_STRUCTURE;
        if(p->is_dir_only&&!directory)continue;bool match=false;
        s=ignore_match(r,p->pattern,name,p->is_glob,&match);if(s!=XR_XIR_OK)return s;if(match){*out=true;return XR_XIR_OK;}
    }
    *out=false;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_source_ignore(XrCompileResources *r,const XlspConfig *config,const char *name,bool directory,bool *out) {
    if(!r)return XR_XIR_BAD_STRUCTURE;return ignore_name(r,config,name,directory,out);
}
XR_FUNC bool xlsp_config_should_ignore(XlspConfig *config,const char *name,bool directory) {
    bool ignored=false;return ignore_name(NULL,config,name,directory,&ignored)==XR_XIR_OK&&ignored;
}
