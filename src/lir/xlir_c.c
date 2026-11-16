/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_c.c - Direct scalar C calls and parallel SSA edge copies
 *
 * KEY CONCEPT:
 *   Wrapping arithmetic widens to unsigned before operations. Signed
 *   reconstruction uses only representable conversions. I64 division guards
 *   possible zero through the located runtime panic boundary, then selects
 *   the wrapping MIN/-1 result before evaluating signed C operators.
 *   No interpreter frame is emitted.
 */
#include "xlir_c.h"
#include "xlir_verify.h"
#include "xlir_internal.h"
#include "xlir_builtin_admission.h"
#include "xlir_state.h"
#include "xlir_state_projection.h"
#include "xlir_pod.h"
#include "xlir_value_layout.h"
#include "xlir_value_homes.h"
#include "xlir_value_image.h"
#include "xray_native_array_slice.h"
#include "xi_cgen_verify_output.h"
#include "../base/xchecks.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct LirCConstructedType {
    XrLirTypeView shape;
    XrLirTypeLayout layout;
    uint32_t pod_index;
    char name[32];
} LirCConstructedType;

enum { LIR_C_FUNCTION_VISITED = 1u, LIR_C_FUNCTION_STRING_BINDING = 2u };

typedef struct LirC {
    const XrLirCRequest *request;
    XrLirCDiagnostic *diagnostic;
    XrLirCStatus status;
    char *text;
    size_t length, capacity;
    uint32_t *closure;
    uint32_t closure_count;
    uint8_t *seen, *fixed_divisors, *used_literals, *used_map_literals, *callable_used;
    LirCConstructedType *constructed_types;
    uint32_t constructed_count, pod_count;
    XrLirFunction function;
    XrLirFunctionView view;
    uint32_t block, instruction, scratch;
    XrLirSource source;
    uint32_t location_count, location_cursor;
    bool runtime, runtime_header, f64, state, arrays, views, displays, maps;
    bool string_values, inline_values, callables, cells;
    XrLirStateLayout state_layout;
    XrtBillingOwner *value_billing;
    XrLirValueLayouts *value_layouts;
    XrLirOwnershipPlan *ownership;
    XrLirValueHomes **value_homes;
    XrLirValueImage *value_image;
    XrLirStateProjection *state_projection;
    const XrLirValueHomes *homes;
    struct LirCValueTypes *value_types;
    const XrLirHomeEvent **callable_events;
    size_t callable_scope_event;
    const XrLirHomeEvent **cell_events;
    size_t cell_operation_cursor, cell_scope_cursor;
    _Atomic(XrCompileResourceStatus) value_allocation_failure;
    size_t home_edge_cursor;
    size_t string_event_cursor, string_workspace_cursor;
    size_t map_stage_event_cursor, map_stage_workspace_cursor;
    size_t view_call_cursor, view_event_cursor;
} LirC;
static void c_values_emit_types(LirC *);
static void c_values_function(LirC *);
static void c_values_declarations(LirC *);
static bool c_values_declare(LirC *, uint32_t);
static bool c_values_instruction(LirC *, const XrLirInstructionView *, uint32_t);
static void c_values_helpers(LirC *);
static void c_values_landing_types(LirC *);
static bool c_values_signature(LirC *, bool);
static void c_values_root(LirC *);
static bool c_values_edge(LirC *, const XrLirEdgeView *, uint32_t);
static void c_values_ensure(LirC *, uint32_t);
static void c_values_replace_begin(LirC *, uint32_t, uint32_t);
static void c_values_replace_end(LirC *, uint32_t);
static bool c_values_owned_type(LirC *, XrLirType);
static void c_cell_scope_release(LirC *, uint32_t);
static void c_cell_events_prepare(LirC *);
static void c_cell_instruction(LirC *, const XrLirInstructionView *, uint32_t);
static void c_map_binding_stage_copy(LirC *, uint32_t, uint32_t, uint32_t);

static void c_error(LirC *c, XrLirCStatus status, const char *message) {
    if (c->status != XR_LIR_C_OK)
        return;
    c->status = status;
    if (c->diagnostic) {
        *c->diagnostic = (XrLirCDiagnostic) {.status = status,
            .function = c->function.id, .block = c->block, .instruction = c->instruction,
            .source = c->source};
        snprintf(c->diagnostic->message, sizeof(c->diagnostic->message), "%s", message);
    }
}
static bool c_resource(LirC *c, XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK)
        return c->status == XR_LIR_C_OK;
    XrLirCStatus mapped;
    switch (status) {
        case XR_COMPILE_RESOURCE_OUT_OF_MEMORY: mapped = XR_LIR_C_OUT_OF_MEMORY; break;
        case XR_COMPILE_RESOURCE_MEMORY_LIMIT: mapped = XR_LIR_C_MEMORY_LIMIT; break;
        case XR_COMPILE_RESOURCE_OVERFLOW: mapped = XR_LIR_C_OVERFLOW; break;
        case XR_COMPILE_RESOURCE_CANCELLED: mapped = XR_LIR_C_CANCELLED; break;
        case XR_COMPILE_RESOURCE_DEADLINE: mapped = XR_LIR_C_DEADLINE; break;
        case XR_COMPILE_RESOURCE_CLOCK_FAILURE: mapped = XR_LIR_C_CLOCK_FAILURE; break;
        default: mapped = XR_LIR_C_BAD_ARGUMENT; break;
    }
    c_error(c, mapped, "C emission resource operation failed");
    return false;
}
static bool c_request_poll(LirC *c) {
    if (c->status != XR_LIR_C_OK)
        return false;
    return !c->request->control ||
        c_resource(c, xr_compile_request_control_poll(c->request->control));
}
static void c_bytes(LirC *c, const char *bytes, size_t length) {
    if (c->status != XR_LIR_C_OK)
        return;
    if (length > SIZE_MAX - c->length - 1) {
        c_error(c, XR_LIR_C_OVERFLOW, "C text exceeds machine capacity");
        return;
    }
    size_t required = c->length + length + 1;
    if (required > c->capacity) {
        size_t grown = c->capacity ? c->capacity : 1024;
        while (grown < required && grown <= SIZE_MAX / 2)
            grown *= 2;
        if (grown < required)
            grown = required;
        void *memory = c->text;
        if (!c_resource(c, xr_compile_resources_resize(c->request->resources, &memory, grown)))
            return;
        c->text = memory;
        c->capacity = grown;
    }
    XR_DCHECK(c->capacity >= required, "owned text contains the appended bytes and terminator");
    memcpy(c->text + c->length, bytes, length);
    c->length += length;
    c->text[c->length] = '\0';
}
static void c_text(LirC *c, const char *text) {
    c_bytes(c, text, strlen(text));
}
static void c_format(LirC *c, const char *format, ...) XR_PRINTF_FMT(2, 3);
static void c_format(LirC *c, const char *format, ...) {
    if (c->status != XR_LIR_C_OK)
        return;
    char buffer[256];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t) length >= sizeof(buffer)) {
        c_error(c, XR_LIR_C_BUDGET, "C formatting capacity exceeded");
        return;
    }
    c_bytes(c, buffer, (size_t) length);
}
static bool c_managed_leaf(XrLirType type) {
    return type == XR_LIR_STRING || type == XR_LIR_ARRAY || type == XR_LIR_MAP_STRING_I64;
}
static const char *c_builtin_type(XrLirType type) {
    if (type == XR_LIR_I64_CELL) return "int64_t *";
    if (type == XR_LIR_FUNCTION_SCOPE) return "uint32_t";
    if (type == XR_LIR_ARRAY || type == XR_LIR_MAP_STRING_I64) return "XrtManagedHeader *";
    if (type == XR_LIR_STRING_CELL || type == XR_LIR_STRING_REF ||
        type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF ||
        type == XR_LIR_MAP_STRING_I64_CELL || type == XR_LIR_MAP_STRING_I64_REF) return "XrtPlace";
    static const char *const types[] = {NULL, "void", "_Bool", "int8_t", "int16_t",
        "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        NULL, "double", "void *", NULL, "XrtManagedHeader *", NULL,
        "XrtModuleState *", "XrtModuleRef *", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL};
    return (unsigned) type < sizeof(types) / sizeof(types[0]) ? types[type] : NULL;
}
static unsigned c_width(XrLirType type) {
    XR_DCHECK(type >= XR_LIR_I8 && type <= XR_LIR_U64, "integer has a fixed width");
    unsigned ordinal = type <= XR_LIR_I64 ? (unsigned) (type - XR_LIR_I8)
        : (unsigned) (type - XR_LIR_U8);
    return 8u << ordinal;
}
static uint64_t c_mask(XrLirType type) {
    unsigned width = c_width(type);
    return width == 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
}
static void c_function_name(LirC *c, uint32_t id) {
    if (id == c->request->entry.id && !c->runtime)
        c_text(c, "xray_lir_entry");
    else
        c_format(c, "xrl_f%" PRIu32, id);
}
static void c_value(LirC *c, uint32_t id) {
    XR_DCHECK(id && id <= c->view.value_count, "operand belongs to this function");
    for (uint32_t p = 0; p < c->view.parameter_count; ++p) {
        if (c->view.parameters[p] == id) {
            c_format(c, "p%" PRIu32, p);
            return;
        }
    }
    if (c->homes && (c->homes->values[id].owner_count || c->homes->values[id].view_workspace)) {
        const XrLirValueHome *home = &c->homes->values[id];
        /* Project only the consumed home; rebuilding every address at each
         * acquisition point makes generated code quadratic in owned values. */
        c_format(c, "(*(XrlValue_%" PRIu32 " *)((unsigned char *)xrl_payload + %zu))", home->type, home->offset);
    } else if (c->homes && c->homes->bindings && c->homes->bindings[id].kind == XR_LIR_HOME_BINDING_LOCAL)
        c_format(c, "(XrtPlace){.frame=xrl_frame, .slot=%zu, .nulls=XRT_PLACE_NONNULL}", c->homes->bindings[id].first_owner);
    else c_format(c, "v%" PRIu32, id);
}
static XrLirType c_value_type(LirC *c, uint32_t id) {
    XrLirType type = XR_LIR_TYPE_INVALID;
    XrLirValue value = {c->request->module, c->function.id, id};
    XrLirStatus status = xr_lir_value_type(c->request->module, value, &type);
    XR_CHECK(status == XR_LIR_OK, "validated C operand has a type");
    return type;
}
static bool c_slice_type(const LirC *c, XrLirType type) {
    return type >= XR_LIR_TYPE_COUNT && type - XR_LIR_TYPE_COUNT < c->constructed_count &&
        c->constructed_types[type - XR_LIR_TYPE_COUNT].shape.kind == XR_LIR_TYPE_SLICE;
}
static bool c_located(const XrLirSource *source) {
    return source->path && source->path_length && source->line && source->column;
}
static bool c_scalar(LirC *, XrLirType);
static void c_visit(LirC *, uint32_t);
static bool c_cell_type(const LirC *c, XrLirType type, XrLirCellType *cell) {
    return xr_lir_cell_value_type(c->request->module,type,cell);
}
static bool c_cell_point(LirC *c, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_CELL_NEW || v->op == XR_LIR_FUNCTION_BIND || v->op == XR_LIR_FUNCTION_SCOPE_LEAVE)
        return v->result && c_cell_type(c,v->type,NULL);
    return (v->op == XR_LIR_CELL_READ || v->op == XR_LIR_CELL_WRITE || v->op == XR_LIR_MAP_BORROW ||
        v->op == XR_LIR_VALUE_RETAIN || v->op == XR_LIR_VALUE_DROP) && v->operand_count &&
        c_cell_type(c,c_value_type(c,v->operands[0]),NULL);
}
#include "xlir_c_fixed.inc.c"
#include "xlir_c_nullable.inc.c"
#include "xlir_c_callable.inc.c"
static bool c_scalar(LirC *c, XrLirType type) {
    XrLirTypeView shape = {0};
    XrLirCellType cell;
    if (c_cell_type(c,type,&cell)) {
        c->runtime = c->cells = true;
        c->maps |= cell.payload == XR_LIR_MAP_STRING_I64;
        return true;
    }
    if (type == XR_LIR_FUNCTION_SCOPE || c_callable_type(c,type,NULL)) {
        c->callables = true;
        return true;
    }
    if (type == XR_LIR_VIEW_FRAME || type == XR_LIR_VIEW_CELL ||
        type == XR_LIR_SLICE_I64 ||
        (type >= XR_LIR_TYPE_COUNT && xr_lir_type_view(c->request->module, type, &shape) == XR_LIR_OK &&
            (shape.kind == XR_LIR_TYPE_CALLABLE || shape.kind == XR_LIR_TYPE_CELL))) {
        char message[128];
        snprintf(message, sizeof(message), "value type %" PRIu32 " has no bound target C execution interface", type);
        c_error(c, XR_LIR_C_UNSUPPORTED, message);
        return false;
    }
    if (!c_type(c, type))
        c_error(c, XR_LIR_C_UNSUPPORTED, "C backend requires integer, bool, unit, f64 transport or data pointer types");
    c->f64 |= type == XR_LIR_F64;
    c->string_values |= type == XR_LIR_STRING || type == XR_LIR_STRING_CELL || type == XR_LIR_STRING_REF;
    c->state |= type == XR_LIR_STATE || type == XR_LIR_STATE_REF;
    c->arrays |= type == XR_LIR_ARRAY || type == XR_LIR_ARRAY_CELL || type == XR_LIR_ARRAY_REF;
    c->maps |= type == XR_LIR_MAP_STRING_I64 || type == XR_LIR_MAP_STRING_I64_CELL || type == XR_LIR_MAP_STRING_I64_REF;
    c->views |= c_slice_type(c, type);
    c->runtime |= c->string_values || c->state || c->arrays || c->views || c->maps;
    return c->status == XR_LIR_C_OK;
}
static bool c_runtime_call(LirC *c, const XrLirInstructionView *v) {
    if (v->op != XR_LIR_CALL)
        return false;
    XrLirFunction callee = {c->request->module, v->callee};
    XrLirFunctionView view;
    xr_lir_function_view(c->request->module, callee, &view);
    return view.kind == XR_LIR_FUNCTION_EXTERNAL;
}
static bool c_display_point(LirC *, const XrLirInstructionView *);
static bool c_string_binding_point(LirC *c, const XrLirInstructionView *v) {
    uint32_t role = xr_lir_string_role(c->request->module, v);
    if (role > XR_LIR_STRING_OP_APPEND_OWNED && role < XR_LIR_STRING_OP_END) return true;
    if (v->op == XR_LIR_CELL_NEW) return v->type == XR_LIR_STRING_CELL;
    if (v->op == XR_LIR_STRING_CELL_DROP) return true;
    return (v->op == XR_LIR_CELL_READ || v->op == XR_LIR_CELL_WRITE ||
        v->op == XR_LIR_STATE_READ || v->op == XR_LIR_STATE_WRITE) && v->operand_count &&
        xr_lir_string_binding(c->request->module,
            (XrLirValue){c->request->module, c->function.id, v->operands[0]});
}
static bool c_string_point(LirC *c, const XrLirInstructionView *v) {
    if (c_string_binding_point(c, v) || c_display_point(c, v) || xr_lir_string_role(c->request->module, v) ||
        xr_lir_range_point(c->request->module, v)) return true;
    if (v->op == XR_LIR_STRING_LITERAL || v->op == XR_LIR_STRING_I64 || v->op == XR_LIR_STRING_F64 ||
        v->op == XR_LIR_STRING_CONCAT || v->op == XR_LIR_STRING_RETAIN ||
        v->op == XR_LIR_STRING_PRINT || v->op == XR_LIR_STRING_LEN) return true;
    return (v->op == XR_LIR_CELL_NEW && v->type == XR_LIR_STRING_CELL) ||
        (v->op == XR_LIR_CELL_READ && v->type == XR_LIR_STRING) ||
        (v->op == XR_LIR_CELL_WRITE && c_value_type(c, v->operands[0]) == XR_LIR_STRING_CELL);
}
#include "xlir_c_state.inc.c"
#include "xlir_c_array.inc.c"
#include "xlir_c_map.inc.c"
#include "xlir_c_record.inc.c"
#include "xlir_c_view.inc.c"
#include "xlir_c_display.inc.c"
#include "xlir_c_string_library.inc.c"
static void c_visit(LirC *c, uint32_t id) {
    XR_DCHECK(id && id <= xr_lir_function_count(c->request->module), "valid callee");
    if (!(c->seen[id - 1] & LIR_C_FUNCTION_VISITED)) {
        XR_DCHECK(c->closure_count < xr_lir_function_count(c->request->module),
                  "each closure function enters the queue once");
        c->seen[id - 1] |= LIR_C_FUNCTION_VISITED;
        c->closure[c->closure_count++] = id;
    }
}
static bool c_has_division(LirC *c) {
    for (uint32_t b = 0; b < c->view.block_count && c->status == XR_LIR_C_OK; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->request->module, c->function, b, &block);
        xr_lir_block_view(c->request->module, block, &view);
        c->block = block.id;
        if (!c_request_poll(c))
            return false;
        for (uint32_t i = 0; i < view.instruction_count && c->status == XR_LIR_C_OK; ++i) {
            XrLirInstructionView v;
            xr_lir_instruction_view(c->request->module, block, i, &v);
            if (v.op == XR_LIR_DIV || v.op == XR_LIR_REM)
                return true;
        }
    }
    return false;
}
/* SSA verification already supplies unique definitions and dominance. Facts
 * are local to this function and never inferred through aliases or edges. */
static void c_prepare_divisors(LirC *c) {
    c->block = c->instruction = 0;
    c->source = (XrLirSource) {0};
    xr_compile_resources_free(c->fixed_divisors);
    c->fixed_divisors = NULL;
    if (!c_has_division(c) || c->status != XR_LIR_C_OK)
        return;
    void *facts = NULL;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources,
                                                  (size_t)c->view.value_count + 1, 1, &facts)))
        return;
    c->fixed_divisors = facts;
    for (uint32_t b = 0; b < c->view.block_count && c->status == XR_LIR_C_OK; ++b) {
        XrLirBlock block;
        XrLirBlockView view;
        xr_lir_block_at(c->request->module, c->function, b, &block);
        xr_lir_block_view(c->request->module, block, &view);
        c->block = block.id;
        if (!c_request_poll(c))
            return;
        for (uint32_t i = 0; i < view.instruction_count && c->status == XR_LIR_C_OK; ++i) {
            XrLirInstructionView v;
            xr_lir_instruction_view(c->request->module, block, i, &v);
            if (v.op == XR_LIR_CONST && v.type == XR_LIR_I64 && v.bits)
                c->fixed_divisors[v.result] = 1;
        }
    }
}
static bool c_arithmetic_point(const LirC *c, const XrLirInstructionView *v) {
    return v->op == XR_LIR_FTOI || ((v->op == XR_LIR_DIV || v->op == XR_LIR_REM) &&
        c->fixed_divisors && !c->fixed_divisors[v->operands[1]]);
}
static void c_admit_instruction(LirC *c, const XrLirInstructionView *v) {
    if (!c_request_poll(c))
        return;
    c_scalar(c, v->type);
    if (c_cell_point(c,v)) {
        c->runtime = c->cells = true;
        if (c_located(&v->source)) {
            if (c->location_count == UINT32_MAX) c_error(c,XR_LIR_C_OVERFLOW,"C source inventory overflows");
            else ++c->location_count;
        }
        return;
    }
    if (v->op == XR_LIR_RET && v->operand_count && c_callable_type(c,c->view.result,NULL)) {
        XrLirCallableValueFact returned;
        if (!c_callable_fact(c,v->operands[0],&returned)) return;
        if (returned.transport != XR_LIR_CALLABLE_TRANSPORT_OWNED && returned.transport != XR_LIR_CALLABLE_TRANSPORT_BORROW) {
            c_error(c,XR_LIR_C_UNSUPPORTED,"Frame environment cannot escape its native activation"); return;
        }
    }
    if (v->op == XR_LIR_FUNCTION_REF) {
        for (uint32_t p = 0; p < v->operand_count; ++p) {
            XrLirType capture = c_value_type(c,v->operands[p]);
            if (capture == XR_LIR_STRING_CELL || capture == XR_LIR_STRING_REF) {
                c_error(c,XR_LIR_C_UNSUPPORTED,"String binding captures require an independent lifetime contract"); return;
            }
        }
    }
    if (c_callable_point(c,v)) {
        if (c_located(&v->source)) {
            if (c->location_count == UINT32_MAX) c_error(c,XR_LIR_C_OVERFLOW,"C source inventory overflows");
            else ++c->location_count;
        }
        (void)c_callable_admit(c,v); return;
    }
    if (c_callable_type(c,v->type,NULL) && v->op != XR_LIR_CALL && v->op != XR_LIR_STATE_READ) {
        c_error(c,XR_LIR_C_UNSUPPORTED,"callable result requires owned environment transport"); return;
    }
    uint32_t string_role = xr_lir_string_role(c->request->module, v);
    if (string_role && (string_role < XR_LIR_STRING_OP_APPEND_OWNED || string_role >= XR_LIR_STRING_OP_END)) {
        c_error(c, XR_LIR_C_UNSUPPORTED, "String operation has no target execution binding");
        return;
    }
    if (v->op == XR_LIR_FUNCTION_REF || v->op == XR_LIR_CALL_INDIRECT || v->op == XR_LIR_FUNCTION_WEAKEN ||
        v->op == XR_LIR_FUNCTION_SCOPE_ENTER || v->op == XR_LIR_FUNCTION_SCOPE_LEAVE || v->op == XR_LIR_FUNCTION_BIND ||
        ((v->op == XR_LIR_CELL_NEW || v->op == XR_LIR_CELL_READ || v->op == XR_LIR_CELL_WRITE) &&
            !c_array_point(c, v) && !c_map_point(c, v) && !c_string_binding_point(c, v))) {
        c_error(c, XR_LIR_C_UNSUPPORTED, "value operation has no bound target C execution interface"); return;
    }
    if (c_string_point(c, v) || c_state_point(v) || c_array_point(c, v) || c_map_point(c, v) || c_record_point(v) || c_view_point(c, v) ||
        c_nullable_managed_point(c, v) ||
        (v->op == XR_LIR_FIXED_ARRAY_GET || v->op == XR_LIR_NULLABLE_UNWRAP)) {
        c->runtime = true;
        if (c_located(&v->source)) {
            if (c->location_count == UINT32_MAX)
                c_error(c, XR_LIR_C_UNSUPPORTED, "source map cannot represent another nonzero ID");
            else ++c->location_count;
        }
    }
    if (c_display_point(c, v)) { c->displays = true; return; }
    if (xr_lir_range_point(c->request->module, v)) return;
    if (c_record_point(v)) return;
    if (c_map_point(c, v)) {
        c->maps = true;
        if (v->op == XR_LIR_MAP_LITERAL_NEW) (void)c_map_literal_admit(c, v);
        return;
    }
    if (string_role) return;
    if (c_view_point(c, v)) return;
    switch (v->op) {
        case XR_LIR_MODULE_BEGIN: case XR_LIR_MODULE_READY:
        case XR_LIR_STATE_CONSTRUCT: case XR_LIR_STATE_PUBLISH:
        case XR_LIR_STATE_INITIALIZE: case XR_LIR_STATE_REFERENCE:
        case XR_LIR_STATE_READ: case XR_LIR_STATE_WRITE: case XR_LIR_STATE_GROUP_INITIALIZE:
            c->state = true;
            break;
        case XR_LIR_CELL_NEW: case XR_LIR_CELL_READ: case XR_LIR_CELL_WRITE:
        case XR_LIR_STRING_I64: case XR_LIR_STRING_F64: case XR_LIR_STRING_CONCAT: case XR_LIR_STRING_RETAIN:
        case XR_LIR_STRING_DROP: case XR_LIR_STRING_CELL_DROP: case XR_LIR_STRING_PRINT:
        case XR_LIR_STRING_LEN:
        case XR_LIR_ARRAY_RETAIN: case XR_LIR_ARRAY_DROP: case XR_LIR_ARRAY_CELL_DROP:
        case XR_LIR_ARRAY_BORROW: case XR_LIR_ARRAY_REF_READ: case XR_LIR_ARRAY_REF_WRITE:
        case XR_LIR_MAP_DROP: case XR_LIR_MAP_CELL_DROP:
            break;
        case XR_LIR_CALL_INTRINSIC: {
            XrLirArrayIntrinsic shape = {0};
            if (!c_array_intrinsic(c, v, &shape))
                c_error(c, XR_LIR_C_UNSUPPORTED, "intrinsic has no native implementation");
            break;
        }
        case XR_LIR_LITERAL_ADDR: case XR_LIR_STRING_LITERAL:
            c->used_literals[v->bits - 1] = 1;
            break;
        case XR_LIR_NULLABLE_NONE: case XR_LIR_NULLABLE_SOME:
        case XR_LIR_NULLABLE_IS_SOME: case XR_LIR_NULLABLE_UNWRAP:
        case XR_LIR_TUPLE_NEW: case XR_LIR_TUPLE_FIELD:
        case XR_LIR_FIXED_ARRAY_NEW: case XR_LIR_FIXED_ARRAY_GET:
        case XR_LIR_CONST: case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL:
        case XR_LIR_NEG: case XR_LIR_ICMP: case XR_LIR_INTCAST:
        case XR_LIR_BR: case XR_LIR_CONDBR: case XR_LIR_RET:
            break;
        case XR_LIR_DIV: case XR_LIR_REM:
            if (v->type != XR_LIR_I64)
                c_error(c, XR_LIR_C_UNSUPPORTED, "division requires i64 operands");
            else if (c_arithmetic_point(c, v)) {
                c->runtime = true;
                if (c_located(&v->source)) {
                    if (c->location_count == UINT32_MAX)
                        c_error(c, XR_LIR_C_OVERFLOW, "source location count exceeds 32 bits");
                    else
                        ++c->location_count;
                }
            }
            break;
        case XR_LIR_FNEG:
            if (v->type != XR_LIR_F64)
                c_error(c, XR_LIR_C_UNSUPPORTED, "floating negation requires f64");
            break;
        case XR_LIR_ITOF: case XR_LIR_FTOI: {
            bool to_float = v->op == XR_LIR_ITOF;
            XrLirType result_type = to_float ? XR_LIR_F64 : XR_LIR_I64;
            XrLirType operand_type = to_float ? XR_LIR_I64 : XR_LIR_F64;
            if (v->type != result_type || c_value_type(c, v->operands[0]) != operand_type) {
                c_error(c, XR_LIR_C_UNSUPPORTED, "numeric conversion requires signed64 and binary64");
                break;
            }
            if (!to_float) {
                c->runtime_header = true;
                c->runtime = true;
                if (c_located(&v->source)) {
                    if (c->location_count == UINT32_MAX)
                        c_error(c, XR_LIR_C_OVERFLOW, "source location count exceeds 32 bits");
                    else ++c->location_count;
                }
            }
            break;
        }
        case XR_LIR_FADD: case XR_LIR_FSUB: case XR_LIR_FMUL: case XR_LIR_FDIV:
        case XR_LIR_FCMP:
            if (c_value_type(c,v->operands[0]) != XR_LIR_F64 ||
                c_value_type(c,v->operands[1]) != XR_LIR_F64)
                c_error(c,XR_LIR_C_UNSUPPORTED,"floating operation requires f64 operands");
            else {
                c->runtime=true;
            }
            break;
        case XR_LIR_CALL:
            c_visit(c, v->callee);
            if (c_runtime_call(c, v) && c_located(&v->source)) {
                if (c->location_count == UINT32_MAX)
                    c_error(c, XR_LIR_C_UNSUPPORTED, "source map cannot represent another nonzero ID");
                else
                    ++c->location_count;
            }
            break;
        default:
            c_error(c, XR_LIR_C_UNSUPPORTED, "operation has no admitted portable C implementation");
            break;
    }
}
static void c_admit(LirC *c) {
    uint32_t count = xr_lir_function_count(c->request->module);
    if ((uint64_t) count * sizeof(uint32_t) > SIZE_MAX) {
        c_error(c, XR_LIR_C_OVERFLOW, "C function closure exceeds machine capacity");
        return;
    }
    void *queue = NULL, *seen = NULL;
    if (!c_resource(c, xr_compile_resources_alloc(c->request->resources,
                                                  (size_t) count * sizeof(uint32_t), &queue)))
        return;
    c->closure = queue;
    if (!c_resource(c, xr_compile_resources_calloc(c->request->resources, count, 1, &seen)))
        return;
    c->seen = seen;
    c_visit(c, c->request->entry.id);
    for (uint32_t cursor = 0; cursor < c->closure_count && c->status == XR_LIR_C_OK; ++cursor) {
        c->block = c->instruction = 0;
        c->source = (XrLirSource) {0};
        c->function = (XrLirFunction) {c->request->module, c->closure[cursor]};
        xr_lir_function_view(c->request->module, c->function, &c->view);
        for (uint32_t p = 0; p < c->view.callable_parameter_count; ++p) {
            XrLirCallableParameter contract = c->view.callable_parameters[p].contract;
            if (contract.mode != XR_PARAM_READ)
                c_error(c, XR_LIR_C_UNSUPPORTED, "owned callable invocation parameters require lifetime transport");
        }
        if (!c_request_poll(c))
            return;
        c_view_admit_signature(c);
        c_array_admit_shapes(c);
        if (!lir_definition_cell_captures(c->request->module,c->function,&c->view)) {
            c_error(c,XR_LIR_C_UNSUPPORTED,"Cell values require a verified capture formal; Cell result and host ABI remain closed");
            return;
        }
        if (c->view.kind == XR_LIR_FUNCTION_EXTERNAL) {
            c->runtime = true;
            continue;
        }
        if (c->view.result == XR_LIR_I64_CELL) {
            c_error(c,XR_LIR_C_UNSUPPORTED,"borrowed callable or Cell cannot leave its activation without owned transport");
            return;
        }
        c_scalar(c, c->view.result);
        for (uint32_t p = 0; p < c->view.parameter_count && c->status == XR_LIR_C_OK; ++p)
            c_scalar(c, c->view.parameter_types[p]);
        if (c->view.value_count >= UINT32_C(8388607))
            c_error(c, XR_LIR_C_UNSUPPORTED, "C temporary index exceeds the W4 limit");
        if (c->status == XR_LIR_C_OK)
            c_prepare_divisors(c);
        for (uint32_t b = 0; b < c->view.block_count && c->status == XR_LIR_C_OK; ++b) {
            XrLirBlock block;
            XrLirBlockView view;
            xr_lir_block_at(c->request->module, c->function, b, &block);
            xr_lir_block_view(c->request->module, block, &view);
            c->block = block.id;
            if (!c_request_poll(c))
                return;
            for (uint32_t p = 0; p < view.parameter_count && c->status == XR_LIR_C_OK; ++p)
                c_scalar(c, c_value_type(c, view.parameters[p]));
            for (uint32_t i = 0; i < view.instruction_count && c->status == XR_LIR_C_OK; ++i) {
                XrLirInstructionView v;
                xr_lir_instruction_view(c->request->module, block, i, &v);
                c->instruction = i + 1;
                c->source = v.source;
                c_admit_instruction(c, &v);
            }
        }
    }

}
static void c_path(LirC *c, const XrLirSource *source) {
    c_text(c, "\"");
    for (size_t n = 0; n < source->path_length && c->status == XR_LIR_C_OK; ++n) {
        unsigned char byte = (unsigned char) source->path[n];
        if (byte == '"' || byte == '\\')
            c_format(c, "\\%c", byte);
        else if (byte < 32 || byte >= 127)
            c_format(c, "\\%03o", (unsigned) byte);
        else
            c_bytes(c, source->path + n, 1);
    }
    c_text(c, "\"");
}
static void c_locations(LirC *c) {
    if (!c->runtime)
        return;
    uint32_t id = 0;
    if (c->location_count)
        c_text(c, "static const XrtSourceLocation xrl_locations[] = {\n");
    for (uint32_t n = 0; n < c->closure_count && c->status == XR_LIR_C_OK; ++n) {
        c->function = (XrLirFunction) {c->request->module, c->closure[n]};
        xr_lir_function_view(c->request->module, c->function, &c->view);
        if (!c_request_poll(c))
            return;
        c_prepare_divisors(c);
        for (uint32_t b = 0; b < c->view.block_count && c->status == XR_LIR_C_OK; ++b) {
            XrLirBlock block;
            XrLirBlockView view;
            xr_lir_block_at(c->request->module, c->function, b, &block);
            xr_lir_block_view(c->request->module, block, &view);
            c->block = block.id;
            if (!c_request_poll(c))
                return;
            for (uint32_t i = 0; i < view.instruction_count && c->status == XR_LIR_C_OK; ++i) {
                XrLirInstructionView v;
                xr_lir_instruction_view(c->request->module, block, i, &v);
                if ((!c_runtime_call(c, &v) && !c_string_point(c, &v) && !c_cell_point(c,&v) && !c_callable_point(c,&v) && !c_state_point(&v) && !c_array_point(c, &v) && !c_map_point(c, &v) && !c_record_point(&v) && !c_view_point(c, &v) &&
                     !c_nullable_managed_point(c, &v) &&
                     !c_arithmetic_point(c, &v) &&
                     v.op != XR_LIR_FIXED_ARRAY_GET && v.op != XR_LIR_NULLABLE_UNWRAP) || !c_located(&v.source))
                    continue;
                ++id;
                c_text(c, "    { ");
                c_path(c, &v.source);
                c_format(c, ", %zu, UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32
                    "), UINT32_C(%" PRIu32 "), UINT32_C(%" PRIu32 ") },\n",
                    v.source.path_length, v.source.line, v.source.column,
                    v.source.end_line, v.source.end_column);
            }
        }
    }
    if (c->location_count)
        c_text(c, "};\n");
    c_text(c, "const XrtSourceMap xrt_standalone_source_map = { ");
    c_format(c, "%s, UINT32_C(%" PRIu32 ") };\n\n",
        c->location_count ? "xrl_locations" : "NULL", c->location_count);
    (void) id;
    XR_DCHECK(c->status != XR_LIR_C_OK || id == c->location_count,
              "static source locations match admitted calls");
}
static void c_signature(LirC *c, bool definition) {
    if (c_values_signature(c, definition)) return;
    XR_DCHECK(c_type(c, c->view.result) != NULL, "function result passed scalar admission");
    if (c->runtime || c->function.id != c->request->entry.id)
        c_text(c, "static ");
    c_format(c, "%s ", c_type(c, c->view.result));
    c_function_name(c, c->function.id);
    c_text(c, "(");
    if (!c->view.parameter_count)
        c_text(c, "void");
    for (uint32_t p = 0; p < c->view.parameter_count && c->status == XR_LIR_C_OK; ++p) {
        if (p)
            c_text(c, ", ");
        c_format(c, "%s p%" PRIu32, c_type(c, c->view.parameter_types[p]), p);
    }
    c_text(c, definition ? ") {\n" : ");\n");
}
static bool c_literal_value(LirC *c, uint32_t id) {
    const XrLirFunctionData *function = &c->request->module->functions[c->function.id - 1];
    const XrLirInstructionView *definition = xr_lir_internal_value_definition(function, id);
    return definition && definition->op == XR_LIR_LITERAL_ADDR;
}
static void c_declarations(LirC *c) {
    c_values_declarations(c);
    c_callable_declarations(c);
    for (uint32_t id = 1; id <= c->view.value_count && c->status == XR_LIR_C_OK; ++id) {
        if (!c_request_poll(c))
            return;
        bool parameter = false;
        for (uint32_t p = 0; p < c->view.parameter_count && c->status == XR_LIR_C_OK; ++p)
            parameter |= c->view.parameters[p] == id;
        if (!parameter) {
            if (c_values_declare(c, id)) continue;
            c_format(c, "    %s v%" PRIu32 ";\n",
                c_literal_value(c, id) ? "const void *" : c_type(c, c_value_type(c, id)), id);
            c_format(c, "    (void)&v%" PRIu32 ";\n", id);
        }
        if (c_value_type(c, id) == XR_LIR_STATE_REF) {
            c_format(c, "    XrtModuleAccess xrl_access%" PRIu32 ";\n", id);
            c_format(c, "    (void)&xrl_access%" PRIu32 ";\n", id);
        }
    }
    /* Unsigned expression storage is shared only across sequential instructions. */
    c->scratch = c->view.value_count + 1;
    XR_DCHECK(c->scratch < UINT32_C(8388608), "expression storage remains visible to W4");
    c_format(c, "    uint64_t v%" PRIu32 ";\n", c->scratch);
    c_format(c, "    (void)&v%" PRIu32 ";\n", c->scratch);
    for (uint32_t p = 0; p < c->view.parameter_count && c->status == XR_LIR_C_OK; ++p)
        c_format(c, "    (void)p%" PRIu32 ";\n", p);
}
static void c_reconstruct(LirC *c, XrLirType type, uint32_t result) {
    c_format(c, "    v%" PRIu32 " = ", result);
    if (type >= XR_LIR_I8 && type <= XR_LIR_I64) {
        uint64_t maximum = c_mask(type) >> 1;
        c_format(c, "v%" PRIu32 " <= UINT64_C(%" PRIu64 ") ? (%s)v%" PRIu32
            " : (%s)(-INT64_C(1) - (int64_t)((~v%" PRIu32 ") & UINT64_C(%" PRIu64 ")));\n",
            c->scratch, maximum, c_type(c, type), c->scratch, c_type(c, type), c->scratch, c_mask(type));
    } else {
        c_format(c, "(%s)v%" PRIu32 ";\n", c_type(c, type), c->scratch);
    }
}
static void c_integer(LirC *c, const XrLirInstructionView *v) {
    if (v->type == XR_LIR_I1) {
        c_format(c, "    v%" PRIu32 " = %u;\n", v->result, (unsigned) v->bits);
        return;
    }
    if (v->op == XR_LIR_CONST) {
        uint64_t mask = c_mask(v->type);
        uint64_t bits = v->bits & mask;
        /* Reconstruct constants here so the host optimizer sees no shared scratch dependency. */
        if (v->type <= XR_LIR_I64 && bits > (mask >> 1)) {
            c_format(c, "    v%" PRIu32 " = (%s)(-INT64_C(1) - INT64_C(%" PRIu64 "));\n",
                     v->result, c_type(c, v->type), (~bits) & mask);
        } else {
            c_format(c, "    v%" PRIu32 " = (%s)UINT64_C(%" PRIu64 ");\n",
                     v->result, c_type(c, v->type), bits);
        }
        return;
    }
    c_format(c, "    v%" PRIu32 " = (", c->scratch);
    if (v->op == XR_LIR_NEG)
        c_text(c, "UINT64_C(0) - ");
    c_text(c, "(uint64_t)");
    c_value(c, v->operands[0]);
    if (v->operand_count == 2) {
        c_text(c, v->op == XR_LIR_ADD ? " + " : v->op == XR_LIR_SUB ? " - " : " * ");
        c_text(c, "(uint64_t)");
        c_value(c, v->operands[1]);
    }
    c_format(c, ") & UINT64_C(%" PRIu64 ");\n", c_mask(v->type));
    c_reconstruct(c, v->type, v->result);
}
static void c_fneg(LirC *c, const XrLirInstructionView *v) {
    c_format(c, "    v%" PRIu32 " = -", v->result);
    c_value(c, v->operands[0]);
    c_text(c, ";\n");
}
static void c_float_operation(LirC *c, const XrLirInstructionView *v) {
    const char *symbol = NULL;
    if (v->op == XR_LIR_FCMP) {
        switch (v->predicate) {
            case XR_LIR_EQ: symbol = " == "; break;
            case XR_LIR_NE: symbol = " != "; break;
            case XR_LIR_LT: symbol = " < "; break;
            case XR_LIR_LE: symbol = " <= "; break;
            case XR_LIR_GT: symbol = " > "; break;
            case XR_LIR_GE: symbol = " >= "; break;
            default: c_error(c, XR_LIR_C_INVALID_LIR, "invalid floating predicate"); return;
        }
    } else {
        symbol = v->op == XR_LIR_FADD ? " + " : v->op == XR_LIR_FSUB ? " - " :
            v->op == XR_LIR_FMUL ? " * " : " / ";
    }
    c_format(c, "    v%" PRIu32 " = ", v->result);
    c_value(c, v->operands[0]);
    c_text(c, symbol);
    c_value(c, v->operands[1]);
    c_text(c, ";\n");
}
static void c_float_conversion(LirC *c, const XrLirInstructionView *v) {
    if (v->op == XR_LIR_ITOF) {
        c_format(c, "    v%" PRIu32 " = (double)", v->result);
        c_value(c, v->operands[0]);
        c_text(c, ";\n");
    } else {
        uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
        c_text(c, "    { uint64_t xrl_bits;\n        memcpy(&xrl_bits, &");
        c_value(c, v->operands[0]); c_text(c, ", sizeof(xrl_bits));\n        ");
        c_value(c, v->result);
        c_format(c, " = xrt_f64_to_i64(xrl_bits, UINT32_C(%" PRIu32 "));\n    }\n", source);
    }
}
static void c_edge(LirC *c, const XrLirEdgeView *edge, uint32_t selected) {
    if (c_values_edge(c, edge, selected)) return;
    XrLirBlock target = {c->request->module, c->function.id, edge->target};
    XrLirBlockView block;
    xr_lir_block_view(c->request->module, target, &block);
    XR_DCHECK(block.parameter_count == edge->argument_count, "validated edge matches its target");
    for (uint32_t a = 0; a < edge->argument_count; ++a)
        if (c_managed_leaf(c_value_type(c, edge->arguments[a]))) {
            c_error(c, XR_LIR_C_INVALID_LIR, "owned edge lost its common transfer");
            return;
        }
    c_text(c, "    {\n");
    for (uint32_t a = 0; a < edge->argument_count && c->status == XR_LIR_C_OK; ++a) {
        uint64_t temp = (uint64_t) c->scratch + 1 + a;
        if (temp >= UINT32_C(8388608)) {
            c_error(c, XR_LIR_C_UNSUPPORTED, "C edge temporary index exceeds the W4 limit");
            return;
        }
        c_format(c, "        %s v%" PRIu64 " = ", c_type(c, c_value_type(c, edge->arguments[a])), temp);
        c_value(c, edge->arguments[a]);
        c_text(c, ";\n");
        if (c_value_type(c, edge->arguments[a]) == XR_LIR_STATE_REF)
            c_format(c, "        XrtModuleAccess xrl_edge_access%" PRIu32
                " = xrl_access%" PRIu32 ";\n", a, edge->arguments[a]);
    }
    for (uint32_t a = 0; a < edge->argument_count && c->status == XR_LIR_C_OK; ++a) {
        c_format(c, "        v%" PRIu32 " = v%" PRIu32 ";\n", block.parameters[a],
                 c->scratch + 1 + a);
        if (c_value_type(c, edge->arguments[a]) == XR_LIR_STATE_REF)
            c_format(c, "        xrl_access%" PRIu32 " = xrl_edge_access%" PRIu32 ";\n",
                block.parameters[a], a);
    }
    c_format(c, "        goto bb%" PRIu32 ";\n    }\n", edge->target);
}
static void c_division(LirC *c, const XrLirInstructionView *v) {
    bool guarded = c_arithmetic_point(c, v);
    if (guarded) {
        uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
        /* One volatile read keeps host constant propagation from diagnosing
         * guest code after a known panic as unreachable. The check and the
         * operation share the same captured value. */
        c_text(c, "    { volatile int64_t xrl_guard = ");
        c_value(c, v->operands[1]);
        c_text(c, ";\n    int64_t xrl_divisor = xrl_guard;\n");
        c_format(c, "    if (xrl_divisor == INT64_C(0)) "
            "xrt_arithmetic_panic(UINT32_C(%u), UINT32_C(%" PRIu32 "));\n",
            v->op == XR_LIR_DIV ? 420u : 421u, source);
    }
    /* Select the wrapping pair before C can evaluate signed overflow. */
    c_format(c, "    v%" PRIu32 " = ", v->result);
    c_value(c, v->operands[0]);
    c_text(c, " == INT64_MIN && ");
    if (guarded)
        c_text(c, "xrl_divisor");
    else
        c_value(c, v->operands[1]);
    c_text(c, " == -INT64_C(1) ? ");
    c_text(c, v->op == XR_LIR_DIV ? "INT64_MIN" : "INT64_C(0)");
    c_text(c, " : ");
    c_value(c, v->operands[0]);
    c_text(c, v->op == XR_LIR_DIV ? " / " : " % ");
    if (guarded)
        c_text(c, "xrl_divisor");
    else
        c_value(c, v->operands[1]);
    c_text(c, guarded ? "; }\n" : ";\n");
}
static void c_instruction(LirC *c, const XrLirInstructionView *v) {
    if (!c_request_poll(c))
        return;
    static const char *const predicates[] = {NULL, "==", "!=", "<", "<=", ">", ">="};
    if (c_cell_point(c,v)) {
        uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
        c_cell_instruction(c,v,source); return;
    }
    if (c_callable_point(c,v)) {
        uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
        c_callable_instruction(c,v,source); return;
    }
    if (v->op >= XR_LIR_NULLABLE_NONE && v->op <= XR_LIR_NULLABLE_UNWRAP) {
        c_nullable_instruction(c, v);
        return;
    }
    if (c_record_point(v)) {
        uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
        c_record_instruction(c, v, source);
        return;
    }
    if (v->op == XR_LIR_FIXED_ARRAY_NEW || v->op == XR_LIR_FIXED_ARRAY_GET ||
        v->op == XR_LIR_TUPLE_NEW || v->op == XR_LIR_TUPLE_FIELD) {
        c_fixed_instruction(c, v);
        return;
    }
    if (c_view_point(c, v)) {
        uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
        c_view_instruction(c, v, source);
        return;
    }
    if (c_array_point(c, v) || v->op == XR_LIR_ARRAY_DROP || v->op == XR_LIR_ARRAY_CELL_DROP) {
        uint32_t source = c_array_point(c, v) && c_located(&v->source) ? ++c->location_cursor : 0;
        c_array_instruction(c, v, source);
        return;
    }
    if (c_map_point(c, v) || v->op == XR_LIR_MAP_DROP || v->op == XR_LIR_MAP_CELL_DROP) {
        uint32_t source = c_map_point(c, v) && c_located(&v->source) ? ++c->location_cursor : 0;
        c_map_instruction(c, v, source);
        return;
    }
    uint32_t string_source = c_string_point(c, v) && c_located(&v->source) ? ++c->location_cursor : 0;
    if (c_string_binding_point(c, v)) {
        c_string_binding_instruction(c, v, string_source);
        return;
    }
    if (c_values_instruction(c, v, string_source)) return;
    if (c_state_point(v)) {
        uint32_t source = c_located(&v->source) ? ++c->location_cursor : 0;
        c_state_instruction(c, v, source);
        return;
    }
    if (xr_lir_range_point(c->request->module, v)) {
        c_text(c, "    xrt_range_check(");
        for (uint32_t p = 0; p < 3; ++p) {
            if (p) c_text(c, ", ");
            c_value(c, v->operands[p]);
        }
        c_format(c, ", UINT32_C(%" PRIu32 "));\n", string_source);
        return;
    }
    if (xr_lir_string_role(c->request->module, v)) {
        c_string_library(c, v, string_source);
        return;
    }
    if (c_display_point(c, v)) {
        c_display_instruction(c, v, string_source);
        return;
    }
    switch (v->op) {
        case XR_LIR_LITERAL_ADDR:
            c_format(c, "    v%" PRIu32 " = xrl_literal%" PRIu64 ";\n", v->result, v->bits);
            break;
        case XR_LIR_CONST:
            if (v->type == XR_LIR_PTR)
                c_format(c, "    v%" PRIu32 " = 0;\n", v->result);
            else if (v->type == XR_LIR_F64) {
                c_format(c, "    v%" PRIu32 " = UINT64_C(%" PRIu64 ");\n", c->scratch, v->bits);
                c_format(c, "    memcpy(&v%" PRIu32 ", &v%" PRIu32 ", sizeof(v%" PRIu32 "));\n",
                         v->result, c->scratch, v->result);
            } else
                c_integer(c, v);
            break;
        case XR_LIR_ADD: case XR_LIR_SUB: case XR_LIR_MUL:
        case XR_LIR_NEG: case XR_LIR_INTCAST:
            c_integer(c, v);
            break;
        case XR_LIR_DIV: case XR_LIR_REM:
            c_division(c, v);
            break;
        case XR_LIR_FNEG:
            c_fneg(c, v);
            break;
        case XR_LIR_ITOF: case XR_LIR_FTOI:
            c_float_conversion(c, v);
            break;
        case XR_LIR_FADD: case XR_LIR_FSUB: case XR_LIR_FMUL: case XR_LIR_FDIV: case XR_LIR_FCMP:
            c_float_operation(c,v);
            break;
        case XR_LIR_ICMP:
            c_format(c, "    v%" PRIu32 " = ", v->result);
            c_value(c, v->operands[0]);
            c_format(c, " %s ", predicates[v->predicate]);
            c_value(c, v->operands[1]);
            c_text(c, ";\n");
            break;
        case XR_LIR_CALL: {
            XrLirFunctionView callee;
            xr_lir_function_view(c->request->module, (XrLirFunction){c->request->module, v->callee}, &callee);
            c_text(c, "    ");
            if (v->result)
                c_format(c, "v%" PRIu32 " = ", v->result);
            bool string_output = false;
            if (c_runtime_call(c, v)) {
                XrLirFunctionView external;
                xr_lir_function_view(c->request->module,
                    (XrLirFunction){c->request->module, v->callee}, &external);
                string_output = external.extern_kind == XR_LIR_EXTERN_PRINT_STRING;
                const char *name = string_output ? "xrt_print_string" :
                    external.extern_kind == XR_LIR_EXTERN_PRINT_I64 ? "xrt_print_i64" :
                    external.extern_kind == XR_LIR_EXTERN_CLOCK_REALTIME ? "xrt_clock_realtime_nanos" :
                    external.extern_kind == XR_LIR_EXTERN_CLOCK_CPU ? "xrt_clock_process_cpu_nanos" :
                    external.extern_kind == XR_LIR_EXTERN_UTC_OFFSET_AT ? "xrt_clock_utc_offset_at" :
                    "xrt_clock_monotonic_nanos";
                c_text(c, name);
            } else c_function_name(c, v->callee);
            c_text(c, "(");
            for (uint32_t a = 0; a < v->operand_count && c->status == XR_LIR_C_OK; ++a) {
                if (a)
                    c_text(c, ", ");
                if (string_output && a == 1) c_text(c, "(size_t)");
                c_value(c, v->operands[a]);
            }
            if (c_runtime_call(c, v)) {
                uint32_t id = c_located(&v->source) ? ++c->location_cursor : 0;
                c_format(c, "%sUINT32_C(%" PRIu32 ")", v->operand_count ? ", " : "", id);
            }
            c_text(c, ");\n");
            break;
        }
        case XR_LIR_BR:
            c_edge(c, &v->edges[0], 1);
            break;
        case XR_LIR_CONDBR:
            c_text(c, "    if (");
            c_value(c, v->operands[0]);
            c_text(c, ") {\n");
            c_edge(c, &v->edges[0], 1);
            c_text(c, "    } else {\n");
            c_edge(c, &v->edges[1], 2);
            c_text(c, "    }\n");
            break;
        case XR_LIR_RET:
            c_text(c, "    return");
            if (v->operand_count) {
                c_text(c, " ");
                c_value(c, v->operands[0]);
            }
            c_text(c, ";\n");
            break;
        default:
            XR_CHECK(false, "C instruction passed admission");
    }
}
static void c_standalone_root(LirC *c, XrLirType result) {
    if (c_managed_leaf(result)) { c_values_root(c); return; }
    const char *type = c_type(c, result);
    XR_DCHECK(type != NULL && c->runtime, "a bound root keeps its admitted result type");
    c->function = c->request->entry;
    c->block = c->instruction = 0;
    c->source = (XrLirSource) {0};
    if (c->state) {
        c_state_root(c, result);
        return;
    }
    c_text(c, "static void xrl_invoke(void *context) {\n");
    if (result == XR_LIR_UNIT)
        c_text(c, "    (void)context;\n    ");
    else
        c_format(c, "    *(%s *)context = ", type);
    c_function_name(c, c->request->entry.id);
    c_text(c, "();\n}\n\n");
    c_format(c, "%s xray_lir_entry(void) {\n", type);
    if (result == XR_LIR_UNIT) {
        c_text(c, "    xrt_run_standalone(&xrt_standalone_source_map, xrl_invoke, NULL);\n");
    } else {
        c_format(c, "    %s result;\n", type);
        c_text(c, "    xrt_run_standalone(&xrt_standalone_source_map, xrl_invoke, &result);\n");
        c_text(c, "    return result;\n");
    }
    c_text(c, "}\n\n");
}
/* Compiler target macros belong to the emitted target, not this host.
 * The selected binary64 closure must fail before unsupported C can execute. */
static void c_preamble(LirC *c) {
    if (c->f64) {
        c_text(c, "#if defined(_M_FP_FAST) || defined(__FAST_MATH__) || "
            "(defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__ > 0)\n"
            "#error F64_TRANSPORT_REQUIRES_NONFAST_FLOATING_POINT_MODE\n#endif\n"
            "#include <stdint.h>\n#include <stddef.h>\n#include <limits.h>\n#include <float.h>\n#include <string.h>\n"
            "#if CHAR_BIT != 8 || FLT_RADIX != 2 || DBL_MANT_DIG != 53 || "
            "DBL_MIN_EXP != -1021 || DBL_MAX_EXP != 1024\n"
            "#error F64_TRANSPORT_REQUIRES_BINARY64\n#endif\n"
            "_Static_assert(CHAR_BIT == 8 && sizeof(double) == 8 && sizeof(uint64_t) == 8, "
            "\"binary64 object size\");\n"
            "_Static_assert(FLT_RADIX == 2 && DBL_MANT_DIG == 53 && DBL_MIN_EXP == -1021 "
            "&& DBL_MAX_EXP == 1024, \"binary64 precision\");\n\n");
    } else {
        c_text(c, "#include <stdint.h>\n#include <stddef.h>\n\n");
    }
    if (c->state && !c->f64) c_text(c, "#include <string.h>\n\n");
}
static void c_literals(LirC *c) {
    for (uint32_t i = 0; i < xr_lir_literal_count(c->request->module) && c_request_poll(c); ++i) {
        if (!c->used_literals[i]) continue;
        XrLirLiteral literal = {0};const uint8_t *bytes = NULL;size_t length = 0;
        xr_lir_literal_at(c->request->module, i, &literal);
        xr_lir_literal_view(c->request->module, literal, &bytes, &length);
        c_format(c, "_Static_assert((uint64_t)(size_t)UINT64_C(%" PRIu64 ") == UINT64_C(%" PRIu64
            "), \"literal length fits size_t\");\n", (uint64_t)length, (uint64_t)length);
        c_format(c, "static const unsigned char xrl_literal%" PRIu32 "[] = {", i + 1);
        for (size_t b = 0; b < length && c_request_poll(c); ++b) c_format(c, "0x%02x,", (unsigned)bytes[b]);
        c_text(c, "0};\n");
    }
}
static void c_emit(LirC *c, XrLirType root_result) {
    XR_DCHECK(c->closure_count && c->closure[0] == c->request->entry.id,
              "emission starts from the explicit execution root");
    c_preamble(c);
    if (c->callables) c_text(c,"#include \"xray_native_function.h\"\n");
    if (c->runtime || c->runtime_header)
        c_text(c, "#include \"xray_native_runtime.h\"\n\n");
    if (c->string_values || c->arrays || c->maps || c->inline_values || c->views || c->cells) c_text(c, "#include <string.h>\n#include \"xray_native_values.h\"\n#include \"xray_native_place.h\"\n\n");
    else if (c->value_types) c_text(c, "typedef struct XrtManagedHeader XrtManagedHeader;\n");
    if (c->value_types) c_text(c, "#include \"xray_native_value_image.h\"\n\n");
    if (c->state) c_text(c, "#include \"xray_native_state.h\"\n\n");
    if (c->views) c_text(c, "#include \"xray_native_array_slice.h\"\n\n");
    if (c->arrays) c_text(c, "#include \"xray_native_array.h\"\n\n");
    if (c->maps) c_text(c, "#include \"xray_native_map.h\"\n\n");
    if (c->displays) c_text(c, "#include \"xray_native_pod.h\"\n\n");
    c_constructed_types(c);
    c_values_landing_types(c);
    c_callable_types(c);
    c_values_helpers(c);
    c_callable_publication(c);
    c_locations(c);
    c_literals(c);
    c_map_literals(c);
    if (c->state) c_state_layout(c);
    else if (c->value_image) c_value_image(c,xr_lir_value_image_view(c->value_image));
    for (uint32_t n = 0; n < c->closure_count && c->status == XR_LIR_C_OK; ++n) {
        c->function = (XrLirFunction) {c->request->module, c->closure[n]};
        xr_lir_function_view(c->request->module, c->function, &c->view);
        c_values_function(c);
        if (!c_request_poll(c))
            return;
        if (c->view.kind == XR_LIR_FUNCTION_EXTERNAL)
            continue;
        c_signature(c, false);
    }
    c_callable_entries(c);
    for (uint32_t n = 0; n < c->closure_count && c->status == XR_LIR_C_OK; ++n) {
        c->function = (XrLirFunction) {c->request->module, c->closure[n]};
        xr_lir_function_view(c->request->module, c->function, &c->view);
        if (!c_request_poll(c))
            return;
        if (c->view.kind == XR_LIR_FUNCTION_EXTERNAL)
            continue;
        c_prepare_divisors(c);
        c_values_function(c);
        c_callable_events_prepare(c);
        c_cell_events_prepare(c);
        c_signature(c, true);
        c_declarations(c);
        c_format(c, "    goto bb%" PRIu32 ";\n", c->view.entry);
        for (uint32_t b = 0; b < c->view.block_count && c->status == XR_LIR_C_OK; ++b) {
            XrLirBlock block;
            XrLirBlockView view;
            xr_lir_block_at(c->request->module, c->function, b, &block);
            xr_lir_block_view(c->request->module, block, &view);
            c->block = block.id;
            if (!c_request_poll(c))
                return;
            c_format(c, "bb%" PRIu32 ": ;\n", block.id);
            for (uint32_t i = 0; i < view.instruction_count && c->status == XR_LIR_C_OK; ++i) {
                XrLirInstructionView v;
                xr_lir_instruction_view(c->request->module, block, i, &v);
                c->instruction = i + 1;
                c->source = v.source;
                c_instruction(c, &v);
            }
        }
        c_text(c, "}\n\n");
    }
    if (c->runtime && c->status == XR_LIR_C_OK)
        c_standalone_root(c, root_result);
    XR_DCHECK(c->status != XR_LIR_C_OK || c->location_cursor == c->location_count,
              "ordinary output calls consume their own static location IDs");
}
XR_FUNC void xr_lir_c_output_free(XrLirCOutput *output) {
    if (!output)
        return;
    xr_compile_resources_free(output->bytes);
    *output = (XrLirCOutput) {0};
}
static void c_verify_text(LirC *c) {
    if (!c_request_poll(c)) return;
    XiCgenVerifyStatus verified = xr_compile_cgen_verify_output_or_ice(c->request->resources,
        c->text, c->length, c->request->translation_unit);
    if (verified == XI_CGEN_VERIFY_PASSED) return;
    XrLirCStatus mapped;
    switch (verified) {
        case XI_CGEN_VERIFY_OUT_OF_MEMORY: mapped = XR_LIR_C_OUT_OF_MEMORY; break;
        case XI_CGEN_VERIFY_MEMORY_LIMIT: mapped = XR_LIR_C_MEMORY_LIMIT; break;
        case XI_CGEN_VERIFY_OVERFLOW: mapped = XR_LIR_C_OVERFLOW; break;
        case XI_CGEN_VERIFY_BUDGET: mapped = XR_LIR_C_BUDGET; break;
        case XI_CGEN_VERIFY_CANCELLED: mapped = XR_LIR_C_CANCELLED; break;
        case XI_CGEN_VERIFY_DEADLINE: mapped = XR_LIR_C_DEADLINE; break;
        case XI_CGEN_VERIFY_CLOCK_FAILURE: mapped = XR_LIR_C_CLOCK_FAILURE; break;
        default: mapped = XR_LIR_C_BAD_ARGUMENT; break;
    }
    c_error(c, mapped, "C output verifier could not complete");
}
#include "xlir_c_value_types.inc.c"
#include "xlir_c_cell.inc.c"
#include "xlir_c_value_transfers.inc.c"
#include "xlir_c_values.inc.c"
static XrLirCStatus c_emit_output(const XrLirCRequest *request, XrLirCOutput *output,
    XrLirCDiagnostic *diagnostic, bool standalone) {
    if (diagnostic)
        *diagnostic = (XrLirCDiagnostic) {0};
    LirC c = {.request = request, .diagnostic = diagnostic};
    XrLirFunctionView entry;
    if (!request || !output || output->bytes || output->length || output->entry_result ||
        output->entry_shape.kind || output->entry_shape.element || output->entry_shape.count ||
        output->entry_layout.size || output->entry_layout.alignment ||
        output->entry_layout.scalar_count || output->entry_layout.managed ||
        !request->resources || !request->translation_unit || !request->module ||
        request->entry.module != request->module ||
        xr_lir_function_view(request->module, request->entry, &entry) != XR_LIR_OK ||
        entry.kind != XR_LIR_FUNCTION_DEFINITION || entry.result == XR_LIR_SLICE_I64 ||
        (entry.parameter_count && (entry.parameter_count != 1 || entry.parameter_types[0] != XR_LIR_STATE))) {
        c_error(&c, XR_LIR_C_BAD_ARGUMENT, "C emission requires empty output and a no-argument root");
        return c.status;
    }
    if (entry.parameter_count) {
        XrLirFunction state_entry = {0};
        if (xr_lir_state_entry(request->module, &state_entry) != XR_LIR_OK ||
            state_entry.id != request->entry.id ||
            xr_lir_state_layout_view(request->module, &c.state_layout) != XR_LIR_OK) {
            c_error(&c, XR_LIR_C_INVALID_LIR, "C entry lost its module state layout");
            return c.status;
        }
        c.state = c.runtime = true;
    }
    if (!c_request_poll(&c))
        return c.status;
    if (xr_lir_boxed_type(request->module, entry.result)) {
        c_error(&c, XR_LIR_C_UNSUPPORTED, "standalone boxed results require an external outcome owner");
        return c.status;
    }
    XrLirCallableType callable_result;
    if (xr_lir_callable_type_view(request->module, entry.result, &callable_result) == XR_LIR_OK) {
        c_error(&c, XR_LIR_C_UNSUPPORTED, "standalone callable results require an external outcome owner");
        return c.status;
    }
#if defined(XR_XIR_PARANOID) && XR_XIR_PARANOID
    XrLirDiagnostic invalid;
    XrLirStatus status = xr_lir_verify(request->module, &invalid);
    if (status != XR_LIR_OK) {
        c.function.id = invalid.function;
        c.block = invalid.block;
        c.instruction = invalid.instruction;
        c.source = invalid.source;
        c_error(&c, status == XR_LIR_OUT_OF_MEMORY ? XR_LIR_C_OUT_OF_MEMORY : XR_LIR_C_INVALID_LIR,
                invalid.message);
        return c.status;
    }
#endif
    if (!c_request_poll(&c))
        return c.status;
    c_prepare_types(&c);
    uint32_t literal_count = xr_lir_literal_count(request->module);
    if (literal_count && c.status == XR_LIR_C_OK) {
        void *used = NULL;
        if (c_resource(&c, xr_compile_resources_calloc(request->resources, literal_count, 1, &used)))
            c.used_literals = used;
    }
    if (c.status == XR_LIR_C_OK)
        c_admit(&c);
    if (c.status == XR_LIR_C_OK)
        c_values_prepare(&c);
    if (c.status == XR_LIR_C_OK)
        c_emit(&c, entry.result);
    if (c.status == XR_LIR_C_OK && standalone) {
        if (c_managed_leaf(entry.result))
            c_text(&c, "\nint main(void) {\n    XrtManagedHeader *result = xray_lir_entry();\n"
                "    xrt_managed_drop(result);\n    return 0;\n}\n");
        else
            c_text(&c, "\nint main(void) {\n    (void)xray_lir_entry();\n    return 0;\n}\n");
    }
    c_verify_text(&c);

    if (c_request_poll(&c)) {
        XrLirTypeView shape = {0};
        XrLirTypeLayout layout = {0};
        XR_CHECK(xr_lir_type_view(request->module, entry.result, &shape) == XR_LIR_OK &&
            xr_lir_type_layout(request->module, entry.result, &layout) == XR_LIR_OK,
            "admitted C result has a complete value layout");
        *output = (XrLirCOutput) {.bytes = c.text, .length = c.length, .entry_result = entry.result,
            .entry_shape = shape, .entry_layout = layout};
        c.text = NULL;
    }
    xr_compile_resources_free(c.text);
    xr_compile_resources_free(c.closure);
    xr_compile_resources_free(c.seen);
    xr_compile_resources_free(c.fixed_divisors);
    xr_compile_resources_free(c.used_literals);
    xr_compile_resources_free(c.used_map_literals);
    xr_compile_resources_free(c.callable_used);
    xr_compile_resources_free(c.constructed_types);
    xr_compile_resources_free(c.callable_events);
    xr_compile_resources_free(c.cell_events);
    c_values_destroy(&c);
    return c.status;
}

XR_FUNC XrLirCStatus xr_lir_emit_c(const XrLirCRequest *request, XrLirCOutput *output,
    XrLirCDiagnostic *diagnostic) {
    return c_emit_output(request, output, diagnostic, false);
}

XR_FUNC XrLirCStatus xr_lir_emit_c_standalone(const XrLirCRequest *request, XrLirCOutput *output,
    XrLirCDiagnostic *diagnostic) {
    return c_emit_output(request, output, diagnostic, true);
}
