/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_workspace_declaration.c - Observe exact lexical declaration kinds
 *
 * KEY CONCEPT:
 *   All containing immutable roots agree before publishing a scalar observation.
 */
#include "xlsp_source_workspace_internal.h"
XR_FUNC XrXirStatus xlsp_source_workspace_declaration_kind(XlspSourceWorkspace *w,const char *uri,
    size_t length,XrLspPosition position,XrXirSourceDeclarationKind *out) {
    if(!w||!uri||length==SIZE_MAX||!out)return XR_XIR_BAD_STRUCTURE;
    bool found=false;XrXirSourceDeclarationKind accepted=XR_XIR_SOURCE_FUNCTION;
    for(size_t i=0;i<w->count;++i) {
        bool contains=false;XrXirStatus status=xlsp_source_snapshot_contains(w->snapshots[i],uri,length,&contains);
        if(status!=XR_XIR_OK)return status;if(!contains)continue;
        XrXirSourceDeclarationKind current=XR_XIR_SOURCE_FUNCTION;
        status=xlsp_source_declaration_kind(w->snapshots[i],uri,length,position,&current);
        if(status!=XR_XIR_OK)return status;
        if(found&&accepted!=current)return XR_XIR_BAD_STRUCTURE;
        accepted=current;found=true;
    }
    if(!found)return XR_XIR_UNRESOLVED;*out=accepted;return XR_XIR_OK;
}
