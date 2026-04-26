/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_cold_coro.c - Cold-path implementations for coroutine ops
 *
 * Holds the noinline bodies for the coroutine control plane and
 * the go / spawn / await family. Function declarations live in
 * xvm_cold_paths.h.
 *
 * Owns:
 *   - vm_collect_all_coros / vm_coro_ctrl
 *   - vm_cold_get_coro (helper)
 *   - vm_go / vm_go_invoke / vm_spawn_cont
 *   - vm_await_recycle_coro / vm_task_consume_result /
 *     vm_await_read_result (helpers)
 *   - vm_await / vm_await_timeout / vm_await_all / vm_await_any
 */

#include "xvm_cold_paths.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include "../runtime/value/xstruct_layout.h"
#include "xvm_checks.h"
#include "xdebug.h"
#include "../runtime/xray_debug_hooks.h"
#include "../runtime/xstrbuf.h"
#include "../runtime/object/xstringbuilder.h"
#include "../runtime/object/xslice.h"
#include "../runtime/object/xjson.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/object/xrange.h"
#include "../runtime/object/xutf8.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/value/xtype_feedback.h"
#include "../coro/xcoro_pool.h"
#include "../coro/xtask.h"
#include "../coro/xdeep_copy.h"

/* ========== Helper: Collect all coroutines from all workers ========== */

// VmCoroEntry and VM_CORO_COLLECT_MAX defined in xvm_cold_paths.h

// Collect coroutines from all workers into a flat array for diagnostic sub-ops.
// Returns the number of entries written. Best-effort snapshot (not atomic).
int vm_collect_all_coros(XrayIsolate *isolate, VmCoroEntry *out, int max_out) {
    XR_DCHECK(isolate != NULL, "vm_collect_all_coros: NULL isolate");
    XrRuntime *runtime = (XrRuntime *)isolate->vm.runtime;
    if (!runtime) return 0;

    int count = 0;

    // Temporary buffer for steal queue snapshots
    XrCoroutine *snap_buf[256];

    for (int wi = 0; wi < runtime->worker_count && count < max_out; wi++) {
        XrWorker *w = &runtime->workers[wi];

        // 1) Ready coroutines from Chase-Lev deque + overflow
        for (int p = 0; p < XR_RUNQ_COUNT && count < max_out; p++) {
            XrRunQueue *rq = &w->p.runq[p];

            // Deque snapshot
            int n = xr_steal_queue_snapshot(&rq->deque, snap_buf,
                        (max_out - count < 256) ? (max_out - count) : 256);
            for (int i = 0; i < n && count < max_out; i++) {
                out[count].coro = snap_buf[i];
                out[count].state = "ready";
                count++;
            }

            // Overflow list
            XrCoroutine *ov = rq->overflow_first;
            while (ov && count < max_out) {
                out[count].coro = ov;
                out[count].state = "ready";
                count++;
                ov = ov->next;
            }
        }

        // 2) LIFO slot
        XrCoroutine *_ls = atomic_load_explicit(&w->p.lifo_slot, memory_order_relaxed);
        if (_ls && count < max_out) {
            out[count].coro = _ls;
            out[count].state = "ready";
            count++;
        }

        // 3) Blocked coroutines
        XrCoroutine *bc = w->p.blocked_head;
        while (bc && count < max_out) {
            out[count].coro = bc;
            out[count].state = "blocked";
            count++;
            bc = bc->next;
        }
    }

    return count;
}

/* ========== Cold Path: OP_CORO_CTRL Sub-operations ========== */

__attribute__((noinline))
int vm_coro_ctrl(XrayIsolate *isolate, XrVMContext *vm_ctx,
                        XrInstruction instr, XrValue *base) {
    XR_DCHECK(isolate != NULL, "vm_coro_ctrl: NULL isolate");
    XR_DCHECK(base != NULL, "vm_coro_ctrl: NULL base");
    int coro_sub = GETARG_C(instr);
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);

    switch (coro_sub) {
    case CORO_CTRL_STATS: {
        XrRuntime *runtime = (XrRuntime *)isolate->vm.runtime;
        if (!runtime) { base[a] = xr_null(); return VM_COLD_BREAK; }

        int blocked_count = 0, ready_count = 0;
        int active_count = xr_runtime_active_coros(runtime);
        uint64_t total_created = 0;
        for (int _si = 0; _si < runtime->worker_count; _si++)
            total_created += runtime->workers[_si].p.stats.spawned_count;

        for (int wi = 0; wi < runtime->worker_count; wi++) {
            XrWorker *w = &runtime->workers[wi];
            blocked_count += w->p.blocked_count;
            for (int p = 0; p < XR_RUNQ_COUNT; p++) {
                ready_count += xr_runq_len(&w->p.runq[p]);
            }
        }

        int total_alive = ready_count + blocked_count + active_count;
        XrMap *result = xr_map_new(COLD_CORO(vm_ctx));
        xr_map_set(result, VM_INTERN_KEY("active"), xr_int(active_count));
        xr_map_set(result, VM_INTERN_KEY("blocked"), xr_int(blocked_count));
        xr_map_set(result, VM_INTERN_KEY("ready"), xr_int(ready_count));
        xr_map_set(result, VM_INTERN_KEY("total"), xr_int(total_alive));
        xr_map_set(result, VM_INTERN_KEY("created"), xr_int((int)total_created));
        base[a] = xr_value_from_map(result);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_LIST: {
        int limit = 0;
        int state_filter = 0;  // 0=all, 1=ready, 2=blocked

        XrValue limit_val = base[b];
        if (XR_IS_INT(limit_val)) limit = (int)XR_TO_INT(limit_val);
        if (limit <= 0) limit = 100000;

        XrValue state_val = base[a + 1];
        if (XR_IS_INT(state_val)) {
            state_filter = (int)XR_TO_INT(state_val);
        } else if (XR_IS_STRING(state_val)) {
            XrString *s = (XrString *)XR_TO_PTR(state_val);
            if (strcmp(s->data, "ready") == 0) state_filter = 1;
            else if (strcmp(s->data, "blocked") == 0) state_filter = 2;
        }

        VmCoroEntry *entries = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
        int total = vm_collect_all_coros(isolate, entries, VM_CORO_COLLECT_MAX);

        XrArray *result = xr_array_new(COLD_CORO(vm_ctx));
        int count = 0;

        for (int i = 0; i < total && count < limit; i++) {
            XrCoroutine *coro = entries[i].coro;
            const char *st = entries[i].state;
            bool is_ready = (strcmp(st, "ready") == 0);
            bool is_blocked = (strcmp(st, "blocked") == 0);

            if (state_filter == 1 && !is_ready) continue;
            if (state_filter == 2 && !is_blocked) continue;

            XrMap *info = xr_map_new(COLD_CORO(vm_ctx));
            xr_map_set(info, VM_INTERN_KEY("id"), xr_int(coro->id));
            xr_map_set(info, VM_INTERN_KEY("name"),
                       coro->name ? xr_string_value(xr_string_intern(isolate, coro->name, strlen(coro->name), 0)) : xr_null());
            xr_map_set(info, VM_INTERN_KEY("state"),
                       xr_string_value(xr_string_intern(isolate, st, strlen(st), 0)));
            if (coro->source_file) {
                char source_buf[XR_MAX_PROPERTY_NAME_LEN];
                snprintf(source_buf, sizeof(source_buf), "%s:%d", coro->source_file, coro->source_line);
                xr_map_set(info, VM_INTERN_KEY("source"),
                           xr_string_value(xr_string_intern(isolate, source_buf, strlen(source_buf), 0)));
            }
            xr_array_push(result, xr_value_from_map(info));
            count++;
        }

        xr_free(entries);
        base[a] = xr_value_from_array(result);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_INFO: {
        XrValue coro_val = base[b];
        if (!xr_value_is_coro(coro_val)) { base[a] = xr_null(); return VM_COLD_BREAK; }

        XrCoroutine *coro = xr_value_to_coro(coro_val);
        XrMap *info = xr_map_new(COLD_CORO(vm_ctx));
        uint32_t flags = xr_coro_flags_load(coro);

        xr_map_set(info, VM_INTERN_KEY("id"), xr_int(coro->id));
        xr_map_set(info, VM_INTERN_KEY("name"),
                   coro->name ? xr_string_value(xr_string_intern(isolate, coro->name, strlen(coro->name), 0)) : xr_null());

        const char *state_str = "unknown";
        if (flags & XR_CORO_FLG_DONE) state_str = "done";
        else if (flags & XR_CORO_FLG_BLOCKED) state_str = "blocked";
        else if (flags & XR_CORO_FLG_RUNNING) state_str = "running";
        else if (flags & XR_CORO_FLG_READY) state_str = "ready";
        xr_map_set(info, VM_INTERN_KEY("state"),
                   xr_string_value(xr_string_intern(isolate, state_str, strlen(state_str), 0)));

        xr_map_set(info, VM_INTERN_KEY("priority"), xr_int(xr_coro_get_priority(flags)));
        xr_map_set(info, VM_INTERN_KEY("reductions"), xr_int(coro->reductions));

        if (coro->source_file) {
            char source_buf[XR_MAX_PROPERTY_NAME_LEN];
            snprintf(source_buf, sizeof(source_buf), "%s:%d", coro->source_file, coro->source_line);
            xr_map_set(info, VM_INTERN_KEY("source"),
                       xr_string_value(xr_string_intern(isolate, source_buf, strlen(source_buf), 0)));
        }

        struct XrMap *coro_locals = (coro->ext) ? coro->ext->locals : NULL;
        if (coro_locals) {
            xr_map_set(info, VM_INTERN_KEY("locals"), xr_value_from_map(coro_locals));
        } else {
            xr_map_set(info, VM_INTERN_KEY("locals"), xr_value_from_map(xr_map_new(COLD_CORO(vm_ctx))));
        }

        xr_map_set(info, VM_INTERN_KEY("waitCount"), xr_int(atomic_load(&coro->wait_count)));
        xr_map_set(info, VM_INTERN_KEY("cancelled"), xr_bool(flags & XR_CORO_FLG_CANCELLED));

        if (flags & XR_CORO_FLG_DONE) {
            xr_map_set(info, VM_INTERN_KEY("result"), coro->result);
        }
        if (flags & XR_CORO_FLG_BLOCKED) {
            const char *reason = coro->wait_channel ? "channel" : "await";
            xr_map_set(info, VM_INTERN_KEY("blockedOn"),
                       xr_string_value(xr_string_intern(isolate, reason, strlen(reason), 0)));
        }

        base[a] = xr_value_from_map(info);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_DUMP: {
        int limit = a; // A = limit
        if (limit == 0) limit = 100;

        VmCoroEntry *entries = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
        int total = vm_collect_all_coros(isolate, entries, VM_CORO_COLLECT_MAX);

        int ready_count = 0, blocked_count = 0;
        for (int i = 0; i < total; i++) {
            if (strcmp(entries[i].state, "ready") == 0) ready_count++;
            else if (strcmp(entries[i].state, "blocked") == 0) blocked_count++;
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
            const char *state_upper = (strcmp(entries[i].state, "blocked") == 0) ? "BLOCKED" : "READY";
            const char *block_reason = "-";
            if (strcmp(entries[i].state, "blocked") == 0) {
                block_reason = coro->wait_channel ? "channel" : "await";
            }

            const char *name = coro->name ? coro->name : "(anonymous)";
            char name_buf[15];
            snprintf(name_buf, sizeof(name_buf), "%.14s", name);

            char source_buf[20] = "-";
            if (coro->source_file) {
                const char *fname = strrchr(coro->source_file, '/');
                fname = fname ? fname + 1 : coro->source_file;
                snprintf(source_buf, sizeof(source_buf), "%.12s:%d", fname, coro->source_line);
            }

            printf("│ %-4d │ %-14s │ %-7s │ %-15s │ %-19s │\n",
                   coro->id, name_buf, state_upper, block_reason, source_buf);
            shown++;
        }

        printf("└──────┴────────────────┴─────────┴─────────────────┴─────────────────────┘\n");
        xr_free(entries);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_STALLED: {
        (void)b;
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched) {
            base[a] = xr_value_from_array(xr_array_new(COLD_CORO(vm_ctx)));
            return VM_COLD_BREAK;
        }
        XrArray *result = xr_array_new(COLD_CORO(vm_ctx));
        base[a] = xr_value_from_array(result);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_DEADLOCKS: {
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched) {
            base[a] = xr_value_from_array(xr_array_new(COLD_CORO(vm_ctx)));
            return VM_COLD_BREAK;
        }
        XrArray *result = xr_array_new(COLD_CORO(vm_ctx));
        base[a] = xr_value_from_array(result);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_TOP: {
        int top_n = 10;
        int metric = 0;  // 0=id, 2=reductions

        XrValue n_val = base[b];
        if (XR_IS_INT(n_val)) {
            top_n = (int)XR_TO_INT(n_val);
            if (top_n <= 0) top_n = 10;
            if (top_n > 1000) top_n = 1000;
        }

        XrValue metric_val = base[a + 1];
        if (XR_IS_STRING(metric_val)) {
            XrString *s = (XrString *)XR_TO_PTR(metric_val);
            if (strcmp(s->data, "reductions") == 0) metric = 2;
            else if (strcmp(s->data, "id") == 0) metric = 0;
        }

        typedef struct { XrCoroutine *coro; const char *state; int64_t value; } TopEntry;
        TopEntry *entries = xr_malloc(sizeof(TopEntry) * VM_CORO_COLLECT_MAX);

        VmCoroEntry *raw = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
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
                if (entries[k].value > entries[max_idx].value) max_idx = k;
            }
            if (max_idx != j) {
                TopEntry tmp = entries[j];
                entries[j] = entries[max_idx];
                entries[max_idx] = tmp;
            }
        }

        XrArray *result = xr_array_new(COLD_CORO(vm_ctx));
        int result_count = (top_n < count) ? top_n : count;
        for (int j = 0; j < result_count; j++) {
            XrCoroutine *coro = entries[j].coro;
            XrMap *info = xr_map_new(COLD_CORO(vm_ctx));
            xr_map_set(info, VM_INTERN_KEY("id"), xr_int(coro->id));
            xr_map_set(info, VM_INTERN_KEY("name"),
                       coro->name ? xr_string_value(xr_string_intern(isolate, coro->name, strlen(coro->name), 0)) : xr_null());
            xr_map_set(info, VM_INTERN_KEY("state"),
                       xr_string_value(xr_string_intern(isolate, entries[j].state, strlen(entries[j].state), 0)));
            xr_map_set(info, VM_INTERN_KEY("reductions"), xr_int(coro->reductions));
            xr_map_set(info, VM_INTERN_KEY("priority"), xr_int(xr_coro_get_priority(xr_coro_flags_load(coro))));
            if (coro->source_file) {
                char source_buf[XR_MAX_PROPERTY_NAME_LEN];
                snprintf(source_buf, sizeof(source_buf), "%s:%d", coro->source_file, coro->source_line);
                xr_map_set(info, VM_INTERN_KEY("source"),
                           xr_string_value(xr_string_intern(isolate, source_buf, strlen(source_buf), 0)));
            }
            xr_array_push(result, xr_value_from_map(info));
        }

        xr_free(entries);
        base[a] = xr_value_from_array(result);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_GROUP_BY: {
        int group_by = 0;  // 0=name, 1=state, 2=priority

        XrValue field_val = base[b];
        if (XR_IS_STRING(field_val)) {
            XrString *s = (XrString *)XR_TO_PTR(field_val);
            if (strcmp(s->data, "state") == 0) group_by = 1;
            else if (strcmp(s->data, "priority") == 0) group_by = 2;
        }

        VmCoroEntry *entries = xr_malloc(sizeof(VmCoroEntry) * VM_CORO_COLLECT_MAX);
        int total = vm_collect_all_coros(isolate, entries, VM_CORO_COLLECT_MAX);

        XrMap *result = xr_map_new(COLD_CORO(vm_ctx));

        for (int i = 0; i < total; i++) {
            XrCoroutine *coro = entries[i].coro;
            const char *key_str;
            char prio_str[16];
            if (group_by == 0) {
                key_str = coro->name ? coro->name : "(anonymous)";
            } else if (group_by == 1) {
                key_str = entries[i].state;
            } else {
                snprintf(prio_str, sizeof(prio_str), "P%d", xr_coro_get_priority(xr_coro_flags_load(coro)));
                key_str = prio_str;
            }
            XrValue key = xr_string_value(xr_string_intern(isolate, key_str, strlen(key_str), 0));
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
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_WHEREIS: {
        XrValue name_val = base[b];
        if (!XR_IS_STRING(name_val)) { base[a] = xr_bool(false); return VM_COLD_BREAK; }
        const char *name_cstr = xr_value_str_data(&name_val);
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched || !sched->coro_registry) { base[a] = xr_bool(false); return VM_COLD_BREAK; }
        XrCoroutine *found = xr_coro_registry_whereis(sched->coro_registry, name_cstr);
        base[a] = xr_bool(found != NULL);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_MONITOR: {
        XrValue name_val = base[b];
        if (!XR_IS_STRING(name_val)) { base[a] = xr_null(); return VM_COLD_BREAK; }
        const char *name_cstr = xr_value_str_data(&name_val);
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched || !sched->coro_registry) { base[a] = xr_null(); return VM_COLD_BREAK; }
        XrChannel *ch = xr_coro_monitor(isolate, sched->coro_registry, name_cstr);
        base[a] = ch ? xr_value_from_channel(ch) : xr_null();
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_DEMONITOR: {
        XrValue name_val = base[b];
        if (!XR_IS_STRING(name_val)) { base[a] = xr_null(); return VM_COLD_BREAK; }
        const char *name_cstr = xr_value_str_data(&name_val);
        XrValue ch_val = base[a + 1];
        if (!xr_value_is_channel(ch_val)) { base[a] = xr_null(); return VM_COLD_BREAK; }
        XrChannel *ch = xr_value_to_channel(ch_val);
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched || !sched->coro_registry) { base[a] = xr_null(); return VM_COLD_BREAK; }
        XrCoroutine *coro = xr_coro_registry_whereis(sched->coro_registry, name_cstr);
        if (coro) {
            xr_coro_demonitor(sched->coro_registry, coro, ch);
        }
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_KILL: {
        XrValue name_val = base[b];
        if (!XR_IS_STRING(name_val)) { base[a] = xr_bool(false); return VM_COLD_BREAK; }
        const char *name_cstr = xr_value_str_data(&name_val);
        XrCoroState *sched = (XrCoroState *)isolate->vm.coro_state;
        if (!sched || !sched->coro_registry) { base[a] = xr_bool(false); return VM_COLD_BREAK; }
        XrCoroutine *target = xr_coro_registry_whereis(sched->coro_registry, name_cstr);
        if (!target || xr_coro_flags_has(target, XR_CORO_FLG_DONE)) {
            base[a] = xr_bool(false);
            return VM_COLD_BREAK;
        }
        xr_coro_flags_set(target, XR_CORO_FLG_CANCEL_REQUESTED);
        xr_coro_request_yield(target);
        base[a] = xr_bool(true);
        return VM_COLD_BREAK;
    }

    case CORO_CTRL_SELF: {
        XrCoroutine *current = COLD_CORO(vm_ctx);
        if (current && current->name) {
            size_t len = strlen(current->name);
            XrString *s = xr_string_intern(isolate, current->name, len, 0);
            base[a] = s ? xr_string_value(s) : xr_null();
        } else {
            base[a] = xr_null();
        }
        return VM_COLD_BREAK;
    }

    default:
        return VM_COLD_BREAK;
    }
}
/* ========== Cold Path: Coroutine Operations ========== */

// vm_cold_get_coro lives in xvm_cold_paths.h so the cold-call /
// cold-object / cold-chan TUs can call it without an owning .c
// file having to re-export it.

__attribute__((noinline))
int vm_go(XrayIsolate *isolate, XrVMContext *vm_ctx,
                  XrInstruction instr, XrValue *base,
                  XrBcCallFrame *frame) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);
    XrInstruction *pc = frame->pc;

    XrValue fn_val = base[b];
    if (!xr_value_is_closure(fn_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "go: expected closure");
    }

    struct XrClosure *closure = xr_value_to_closure(fn_val);
    XrProto *proto = closure->proto;

    if (!proto->is_coro_safe) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
            "go: closure captures non-thread-safe variables, cannot run in coroutine\n"
            "hint: use 'shared const' to declare shared variables, or pass via arguments");
    }
    if (c != proto->numparams) {
        VM_COLD_THROW(frame, pc, XR_ERR_WRONG_ARG_COUNT,
            "go: argument count mismatch (expected %d, got %d)", proto->numparams, c);
    }

    // Defer source_file/source_line to lazy access (coro->spawn_file/spawn_line)
    const char *coro_name = NULL;

    XrInstruction next_inst = *pc;
    if (GET_OPCODE(next_inst) == OP_NOP && GETARG_A(next_inst) == 1) {
        int name_idx = GETARG_Bx(next_inst);
        XrValue name_val = PROTO_CONSTANT(frame->closure->proto, name_idx);
        if (XR_IS_STRING(name_val))
            coro_name = xr_value_to_string(isolate, name_val)->data;
        pc++;
        next_inst = *pc;
    }

    int coro_priority = 1;
    if (GET_OPCODE(next_inst) == OP_NOP && GETARG_A(next_inst) == 2) {
        coro_priority = GETARG_Bx(next_inst);
        pc++;
        next_inst = *pc;
    }

    XrValue *args = (c > 0) ? &base[b + 1] : NULL;
    // Defer source_file/source_line to lazy access (coro->spawn_file/spawn_line)
    XrCoroutine *coro = xr_coro_create(isolate, closure, args, c,
                                        coro_name, NULL, 0);
    if (!coro) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to create coroutine");
    }

    if (coro_priority != 1) {
        uint32_t flags = atomic_load(&coro->flags);
        flags = xr_coro_set_priority_flags(flags, coro_priority);
        atomic_store(&coro->flags, flags);
    }

    XrRuntime *runtime = (XrRuntime *)isolate->vm.runtime;
    if (!runtime) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: Runtime not initialized");
    }

    XrCoroutine *parent = vm_cold_get_coro(vm_ctx);
    if (parent && parent->current_scope) {
        coro->parent_scope = parent->current_scope;
        atomic_fetch_add_explicit(&parent->current_scope->count, 1, memory_order_relaxed);
    } else if (runtime->current_scope) {
        coro->parent_scope = runtime->current_scope;
        atomic_fetch_add_explicit(&runtime->current_scope->count, 1, memory_order_relaxed);
    }

    frame->pc = pc;
    // coro_init_common already sets XR_CORO_FLG_READY
    xr_runtime_spawn(runtime, coro);

    base[a] = xr_value_from_coro(coro);
    frame->pc = pc;
    return VM_COLD_BREAK;
}

__attribute__((noinline))
int vm_go_invoke(XrayIsolate *isolate, XrVMContext *vm_ctx,
                         XrInstruction instr, XrValue *base,
                         XrBcCallFrame *frame, XrInstruction *pc) {
    (void)vm_ctx;
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int nargs = GETARG_C(instr);

    XrValue receiver = base[a + 1];
    int method_symbol = PROTO_SYMBOL(frame->closure->proto, b);

    XrValue result = xr_null();

    if (XR_IS_ARRAY(receiver)) {
        const XrMethodSlot *slot = xr_method_table_lookup(
            XR_TID_ARRAY, method_symbol, SYMBOL_BUILTIN_COUNT);
        result = slot ? slot->fn(isolate, receiver, &base[a + 2], nargs)
                      : XR_NOTFOUND;
    } else if (XR_IS_MAP(receiver)) {
        const XrMethodSlot *slot = xr_method_table_lookup(
            XR_TID_MAP, method_symbol, SYMBOL_BUILTIN_COUNT);
        result = slot ? slot->fn(isolate, receiver, &base[a + 2], nargs)
                      : XR_NOTFOUND;
    } else if (XR_IS_STRING(receiver)) {
        const XrMethodSlot *slot = xr_method_table_lookup(
            XR_TID_STRING, method_symbol, SYMBOL_BUILTIN_COUNT);
        result = slot ? slot->fn(isolate, receiver, &base[a + 2], nargs)
                      : XR_NOTFOUND;
    } else if (XR_IS_PTR(receiver)) {
        uint8_t gc_type = XR_HEAP_TYPE(receiver);
        XrClass *native_class = isolate->native_type_classes[gc_type];
        if (native_class) {
            XrMethod *method = xr_class_lookup_method(native_class, method_symbol);
            if (method && method->type == XMETHOD_PRIMITIVE && method->as.primitive) {
                result = method->as.primitive(isolate, &base[a + 1], nargs + 1);
            } else {
                VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "go: method not found");
            }
        } else {
            VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "go: method call not supported for this type");
        }
    } else {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD, "go: method call not supported for this type");
    }

    if (unlikely(XR_IS_NOTFOUND(result))) {
        XrSymbolTable *_st = (XrSymbolTable*)isolate->symbol_table;
        const char *_mn = xr_symbol_get_name_in_table(_st, method_symbol);
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_NO_METHOD,
            "type '%s' has no method '%s'", xr_typeid_name(xr_value_typeid(receiver)), _mn ? _mn : "?");
    }

    XrCoroutine *coro = (XrCoroutine *)xr_gc_alloc(&isolate->gc, sizeof(XrCoroutine), XR_TCOROUTINE);
    if (!coro) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to create coroutine");
    }
    memset(&coro->jit_ctx, 0, sizeof(XrCoroutine) - offsetof(XrCoroutine, jit_ctx));
    coro->isolate = isolate;
    xr_coro_flags_set(coro, XR_CORO_FLG_DONE);
    coro->result = result;

    base[a] = xr_value_from_coro(coro);
    return VM_COLD_BREAK;
}

__attribute__((noinline))
int vm_spawn_cont(XrayIsolate *isolate, XrVMContext *vm_ctx,
                          XrInstruction instr, XrValue *base,
                          XrBcCallFrame *frame) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c_raw = GETARG_C(instr);
    // C bit 7: fire-and-forget flag (go used as statement, result never awaited)
    bool fire_and_forget = (c_raw & 0x80) != 0;
    int c = c_raw & 0x7F;  // actual argument count
    XrInstruction *pc = frame->pc;

    XrValue fn_val = base[b];
    if (!xr_value_is_closure(fn_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "go: expected closure");
    }

    struct XrClosure *closure = xr_value_to_closure(fn_val);
    XrProto *proto = closure->proto;

    if (!proto->is_coro_safe) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
            "go: closure captures non-thread-safe variables");
    }
    if (c != proto->numparams) {
        VM_COLD_THROW(frame, pc, XR_ERR_WRONG_ARG_COUNT,
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
    // Defer source_file/source_line to lazy access (coro->spawn_file/spawn_line)
    XrCoroutine *coro = xr_coro_create(isolate, closure, args, c,
                                        coro_name, NULL, 0);
    if (!coro) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to create coroutine");
    }

    if (coro_priority != 1) {
        uint32_t flags = atomic_load(&coro->flags);
        flags = xr_coro_set_priority_flags(flags, coro_priority);
        atomic_store(&coro->flags, flags);
    }
    XrRuntime *runtime = (XrRuntime *)isolate->vm.runtime;
    if (!runtime) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: Runtime not initialized");
    }

    XrCoroutine *parent = vm_cold_get_coro(vm_ctx);

    // Allocate GC-managed Task handle on executor's heap
    XrTask *task = xr_task_create(parent, coro);
    if (!task) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "go: failed to create task");
    }

    // Store link_mode on task for runtime association
    task->link_mode = (uint8_t)link_mode;

    // Mark fire-and-forget coros as recyclable for deferred recycle
    if (fire_and_forget)
        coro->gc_flags |= XR_CORO_GC_RECYCLABLE;

    // Scope tracking: link coro to scope, register waiter on task
    {
        XrScopeContext *_scope = parent ? parent->current_scope : NULL;
        if (!_scope && runtime->current_scope)
            _scope = runtime->current_scope;
        if (_scope && parent) {
            coro->parent_scope = _scope;
            // Protect child list prepend with the scope spinlock
            while (atomic_exchange_explicit(&_scope->child_lock, true, memory_order_acquire)) {}
            coro->scope_sibling = _scope->first_child;
            _scope->first_child = coro;
            atomic_store_explicit(&_scope->child_lock, false, memory_order_release);
            atomic_fetch_add_explicit(&_scope->count, 1, memory_order_relaxed);
            atomic_fetch_add_explicit(&parent->wait_count, 1, memory_order_relaxed);
            task->waiter = parent;
            task->waiter_index = -2;  // scope mode

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
    if (task->link_mode == XR_LINK_LINKED && parent && parent->task && !coro->parent_scope) {
        xr_task_attach_child(parent->task, task);
    }

    /* monitored go: no auto-Channel here. Codegen only allocates one register
     * for go result, writing base[a+1] would corrupt the stack frame.
     * Users should call task.monitor() to get the notification Channel. */

    base[a] = xr_value_from_task(task);
    frame->pc = pc;

    // coro_init_common already sets XR_CORO_FLG_READY — no redundant atomic OR needed
    if (!parent) {
        xr_runtime_spawn(runtime, coro);
        return VM_COLD_BREAK;
    }

    parent->pending_spawn = coro;
    return VM_COLD_SPAWN_CONT;
}

#define AWAIT_TIMEOUT_SPINS 100000000

/* Recycle completed coro back to pool after await consumes result.
 * Uses recycle_local (thorough reset + pool addition) to prevent
 * memory leak from pool-allocated coros that GC cannot reclaim. */
static inline void vm_await_recycle_coro(XrCoroutine *coro) {
    XrWorker *w = xr_current_worker();
    if (w && xr_coro_flags_has(coro, XR_CORO_FLG_DONE)) {
        xr_coro_recycle_local(w, coro);
    } else {
        xr_coro_release_heap(coro);
    }
}

/* Read task->result with deep copy to dst_coro's heap, then detach + recycle executor.
 * After this call, task->result points to the copied value (safe for re-await). */
static inline XrValue vm_task_consume_result(XrayIsolate *isolate,
                                              XrTask *task,
                                              XrCoroutine *dst_coro,
                                              int discard_result) {
    XrValue res = task->result;
    if (!discard_result && dst_coro && xr_value_needs_copy(res)) {
        res = xr_deep_copy_to_coro(isolate, res, dst_coro);
        task->result = res; // update for re-await safety
    }
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

// Read coro->result with deep copy if needed
static inline XrValue vm_await_read_result(XrayIsolate *isolate,
                                            XrCoroutine *coro,
                                            XrCoroutine *current,
                                            int discard_result) {
    if (discard_result) return xr_null();
    if (!XR_IS_PTR(coro->result)) return coro->result;
    int copy_count = 0;
    XrValue v = xr_deep_copy_to_coro_counted(isolate, coro->result, current, &copy_count);
    if (current && copy_count > 0) current->reductions -= copy_count * 10;
    return v;
}

__attribute__((noinline))
int vm_await(XrayIsolate *isolate, XrVMContext *vm_ctx,
                     XrInstruction instr, XrValue *base,
                     XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int discard_result = GETARG_C(instr);

    XrValue task_val = base[b];

    /* ============ Task path (new: go returns XrTask) ============ */
    if (xr_value_is_task(task_val)) {
        XrTask *task = xr_value_to_task(task_val);

        // Fast path: task already completed — works for re-await too
        XrCoroutine *current = vm_cold_get_coro(vm_ctx);
        uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
        if (tstate == XR_TASK_COMPLETED) {
            base[a] = vm_task_consume_result(isolate, task, current, discard_result);
            return VM_COLD_BREAK;
        }
        if (tstate == XR_TASK_FAILED || tstate == XR_TASK_CANCELLED) {
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }

        // Slow path: task still active, need to suspend
        XrRuntime *rt = (XrRuntime *)isolate->vm.runtime;
        if (!rt) {
            VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "await: runtime not initialized");
        }

        // CAS on task->await_state (await coordination lives on the task)
        if (current) {
            __atomic_store_n(&task->waiter_index, -1, __ATOMIC_RELAXED);
            __atomic_store_n(&task->waiter, current, __ATOMIC_RELEASE);

            int expected = XR_AWAIT_NONE;
            if (atomic_compare_exchange_strong_explicit(&task->await_state, &expected,
                                             XR_AWAIT_WAITING,
                                             memory_order_acq_rel, memory_order_acquire)) {
                // Store task in await_task for post-check
                atomic_store_explicit(&current->await_task, task, memory_order_release);
                uint32_t old_flags = xr_coro_flags_load(current);
                uint32_t new_flags = xr_coro_set_wait_reason_flags(old_flags,
                                      XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
                atomic_store_explicit(&current->flags, new_flags, memory_order_release);
                frame->pc = pc - 1;
                return VM_COLD_BLOCKED;
            }

            if (expected == XR_AWAIT_WAITING) {
                atomic_store_explicit(&current->await_task, task, memory_order_release);
                uint32_t old_flags2 = xr_coro_flags_load(current);
                uint32_t new_flags2 = xr_coro_set_wait_reason_flags(old_flags2,
                                      XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
                atomic_store_explicit(&current->flags, new_flags2, memory_order_release);
                frame->pc = pc - 1;
                return VM_COLD_BLOCKED;
            }

            // RESOLVED: result already cached in task by executor_complete
            XR_CHECK(expected == XR_AWAIT_RESOLVED, "await: unexpected state, expected RESOLVED");
            __atomic_store_n(&task->waiter, NULL, __ATOMIC_RELAXED);
            base[a] = vm_task_consume_result(isolate, task, current, discard_result);
            atomic_store_explicit(&task->await_state, XR_AWAIT_NONE, memory_order_relaxed);
            return VM_COLD_BREAK;
        }

        // Main thread: spin wait
        int spin_count = 0;
        int total_spins = 0;
        while (!xr_task_is_done(task)) {
            if (++total_spins > AWAIT_TIMEOUT_SPINS) {
                fprintf(stderr, "[xray] warn: await: task wait timeout\n");
                base[a] = xr_null();
                return VM_COLD_BREAK;
            }
            if (!atomic_load(&rt->running)) {
                fprintf(stderr, "[xray] warn: await: runtime stopped\n");
                base[a] = xr_null();
                return VM_COLD_BREAK;
            }
            if (++spin_count > 1000) {
                spin_count = 0;
                XrWorker *w = xr_current_worker();
                if (w && w->p.timer_wheel) {
                    int64_t now = xr_monotonic_ticks();
                    xr_bump_timers(w->p.timer_wheel, now);
                }
                sched_yield();
            }
        }
        base[a] = vm_task_consume_result(isolate, task, NULL, discard_result);
        return VM_COLD_BREAK;
    }

    VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "await: expected task");
}

__attribute__((noinline))
int vm_await_timeout(XrayIsolate *isolate, XrVMContext *vm_ctx,
                             XrInstruction instr, XrValue *base,
                             XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int c = GETARG_C(instr);

    XrValue await_val = base[b];
    XrValue timeout_val = base[c];

    int64_t timeout_ms = 0;
    if (XR_IS_INT(timeout_val)) timeout_ms = XR_TO_INT(timeout_val);
    else if (XR_IS_FLOAT(timeout_val)) timeout_ms = (int64_t)XR_TO_FLOAT(timeout_val);

    XrCoroutine *caller = vm_cold_get_coro(vm_ctx);

    // Task path
    if (xr_value_is_task(await_val)) {
        XrTask *task = xr_value_to_task(await_val);
        uint8_t tstate = atomic_load_explicit(&task->state, memory_order_acquire);
        if (tstate == XR_TASK_COMPLETED) {
            base[a] = vm_task_consume_result(isolate, task, caller, 0);
            return VM_COLD_BREAK;
        }
        if (tstate == XR_TASK_FAILED || tstate == XR_TASK_CANCELLED) {
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }

        // Woken from timeout
        if (caller && xr_coro_resume_load(caller) == XR_RESUME_TIMEOUT) {
            xr_coro_resume_store(caller, XR_RESUME_OK);
            task->waiter = NULL;
            base[a] = xr_null();
            return VM_COLD_BREAK;
        }

        // Woken from normal completion (read task->await_state)
        if (caller && atomic_load_explicit(&task->await_state, memory_order_acquire) == XR_AWAIT_RESOLVED) {
            base[a] = vm_task_consume_result(isolate, task, caller, 0);
            atomic_store_explicit(&task->await_state, XR_AWAIT_NONE, memory_order_relaxed);
            if (caller->ext && atomic_load_explicit(&caller->ext->timer_active, memory_order_relaxed)) {
                XrWorker *worker = xr_current_worker();
                if (worker && worker->p.timer_wheel)
                    xr_twheel_cancel_timer(worker->p.timer_wheel, &caller->ext->timer);
                atomic_store_explicit(&caller->ext->timer_active, false, memory_order_relaxed);
            }
            return VM_COLD_BREAK;
        }

        XrRuntime *rt = (XrRuntime *)isolate->vm.runtime;
        frame->pc = pc;
        vm_ctx->stack_top = base + frame->closure->proto->maxstacksize;

        XrCoroutine *current = vm_cold_get_coro(vm_ctx);
        if (current && rt) {
            __atomic_store_n(&task->waiter_index, -1, __ATOMIC_RELAXED);
            __atomic_store_n(&task->waiter, current, __ATOMIC_RELEASE);
            current->channel_deadline = xr_monotonic_ticks() + timeout_ms;

            XrWorker *worker = xr_current_worker();
            if (worker && timeout_ms > 0)
                xr_worker_add_sleep_timer(worker, current, timeout_ms);

            uint32_t old_flags = xr_coro_flags_load(current);
            uint32_t new_flags = xr_coro_set_wait_reason_flags(old_flags, XR_CORO_WAIT_AWAIT >> XR_CORO_WAIT_SHIFT);
            atomic_store(&current->flags, new_flags);

            frame->pc = pc - 1;
            return VM_COLD_BLOCKED;
        }

        // Main thread: synchronous wait
        struct timeval start_time;
        gettimeofday(&start_time, NULL);
        int spin_count = 0;
        while (!xr_task_is_done(task)) {
            struct timeval now;
            gettimeofday(&now, NULL);
            int64_t elapsed_ms = (now.tv_sec - start_time.tv_sec) * 1000 +
                                 (now.tv_usec - start_time.tv_usec) / 1000;
            if (elapsed_ms >= timeout_ms) {
                base[a] = xr_null();
                return VM_COLD_BREAK;
            }
            XrWorker *w = xr_current_worker();
            if (w && w->p.timer_wheel) {
                int64_t tnow = xr_monotonic_ticks();
                xr_bump_timers(w->p.timer_wheel, tnow);
            }
            if (++spin_count > 1000) { spin_count = 0; sched_yield(); }
        }
        base[a] = vm_task_consume_result(isolate, task, NULL, 0);
        return VM_COLD_BREAK;
    }

    VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "await: expected task");
}

__attribute__((noinline))
int vm_await_all(XrayIsolate *isolate, XrVMContext *vm_ctx,
                         XrInstruction instr, XrValue *base,
                         XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);

    XrValue arr_val = base[b];
    if (!xr_value_is_array(arr_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH, "await.all: expected array");
    }

    XrArray *tasks = xr_value_to_array(arr_val);
    int count = xr_array_size(tasks);
    XrCoroutine *caller = xr_current_coro(isolate);

    // Task-only helpers
    #define ELEM_IS_TASK(cv) xr_value_is_task(cv)

    // Fast path: check if all done
    bool all_done = true;
    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!ELEM_IS_TASK(cv)) continue;
        if (!xr_task_is_done(xr_value_to_task(cv))) {
            all_done = false;
            break;
        }
    }

    if (all_done) {
        XrArray *results = xr_array_with_capacity(COLD_CORO(vm_ctx), count);
        results->length = count;
        XrValue *rdata = (XrValue*)results->data;
        for (int j = 0; j < count; j++) {
            XrValue cv = xr_array_get(tasks, j);
            if (!ELEM_IS_TASK(cv)) { rdata[j] = xr_null(); continue; }
            rdata[j] = vm_task_consume_result(isolate, xr_value_to_task(cv), caller, 0);
        }
        base[a] = xr_value_from_array(results);
        return VM_COLD_BREAK;
    }

    frame->pc = pc - 1;
    vm_ctx->stack_top = base + frame->closure->proto->maxstacksize;

    XrRuntime *rt = (XrRuntime *)isolate->vm.runtime;
    if (!rt) {
        VM_COLD_THROW(frame, pc, XR_ERR_CORO_DEAD, "await.all: runtime not initialized");
    }

    if (caller) {
        caller->await_results = NULL;
        atomic_store(&caller->wait_count, 1);

        for (int j = 0; j < count; j++) {
            XrValue cv = xr_array_get(tasks, j);
            if (!ELEM_IS_TASK(cv)) continue;
            atomic_fetch_add(&caller->wait_count, 1);
            XrTask *t = xr_value_to_task(cv);
            __atomic_store_n(&t->waiter_index, j, __ATOMIC_RELAXED);
            __atomic_store_n(&t->waiter, caller, __ATOMIC_RELEASE);
            if (xr_task_is_done(t)) {
                XrCoroutine *w = __atomic_exchange_n(&t->waiter, NULL, __ATOMIC_ACQ_REL);
                if (w == caller)
                    atomic_fetch_sub(&caller->wait_count, 1);
            }
        }

        int remaining = atomic_fetch_sub(&caller->wait_count, 1) - 1;
        if (remaining == 0) {
            return VM_COLD_STARTFUNC;
        }

        uint32_t old_flags = xr_coro_flags_load(caller);
        atomic_store(&caller->flags,
            xr_coro_set_wait_reason_flags(old_flags, XR_CORO_WAIT_AWAIT_ALL >> XR_CORO_WAIT_SHIFT));
        return VM_COLD_BLOCKED;
    }

    // Main thread: spin wait
    int total_spins = 0, spin_count = 0;
    for (;;) {
        if (++total_spins > AWAIT_TIMEOUT_SPINS) {
            fprintf(stderr, "[xray] warn: await.all: timeout\n");
            break;
        }
        if (!atomic_load(&rt->running)) break;
        bool ad = true;
        for (int j = 0; j < count; j++) {
            XrValue cv = xr_array_get(tasks, j);
            if (!ELEM_IS_TASK(cv)) continue;
            if (!xr_task_is_done(xr_value_to_task(cv))) { ad = false; break; }
        }
        if (ad) break;
        if (++spin_count > 1000) { spin_count = 0; sched_yield(); }
    }
    return VM_COLD_STARTFUNC;

    #undef ELEM_IS_TASK
}

__attribute__((noinline))
int vm_await_any(XrayIsolate *isolate, XrVMContext *vm_ctx,
                         XrInstruction instr, XrValue *base,
                         XrBcCallFrame *frame, XrInstruction *pc) {
    int a = GETARG_A(instr);
    int b = GETARG_B(instr);
    int mode = GETARG_C(instr);

    XrValue arr_val = base[b];
    if (!xr_value_is_array(arr_val)) {
        VM_COLD_THROW(frame, pc, XR_ERR_TYPE_MISMATCH,
            mode == 0 ? "await.any: expected array" : "await.anySuccess: expected array");
    }

    XrArray *tasks = xr_value_to_array(arr_val);
    int count = xr_array_size(tasks);
    XrCoroutine *current = vm_cold_get_coro(vm_ctx);

    // Fast path: check if any already done
    int done_count = 0;
    for (int j = 0; j < count; j++) {
        XrValue cv = xr_array_get(tasks, j);
        if (!xr_value_is_task(cv)) continue;
        XrTask *t = xr_value_to_task(cv);
        if (xr_task_is_done(t)) {
            if (mode == 1) done_count++;
            if (mode == 0 || !XR_IS_STRING(t->error)) {
                base[a] = vm_task_consume_result(isolate, t, current, 0);
                return VM_COLD_BREAK;
            }
        }
    }

    if (mode == 1 && done_count == count) {
        base[a] = xr_null();
        return VM_COLD_BREAK;
    }

    // Slow path: wait
    if (current) {
        frame->pc = pc - 1;
        atomic_store(&current->any_done, false);
        atomic_store(&current->wait_count, 1);

        for (int j = 0; j < count; j++) {
            XrValue cv = xr_array_get(tasks, j);
            if (!xr_value_is_task(cv)) continue;
            atomic_fetch_add(&current->wait_count, 1);
            int widx = (mode == 0) ? -3 : -4;
            XrTask *t = xr_value_to_task(cv);
            __atomic_store_n(&t->waiter_index, widx, __ATOMIC_RELAXED);
            __atomic_store_n(&t->waiter, current, __ATOMIC_RELEASE);
            if (xr_task_is_done(t)) {
                XrCoroutine *w = __atomic_exchange_n(&t->waiter, NULL, __ATOMIC_ACQ_REL);
                if (w == current) {
                    if (mode == 0 || !XR_IS_STRING(t->error)) {
                        bool expected = false;
                        if (atomic_compare_exchange_strong(&current->any_done, &expected, true))
                            current->result = t->result;
                    }
                    atomic_fetch_sub(&current->wait_count, 1);
                }
            }
        }

        int remaining = atomic_fetch_sub(&current->wait_count, 1) - 1;
        if (atomic_load(&current->any_done) || (mode == 1 && remaining == 0)) {
            return VM_COLD_STARTFUNC;
        }

        uint32_t old_flags = xr_coro_flags_load(current);
        atomic_store(&current->flags,
            xr_coro_set_wait_reason_flags(old_flags, XR_CORO_WAIT_AWAIT_ANY >> XR_CORO_WAIT_SHIFT));
        return VM_COLD_BLOCKED;
    } else {
        // Main thread: poll wait
        int spin = 0;
        while (true) {
            done_count = 0;
            for (int j = 0; j < count; j++) {
                XrValue cv = xr_array_get(tasks, j);
                if (!xr_value_is_task(cv)) continue;
                XrTask *t = xr_value_to_task(cv);
                if (xr_task_is_done(t)) {
                    if (mode == 1) done_count++;
                    if (mode == 0 || !XR_IS_STRING(t->error)) {
                        base[a] = vm_task_consume_result(isolate, t, NULL, 0);
                        return VM_COLD_BREAK;
                    }
                }
            }
            if (mode == 1 && done_count == count) {
                base[a] = xr_null();
                return VM_COLD_BREAK;
            }
            if (++spin > 1000) { spin = 0; sched_yield(); }
        }
    }
}
