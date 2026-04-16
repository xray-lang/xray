/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_keywords.h - Shared keyword and builtin definitions for LSP
 *
 * KEY CONCEPT:
 *   Centralized definitions to eliminate code duplication across
 *   xlsp_analysis.c and xlsp_completion.c.
 */

#ifndef XLSP_KEYWORDS_H
#define XLSP_KEYWORDS_H

// xray language keywords (defined in xlsp_keywords.c)
extern const char *xr_keywords[];

// Builtin functions (defined in xlsp_keywords.c)
extern const char *xr_builtins[];

#endif // XLSP_KEYWORDS_H
