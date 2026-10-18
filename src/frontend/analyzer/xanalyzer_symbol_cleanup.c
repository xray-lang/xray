/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_symbol_cleanup.c - Non-admitting cleanup of symbol metadata
 */
#include "xanalyzer_symbol.h"
#include "xanalyzer_signature.h"
#include "../../base/xmalloc.h"

// Release dynamic fields owned by XaSymbolLinks (inline helper, no free of struct itself)
XR_FUNC void xa_symbol_links_dispose_contents(XaSymbolLinks *links) {
    if (!links)
        return;
    xa_symbol_links_clear_function_sig(links);
    xr_compile_state_free((void *) links->deprecated_message);
    links->deprecated_message = NULL;
    links->is_deprecated = false;
    xa_symbol_links_clear_type_params(links);
    if (links->enum_info) {
        xa_enum_info_free(links->enum_info);
        links->enum_info = NULL;
    }
    if (links->owns_class_info && links->class_info)
        xa_class_info_free(links->class_info);
    links->class_info = NULL;
    links->owns_class_info = false;
    XaRefLocation *ref = links->references;
    while (ref) {
        XaRefLocation *next = ref->next;
        xr_compile_state_free(ref);
        ref = next;
    }
    links->references = NULL;
    links->ref_count = 0;
}
