/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_watcher_registration.c - Complete owned dynamic-registration parameters
 */
#include "xlsp_watcher_registration.h"
#include "xlsp_index_json.h"
#include "xlsp_types.h"
static XrJsonValue *registration_array(XrJsonValue *child) {
    if (!child) return NULL;
    XrJsonValue *array = xjson_new_array(); if (!array) { xjson_free(child); return NULL; }
    xjson_array_push(array, child);
    if (array->as.array.count != 1) { xjson_free(child); xjson_free(array); return NULL; }
    return array;
}
static XrJsonValue *registration_watcher(void) {
    XrJsonValue *watcher = xjson_new_object(); if (!watcher) return NULL;
    if (!index_json_member(watcher, "globPattern", xjson_new_string("**/*.xr")) ||
        !index_json_member(watcher, "kind", xjson_new_number(LSP_WATCH_ALL))) { xjson_free(watcher); return NULL; }
    return watcher;
}
static XrJsonValue *registration_options(void) {
    XrJsonValue *options = xjson_new_object(); if (!options) return NULL;
    if (!index_json_member(options, "watchers", registration_array(registration_watcher()))) { xjson_free(options); return NULL; }
    return options;
}
static XrJsonValue *registration_entry(void) {
    XrJsonValue *entry = xjson_new_object(); if (!entry) return NULL;
    if (!index_json_member(entry, "id", xjson_new_string("xray-file-watcher")) ||
        !index_json_member(entry, "method", xjson_new_string("workspace/didChangeWatchedFiles")) ||
        !index_json_member(entry, "registerOptions", registration_options())) { xjson_free(entry); return NULL; }
    return entry;
}
XR_FUNC XrJsonValue *xlsp_watcher_registration_params(void) {
    XrJsonValue *params = xjson_new_object(); if (!params) return NULL;
    if (!index_json_member(params, "registrations", registration_array(registration_entry()))) { xjson_free(params); return NULL; }
    return params;
}
