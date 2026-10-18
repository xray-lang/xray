/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_SOURCE_FORMAT_H
#define XLSP_SOURCE_FORMAT_H
#include "xlsp_source_buffer.h"
#include "../../frontend/format/xfmt.h"

/* Parse and format on the original supplied ledger. Syntax errors produce no
 * edits; resource/configuration failures preserve output. A changed document
 * gets one complete edit ending at the actual UTF16 EOF. Equal bytes produce
 * no edit. JSON owns all output after parser/Session/text/formatter death. */
XR_FUNC XrXirStatus xlsp_source_format_json(XrCompileResources *,const XlspSourceDocument *,
    const XrFmtConfig *,XrJsonValue **);
#endif // XLSP_SOURCE_FORMAT_H
