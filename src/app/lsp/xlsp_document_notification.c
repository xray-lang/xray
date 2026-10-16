/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_document_notification.c - Validate editor identity before table access
 */
#include "xlsp_document_notification.h"
#include "../../base/xutf8.h"
#include <limits.h>
#include <string.h>

static bool notification_member(const XrJsonValue *object, const char *name,
    const XrJsonValue **out) {
    if (!object || object->type != XR_JSON_OBJECT || object->as.object.count < 0 ||
        object->as.object.capacity < object->as.object.count ||
        (object->as.object.count && !object->as.object.members)) return false;
    size_t length = strlen(name);
    for (int i = 0; i < object->as.object.count; ++i) {
        const XrJsonMember *member = &object->as.object.members[i];
        if (!member->key || !member->value || member->key_len == SIZE_MAX ||
            !xr_utf8_validate(member->key, member->key_len)) return false;
        if (member->key_len == length && !memcmp(member->key, name, length)) {
            if (*out) return false;
            *out = member->value;
        }
    }
    return *out != NULL;
}
static bool notification_string(const XrJsonValue *value, bool c_name) {
    return value && value->type == XR_JSON_STRING && value->as.string &&
        value->string_len != SIZE_MAX && xr_utf8_validate(value->as.string, value->string_len) &&
        (!c_name || !memchr(value->as.string, 0, value->string_len));
}
XR_FUNC XrXirStatus xlsp_document_notification_decode(const XrJsonValue *params,
    XlspDocumentNotificationKind kind, XlspDocumentNotification *out) {
    if (!out || out->kind || out->uri || out->text || out->changes || out->version ||
        kind < XLSP_DOCUMENT_NOTIFICATION_OPEN || kind > XLSP_DOCUMENT_NOTIFICATION_CLOSE)
        return XR_XIR_BAD_STRUCTURE;
    const XrJsonValue *document = NULL, *uri = NULL, *version = NULL, *language = NULL;
    XlspDocumentNotification result = {.kind = kind};
    if (!notification_member(params, "textDocument", &document) ||
        !notification_member(document, "uri", &uri) || !notification_string(uri, true))
        return XR_XIR_BAD_STRUCTURE;
    result.uri = uri;
    if (kind != XLSP_DOCUMENT_NOTIFICATION_CLOSE) {
        if (!notification_member(document, "version", &version) || version->type != XR_JSON_NUMBER ||
            !version->is_integer || version->as.integer < INT32_MIN || version->as.integer > INT32_MAX)
            return XR_XIR_BAD_STRUCTURE;
        result.version = (int)version->as.integer;
    }
    if (kind == XLSP_DOCUMENT_NOTIFICATION_OPEN) {
        if (!notification_member(document, "languageId", &language) || !notification_string(language, true) ||
            !notification_member(document, "text", &result.text) || !notification_string(result.text, false))
            return XR_XIR_BAD_STRUCTURE;
    }
    if (kind == XLSP_DOCUMENT_NOTIFICATION_CHANGE) {
        if (!notification_member(params, "contentChanges", &result.changes) ||
            result.changes->type != XR_JSON_ARRAY || result.changes->as.array.count < 0 ||
            result.changes->as.array.capacity < result.changes->as.array.count ||
            (result.changes->as.array.count && !result.changes->as.array.items)) return XR_XIR_BAD_STRUCTURE;
    }
    *out = result;
    return XR_XIR_OK;
}
