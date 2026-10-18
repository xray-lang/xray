/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_on_type.h - Owned lexical indentation edits
 *
 * KEY CONCEPT:
 *   The canonical lexer authenticates the typed prefix. Incomplete lexical
 *   regions produce no edit, while incomplete programs can still be indented.
 */
#ifndef XLSP_SOURCE_ON_TYPE_H
#define XLSP_SOURCE_ON_TYPE_H
#include "xlsp_source_buffer.h"
typedef struct XlspSourceOnTypeInput {
    XrLspPosition position;
    uint32_t tab_size;
    bool insert_spaces;
    char trigger;
} XlspSourceOnTypeInput;
/* The original ledger covers capture, strict UTF16, lexer and full indentation.
 * Only leading whitespace is replaced. Results survive all producers.
 * A semicolon is a validated no-op; comments/literals are never brace syntax. */
XR_FUNC XrXirStatus xlsp_source_on_type_json(XrCompileResources *,const XlspSourceDocument *,
    const XlspSourceOnTypeInput *,XrJsonValue **);
#endif // XLSP_SOURCE_ON_TYPE_H
