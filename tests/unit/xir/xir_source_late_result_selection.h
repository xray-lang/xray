/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_late_result_selection.h - Source identity selection for existing fixture producers
 *
 * KEY CONCEPT:
 *   VM and native consume exports from the same Lowered fixture with independent expectations.
 */
#ifndef XIR_SOURCE_LATE_RESULT_SELECTION_H
#define XIR_SOURCE_LATE_RESULT_SELECTION_H
static SourceLateResultEntries source_late_result_select(const XrXirModule *module) {
    const char *names[] = {"lateResultNumber","lateResultText","lateResultArray","lateResultCount"};
    uint32_t selected[4] = {UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    for (uint32_t f = 0; f < module->function_count; ++f) for (uint32_t e = 0; e < 4; ++e) {
        const XrXirFunction *function = &module->functions[f];
        if (function->name_length != strlen(names[e]) || memcmp(function->name,names[e],function->name_length)) continue;
        CHECK(selected[e] == UINT32_MAX); selected[e] = f;
        CHECK(module->declarations->functions[f].exported && !function->parameter_count);
    }
    for (uint32_t e = 0; e < 4; ++e) CHECK(selected[e] != UINT32_MAX);
    return (SourceLateResultEntries){{selected[0],selected[1],selected[2]},selected[3]};
}
#endif // XIR_SOURCE_LATE_RESULT_SELECTION_H
