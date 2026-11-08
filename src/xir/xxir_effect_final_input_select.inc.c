/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_final_input_select.inc.c - Current opcode selection for proof reads
 *
 * KEY CONCEPT:
 *   Every pass reaches actual canonical EOF. Only an actual opcode excludes
 *   an irrelevant row; selected operations still consume their complete row.
 */
enum { EFFECT_FINAL_SELECT_REFERENCE=1,EFFECT_FINAL_SELECT_EDGE=2,
    EFFECT_FINAL_SELECT_PRODUCER=3,EFFECT_FINAL_SELECT_INDIRECT=4,
    EFFECT_FINAL_SELECT_CAPTURE=5,EFFECT_FINAL_SELECT_WRITE=6,
    EFFECT_FINAL_SELECT_RETURN=7,EFFECT_FINAL_SELECT_BARRIER=8,
    EFFECT_FINAL_SELECT_RESULT=9 };
typedef struct EffectFinalInputSelect {
    EffectBodyRecordCursor cursor;
    uint32_t first,next,end,selection;
} EffectFinalInputSelect;

/* Numeric positions survive a caller's allocation or release callback. Each
 * read reopens the actual run and its neighboring boundary before decoding;
 * no borrowed run pointer or saved semantic verdict crosses that callback. */
