/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_variables.c - Variable references, children expansion, set variable
 */

#include "xdap_debug.h"
#include "../../runtime/xisolate_api.h"
#include "../../runtime/xexec_state.h"
#include "../../runtime/xexec_frame.h"
#include "../../runtime/value/xvalue.h"
#include "../../runtime/value/xchunk.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/object/xstring.h"
#include "../../runtime/object/xarray.h"
#include "../../runtime/object/xmap.h"
#include "../../runtime/object/xjson.h"
#include "../../runtime/class/xinstance.h"
#include "../../runtime/symbol/xsymbol_table.h"
#include "../../coro/xcoroutine.h"
#include "../../coro/xcoro_flags.h"
#include "../../coro/xworker.h"
#include "../../base/xmalloc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Variable Reference API (for object expansion)
// ============================================================================

int xr_debug_create_var_ref(XrayIsolate *isolate, XdapVarRefType type, int frame_idx,
                            XrValue value) {
    XrDebugState *dbg = (XrDebugState *) xr_isolate_get_debug_state(isolate);
    if (!dbg)
        return 0;

    // Grow array if needed
    if (dbg->var_refs_count >= dbg->var_refs_capacity) {
        int new_cap = dbg->var_refs_capacity ? dbg->var_refs_capacity * 2 : 32;
        XrDebugVarRef *new_arr =
            (XrDebugVarRef *) xr_realloc(dbg->var_refs, (size_t) new_cap * sizeof(XrDebugVarRef));
        if (!new_arr)
            return 0;
        dbg->var_refs = new_arr;
        dbg->var_refs_capacity = new_cap;
    }

    int idx = dbg->var_refs_count++;
    int id = dbg->next_var_ref_id++;
    dbg->var_refs[idx].id = id;
    dbg->var_refs[idx].type = type;
    dbg->var_refs[idx].frame_idx = frame_idx;
    dbg->var_refs[idx].value = value;

    return id;
}

XrDebugVarRef *xr_debug_get_var_ref(XrayIsolate *isolate, int ref_id) {
    XrDebugState *dbg = (XrDebugState *) xr_isolate_get_debug_state(isolate);
    if (!dbg)
        return NULL;

    // O(1) lookup: IDs are sequential from XDAP_VAR_REF_ID_BASE
    int idx = ref_id - XDAP_VAR_REF_ID_BASE;
    if (idx < 0 || idx >= dbg->var_refs_count)
        return NULL;
    if (dbg->var_refs[idx].id != ref_id)
        return NULL;
    return &dbg->var_refs[idx];
}

void xr_debug_clear_var_refs(XrayIsolate *isolate) {
    XrDebugState *dbg = (XrDebugState *) xr_isolate_get_debug_state(isolate);
    if (!dbg)
        return;

    dbg->var_refs_count = 0;
    dbg->next_var_ref_id = XDAP_VAR_REF_ID_BASE;
}

void xr_debug_var_info_free(XdapVarInfo *info) {
    if (!info)
        return;
    xr_free(info->name);
    xr_free(info->value);
    xr_free(info->type);
}

void xr_debug_var_info_array_free(XdapVarInfo *vars, int count) {
    if (!vars)
        return;
    for (int i = 0; i < count; i++) {
        xr_free(vars[i].name);
        xr_free(vars[i].value);
        xr_free(vars[i].type);
    }
    xr_free(vars);
}

bool xr_debug_value_is_expandable(XrayIsolate *isolate, XrValue value) {
    (void) isolate;
    if (XR_IS_ARRAY(value))
        return true;
    if (XR_IS_MAP(value))
        return true;
    if (XR_IS_JSON(value))
        return true;
    if (XR_IS_PTR(value)) {
        XrGCHeader *hdr = XR_TO_PTR(value);
        if (hdr->type == XR_TINSTANCE)
            return true;
        if (hdr->type == XR_TARRAY_SLICE)
            return true;
    }
    return false;
}

XdapVarRefType xr_debug_get_ref_type(XrValue value) {
    if (XR_IS_ARRAY(value))
        return XDAP_REF_ARRAY;
    if (XR_IS_MAP(value))
        return XDAP_REF_MAP;
    if (XR_IS_JSON(value))
        return XDAP_REF_OBJECT;
    if (XR_IS_PTR(value)) {
        XrGCHeader *hdr = XR_TO_PTR(value);
        if (hdr->type == XR_TINSTANCE)
            return XDAP_REF_INSTANCE;
        if (hdr->type == XR_TARRAY_SLICE)
            return XDAP_REF_ARRAY;
    }
    return XDAP_REF_INVALID;
}

// ============================================================================
// Children Expansion Helpers
// ============================================================================

static int get_array_children(XrayIsolate *isolate, XrArray *arr, XdapVarInfo **out_vars) {
    int count = xr_array_size(arr);
    if (count == 0) {
        *out_vars = NULL;
        return 0;
    }

    XdapVarInfo *vars = (XdapVarInfo *) xr_calloc(count, sizeof(XdapVarInfo));
    if (!vars) {
        *out_vars = NULL;
        return 0;
    }

    for (int i = 0; i < count; i++) {
        XrValue elem = xr_array_get(arr, i);

        char name_buf[32];
        snprintf(name_buf, sizeof(name_buf), "[%d]", i);
        vars[i].name = xr_strdup(name_buf);
        vars[i].value = xr_value_to_debug_string(isolate, elem);
        vars[i].type = xr_strdup(xr_value_type_name(elem));
        vars[i].indexed_count = count;

        if (xr_debug_value_is_expandable(isolate, elem)) {
            vars[i].var_ref =
                xr_debug_create_var_ref(isolate, xr_debug_get_ref_type(elem), -1, elem);
        }
    }

    *out_vars = vars;
    return count;
}

static int get_map_children(XrayIsolate *isolate, XrMap *map, XdapVarInfo **out_vars) {
    uint32_t count = map->count;
    if (count == 0 || xr_map_isdummy(map)) {
        *out_vars = NULL;
        return 0;
    }

    XdapVarInfo *vars = (XdapVarInfo *) xr_calloc(count, sizeof(XdapVarInfo));
    int idx = 0;
    uint32_t size = xr_map_sizenode(map);

    for (uint32_t i = 0; i < size && idx < (int) count; i++) {
        XrMapNode *node = xr_map_node(map, i);
        if (XR_MAP_NODE_EMPTY(node))
            continue;

        char *key_str = xr_value_to_debug_string(isolate, node->key);
        vars[idx].name = key_str;
        vars[idx].value = xr_value_to_debug_string(isolate, node->value);
        vars[idx].type = xr_strdup(xr_value_type_name(node->value));
        vars[idx].named_count = count;

        if (xr_debug_value_is_expandable(isolate, node->value)) {
            vars[idx].var_ref = xr_debug_create_var_ref(isolate, xr_debug_get_ref_type(node->value),
                                                        -1, node->value);
        }
        idx++;
    }

    *out_vars = vars;
    return idx;
}

static int get_json_children(XrayIsolate *isolate, XrJson *json, XdapVarInfo **out_vars) {
    uint16_t count = xr_json_field_count(isolate, json);
    if (count == 0) {
        *out_vars = NULL;
        return 0;
    }

    XdapVarInfo *vars = (XdapVarInfo *) xr_calloc(count, sizeof(XdapVarInfo));

    XrShape *shape = xr_json_shape(isolate, json);
    for (int i = 0; i < count; i++) {
        XrValue val = xr_json_get_field_any(isolate, json, i);

        const char *field_name = NULL;
        if (shape && shape->field_symbols && i < shape->field_count) {
            SymbolId sym = shape->field_symbols[i];
            if (xr_isolate_get_symbol_table(isolate)) {
                field_name = xr_symbol_get_name_in_table(xr_isolate_get_symbol_table(isolate), sym);
            }
        }

        vars[i].name = xr_strdup(field_name ? field_name : "<field>");
        vars[i].value = xr_value_to_debug_string(isolate, val);
        vars[i].type = xr_strdup(xr_value_type_name(val));
        vars[i].named_count = count;

        if (xr_debug_value_is_expandable(isolate, val)) {
            vars[i].var_ref = xr_debug_create_var_ref(isolate, xr_debug_get_ref_type(val), -1, val);
        }
    }

    *out_vars = vars;
    return count;
}

static int get_instance_children(XrayIsolate *isolate, XrInstance *inst, XdapVarInfo **out_vars) {
    if (!inst || !inst->klass) {
        *out_vars = NULL;
        return 0;
    }

    int count = inst->klass->field_count;
    if (count == 0) {
        *out_vars = NULL;
        return 0;
    }

    XdapVarInfo *vars = (XdapVarInfo *) xr_calloc(count, sizeof(XdapVarInfo));

    for (int i = 0; i < count; i++) {
        XrValue val = inst->fields[i];

        const char *field_name = NULL;
        XrClass *klass = inst->klass;
        if (klass && klass->fields && i < klass->field_count) {
            field_name = klass->fields[i].name;
        }

        vars[i].name = xr_strdup(field_name ? field_name : "<field>");
        vars[i].value = xr_value_to_debug_string(isolate, val);
        vars[i].type = xr_strdup(xr_value_type_name(val));
        vars[i].named_count = count;

        if (xr_debug_value_is_expandable(isolate, val)) {
            vars[i].var_ref = xr_debug_create_var_ref(isolate, xr_debug_get_ref_type(val), -1, val);
        }
    }

    *out_vars = vars;
    return count;
}

static int get_scope_locals(XrayIsolate *isolate, int frame_idx, XdapVarInfo **out_vars) {
    XrDebugFrameCtx fctx;
    xr_debug_get_frame_ctx_ex(isolate, &fctx);

    if (frame_idx < 0 || frame_idx >= fctx.frame_count) {
        *out_vars = NULL;
        return 0;
    }

    int actual_idx = fctx.frame_count - 1 - frame_idx;
    XrBcCallFrame *frame = &fctx.frames[actual_idx];

    if (!frame->closure || !frame->closure->proto) {
        *out_vars = NULL;
        return 0;
    }

    XrProto *proto = frame->closure->proto;
    int count = PROTO_LOCVAR_COUNT(proto);
    if (count == 0) {
        *out_vars = NULL;
        return 0;
    }

    XdapVarInfo *vars = (XdapVarInfo *) xr_calloc(count, sizeof(XdapVarInfo));
    XrValue *base = fctx.stack + frame->base_offset;

    for (int i = 0; i < count; i++) {
        XrLocVar locvar = PROTO_LOCVAR(proto, i);
        XrValue val = base[i];

        vars[i].name = xr_strdup(locvar.name ? locvar.name : "<unnamed>");
        vars[i].value = xr_value_to_debug_string(isolate, val);
        vars[i].type = xr_strdup(xr_value_type_name(val));

        if (xr_debug_value_is_expandable(isolate, val)) {
            vars[i].var_ref =
                xr_debug_create_var_ref(isolate, xr_debug_get_ref_type(val), frame_idx, val);
        }
    }

    *out_vars = vars;
    return count;
}

int xr_debug_get_var_children(XrayIsolate *isolate, int ref_id, XdapVarInfo **out_vars) {
    XrDebugVarRef *ref = xr_debug_get_var_ref(isolate, ref_id);
    if (!ref) {
        *out_vars = NULL;
        return 0;
    }
    switch (ref->type) {
        case XDAP_REF_SCOPE_LOCALS:
            return get_scope_locals(isolate, ref->frame_idx, out_vars);

        case XDAP_REF_ARRAY:
            if (XR_IS_ARRAY(ref->value)) {
                return get_array_children(isolate, XR_TO_ARRAY(ref->value), out_vars);
            }
            // Handle ArraySlice (same structure as Array)
            if (XR_IS_PTR(ref->value)) {
                XrGCHeader *hdr = XR_TO_PTR(ref->value);
                if (hdr->type == XR_TARRAY_SLICE) {
                    return get_array_children(isolate, (XrArray *) hdr, out_vars);
                }
            }
            break;

        case XDAP_REF_MAP:
            if (XR_IS_MAP(ref->value)) {
                return get_map_children(isolate, XR_TO_MAP(ref->value), out_vars);
            }
            break;

        case XDAP_REF_OBJECT:
            if (XR_IS_PTR(ref->value)) {
                XrGCHeader *hdr = XR_TO_PTR(ref->value);
                if (hdr->type == XR_TJSON) {
                    return get_json_children(isolate, (XrJson *) hdr, out_vars);
                }
            }
            break;

        case XDAP_REF_INSTANCE:
            if (XR_IS_PTR(ref->value)) {
                XrGCHeader *hdr = XR_TO_PTR(ref->value);
                if (hdr->type == XR_TINSTANCE) {
                    return get_instance_children(isolate, (XrInstance *) hdr, out_vars);
                }
            }
            break;

        default:
            break;
    }

    *out_vars = NULL;
    return 0;
}

// ============================================================================
// Local Variable Inspection
// ============================================================================

int xr_debug_get_local_count(XrayIsolate *isolate, int frame_idx) {
    XrDebugFrameCtx fctx;
    xr_debug_get_frame_ctx_ex(isolate, &fctx);

    if (frame_idx < 0 || frame_idx >= fctx.frame_count) {
        return 0;
    }

    int actual_idx = fctx.frame_count - 1 - frame_idx;
    XrBcCallFrame *frame = &fctx.frames[actual_idx];

    if (!frame->closure || !frame->closure->proto) {
        return 0;
    }

    return PROTO_LOCVAR_COUNT(frame->closure->proto);
}

bool xr_debug_get_local(XrayIsolate *isolate, int frame_idx, int local_idx, const char **out_name,
                        char **out_value, char **out_type) {
    XrDebugFrameCtx fctx;
    xr_debug_get_frame_ctx_ex(isolate, &fctx);

    if (frame_idx < 0 || frame_idx >= fctx.frame_count) {
        return false;
    }

    int actual_idx = fctx.frame_count - 1 - frame_idx;
    XrBcCallFrame *frame = &fctx.frames[actual_idx];

    if (!frame->closure || !frame->closure->proto) {
        return false;
    }

    XrProto *proto = frame->closure->proto;

    int locvar_count = PROTO_LOCVAR_COUNT(proto);
    if (local_idx < 0 || local_idx >= locvar_count) {
        return false;
    }

    // Get local name from debug info
    if (out_name) {
        XrLocVar locvar = PROTO_LOCVAR(proto, local_idx);
        if (locvar.name) {
            *out_name = locvar.name;
        } else {
            *out_name = "<unnamed>";
        }
    }

    // Get value from stack
    XrValue *base = fctx.stack + frame->base_offset;
    XrValue val = base[local_idx];

    if (out_value) {
        *out_value = xr_value_to_debug_string(isolate, val);
    }

    if (out_type) {
        *out_type = xr_strdup(xr_value_type_name(val));
    }

    return true;
}

// ============================================================================
// Set Variable API
// ============================================================================

// Parse a debug value string into XrValue. Returns true on success.
static bool parse_debug_value(XrayIsolate *isolate, const char *value, XrValue *out) {
    if (!value)
        return false;

    if (strcmp(value, "null") == 0) {
        *out = xr_null();
        return true;
    }
    if (strcmp(value, "true") == 0) {
        *out = xr_bool(true);
        return true;
    }
    if (strcmp(value, "false") == 0) {
        *out = xr_bool(false);
        return true;
    }
    if (value[0] == '"') {
        size_t len = strlen(value);
        if (len >= 2 && value[len - 1] == '"') {
            XrString *str = xr_string_intern(isolate, value + 1, len - 2, 0);
            *out = xr_string_value(str);
            return true;
        }
        return false;
    }
    // Try integer then float
    char *endptr = NULL;
    long long ival = strtoll(value, &endptr, 10);
    if (*endptr == '\0') {
        *out = xr_int(ival);
        return true;
    }
    double fval = strtod(value, &endptr);
    if (*endptr == '\0') {
        *out = xr_float(fval);
        return true;
    }
    return false;
}

char *xr_debug_set_variable(XrayIsolate *isolate, int var_ref, const char *name,
                            const char *value) {
    if (!isolate || !name || !value)
        return NULL;

    XrDebugState *dbg = (XrDebugState *) xr_isolate_get_debug_state(isolate);
    if (!dbg)
        return NULL;

    XrValue new_val;
    if (!parse_debug_value(isolate, value, &new_val))
        return NULL;

    // Find the variable reference
    XrDebugVarRef *ref = xr_debug_get_var_ref(isolate, var_ref);

    // Scope locals: find register by name and write
    if (!ref || ref->type == XDAP_REF_SCOPE_LOCALS) {
        int frame_idx = ref ? ref->frame_idx : 0;
        XrDebugFrameCtx fctx;
        xr_debug_get_frame_ctx_ex(isolate, &fctx);

        if (frame_idx < 0 || frame_idx >= fctx.frame_count)
            return NULL;

        int actual_idx = fctx.frame_count - 1 - frame_idx;
        XrBcCallFrame *frame = &fctx.frames[actual_idx];
        if (!frame->closure || !frame->closure->proto)
            return NULL;

        XrProto *proto = frame->closure->proto;
        XrValue *base = fctx.stack + frame->base_offset;

        int locvar_count = PROTO_LOCVAR_COUNT(proto);
        for (int i = 0; i < locvar_count; i++) {
            XrLocVar locvar = PROTO_LOCVAR(proto, i);
            if (locvar.name && strcmp(locvar.name, name) == 0) {
                base[i] = new_val;
                return xr_value_to_debug_string(isolate, new_val);
            }
        }
        return NULL;
    }

    // Array element: name is "[N]"
    if (ref->type == XDAP_REF_ARRAY && XR_IS_PTR(ref->value)) {
        XrArray *arr = (XrArray *) XR_TO_PTR(ref->value);
        int idx = atoi(name[0] == '[' ? name + 1 : name);
        if (idx >= 0 && idx < xr_array_size(arr)) {
            xr_array_set(arr, idx, new_val);
            return xr_value_to_debug_string(isolate, new_val);
        }
        return NULL;
    }

    // Map entry: name is the key display string
    if (ref->type == XDAP_REF_MAP && XR_IS_PTR(ref->value)) {
        XrMap *map = (XrMap *) XR_TO_PTR(ref->value);
        // Try to find the entry by iterating and matching display name
        uint32_t size = xr_map_sizenode(map);
        for (uint32_t i = 0; i < size; i++) {
            XrMapNode *node = xr_map_node(map, i);
            if (XR_MAP_NODE_EMPTY(node))
                continue;
            char *key_str = xr_value_to_debug_string(isolate, node->key);
            bool match = key_str && strcmp(key_str, name) == 0;
            xr_free(key_str);
            if (match) {
                node->value = new_val;
                return xr_value_to_debug_string(isolate, new_val);
            }
        }
        return NULL;
    }

    // Json object field
    if (ref->type == XDAP_REF_OBJECT && XR_IS_PTR(ref->value)) {
        XrJson *json = (XrJson *) XR_TO_PTR(ref->value);
        xr_json_set_by_key(isolate, json, name, new_val);
        return xr_value_to_debug_string(isolate, new_val);
    }

    // Instance field
    if (ref->type == XDAP_REF_INSTANCE && XR_IS_PTR(ref->value)) {
        XrInstance *inst = (XrInstance *) XR_TO_PTR(ref->value);
        xr_instance_set_field(isolate, inst, name, new_val);
        return xr_value_to_debug_string(isolate, new_val);
    }

    return NULL;
}

// ============================================================================
// Coroutine Debugging
// ============================================================================

// Map coroutine state + wait reason to human-readable string
static const char *coro_state_string(XrCoroutine *coro) {
    uint8_t st = atomic_load_explicit(&coro->coro_state, memory_order_relaxed);
    switch (st) {
        case XR_CORO_STATE_RUNNING:
            return "running";
        case XR_CORO_STATE_READY:
            return "ready";
        case XR_CORO_STATE_DONE:
            return "done";
        case XR_CORO_STATE_BLOCKED: {
            uint32_t flags = atomic_load_explicit(&coro->flags, memory_order_relaxed);
            uint32_t wait = flags & XR_CORO_WAIT_MASK;
            switch (wait) {
                case XR_CORO_WAIT_CHANNEL_SEND:
                    return "blocked (channel send)";
                case XR_CORO_WAIT_CHANNEL_RECV:
                    return "blocked (channel recv)";
                case XR_CORO_WAIT_AWAIT:
                    return "blocked (await)";
                case XR_CORO_WAIT_AWAIT_ALL:
                    return "blocked (await all)";
                case XR_CORO_WAIT_SLEEP:
                    return "blocked (sleep)";
                case XR_CORO_WAIT_IO:
                    return "blocked (I/O)";
                case XR_CORO_WAIT_SELECT:
                    return "blocked (select)";
                default:
                    return "blocked";
            }
        }
        default:
            return "unknown";
    }
}

int xr_debug_get_coro_count(XrayIsolate *isolate) {
    if (!xr_isolate_get_vm_state(isolate)->coro_state)
        return 0;

    XrCoroState *sched = (XrCoroState *) xr_isolate_get_vm_state(isolate)->coro_state;
    int count = 0;

    // Ready queues (all priority levels)
    for (int p = 0; p < XR_CORO_PRIORITY_COUNT; p++) {
        XrCoroutine *coro = sched->ready_head[p];
        while (coro) {
            count++;
            coro = coro->sched_link;
        }
    }

    // Blocked coroutines in per-worker queues
    XrRuntime *rt = (XrRuntime *) xr_isolate_get_vm_state(isolate)->runtime;
    if (rt) {
        for (int w = 0; w < rt->worker_count; w++) {
            XrCoroutine *bc = rt->workers[w].p.blocked_head;
            while (bc) {
                count++;
                bc = bc->sched_link;
            }
        }
    }

    return count;
}

bool xr_debug_get_coro_info(XrayIsolate *isolate, int coro_idx, int *out_id, const char **out_name,
                            const char **out_state) {
    if (!xr_isolate_get_vm_state(isolate)->coro_state)
        return false;

    XrCoroState *sched = (XrCoroState *) xr_isolate_get_vm_state(isolate)->coro_state;
    int idx = 0;
    XrCoroutine *target = NULL;

    // Ready queues
    if (!target) {
        for (int p = 0; p < XR_CORO_PRIORITY_COUNT && !target; p++) {
            XrCoroutine *coro = sched->ready_head[p];
            while (coro && !target) {
                if (idx == coro_idx)
                    target = coro;
                idx++;
                coro = coro->sched_link;
            }
        }
    }

    // Blocked coroutines in per-worker queues
    if (!target) {
        XrRuntime *rt = (XrRuntime *) xr_isolate_get_vm_state(isolate)->runtime;
        if (rt) {
            for (int w = 0; w < rt->worker_count && !target; w++) {
                XrCoroutine *bc = rt->workers[w].p.blocked_head;
                while (bc && !target) {
                    if (idx == coro_idx)
                        target = bc;
                    idx++;
                    bc = bc->sched_link;
                }
            }
        }
    }

    if (!target)
        return false;

    if (out_id)
        *out_id = target->id;
    if (out_name)
        *out_name = target->name ? target->name : "<unnamed>";
    if (out_state)
        *out_state = coro_state_string(target);

    return true;
}

// ============================================================================
// Disassembly API
// ============================================================================

static char *format_instruction(XrProto *proto, int offset) {
    XrInstruction inst = PROTO_CODE(proto, offset);
    OpCode op = GET_OPCODE(inst);
    const char *name = xr_opcode_name(op);

    char buf[256];
    uint8_t a = GETARG_A(inst);
    uint8_t b = GETARG_B(inst);
    uint8_t c = GETARG_C(inst);
    int sbx = GETARG_sBx(inst);
    uint16_t bx = GETARG_Bx(inst);
    (void) proto;

    snprintf(buf, sizeof(buf), "%-16s A=%d B=%d C=%d Bx=%d sBx=%d", name, a, b, c, bx, sbx);

    return xr_strdup(buf);
}

static char *get_instruction_comment(XrProto *proto, int offset) {
    XrInstruction inst = PROTO_CODE(proto, offset);
    OpCode op = GET_OPCODE(inst);
    uint16_t bx = GETARG_Bx(inst);

    // Show constant value for LOADK
    if (op == OP_LOADK && bx < (uint16_t) PROTO_CONST_COUNT(proto)) {
        XrValue val = PROTO_CONSTANT(proto, bx);
        if (XR_IS_STRING(val)) {
            XrString *str = XR_TO_STRING(val);
            char buf[64];
            snprintf(buf, sizeof(buf), "; \"%.*s\"", (int) (str->length > 40 ? 40 : str->length),
                     str->data);
            return xr_strdup(buf);
        } else if (XR_IS_INT(val)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "; %lld", (long long) XR_TO_INT(val));
            return xr_strdup(buf);
        } else if (XR_IS_FLOAT(val)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "; %g", XR_TO_FLOAT(val));
            return xr_strdup(buf);
        }
    }

    // Show function name for CLOSURE
    if (op == OP_CLOSURE && bx < (uint16_t) PROTO_PROTO_COUNT(proto)) {
        XrProto *child = PROTO_PROTO(proto, bx);
        if (child && child->name) {
            char buf[64];
            snprintf(buf, sizeof(buf), "; fn %s", child->name->data);
            return xr_strdup(buf);
        }
    }

    return NULL;
}

int xr_debug_get_disassembly(XrayIsolate *isolate, int frame_idx, XdapDisasmInstr **out_instrs,
                             int *out_count) {
    if (!isolate || !out_instrs || !out_count)
        return -1;

    *out_instrs = NULL;
    *out_count = 0;

    XrDebugFrameCtx fctx;
    xr_debug_get_frame_ctx_ex(isolate, &fctx);

    if (frame_idx < 0 || frame_idx >= fctx.frame_count)
        return -1;

    int actual_idx = fctx.frame_count - 1 - frame_idx;
    XrBcCallFrame *frame = &fctx.frames[actual_idx];

    if (!frame->closure || !frame->closure->proto)
        return -1;

    XrProto *proto = frame->closure->proto;
    int code_count = PROTO_CODE_COUNT(proto);

    if (code_count == 0)
        return 0;

    XdapDisasmInstr *instrs = (XdapDisasmInstr *) xr_calloc(code_count, sizeof(XdapDisasmInstr));

    for (int i = 0; i < code_count; i++) {
        instrs[i].offset = i;

        // Get line number
        int lineinfo_count = DYNARRAY_COUNT(&proto->lineinfo);
        instrs[i].line = (i < lineinfo_count) ? PROTO_LINE(proto, i) : -1;

        // Format instruction
        instrs[i].instruction = format_instruction(proto, i);
        instrs[i].comment = get_instruction_comment(proto, i);
    }

    *out_instrs = instrs;
    *out_count = code_count;
    return 0;
}

int xr_debug_get_current_pc(XrayIsolate *isolate, int frame_idx) {
    if (!isolate)
        return -1;

    XrDebugFrameCtx fctx;
    xr_debug_get_frame_ctx_ex(isolate, &fctx);

    if (frame_idx < 0 || frame_idx >= fctx.frame_count)
        return -1;

    int actual_idx = fctx.frame_count - 1 - frame_idx;
    XrBcCallFrame *frame = &fctx.frames[actual_idx];

    if (!frame->closure || !frame->closure->proto)
        return -1;

    // PC is stored as pointer, convert to offset
    XrProto *proto = frame->closure->proto;
    XrInstruction *code_start = &PROTO_CODE(proto, 0);
    int pc = (int) (frame->pc - code_start);

    // PC points to next instruction, so current is pc-1
    return (pc > 0) ? pc - 1 : 0;
}

void xr_debug_free_disasm(XdapDisasmInstr *instrs, int count) {
    if (!instrs)
        return;

    for (int i = 0; i < count; i++) {
        xr_free(instrs[i].instruction);
        xr_free(instrs[i].comment);
    }
    xr_free(instrs);
}

// ============================================================================
// Async Stack Trace (lazy capture - walk parent chain)
// ============================================================================

bool xr_debug_get_async_stack(XrayIsolate *isolate, int *out_depth, const char ***out_names,
                              const char ***out_files, int **out_lines) {
    if (!isolate || !out_depth)
        return false;

    XrDebugState *dbg = (XrDebugState *) xr_isolate_get_debug_state(isolate);
    if (!dbg)
        return false;

    *out_depth = 0;
    if (out_names)
        *out_names = NULL;
    if (out_files)
        *out_files = NULL;
    if (out_lines)
        *out_lines = NULL;

    // Get current coroutine from VM state
    XrCoroutine *coro = (XrCoroutine *) xr_isolate_get_vm_state(isolate)->current_coro;
    if (!coro || !coro->parent_coro)
        return false;

    // Walk parent chain to build async stack trace
    int depth = 0;
    XrCoroutine *c = coro;

    while (c && depth < 8) {
        if (c->spawn_file || c->spawn_line > 0) {
            dbg->async_names[depth] = "<go>";
            dbg->async_files[depth] = c->spawn_file;
            dbg->async_lines[depth] = c->spawn_line;
            depth++;
        }
        c = c->parent_coro;
    }

    if (depth == 0)
        return false;

    *out_depth = depth;
    if (out_names)
        *out_names = dbg->async_names;
    if (out_files)
        *out_files = dbg->async_files;
    if (out_lines)
        *out_lines = dbg->async_lines;

    return true;
}

// ============================================================================
// Hot Reload (Not Implemented — see comments in xdap_debug.c for rationale)
// ============================================================================

bool xr_debug_hot_reload(XrayIsolate *isolate, const char *path) {
    (void) isolate;
    (void) path;
    return false;
}
