/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_program_final_roots.h - Independent literal checks against retained Final facts
 */
#ifndef XIR_PROGRAM_FINAL_ROOTS_H
#define XIR_PROGRAM_FINAL_ROOTS_H
#include "xir/xxir_program_internal.h"
#include "xir/xxir_effects.h"
static inline XrXirRootEffects test_program_final_roots(const XrXirProgram *program,uint32_t entry) {
    CHECK(program && program->permissions && entry<program->entry_count);
    CHECK(program->permissions->function_count==program->entry_count);
    XrXirEffectFacts facts=xr_xir_effects_read(program->permissions->effects,entry);
    CHECK(facts.status==XR_XIR_OK);return facts.root;
}
#endif // XIR_PROGRAM_FINAL_ROOTS_H
