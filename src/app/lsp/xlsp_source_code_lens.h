/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_SOURCE_CODE_LENS_H
#define XLSP_SOURCE_CODE_LENS_H
#include "xlsp_source_workspace.h"

/* The caller supplies the workspace's original ledger. Declarations and
 * deduplicated cross-document references come only from that Checked owner.
 * JSON owns its ranges and titles after workspace/input/ledger lease death.
 * Failure preserves output; no name counting or partial result is published. */
XR_FUNC XrXirStatus xlsp_source_code_lens_json(XrCompileResources *,
    XlspSourceWorkspace *,const char *,size_t,XrJsonValue **);

#endif // XLSP_SOURCE_CODE_LENS_H
