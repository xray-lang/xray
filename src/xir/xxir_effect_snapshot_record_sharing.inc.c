/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_record_sharing.inc.c - Retained exact physical records
 *
 * KEY CONCEPT:
 *   Shared encoding carries no semantic product. The complete ten-word
 *   comparison precedes the same current-source semantic intake as writing.
 */
typedef struct EffectBodyRecordShareRequest {
    uint32_t function;
    const XrXirFunction *source;
    const EffectBodyWriteSink *sink;
} EffectBodyRecordShareRequest;
