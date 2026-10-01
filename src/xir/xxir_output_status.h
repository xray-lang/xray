/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_output_status.h - Closed synchronous output failure domain
 *
 * KEY CONCEPT:
 *   Resource and admission failures never become a language write result.
 */
#ifndef XXIR_OUTPUT_STATUS_H
#define XXIR_OUTPUT_STATUS_H
typedef enum XrXirOutputStatus {
    XR_XIR_OUTPUT_OK = 0, XR_XIR_OUTPUT_OOM = 1, XR_XIR_OUTPUT_LIMIT = 2,
    XR_XIR_OUTPUT_ERROR = 3, XR_XIR_OUTPUT_BAD_ARGUMENT = 4, XR_XIR_OUTPUT_BAD_ABI = 5
} XrXirOutputStatus;
#endif // XXIR_OUTPUT_STATUS_H
