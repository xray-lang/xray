/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_document_notification.h - Unambiguous editor notification inputs
 */
#ifndef XLSP_DOCUMENT_NOTIFICATION_H
#define XLSP_DOCUMENT_NOTIFICATION_H
#include "../../base/xjson.h"
#include "../../xir/xxir.h"

typedef enum XlspDocumentNotificationKind {
    XLSP_DOCUMENT_NOTIFICATION_NONE,
    XLSP_DOCUMENT_NOTIFICATION_OPEN,
    XLSP_DOCUMENT_NOTIFICATION_CHANGE,
    XLSP_DOCUMENT_NOTIFICATION_CLOSE
} XlspDocumentNotificationKind;

/* All values borrow the immutable input DOM until the synchronous handler
 * captures them into the document transaction. Decoding allocates nothing,
 * rejects ambiguous selected fields, and preserves output on every failure. */
typedef struct XlspDocumentNotification {
    XlspDocumentNotificationKind kind;
    const XrJsonValue *uri, *text, *changes;
    int version;
} XlspDocumentNotification;
XR_FUNC XrXirStatus xlsp_document_notification_decode(const XrJsonValue *,
    XlspDocumentNotificationKind, XlspDocumentNotification *);
#endif // XLSP_DOCUMENT_NOTIFICATION_H
