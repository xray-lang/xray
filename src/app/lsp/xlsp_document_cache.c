/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_document_cache.c - Release document-owned display caches
 */
#include "xlsp_cache.h"
#include "xlsp_imports.h"
#include "../../base/xmalloc.h"

XR_FUNC void xlsp_free_document_cache(XrLspDocument *doc) {
    if (!doc)
        return;

    // Free diagnostics cache
    if (doc->cached_diagnostics) {
        xjson_free((XrJsonValue *) doc->cached_diagnostics);
        doc->cached_diagnostics = NULL;
    }
}

XR_FUNC void xlsp_free_imports(XlspImportInfo *imports) {
    while (imports) {
        XlspImportInfo *next = imports->next;
        xr_free(imports->module_name);
        xr_free(imports->import_path);
        xr_free(imports->resolved_path);
        xr_free(imports);
        imports = next;
    }
}

XR_FUNC void xlsp_invalidate_import_cache(XrLspDocument *doc) {
    if (!doc)
        return;
    if (doc->cached_imports) {
        xlsp_free_imports(doc->cached_imports);
        doc->cached_imports = NULL;
    }
    doc->imports_content_hash = XLSP_CONTENT_HASH_UNINITIALIZED;
}
