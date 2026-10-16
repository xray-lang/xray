/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLSP_SOURCE_SIGNATURE_H
#define XLSP_SOURCE_SIGNATURE_H
#include "xlsp_source_syntax.h"
typedef struct XlspSourceSignature {
    XlspSourceHover label;
    XlspSourceHover *parameters;
    size_t parameter_count,active_argument;
} XlspSourceSignature;
typedef struct XlspSourceSignatureSelection {
    XrCompileResources *resources;
    const char *uri;
    size_t uri_length;
    XlspSourceCallSyntax call;
} XlspSourceSignatureSelection;
/* Borrow document bytes only while the immutable snapshot lives. */
XR_FUNC XrXirStatus xlsp_source_snapshot_document(const XlspSourceSnapshot *,const char *,size_t,XlspSourceDocument *);
/* Complete Checked declarations supply names, modes and types. Missing receiver
 * substitutions or display facts remain explicit failures. Owned output pins the
 * original compiler ledger and survives every Source and syntax producer. */
XR_FUNC XrXirStatus xlsp_source_signature(const XlspSourceSnapshot *,const XlspSourceSignatureSelection *,XlspSourceSignature *);
XR_FUNC void xlsp_source_signature_free(XlspSourceSignature *);
typedef struct XlspSourceSignatureRequest {
    XrCompileResources *resources;
    XrCompilerSession *session;
    XlspSourceSnapshot *const *snapshots;
    size_t count;
    const char *uri;
    size_t uri_length;
    XrLspPosition position;
} XlspSourceSignatureRequest;
XR_FUNC XrXirStatus xlsp_source_signature_many_json(const XlspSourceSignatureRequest *,XrJsonValue **);
#endif // XLSP_SOURCE_SIGNATURE_H
