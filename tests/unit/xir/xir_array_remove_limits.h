/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_remove_limits.h - Actual domain and retain failure boundaries
 *
 * KEY CONCEPT:
 *   Restore injected ceilings before checking the unchanged root and alias.
 */
#ifndef XIR_ARRAY_REMOVE_LIMITS_H
#define XIR_ARRAY_REMOVE_LIMITS_H
static void remove_runtime_limits(RemoveCompile *run) {
    const size_t live = runtime_live, bytes = runtime_bytes;
    for (unsigned empty = 0; empty < 2; ++empty) {
        for (unsigned shift = 0; shift < 2; ++shift) {
            for (unsigned mode = 0; mode < 4; ++mode) {
                XrXirInstance *instance = remove_open(run);
                const char *saved = empty ? "savedEmpty" : shift ? "savedHead" : "savedTail";
                const char *method = empty ? (shift ? "removeEmptyShift" : "removeEmptyPop") : (shift ? "removeHead" : "removeTail");
                XrXirValue alias = remove_run(instance, remove_find(run->module, saved));
                XirObject *backing = object_pointer(&alias); CHECK(backing);
                _Atomic uint32_t *references = mode == 1 ? &backing->references :
                    mode == 2 ? &instance->domain->references : &run->program->arena->references;
                uint32_t original = atomic_load(references);
                uint64_t limit = instance->domain->limit;
                if (!mode) instance->domain->limit = instance->domain->stats.live_bytes;
                else atomic_store(references, UINT32_MAX);
                XrXirCallStatus status = xr_xir_instance_start(instance, remove_find(run->module, method), NULL, 0);
                const XrXirCallStatus started = status;
                if (status == XR_XIR_CALL_READY) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
                instance->domain->limit = limit;
                if (mode) atomic_store(references, original);
                if (status != XR_XIR_CALL_LIMIT) fprintf(stderr, "remove limit empty%u shift%u mode%u start%u status%u\n", empty, shift, mode, started, status);
                CHECK(status == XR_XIR_CALL_LIMIT);
                XrXirValue absent = {0}; CHECK(xr_xir_instance_take_result(instance, &absent) == XR_XIR_CALL_BAD_STATE && !absent.type && !absent.payload);
                if (empty) {
                    XrXirValue root = remove_run(instance, remove_find(run->module, saved));
                    CHECK(root.type == alias.type && root.payload == alias.payload); xr_xir_value_drop(&root);
                } else remove_state(instance, run, shift != 0, false);
                static const int64_t old[] = {1, 0, 3};
                remove_array(&alias, old, empty ? 0 : 3, !empty, 1); xr_xir_value_drop(&alias);
                CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY && runtime_live == live && runtime_bytes == bytes);
            }
        }
    }
    puts("remove empty/nonempty two methods16 domain/backing/domain-retain/arena limits root+alias unchanged physical0");
}
#endif // XIR_ARRAY_REMOVE_LIMITS_H
