/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_execution_collect.inc.c - Actual ordered uses of registered cleanups
 */
static bool effect_check_cleanup_exit(EffectCheckBuild *build, EffectCheckCleanupExit exit) {
    if (exit.from == exit.to) return true;
    EffectCheckedRoots *roots = build->roots;
    if (roots->cleanup_exits == roots->cleanup_exit_capacity) {
        EffectCheckCleanupExit *values = xir_compile_array_grow(build->memory, roots->cleanup_exit,
            roots->cleanup_exits, sizeof(*values), &roots->cleanup_exit_capacity, &build->status);
        if (!values) return false;
        roots->cleanup_exit = values;
    }
    roots->cleanup_exit[roots->cleanup_exits++] = exit;
    return true;
}

static bool effect_check_cleanup_exits(EffectCheckBuild *build, uint32_t function) {
    const XrXirFunction *body = &build->module->functions[function];
    EffectCheckedRoots *roots = build->roots;
    roots->function[function].cleanup_first = roots->cleanup_exits;
    for (uint32_t b = 0; b < body->block_count; ++b) {
        const XrXirBlock *block = &body->blocks[b];
        if (!block->count || block->first >= body->instruction_count ||
            block->count > body->instruction_count - block->first) {build->status = XR_XIR_BAD_STRUCTURE; return false;}
        uint32_t instruction = block->first + block->count - 1;
        const XrXirInstruction *end = &body->instructions[instruction];
        uint32_t target = block->frontier;
        XirExecutionPort port = XIR_EXECUTION_NORMAL;
        if (end->op == XR_XIR_CLEANUP_LEAVE || end->op == XR_XIR_CLEANUP_ERROR) {
            target = (uint32_t)end->immediate;
            if (end->op == XR_XIR_CLEANUP_ERROR) port = XIR_EXECUTION_ERROR;
        } else if (end->op == XR_XIR_RETURN || end->op == XR_XIR_THROW) {
            target = 0;
            if (end->op == XR_XIR_THROW) port = XIR_EXECUTION_ERROR;
        }
        if (!effect_check_cleanup_exit(build, (EffectCheckCleanupExit){instruction, b, block->frontier, target, port}))
            return false;
        uint32_t panic_target = 0;
        if (block->panic) {
            if (block->panic >= body->block_count) {build->status = XR_XIR_BAD_STRUCTURE; return false;}
            panic_target = body->blocks[block->panic].frontier;
        }
        if (!effect_check_cleanup_exit(build,
                (EffectCheckCleanupExit){UINT32_MAX, b, block->frontier, panic_target, XIR_EXECUTION_PANIC})) return false;
    }
    roots->function[function].cleanup_count = roots->cleanup_exits - roots->function[function].cleanup_first;
    return true;
}

static XrXirStatus effect_check_cleanup_uses(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    roots->cleanup_uses = 0;
    for (uint32_t f = 0; f < roots->functions; ++f) {
        EffectCheckFunction function = roots->function[f];
        for (uint32_t e = 0; e < function.cleanup_count; ++e) {
            EffectCheckCleanupExit exit = roots->cleanup_exit[function.cleanup_first + e];
            uint32_t frontier = exit.from, order = 0;
            while (frontier != exit.to) {
                if (!effect_check_poll(build)) return build->status;
                if (!frontier || frontier > function.values - function.parameters) return XR_XIR_BAD_STRUCTURE;
                const EffectCheckNode *registration = &roots->node[function.first + function.parameters + frontier - 1];
                if (registration->cleanup_call >= roots->calls || registration->cleanup_parent >= frontier)
                    return XR_XIR_BAD_STRUCTURE;
                if (roots->cleanup_uses == roots->cleanup_use_capacity) {
                    EffectCheckCleanupUse *values = xir_compile_array_grow(build->memory, roots->cleanup_use,
                        roots->cleanup_uses, sizeof(*values), &roots->cleanup_use_capacity, &build->status);
                    if (!values) return build->status;
                    roots->cleanup_use = values;
                }
                roots->cleanup_use[roots->cleanup_uses++] = (EffectCheckCleanupUse){f, exit.instruction,
                    exit.block, order++, registration->cleanup_call, exit.port};
                frontier = registration->cleanup_parent;
            }
        }
    }
    return XR_XIR_OK;
}
