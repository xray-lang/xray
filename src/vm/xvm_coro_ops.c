/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_coro_ops.c - Dispatch helpers for coroutine operations
 *
 * Implements the coroutine control plane and the go / await
 * family. Declarations live in xvm_dispatch_helpers.h.
 *
 * Owns:
 *   - vm_collect_all_coros / vm_coro_ctrl
 *   - vm_get_coro (helper)
 *   - vm_go
 *   - vm_task_consume_result (helper)
 *   - vm_await / vm_await_timeout / vm_await_all / vm_await_any
 */

#include "xvm_dispatch_helpers.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../os/os_time.h"
#include "../runtime/value/xstruct_layout.h"
#include "xvm_checks.h"
#include "xdebug.h"
#include "../runtime/xray_debug_hooks.h"
#include "../runtime/xstrbuf.h"
#include "../runtime/object/xstringbuilder.h"

#include "../runtime/object/xjson.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/object/xrange.h"
#include "../base/xutf8.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_feedback.h"
#include "../coro/xcoro_pool.h"
#include "../coro/xtask.h"

/* ========== Helper: Collect all coroutines from the runtime ========== */

// VmCoroEntry and VM_CORO_COLLECT_MAX defined in xvm_dispatch_helpers.h

static XrValue vm_coro_name_value(XrayIsolate *isolate, const XrCoroutine *coro) {
    const char *name = xr_coro_name(coro);
    if (!name)
        return xr_null();
    XrString *s = xr_string_intern(isolate, name, strlen(name), 0);
    return s ? xr_string_value(s) : xr_null();
}

static void vm_coro_set_source_field(XrayIsolate *isolate, XrMap *info, const XrCoroutine *coro) {
    const char *source_file = xr_coro_source_file(coro);
    if (!source_file)
        return;
    char source_buf[XR_MAX_PROPERTY_NAME_LEN];
    snprintf(source_buf, sizeof(source_buf), "%s:%d", source_file, xr_coro_source_line(coro));
    XrString *source = xr_string_intern(isolate, source_buf, strlen(source_buf), 0);
    if (source)
        xr_map_set(info, VM_INTERN_KEY("source"), xr_string_value(source));
}

// Collect coroutines from runtime queues into a flat array for diagnostic sub-ops.
// Returns the number of entries written. Best-effort snapshot (not atomic).
int vm_collect_all_coros(XrayIsolate *isolate, VmCoroEntry *out, int max_out) {
    XR_DCHECK(isolate != NULL, "vm_collect_all_coros: NULL isolate");
    XrRuntime *runtime = (XrRuntime *) isolate->vm.runtime;
    if (!runtime)
        return 0;
    return xr_runtime_collect_coros(runtime, out, max_out);
}

/* ========== Dispatch: OP_CORO_CTRL Sub-operations ========== */

XR_FUNC XrDispatchAction vm_coro_ctrl(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base) {
    XR_DCHECK(isolate != NULL, "vm_coro_ctrl: NULL isolate");
    XR_DCHECK(base != NULL, "vm_coro_ctrl: NULL base");
    int coro_sub = GETARG_C(instr);
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);

    switch (coro_sub) {
        case CORO_CTRL_STATS: {
            XrRuntime *runtime = (XrRuntime *) isolate->vm.runtime;
            if (!runtime) {
                base[a] = xr_null();
                return XR_DISP_NEXT;
            }

            int blocked_count = 0, ready_count = 0, active_count = 0;
            uint64_t total_created = 0;
            for (int _si = 0; _si < runtime->worker_count; _si++)
                total_created += runtime->workers[_si].p.stats.spawned_count;

            VmCoroEntry *entries = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
            if (entries) {
                int total = vm_collect_all_coros(isolate, entries, VM_CORO_COLLECT_MAX);
                for (int i = 0; i < total; i++) {
                    if (strcmp(entries[i].state, "ready") == 0)
                        ready_count++;
                    else if (strcmp(entries[i].state, "blocked") == 0)
                        blocked_count++;
                    else if (strcmp(entries[i].state, "running") == 0)
                        active_count++;
                }
                xr_free(entries);
            } else {
                active_count = xr_runtime_active_coros(runtime);
                for (int wi = 0; wi < runtime->worker_count; wi++) {
                    XrWorker *w = &runtime->workers[wi];
                    blocked_count += w->p.blocked_count;
                    for (int p = 0; p < XR_RUNQ_COUNT; p++) {
                        ready_count += xr_runq_len(&w->p.runq[p]);
                    }
                }
            }

            int total_alive = ready_count + blocked_count + active_count;
            XrMap *result = xr_map_new(vm_get_coro(vm_ctx));
            xr_map_set(result, VM_INTERN_KEY("active"), xr_int(active_count));
            xr_map_set(result, VM_INTERN_KEY("blocked"), xr_int(blocked_count));
            xr_map_set(result, VM_INTERN_KEY("ready"), xr_int(ready_count));
            xr_map_set(result, VM_INTERN_KEY("total"), xr_int(total_alive));
            xr_map_set(result, VM_INTERN_KEY("created"), xr_int((int) total_created));
            base[a] = xr_value_from_map(result);
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_LIST: {
            int limit = 0;
            int state_filter = 0;  // 0=all, 1=ready, 2=blocked

            XrValue limit_val = base[b];
            if (XR_IS_INT(limit_val))
                limit = (int) XR_TO_INT(limit_val);
            if (limit <= 0)
                limit = 100000;

            XrValue state_val = base[a + 1];
            if (XR_IS_INT(state_val)) {
                state_filter = (int) XR_TO_INT(state_val);
            } else if (XR_IS_STRING(state_val)) {
                XrString *s = (XrString *) XR_TO_PTR(state_val);
                if (strcmp(s->data, "ready") == 0)
                    state_filter = 1;
                else if (strcmp(s->data, "blocked") == 0)
                    state_filter = 2;
            }

            VmCoroEntry *entries = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
            if (!entries) {
                base[a] = xr_value_from_array(xr_array_new(vm_get_coro(vm_ctx)));
                return XR_DISP_NEXT;
            }
            int total = vm_collect_all_coros(isolate, entries, VM_CORO_COLLECT_MAX);

            XrArray *result = xr_array_new(vm_get_coro(vm_ctx));
            int count = 0;

            for (int i = 0; i < total && count < limit; i++) {
                XrCoroutine *coro = entries[i].coro;
                const char *st = entries[i].state;
                bool is_ready = (strcmp(st, "ready") == 0);
                bool is_blocked = (strcmp(st, "blocked") == 0);

                if (state_filter == 1 && !is_ready)
                    continue;
                if (state_filter == 2 && !is_blocked)
                    continue;

                XrMap *info = xr_map_new(vm_get_coro(vm_ctx));
                xr_map_set(info, VM_INTERN_KEY("id"), xr_int(coro->id));
                xr_map_set(info, VM_INTERN_KEY("name"), vm_coro_name_value(isolate, coro));
                xr_map_set(info, VM_INTERN_KEY("state"),
                           xr_string_value(xr_string_intern(isolate, st, strlen(st), 0)));
                vm_coro_set_source_field(isolate, info, coro);
                xr_array_push(result, xr_value_from_map(info));
                count++;
            }

            xr_free(entries);
            base[a] = xr_value_from_array(result);
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_INFO: {
            XrValue coro_val = base[b];
            if (!xr_value_is_coro(coro_val)) {
                base[a] = xr_null();
                return XR_DISP_NEXT;
            }

            XrCoroutine *coro = xr_value_to_coro(coro_val);
            XrMap *info = xr_map_new(vm_get_coro(vm_ctx));
            uint32_t flags = xr_coro_flags_load(coro);

            xr_map_set(info, VM_INTERN_KEY("id"), xr_int(coro->id));
            xr_map_set(info, VM_INTERN_KEY("name"), vm_coro_name_value(isolate, coro));

            const char *state_str = "unknown";
            if (flags & XR_CORO_FLG_DONE)
                state_str = "done";
            else if (flags & XR_CORO_FLG_BLOCKED)
                state_str = "blocked";
            else if (flags & XR_CORO_FLG_RUNNING)
                state_str = "running";
            else if (flags & XR_CORO_FLG_READY)
                state_str = "ready";
            xr_map_set(info, VM_INTERN_KEY("state"),
                       xr_string_value(xr_string_intern(isolate, state_str, strlen(state_str), 0)));

            xr_map_set(info, VM_INTERN_KEY("priority"), xr_int(xr_coro_get_priority(flags)));
            xr_map_set(info, VM_INTERN_KEY("reductions"), xr_int(coro->reductions));

            vm_coro_set_source_field(isolate, info, coro);

            struct XrMap *coro_locals = (coro->ext) ? coro->ext->locals : NULL;
            if (coro_locals) {
                xr_map_set(info, VM_INTERN_KEY("locals"), xr_value_from_map(coro_locals));
            } else {
                xr_map_set(info, VM_INTERN_KEY("locals"),
                           xr_value_from_map(xr_map_new(vm_get_coro(vm_ctx))));
            }

            const XrCoroWaitState *wait = xr_coro_wait_state_const(coro);
            int wait_count = wait ? atomic_load(&wait->wait_count) : 0;
            xr_map_set(info, VM_INTERN_KEY("waitCount"), xr_int(wait_count));
            xr_map_set(info, VM_INTERN_KEY("cancelled"), xr_bool(flags & XR_CORO_FLG_CANCELLED));

            if (flags & XR_CORO_FLG_DONE) {
                xr_map_set(info, VM_INTERN_KEY("result"), coro->result);
            }
            if (flags & XR_CORO_FLG_BLOCKED) {
                void *wait_channel =
                    coro->ext ? atomic_load_explicit(&coro->ext->wait_channel, memory_order_acquire)
                              : NULL;
                const char *reason = wait_channel ? "channel" : "await";
                xr_map_set(info, VM_INTERN_KEY("blockedOn"),
                           xr_string_value(xr_string_intern(isolate, reason, strlen(reason), 0)));
            }

            base[a] = xr_value_from_map(info);
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_DUMP: {
            int limit = a;  // A = limit
            if (limit == 0)
                limit = 100;

            VmCoroEntry *entries = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
            if (!entries)
                return XR_DISP_NEXT;
            int total = vm_collect_all_coros(isolate, entries, VM_CORO_COLLECT_MAX);

            int ready_count = 0, blocked_count = 0;
            for (int i = 0; i < total; i++) {
                if (strcmp(entries[i].state, "ready") == 0)
                    ready_count++;
                else if (strcmp(entries[i].state, "blocked") == 0)
                    blocked_count++;
            }

            printf("┌─────────────────────────────────────────────────────────────────────────┐\n");
            printf("│                     Coroutine Status Snapshot                           │\n");
            printf("├─────────────────────────────────────────────────────────────────────────┤\n");
            printf("│ Stats: Total %-5d | Ready %-4d | Blocked %-4d                        │\n",
                   total, ready_count, blocked_count);
            printf("├──────┬────────────────┬─────────┬─────────────────┬─────────────────────┤\n");
            printf("│ ID   │ Name           │ State   │ Block Reason    │ Location            │\n");
            printf("├──────┼────────────────┼─────────┼─────────────────┼─────────────────────┤\n");

            int shown = 0;
            for (int i = 0; i < total && shown < limit; i++) {
                XrCoroutine *coro = entries[i].coro;
                const char *state_upper = "READY";
                if (strcmp(entries[i].state, "blocked") == 0)
                    state_upper = "BLOCKED";
                else if (strcmp(entries[i].state, "running") == 0)
                    state_upper = "RUNNING";
                const char *block_reason = "-";
                if (strcmp(entries[i].state, "blocked") == 0) {
                    void *wait_channel = coro->ext ? atomic_load_explicit(&coro->ext->wait_channel,
                                                                          memory_order_acquire)
                                                   : NULL;
                    block_reason = wait_channel ? "channel" : "await";
                }

                const char *name = xr_coro_name(coro);
                name = name ? name : "(anonymous)";
                char name_buf[15];
                snprintf(name_buf, sizeof(name_buf), "%.14s", name);

                char source_buf[20] = "-";
                const char *source_file = xr_coro_source_file(coro);
                if (source_file) {
                    const char *fname = strrchr(source_file, '/');
                    fname = fname ? fname + 1 : source_file;
                    snprintf(source_buf, sizeof(source_buf), "%.12s:%d", fname,
                             xr_coro_source_line(coro));
                }

                printf("│ %-4d │ %-14s │ %-7s │ %-15s │ %-19s │\n", coro->id, name_buf, state_upper,
                       block_reason, source_buf);
                shown++;
            }

            printf("└──────┴────────────────┴─────────┴─────────────────┴─────────────────────┘\n");
            xr_free(entries);
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_STALLED: {
            (void) b;
            XrCoroState *sched = (XrCoroState *) isolate->vm.coro_state;
            if (!sched) {
                base[a] = xr_value_from_array(xr_array_new(vm_get_coro(vm_ctx)));
                return XR_DISP_NEXT;
            }
            XrArray *result = xr_array_new(vm_get_coro(vm_ctx));
            base[a] = xr_value_from_array(result);
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_DEADLOCKS: {
            XrCoroState *sched = (XrCoroState *) isolate->vm.coro_state;
            if (!sched) {
                base[a] = xr_value_from_array(xr_array_new(vm_get_coro(vm_ctx)));
                return XR_DISP_NEXT;
            }
            XrArray *result = xr_array_new(vm_get_coro(vm_ctx));
            base[a] = xr_value_from_array(result);
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_TOP: {
            int top_n = 10;
            int metric = 0;  // 0=id, 2=reductions

            XrValue n_val = base[b];
            if (XR_IS_INT(n_val)) {
                top_n = (int) XR_TO_INT(n_val);
                if (top_n <= 0)
                    top_n = 10;
                if (top_n > 1000)
                    top_n = 1000;
            }

            XrValue metric_val = base[a + 1];
            if (XR_IS_STRING(metric_val)) {
                XrString *s = (XrString *) XR_TO_PTR(metric_val);
                if (strcmp(s->data, "reductions") == 0)
                    metric = 2;
                else if (strcmp(s->data, "id") == 0)
                    metric = 0;
            }

            typedef struct {
                XrCoroutine *coro;
                const char *state;
                int64_t value;
            } TopEntry;
            TopEntry *entries = xr_malloc(sizeof(TopEntry) * VM_CORO_COLLECT_MAX);
            if (!entries) {
                base[a] = xr_value_from_array(xr_array_new(vm_get_coro(vm_ctx)));
                return XR_DISP_NEXT;
            }
            memset(entries, 0, sizeof(TopEntry) * VM_CORO_COLLECT_MAX);

            VmCoroEntry *raw = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
            if (!raw) {
                xr_free(entries);
                base[a] = xr_value_from_array(xr_array_new(vm_get_coro(vm_ctx)));
                return XR_DISP_NEXT;
            }
            int count = vm_collect_all_coros(isolate, raw, VM_CORO_COLLECT_MAX);
            for (int i = 0; i < count; i++) {
                entries[i].coro = raw[i].coro;
                entries[i].state = raw[i].state;
                entries[i].value = (metric == 2) ? raw[i].coro->reductions : raw[i].coro->id;
            }
            xr_free(raw);

            // Partial selection sort for top N
            for (int j = 0; j < top_n && j < count; j++) {
                int max_idx = j;
                for (int k = j + 1; k < count; k++) {
                    if (entries[k].value > entries[max_idx].value)
                        max_idx = k;
                }
                if (max_idx != j) {
                    TopEntry tmp = entries[j];
                    entries[j] = entries[max_idx];
                    entries[max_idx] = tmp;
                }
            }

            XrArray *result = xr_array_new(vm_get_coro(vm_ctx));
            int result_count = (top_n < count) ? top_n : count;
            for (int j = 0; j < result_count; j++) {
                XrCoroutine *coro = entries[j].coro;
                XrMap *info = xr_map_new(vm_get_coro(vm_ctx));
                xr_map_set(info, VM_INTERN_KEY("id"), xr_int(coro->id));
                xr_map_set(info, VM_INTERN_KEY("name"), vm_coro_name_value(isolate, coro));
                xr_map_set(info, VM_INTERN_KEY("state"),
                           xr_string_value(xr_string_intern(isolate, entries[j].state,
                                                            strlen(entries[j].state), 0)));
                xr_map_set(info, VM_INTERN_KEY("reductions"), xr_int(coro->reductions));
                xr_map_set(info, VM_INTERN_KEY("priority"),
                           xr_int(xr_coro_get_priority(xr_coro_flags_load(coro))));
                vm_coro_set_source_field(isolate, info, coro);
                xr_array_push(result, xr_value_from_map(info));
            }

            xr_free(entries);
            base[a] = xr_value_from_array(result);
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_GROUP_BY: {
            int group_by = 0;  // 0=name, 1=state, 2=priority

            XrValue field_val = base[b];
            if (XR_IS_STRING(field_val)) {
                XrString *s = (XrString *) XR_TO_PTR(field_val);
                if (strcmp(s->data, "state") == 0)
                    group_by = 1;
                else if (strcmp(s->data, "priority") == 0)
                    group_by = 2;
            }

            VmCoroEntry *entries = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
            if (!entries) {
                base[a] = xr_value_from_map(xr_map_new(vm_get_coro(vm_ctx)));
                return XR_DISP_NEXT;
            }
            int total = vm_collect_all_coros(isolate, entries, VM_CORO_COLLECT_MAX);

            XrMap *result = xr_map_new(vm_get_coro(vm_ctx));

            for (int i = 0; i < total; i++) {
                XrCoroutine *coro = entries[i].coro;
                const char *key_str;
                char prio_str[16];
                if (group_by == 0) {
                    key_str = xr_coro_name(coro);
                    if (!key_str)
                        key_str = "(anonymous)";
                } else if (group_by == 1) {
                    key_str = entries[i].state;
                } else {
                    snprintf(prio_str, sizeof(prio_str), "P%d",
                             xr_coro_get_priority(xr_coro_flags_load(coro)));
                    key_str = prio_str;
                }
                XrValue key =
                    xr_string_value(xr_string_intern(isolate, key_str, strlen(key_str), 0));
                bool found = false;
                XrValue existing = xr_map_get(result, key, &found);
                if (found && XR_IS_INT(existing)) {
                    xr_map_set(result, key, xr_int(XR_TO_INT(existing) + 1));
                } else {
                    xr_map_set(result, key, xr_int(1));
                }
            }

            xr_free(entries);
            base[a] = xr_value_from_map(result);
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_WHEREIS: {
            XrValue name_val = base[b];
            if (!XR_IS_STRING(name_val)) {
                base[a] = xr_bool(false);
                return XR_DISP_NEXT;
            }
            const char *name_cstr = xr_value_str_data(&name_val);
            XrCoroState *sched = (XrCoroState *) isolate->vm.coro_state;
            if (!sched || !sched->coro_registry) {
                base[a] = xr_bool(false);
                return XR_DISP_NEXT;
            }
            XrCoroutine *found = xr_coro_registry_whereis(sched->coro_registry, name_cstr);
            base[a] = xr_bool(found != NULL);
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_MONITOR: {
            XrValue name_val = base[b];
            if (!XR_IS_STRING(name_val)) {
                base[a] = xr_null();
                return XR_DISP_NEXT;
            }
            const char *name_cstr = xr_value_str_data(&name_val);
            XrCoroState *sched = (XrCoroState *) isolate->vm.coro_state;
            if (!sched || !sched->coro_registry) {
                base[a] = xr_null();
                return XR_DISP_NEXT;
            }
            XrChannel *ch = xr_coro_monitor(isolate, sched->coro_registry, name_cstr);
            base[a] = ch ? xr_value_from_channel(ch) : xr_null();
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_DEMONITOR: {
            XrValue name_val = base[b];
            if (!XR_IS_STRING(name_val)) {
                base[a] = xr_null();
                return XR_DISP_NEXT;
            }
            const char *name_cstr = xr_value_str_data(&name_val);
            XrValue ch_val = base[a + 1];
            if (!xr_value_is_channel(ch_val)) {
                base[a] = xr_null();
                return XR_DISP_NEXT;
            }
            XrChannel *ch = xr_value_to_channel(ch_val);
            XrCoroState *sched = (XrCoroState *) isolate->vm.coro_state;
            if (!sched || !sched->coro_registry) {
                base[a] = xr_null();
                return XR_DISP_NEXT;
            }
            XrCoroutine *coro = xr_coro_registry_whereis(sched->coro_registry, name_cstr);
            if (coro) {
                xr_coro_demonitor(sched->coro_registry, coro, ch);
            }
            base[a] = xr_null();
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_KILL: {
            XrValue name_val = base[b];
            if (!XR_IS_STRING(name_val)) {
                base[a] = xr_bool(false);
                return XR_DISP_NEXT;
            }
            const char *name_cstr = xr_value_str_data(&name_val);
            XrCoroState *sched = (XrCoroState *) isolate->vm.coro_state;
            if (!sched || !sched->coro_registry) {
                base[a] = xr_bool(false);
                return XR_DISP_NEXT;
            }
            XrCoroutine *target = xr_coro_registry_whereis(sched->coro_registry, name_cstr);
            if (!target || xr_coro_flags_has(target, XR_CORO_FLG_DONE)) {
                base[a] = xr_bool(false);
                return XR_DISP_NEXT;
            }
            xr_coro_flags_set(target, XR_CORO_FLG_CANCEL_REQUESTED);
            xr_coro_request_yield(target);
            base[a] = xr_bool(true);
            return XR_DISP_NEXT;
        }

        case CORO_CTRL_SELF: {
            XrCoroutine *current = vm_get_coro(vm_ctx);
            const char *name = xr_coro_name(current);
            if (name) {
                size_t len = strlen(name);
                XrString *s = xr_string_intern(isolate, name, len, 0);
                base[a] = s ? xr_string_value(s) : xr_null();
            } else {
                base[a] = xr_null();
            }
            return XR_DISP_NEXT;
        }

        default:
            return XR_DISP_NEXT;
    }
}
/* ========== Dispatch: Coroutine Operations ========== */

// vm_get_coro lives in xvm_dispatch_helpers.h so the invoke /
// props / chan-ops TUs can call it without an owning .c
// file having to re-export it.

XR_FUNC XrDispatchAction vm_go(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                               XrValue *base, XrBcCallFrame *frame) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c_raw = GETARG_C(instr);
    // C bit 7: fire-and-forget flag (go used as statement, result never awaited)
    bool fire_and_forget = (c_raw & 0x80) != 0;
    int c = c_raw & 0x7F;  // actual argument count
    XrInstruction *pc = frame->pc;

    XrValue fn_val = base[b];
    if (!xr_value_is_closure(fn_val)) {
        VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "go: expected closure");
    }

    struct XrClosure *closure = xr_value_to_closure(fn_val);
    XrProto *proto = closure->proto;

    if (!proto->is_coro_safe) {
        VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "go: closure captures non-thread-safe variables");
    }
    if (c != proto->numparams) {
        VM_THROW(frame, pc, XR_ERR_WRONG_ARG_COUNT,
                 "go: argument count mismatch (expected %d, got %d)", proto->numparams, c);
    }

    // Fast path: skip debug info computation, parse NOP annotations inline
    const char *coro_name = NULL;
    int coro_priority = 1;

    XrInstruction next_inst = *pc;
    int link_mode = 0;

    if (GET_OPCODE(next_inst) == OP_NOP && GETARG_A(next_inst) == 1) {
        int name_idx = GETARG_Bx(next_inst);
        XrValue name_val = PROTO_CONSTANT(frame->closure->proto, name_idx);
        if (XR_IS_STRING(name_val))
            coro_name = xr_value_to_string(isolate, name_val)->data;
        pc++;
        next_inst = *pc;
    }
    if (GET_OPCODE(next_inst) == OP_NOP && GETARG_A(next_inst) == 2) {
        coro_priority = GETARG_Bx(next_inst);
        pc++;
        next_inst = *pc;
    }
    if (GET_OPCODE(next_inst) == OP_NOP && GETARG_A(next_inst) == 3) {
        link_mode = GETARG_Bx(next_inst);
        pc++;
        next_inst = *pc;
    }
    XrValue *args = (c > 0) ? &base[b + 1] : NULL;
    // Debug source metadata is populated lazily for named coroutines.
    XrCoroutine *coro = xr_coro_create_vm_closure(isolate, closure, args, c, coro_name, NULL, 0);
    if (!coro) {
        VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to create coroutine");
    }

    if (coro_priority != 1) {
        xr_coro_set_priority(coro, coro_priority);
    }
    XrRuntime *runtime = (XrRuntime *) isolate->vm.runtime;
    if (!runtime) {
        VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: Runtime not initialized");
    }

    XrCoroutine *parent = vm_get_coro(vm_ctx);

    // Allocate GC-managed Task handle on executor's heap
    XrTask *task = xr_task_create(parent, coro);
    if (!task) {
        VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to create task");
    }

    // Store link_mode on task for runtime association
    task->link_mode = (uint8_t) link_mode;

    // Mark fire-and-forget coros as recyclable for deferred recycle
    if (fire_and_forget)
        coro->gc_flags |= XR_CORO_GC_RECYCLABLE;

    if (parent && !xr_coro_set_pending_spawn(parent, coro)) {
        xr_coro_destroy(coro);
        VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to record spawned coroutine");
    }

    // Scope tracking: link coro to scope
    {
        XrScopeContext *_scope = parent ? parent->current_scope : NULL;
        if (!_scope && runtime->current_scope)
            _scope = runtime->current_scope;
        if (_scope && parent) {
            if (!xr_coro_set_parent_scope(coro, _scope)) {
                (void) xr_coro_set_pending_spawn(parent, NULL);
                xr_coro_destroy(coro);
                VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to attach coroutine to scope");
            }
            // Protect child list prepend with the scope spinlock
            while (atomic_exchange_explicit(&_scope->child_lock, true, memory_order_acquire)) {
            }
            if (!xr_coro_set_scope_sibling(coro, _scope->first_child)) {
                atomic_store_explicit(&_scope->child_lock, false, memory_order_release);
                (void) xr_coro_set_pending_spawn(parent, NULL);
                xr_coro_destroy(coro);
                VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to attach coroutine to scope");
            }
            _scope->first_child = coro;
            atomic_store_explicit(&_scope->child_lock, false, memory_order_release);
            atomic_fetch_add_explicit(&_scope->count, 1, memory_order_relaxed);

            // In linked scope, auto-link children for error propagation.
            // Supervisor scope children stay XR_LINK_NONE — errors are collected
            // by the scope itself, no per-child propagation needed.
            if (link_mode == XR_LINK_NONE && _scope->mode == XR_SCOPE_LINKED) {
                task->link_mode = XR_LINK_LINKED;
            }
        }
    }

    /* linked go (standalone, NOT in scope): establish parent-child Task hierarchy.
     * Scope children use scope-based error propagation (first_error + SCOPE_EXIT).
     * Only standalone linked go uses Task hierarchy (fail_with_propagation). */
    if (task->link_mode == XR_LINK_LINKED && parent && parent->task &&
        !xr_coro_parent_scope(coro)) {
        xr_task_attach_child(parent->task, task);
    }

    /* monitored go: no auto-Channel here. Codegen only allocates one register
     * for go result, writing base[a+1] would corrupt the stack frame.
     * Users should call task.monitor() to get the notification Channel. */

    base[a] = xr_value_from_task(task);
    frame->pc = pc;

    // xr_coro_init_shell already sets XR_CORO_FLG_READY.
    if (!parent) {
        xr_runtime_spawn(runtime, coro);
        return XR_DISP_NEXT;
    }

    return XR_DISP_GO_CHILD;
}

#define AWAIT_TIMEOUT_SPINS 100000000

/* Read task->result with deep copy to dst_coro's heap, then detach executor.
 * After this call, task->result points to the copied value (safe for re-await). */
static inline XrValue vm_task_consume_result(XrayIsolate *isolate, XrTask *task,
                                             XrCoroutine *dst_coro, int discard_result) {
    XrValue res = xr_coro_await_result_value(isolate, dst_coro, task, discard_result != 0);
    /* Detach executor only — do NOT recycle.
     * Task lives on executor's Immix heap; parent's tasks array still
     * references it. Recycling frees the Immix block, causing
     * use-after-free when parent's GC scans the dangling Task pointer. */
    XrCoroutine *exec = task->coro;
    if (exec) {
        task->coro = NULL;
        exec->task = NULL;
    }
    return discard_result ? xr_null() : res;
}

static inline void vm_task_detach_completed_executor(XrTask *task) {
    if (!task)
        return;
    uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
    if (tstate != XR_TASK_COMPLETED)
        return;

    XrCoroutine *exec = task->coro;
    if (exec) {
        task->coro = NULL;
        exec->task = NULL;
    }
}

XR_FUNC XrDispatchAction vm_await(XrayIsolate *isolate, XrVMContext *vm_ctx, XrInstruction instr,
                                  XrValue *base, XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int discard_result = GETARG_C(instr);

    XrValue task_val = base[b];

    /* ============ Task path (new: go returns XrTask) ============ */
    if (xr_value_is_task(task_val)) {
        XrTask *task = xr_value_to_task(task_val);

        // Fast path: task already completed — works for re-await too
        XrCoroutine *current = vm_get_coro(vm_ctx);
        XrSlotRef result_slot = xr_slot_xvalue_ptr(&base[a]);
        if (current) {
            XrCoroBlockResult resumed = xr_coro_await_task_resume_slot(
                isolate, current, task, result_slot, discard_result != 0);
            if (resumed.kind == XR_CORO_BLOCK_READY) {
                vm_task_detach_completed_executor(task);
                return XR_DISP_NEXT;
            }
            if (resumed.kind == XR_CORO_BLOCK_TIMEOUT || resumed.kind == XR_CORO_BLOCK_CLOSED) {
                return XR_DISP_NEXT;
            }
            if (resumed.kind == XR_CORO_BLOCK_ERROR) {
                VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "await: unable to resume");
            }
        }

        uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
        if (!current && tstate == XR_TASK_COMPLETED) {
            base[a] = vm_task_consume_result(isolate, task, current, discard_result);
            return XR_DISP_NEXT;
        }
        if (tstate == XR_TASK_FAILED || tstate == XR_TASK_CANCELLED) {
            base[a] = xr_null();
            return XR_DISP_NEXT;
        }

        // Slow path: task still active, need to suspend
        XrRuntime *rt = (XrRuntime *) isolate->vm.runtime;
        if (!rt) {
            VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "await: runtime not initialized");
        }

        if (current) {
            vm_ctx->stack_top = base + frame->closure->proto->maxstacksize;
            XrCoroBlockResult await_result = xr_coro_await_task_slot(
                isolate, current, task, result_slot, -1, discard_result != 0);
            if (await_result.kind == XR_CORO_BLOCK_BLOCKED) {
                frame->pc = pc - 1;
                return XR_DISP_BLOCKED;
            }
            if (await_result.kind == XR_CORO_BLOCK_READY) {
                vm_task_detach_completed_executor(task);
                return XR_DISP_NEXT;
            }
            if (await_result.kind == XR_CORO_BLOCK_CLOSED ||
                await_result.kind == XR_CORO_BLOCK_TIMEOUT ||
                await_result.kind == XR_CORO_BLOCK_NO_CORO) {
                return XR_DISP_NEXT;
            }
            VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "await: unable to block");
        }

        // Main thread: spin wait
        int spin_count = 0;
        int total_spins = 0;
        while (!xr_task_is_done(task)) {
            if (++total_spins > AWAIT_TIMEOUT_SPINS) {
                fprintf(stderr, "[xray] warn: await: task wait timeout\n");
                base[a] = xr_null();
                return XR_DISP_NEXT;
            }
            if (!atomic_load(&rt->running)) {
                fprintf(stderr, "[xray] warn: await: runtime stopped\n");
                base[a] = xr_null();
                return XR_DISP_NEXT;
            }
            if (++spin_count > 1000) {
                spin_count = 0;
                XrWorker *w = xr_current_worker();
                if (w && w->p.timer_wheel) {
                    int64_t now = xr_monotonic_ticks();
                    xr_bump_timers(w->p.timer_wheel, now);
                }
                xr_thread_yield();
            }
        }
        base[a] = vm_task_consume_result(isolate, task, NULL, discard_result);
        return XR_DISP_NEXT;
    }

    VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "await: expected task");
}

XR_FUNC XrDispatchAction vm_await_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                          XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                          XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);

    XrValue await_val = base[b];
    XrValue timeout_val = base[c];

    int64_t timeout_ms = 0;
    if (XR_IS_INT(timeout_val))
        timeout_ms = XR_TO_INT(timeout_val);
    else if (XR_IS_FLOAT(timeout_val))
        timeout_ms = (int64_t) XR_TO_FLOAT(timeout_val);

    XrCoroutine *caller = vm_get_coro(vm_ctx);

    // Task path
    if (xr_value_is_task(await_val)) {
        XrTask *task = xr_value_to_task(await_val);
        XrSlotRef result_slot = xr_slot_xvalue_ptr(&base[a]);
        if (caller) {
            XrCoroBlockResult resumed =
                xr_coro_await_task_resume_slot(isolate, caller, task, result_slot, false);
            if (resumed.kind == XR_CORO_BLOCK_TIMEOUT || resumed.kind == XR_CORO_BLOCK_CLOSED) {
                return XR_DISP_NEXT;
            }
            if (resumed.kind == XR_CORO_BLOCK_READY) {
                vm_task_detach_completed_executor(task);
                return XR_DISP_NEXT;
            }
            if (resumed.kind == XR_CORO_BLOCK_ERROR) {
                VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "await: unable to resume");
            }
        }

        uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
        if (!caller && tstate == XR_TASK_COMPLETED) {
            base[a] = vm_task_consume_result(isolate, task, caller, 0);
            return XR_DISP_NEXT;
        }
        if (tstate == XR_TASK_FAILED || tstate == XR_TASK_CANCELLED) {
            base[a] = xr_null();
            return XR_DISP_NEXT;
        }

        XrRuntime *rt = (XrRuntime *) isolate->vm.runtime;
        frame->pc = pc;
        vm_ctx->stack_top = base + frame->closure->proto->maxstacksize;

        XrCoroutine *current = vm_get_coro(vm_ctx);
        if (current && rt) {
            XrCoroBlockResult await_result =
                xr_coro_await_task_slot(isolate, current, task, result_slot, timeout_ms, false);
            if (await_result.kind == XR_CORO_BLOCK_BLOCKED) {
                frame->pc = pc - 1;
                return XR_DISP_BLOCKED;
            }
            if (await_result.kind == XR_CORO_BLOCK_READY) {
                vm_task_detach_completed_executor(task);
                return XR_DISP_NEXT;
            }
            if (await_result.kind == XR_CORO_BLOCK_TIMEOUT ||
                await_result.kind == XR_CORO_BLOCK_CLOSED ||
                await_result.kind == XR_CORO_BLOCK_NO_CORO) {
                return XR_DISP_NEXT;
            }
            VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "await: unable to block");
        }

        // Main thread: synchronous wait
        uint64_t start_ns = xr_time_monotonic_ns();
        int spin_count = 0;
        while (!xr_task_is_done(task)) {
            int64_t elapsed_ms = (int64_t) ((xr_time_monotonic_ns() - start_ns) / 1000000ULL);
            if (elapsed_ms >= timeout_ms) {
                base[a] = xr_null();
                return XR_DISP_NEXT;
            }
            XrWorker *w = xr_current_worker();
            if (w && w->p.timer_wheel) {
                int64_t tnow = xr_monotonic_ticks();
                xr_bump_timers(w->p.timer_wheel, tnow);
            }
            if (++spin_count > 1000) {
                spin_count = 0;
                xr_thread_yield();
            }
        }
        base[a] = vm_task_consume_result(isolate, task, NULL, 0);
        return XR_DISP_NEXT;
    }

    VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "await: expected task");
}

XR_FUNC XrDispatchAction vm_await_all(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);

    XrValue arr_val = base[b];
    if (!xr_value_is_array(arr_val)) {
        VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "await all: expected array");
    }

    XrArray *tasks = xr_value_to_array(arr_val);
    int count = xr_array_size(tasks);
    XrCoroutine *caller = xr_current_coro(isolate);

    // Fast path: check if all tasks are done.
    bool all_done = true;
    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv))
            continue;
        if (!xr_task_is_done(xr_value_to_task(cv))) {
            all_done = false;
            break;
        }
    }

    if (all_done) {
        XrArray *results = xr_array_with_capacity(vm_get_coro(vm_ctx), count);
        results->length = count;
        XrValue *rdata = (XrValue *) results->data;
        for (int j = 0; j < count; j++) {
            XrValue cv = xr_array_get(tasks, j);
            if (!xr_value_is_task(cv)) {
                rdata[j] = xr_null();
                continue;
            }
            rdata[j] = vm_task_consume_result(isolate, xr_value_to_task(cv), caller, 0);
        }
        base[a] = xr_value_from_array(results);
        return XR_DISP_NEXT;
    }

    frame->pc = pc - 1;
    vm_ctx->stack_top = base + frame->closure->proto->maxstacksize;

    XrRuntime *rt = (XrRuntime *) isolate->vm.runtime;
    if (!rt) {
        VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "await all: runtime not initialized");
    }

    if (caller) {
        XrCoroBlockResult block = xr_coro_await_all_tasks(caller, tasks);
        if (block.kind == XR_CORO_BLOCK_READY) {
            return XR_DISP_RESTART;
        }
        if (block.kind == XR_CORO_BLOCK_BLOCKED) {
            return XR_DISP_BLOCKED;
        }
        VM_THROW(frame, pc, XR_ERR_CORO_DEAD, "await all: unable to block");
    }

    // Main thread: spin wait
    int total_spins = 0, spin_count = 0;
    for (;;) {
        if (++total_spins > AWAIT_TIMEOUT_SPINS) {
            fprintf(stderr, "[xray] warn: await all: timeout\n");
            break;
        }
        if (!atomic_load(&rt->running))
            break;
        bool ad = true;
        for (int j = 0; j < count; j++) {
            XrValue cv = xr_array_get(tasks, j);
            if (!xr_value_is_task(cv))
                continue;
            if (!xr_task_is_done(xr_value_to_task(cv))) {
                ad = false;
                break;
            }
        }
        if (ad)
            break;
        if (++spin_count > 1000) {
            spin_count = 0;
            xr_thread_yield();
        }
    }
    return XR_DISP_RESTART;
}

XR_FUNC XrDispatchAction vm_await_any(XrayIsolate *isolate, XrVMContext *vm_ctx,
                                      XrInstruction instr, XrValue *base, XrBcCallFrame *frame,
                                      XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int mode = GETARG_C(instr);

    XrValue arr_val = base[b];
    if (!xr_value_is_array(arr_val)) {
        VM_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
                 mode == 0 ? "await any: expected array" : "await anySuccess: expected array");
    }

    XrArray *tasks = xr_value_to_array(arr_val);
    int count = xr_array_size(tasks);
    XrCoroutine *current = vm_get_coro(vm_ctx);

    // Fast path: check if any task is already done.
    int done_count = 0;
    int task_count = 0;
    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv))
            continue;
        task_count++;
        XrTask *t = xr_value_to_task(cv);
        if (xr_task_is_done(t)) {
            if (mode == 1)
                done_count++;
            if (mode == 0 || XR_IS_NULL(t->error)) {
                base[a] = vm_task_consume_result(isolate, t, current, 0);
                return XR_DISP_NEXT;
            }
        }
    }

    if (task_count == 0 || (mode == 1 && done_count == task_count)) {
        base[a] = xr_null();
        return XR_DISP_NEXT;
    }

    // Slow path: wait
    if (current) {
        frame->pc = pc - 1;
        XrCoroBlockResult block = xr_coro_await_any_task(current, tasks, mode == 1);
        if (block.kind == XR_CORO_BLOCK_READY) {
            return XR_DISP_RESTART;
        }
        if (block.kind == XR_CORO_BLOCK_BLOCKED) {
            return XR_DISP_BLOCKED;
        }
        VM_THROW(frame, pc, XR_ERR_CORO_DEAD,
                 mode == 0 ? "await any: unable to block" : "await anySuccess: unable to block");
    } else {
        // Main thread: poll wait
        int spin = 0;
        while (true) {
            done_count = 0;
            task_count = 0;
            for (int j = 0; j < count; j++) {
                XrValue cv = xr_array_get(tasks, j);
                if (!xr_value_is_task(cv))
                    continue;
                task_count++;
                XrTask *t = xr_value_to_task(cv);
                if (xr_task_is_done(t)) {
                    if (mode == 1)
                        done_count++;
                    if (mode == 0 || XR_IS_NULL(t->error)) {
                        base[a] = vm_task_consume_result(isolate, t, NULL, 0);
                        return XR_DISP_NEXT;
                    }
                }
            }
            if (task_count == 0 || (mode == 1 && done_count == task_count)) {
                base[a] = xr_null();
                return XR_DISP_NEXT;
            }
            if (++spin > 1000) {
                spin = 0;
                xr_thread_yield();
            }
        }
    }
}
