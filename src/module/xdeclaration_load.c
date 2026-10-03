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
#include "../base/xio_policy.h"

static XrDeclarationStatus file_status(XrFileReadStatus status) {
    switch (status) {
        case XR_FILE_READ_OK: return XR_DECLARATION_OK;
        case XR_FILE_READ_MISSING: return XR_DECLARATION_ABSENT;
        case XR_FILE_READ_FORBIDDEN: return XR_DECLARATION_FORBIDDEN;
        case XR_FILE_READ_LIMIT: return XR_DECLARATION_LIMIT;
        case XR_FILE_READ_OUT_OF_MEMORY: return XR_DECLARATION_OUT_OF_MEMORY;
        case XR_FILE_READ_IO: return XR_DECLARATION_IO;
        case XR_FILE_READ_BAD_ARGUMENT: return XR_DECLARATION_BAD_ARGUMENT;
    }
    return XR_DECLARATION_IO;
}

static XrDeclarationStatus parse_status(XrTomlParseStatus status) {
    switch (status) {
    case XR_TOML_PARSE_OK: return XR_DECLARATION_OK;
    case XR_TOML_PARSE_INVALID: return XR_DECLARATION_INVALID;
    case XR_TOML_PARSE_LIMIT: case XR_TOML_PARSE_BUDGET: return XR_DECLARATION_LIMIT;
    case XR_TOML_PARSE_OUT_OF_MEMORY: return XR_DECLARATION_OUT_OF_MEMORY;
    case XR_TOML_PARSE_IO: return XR_DECLARATION_IO;
    case XR_TOML_PARSE_BAD_ARGUMENT: return XR_DECLARATION_BAD_ARGUMENT;
    }
    return XR_DECLARATION_INVALID;
}
XR_FUNC XrDeclarationStatus xr_compile_declaration_manifest_load(
    XrCompileResources *resources, const char *physical_root,
    const XrDeclarationInputLimits *limits, XrDeclarationManifest **output) {
    if (!resources || !physical_root || !limits || !output) return XR_DECLARATION_BAD_ARGUMENT;
    XrOsIoPolicy policy = xr_compile_io_policy(resources);
    XrFileBytes bytes = {0};
    XrDeclarationStatus status = file_status(xr_os_io_read_under_root(&policy,
        physical_root, "xray.toml", limits->parsing.input_bytes, &bytes));
    if (status != XR_DECLARATION_OK) return status;
    XrTomlValue *document = NULL;
    status = parse_status(xtoml_parse_owned(&policy, bytes.data, bytes.size,
        &limits->parsing, &document));
    policy.free(policy.context, bytes.data);
    if (status != XR_DECLARATION_OK) return status;
    status = xr_compile_declaration_manifest_read(resources, document, limits->records, output);
    xtoml_owned_free(document);
    return status;
}
