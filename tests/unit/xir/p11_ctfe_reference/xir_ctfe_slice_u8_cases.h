/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_ctfe_slice_u8_cases.h - Independent byte and scope constant answers
 */
#ifndef XIR_CTFE_SLICE_U8_CASES_H
#define XIR_CTFE_SLICE_U8_CASES_H
typedef struct SliceConstantCase { const char *name; int64_t expected; } SliceConstantCase;
static const SliceConstantCase slice_constant_cases[] = {{"slice_u8_local",79},{"slice_u8_string",366}};
#endif // XIR_CTFE_SLICE_U8_CASES_H
