/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLSP_SOURCE_BUFFER_H
#define XLSP_SOURCE_BUFFER_H
#include "xlsp_source_query.h"
#include "../../base/xjson.h"
typedef struct XlspSourceBuffer XlspSourceBuffer;
/* Applies the complete notification sequentially to private storage. No output
 * on any invalid range/version/UTF8, allocation or work failure. Borrowed inputs
 * may die immediately after success. No parser or semantic result is implied. */
XR_FUNC XrXirStatus xlsp_source_buffer_new(XrCompileResources *,const char *,size_t,int,XlspSourceBuffer **);
XR_FUNC XrXirStatus xlsp_source_buffer_edit_json(XrCompileResources *,const char *,size_t,int,
    const XrJsonValue *,int,XlspSourceBuffer **);
XR_FUNC void xlsp_source_buffer_free(XlspSourceBuffer *);
XR_FUNC const char *xlsp_source_buffer_text(const XlspSourceBuffer *);
XR_FUNC size_t xlsp_source_buffer_length(const XlspSourceBuffer *);
XR_FUNC int xlsp_source_buffer_version(const XlspSourceBuffer *);
XR_FUNC const uint32_t *xlsp_source_buffer_lines(const XlspSourceBuffer *,int *);
#endif
