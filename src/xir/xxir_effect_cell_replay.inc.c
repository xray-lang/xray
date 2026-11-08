/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_cell_replay.inc.c - Call-local physical rows for complete Cell replay
 *
 * KEY CONCEPT:
 *   Every canonical field is decoded before the unchanged provider compares
 *   its complete proof. The private rows and borrowed metadata expire together;
 *   no permission or comparison result survives this read-only operation.
 */
typedef struct EffectCellReplayRows {
    XrXirModule module;
    XrXirFunction *functions;
} EffectCellReplayRows;
