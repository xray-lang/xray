/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdeclaration_load.c - Bounded manifest file, syntax and schema admission
 *
 * KEY CONCEPT:
 *   Discard temporary file/DOM owners before publishing copied selectors.
 */
#include "xdeclaration_load.h"
#include "../os/os_file_read.h"
#include "../base/xmalloc.h"

static XrDeclarationStatus file_status(XrFileReadStatus status) {
    switch (status) {
        case XR_FILE_READ_OK: return XR_DECLARATION_OK;
        case XR_FILE_READ_MISSING: return XR_DECLARATION_ABSENT;
        case XR_FILE_READ_FORBIDDEN: return XR_DECLARATION_FORBIDDEN;
        case XR_FILE_READ_LIMIT: return XR_DECLARATION_LIMIT;
        case XR_FILE_READ_OUT_OF_MEMORY: return XR_DECLARATION_OUT_OF_MEMORY;
        case XR_FILE_READ_IO: return XR_DECLARATION_IO;
    }
    return XR_DECLARATION_IO;
}

XR_FUNC XrDeclarationStatus xr_declaration_manifest_load(const char *physical_root,
    XrDeclarationInputBudget budget, XrDeclarationManifest **output, size_t *work_used) {
    if (work_used) *work_used = 0;
    if (output) *output = NULL;
    if (!output) return XR_DECLARATION_INVALID;
    XrFileBytes bytes = {0};
    XrDeclarationStatus status = file_status(xr_file_read_under_root(physical_root, "xray.toml",
        budget.parsing.input_bytes, &bytes));
    if (status != XR_DECLARATION_OK) return status;
    XrTomlParseStatus parsed;
    size_t parsed_work = 0, record_work = 0;
    XrTomlValue *document = xtoml_parse_limited(bytes.data, bytes.size, budget.parsing, &parsed, &parsed_work);
    xr_free(bytes.data);
    if (work_used) *work_used = parsed_work;
    if (!document) {
        if (parsed == XR_TOML_PARSE_LIMIT) return XR_DECLARATION_LIMIT;
        if (parsed == XR_TOML_PARSE_OUT_OF_MEMORY) return XR_DECLARATION_OUT_OF_MEMORY;
        return XR_DECLARATION_INVALID;
    }
    size_t remaining = budget.parsing.work - parsed_work;
    if (remaining < budget.records.work) budget.records.work = (uint32_t)remaining;
    status = xr_declaration_manifest_read(document, budget.records, output, &record_work);
    if (work_used) *work_used = parsed_work + record_work;
    xtoml_free(document);
    return status;
}
