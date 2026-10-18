/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_file_storage.h - Original-state file records and scoped cursors
 */
#ifndef XANALYZER_FILE_STORAGE_H
#define XANALYZER_FILE_STORAGE_H
#include "xanalyzer.h"

/* The registry/global scope are real and live; file storage/list/count are
 * empty. The private index retains that registry, never a parser or VM. */
XR_FUNC XrCompileResourceStatus xa_analyzer_files_open(XaAnalyzer *analyzer);
/* Invalidates live cursor tokens before freeing records/index. Token storage
 * stays alive until pop or this cleanup. Scope children remain parent-owned;
 * immutable origin text lives through the registry's existing pool pin. */
XR_FUNC void xa_analyzer_files_close(XaAnalyzer *analyzer);
/* Existing owned metadata remains observable for invariant checks after a
 * failure. This scalar observation performs no resource admission. */
XR_FUNC uint32_t xa_analyzer_file_index_size(const XaAnalyzer *analyzer);
/* Healthy NULL paths are an explicit absence. Failures preserve outputs;
 * borrowed entries end at remove/close. Creation commits its index/list/parent
 * edge together, but consumed arena bytes are never rolled back. */
XR_FUNC XrCompileResourceStatus xa_analyzer_file_find(XaAnalyzer *analyzer,
    const char *path, XaFileEntry **output);
XR_FUNC XrCompileResourceStatus xa_analyzer_file_get_or_create(XaAnalyzer *analyzer,
    const char *path, XaScope *parent, XaFileEntry **output);
/* Removes the record/index only, after the semantic caller retires symbols.
 * Scope storage remains parent-owned; this is not whole-analysis rollback. */
XR_FUNC XrCompileResourceStatus xa_analyzer_file_remove_record(XaAnalyzer *analyzer,
    const char *path, bool *removed);
#endif // XANALYZER_FILE_STORAGE_H
