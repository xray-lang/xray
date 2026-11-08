/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_role_frame.inc.c - Current live ranges for role replay
 *
 * A ledger containment query never reads a proposed payload address. These
 * checks establish current geometry, not semantic permission or a reservation.
 */
typedef struct EffectBodyFrame {
    const XrXirCompileContext *work;
    const EffectBodyMaterial *material;
    uint32_t functions;
} EffectBodyFrame;
