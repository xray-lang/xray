/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_INITIALIZE_RESULT_H
#define XLSP_INITIALIZE_RESULT_H
#include "xlsp_server.h"
#include "../../base/xjson.h"
/* Returns a complete owned capability result or NULL; never a partial tree. */
typedef struct XlspInitializeOptions { bool prepare_rename, code_action_literals; } XlspInitializeOptions;
XR_FUNC XrJsonValue *xlsp_initialize_result(const XrLspServer *, const XlspInitializeOptions *);
#endif // XLSP_INITIALIZE_RESULT_H
