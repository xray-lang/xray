/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */
/* Compiler-private canonical source data; no runtime ABI or caller privilege. */
#ifndef XXIR_PRELUDE_SOURCE_INTERNAL_H
#define XXIR_PRELUDE_SOURCE_INTERNAL_H
#include <stdint.h>
#include "xxir_prelude_identity.h"
typedef struct XirPreludeEnum {
    const char *name;
    uint32_t native_id, variant_count, parameter_count, field_count;
    const char *const *variants;
} XirPreludeEnum;
typedef struct XirPreludeSource {
    const char *text;
    uint8_t input_sha256[32];
    const XirPreludeEnum *enums;
    uint32_t enum_count;
    uint8_t range_input_sha256[32];
} XirPreludeSource;
#include "../shared/xprelude_source.inc.c"
#endif
