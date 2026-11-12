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
        uint32_t panic_target = 0;
        if (block->panic) {
            if (block->panic >= body->block_count) {build->status = XR_XIR_BAD_STRUCTURE; return false;}
            panic_target = body->blocks[block->panic].frontier;
        }
        if (!effect_check_cleanup_exit(build,
                (EffectCheckCleanupExit){instruction, b, block->frontier, target, port})) return false;
        bool panic_port = false;
        for (uint32_t i = block->first; i <= instruction; ++i)
            panic_port |= xr_xir_instruction_has_panic_port(body->instructions[i].op) &&
                !xr_xir_instruction_cleanup_exit(body->instructions[i].op);
        if (panic_port && !effect_check_cleanup_exit(build,
                (EffectCheckCleanupExit){UINT32_MAX, b, block->frontier, panic_target, XIR_EXECUTION_PANIC})) return false;
    }
    roots->function[function].cleanup_count = roots->cleanup_exits - roots->function[function].cleanup_first;
    return true;
}

typedef struct EffectCleanupHandler { uint32_t block, frontier; } EffectCleanupHandler;

static bool effect_check_cleanup_use(EffectCheckBuild *build, EffectCheckCleanupUse use) {
    EffectCheckedRoots *roots = build->roots;
    if (roots->cleanup_uses == roots->cleanup_use_capacity) {
        EffectCheckCleanupUse *values = xir_compile_array_grow(build->memory, roots->cleanup_use,
            roots->cleanup_uses, sizeof(*values), &roots->cleanup_use_capacity, &build->status);
        if (!values) return false;
        roots->cleanup_use = values;
    }
    roots->cleanup_use[roots->cleanup_uses++] = use;
    return true;
}

static XrXirStatus effect_check_cleanup_exit_uses(EffectCheckBuild *build, uint32_t f,
    EffectCheckCleanupExit exit) {
    EffectCheckedRoots *roots = build->roots;
    EffectCheckFunction function = roots->function[f];
    EffectCleanupHandler *handlers = NULL;
    uint32_t count = 0, capacity = 0, frontier = exit.from, order = 0;
    bool normal_done = false;
    XrXirStatus status = XR_XIR_OK;
    for (;;) {
        normal_done |= frontier == exit.to;
        for (uint32_t h = 0; h < count;) {
            if (handlers[h].frontier == frontier) handlers[h] = handlers[--count];
            else ++h;
        }
        if (normal_done && !count) break;
        if (!effect_check_poll(build)) {status = build->status; break;}
        if (!frontier || frontier > function.values - function.parameters) {status = XR_XIR_BAD_STRUCTURE; break;}
        EffectCheckNode registration = roots->node[function.first + function.parameters + frontier - 1];
        if (registration.cleanup_call >= roots->calls || registration.cleanup_parent >= frontier) {
            status = XR_XIR_BAD_STRUCTURE; break;
        }
        for (uint32_t h = 0; h < count; ++h) {
            if (!effect_check_cleanup_use(build, (EffectCheckCleanupUse){f, exit.instruction,
                    exit.block, order, registration.cleanup_call, exit.port, true, handlers[h].block})) {
                status = build->status; break;
            }
        }
        if (status != XR_XIR_OK) break;
        if (!normal_done) {
            if (!effect_check_cleanup_use(build, (EffectCheckCleanupUse){f, exit.instruction,
                    exit.block, order, registration.cleanup_call, exit.port, false, registration.cleanup_handler})) {
                status = build->status; break;
            }
            if (exit.port != XIR_EXECUTION_PANIC) {
                bool known = false;
                for (uint32_t h = 0; h < count; ++h) known |= handlers[h].block == registration.cleanup_handler;
                if (!known) {
                    if (count == capacity) {
                        EffectCleanupHandler *values = xir_compile_array_grow(build->memory, handlers,
                            count, sizeof(*values), &capacity, &status);
                        if (!values) break;
                        handlers = values;
                    }
                    handlers[count++] = (EffectCleanupHandler){registration.cleanup_handler, registration.cleanup_panic_frontier};
                }
            }
        }
        ++order;
        frontier = registration.cleanup_parent;
    }
    xr_compile_resources_free(handlers);
    return status;
}

static XrXirStatus effect_check_cleanup_uses(EffectCheckBuild *build) {
    EffectCheckedRoots *roots = build->roots;
    roots->cleanup_uses = 0;
    for (uint32_t f = 0; f < roots->functions; ++f) {
        EffectCheckFunction function = roots->function[f];
        for (uint32_t e = 0; e < function.cleanup_count; ++e) {
            XrXirStatus status = effect_check_cleanup_exit_uses(build, f,
                roots->cleanup_exit[function.cleanup_first + e]);
            if (status != XR_XIR_OK) return status;
        }
    }
    return XR_XIR_OK;
}
