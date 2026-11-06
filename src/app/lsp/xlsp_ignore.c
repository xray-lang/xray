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
 *   expansion. Matching reads configuration and name bytes without retaining them.
 */
#include "xlsp_ignore.h"
static XrXirStatus ignore_match(const char *pattern,const char *name,bool glob,bool *out) {
    const char *star=NULL,*retry=NULL;
    while(*name) {
        if(glob&&*pattern=='*'){star=pattern++;retry=name;}
        else if((glob&&*pattern=='?')||(*pattern&&*pattern==*name)){++pattern;++name;}
        else if(star){pattern=star+1;name=++retry;}
        else {*out=false;return XR_XIR_OK;}
    }
    if(glob)while(*pattern=='*')++pattern;
    *out=!*pattern;return XR_XIR_OK;
}
static XrXirStatus ignore_name(const XlspConfig *config,const char *name,bool directory,bool *out) {
    if(!config||!name||!out||config->ignore_pattern_count<0||
        (config->ignore_pattern_count&&!config->ignore_patterns))return XR_XIR_BAD_STRUCTURE;
    if(name[0]=='.'){*out=true;return XR_XIR_OK;}
    for(int i=0;i<config->ignore_pattern_count;++i) {
        const XlspIgnorePattern *p=&config->ignore_patterns[i];if(!p->pattern)return XR_XIR_BAD_STRUCTURE;
        if(p->is_dir_only&&!directory)continue;bool match=false;
        XrXirStatus s=ignore_match(p->pattern,name,p->is_glob,&match);if(s!=XR_XIR_OK)return s;
        if(match){*out=true;return XR_XIR_OK;}
    }
    *out=false;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xlsp_source_ignore(XrCompileResources *r,const XlspConfig *config,const char *name,bool directory,bool *out) {
    if(!r)return XR_XIR_BAD_STRUCTURE;return ignore_name(config,name,directory,out);
}
XR_FUNC bool xlsp_config_should_ignore(XlspConfig *config,const char *name,bool directory) {
    bool ignored=false;return ignore_name(config,name,directory,&ignored)==XR_XIR_OK&&ignored;
}
