/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule_position.h - Exact source diagnostic display coordinates
 *
 * KEY CONCEPT:
 *   Exact compiler byte positions project while their actual text is alive.
 *   UTF-16 coordinates carry display facts, never semantic authority.
 */
#ifndef XMODULE_POSITION_H
#define XMODULE_POSITION_H
#include "xmodule_identity.h"
typedef struct XrModuleSourcePosition {uint32_t line,column;} XrModuleSourcePosition;
/* One-based compiler bytes to zero-based UTF-16. CRLF is one editor line;
 * standalone CR advances the editor row while retaining compiler byte rules.
 * No allocation; invalid boundaries preserve output. */
XR_FUNC XrModuleStatus xr_module_source_position(
    const char *text,size_t length,int line,int column,XrModuleSourcePosition *out);
#endif // XMODULE_POSITION_H
