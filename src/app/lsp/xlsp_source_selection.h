/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_SOURCE_SELECTION_H
#define XLSP_SOURCE_SELECTION_H
#include "xlsp_source_buffer.h"

/* Text selection is a display projection: Source identifier bytes, the exact
 * logical line, then the real document extent. It carries no symbol authority.
 * The transaction captures validated text on the supplied original ledger;
 * invalid UTF16 positions and any allocation/work failure preserve output.
 * Equal ancestors are omitted. JSON survives every text/ledger producer. */
XR_FUNC XrXirStatus xlsp_source_selection_json(XrCompileResources *,const char *,size_t,
    const XrJsonValue *,XrJsonValue **);
#endif // XLSP_SOURCE_SELECTION_H
