/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdap_inspect.c - Variable and stack inspection implementation
 */

#include "xdap_inspect.h"
#include "xdap_debug.h"
#include "../../runtime/xisolate_api.h"
#include "../../coro/xcoroutine.h"
#include "../../runtime/xexec_frame.h"
#include "../../runtime/value/xvalue.h"
#include "../../runtime/value/xchunk.h"
#include "../../runtime/value/xtype_names.h"
#include "../../runtime/object/xstring.h"
#include "../../runtime/object/xarray.h"
#include "../../runtime/object/xmap.h"
#include "../../runtime/object/xjson.h"
#include "../../runtime/class/xinstance.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Stack Frames
// ============================================================================

XrJsonValue *xdap_inspect_stack_frames(XdapController *ctrl, XrCoroutine *coro,
                                        int *frame_id_out) {
    XrJsonValue *frames = xlsp_json_new_array();
    
    if (!coro || !ctrl) {
        if (frame_id_out) *frame_id_out = 0;
        return frames;
    }
    
    int frame_id = 0;
    
    for (int i = coro->vm_ctx.frame_count - 1; i >= 0; i--) {
        XrBcCallFrame *frame = &coro->vm_ctx.frames[i];
        
        if (!frame->closure || !frame->closure->proto) continue;
        
        XrProto *proto = frame->closure->proto;
        
        XrJsonValue *f = xlsp_json_new_object();
        xlsp_json_object_set(f, "id", xlsp_json_new_number(frame_id));
        
        // Function name
        const char *name = proto->name ? XR_STRING_CHARS(proto->name) : "<anonymous>";
        xlsp_json_object_set(f, "name", xlsp_json_new_string(name));
        
        // Source
        if (proto->source_file) {
            XrJsonValue *source = xlsp_json_new_object();
            xlsp_json_object_set(source, "path", xlsp_json_new_string(proto->source_file));
            xlsp_json_object_set(f, "source", source);
        }
        
        // Line number
        int pc_offset = (int)(frame->pc - PROTO_CODE_BASE(proto));
        int line = 1;
        int line_count = PROTO_LINE_COUNT(proto);
        if (pc_offset >= 0 && pc_offset < line_count) {
            line = PROTO_LINE(proto, pc_offset);
        }
        xlsp_json_object_set(f, "line", xlsp_json_new_number(line));
        xlsp_json_object_set(f, "column", xlsp_json_new_number(0));
        
        xlsp_json_array_push(frames, f);
        frame_id++;
    }
    
    if (frame_id_out) *frame_id_out = frame_id;
    return frames;
}

bool xdap_inspect_get_frame_info(XdapController *ctrl, XrCoroutine *coro,
                                  int frame_idx, const char **out_name,
                                  const char **out_source, int *out_line) {
    if (!ctrl || !coro) return false;
    
    int actual_idx = coro->vm_ctx.frame_count - 1 - frame_idx;
    if (actual_idx < 0 || actual_idx >= coro->vm_ctx.frame_count) return false;
    
    XrBcCallFrame *frame = &coro->vm_ctx.frames[actual_idx];
    if (!frame->closure || !frame->closure->proto) return false;
    
    XrProto *proto = frame->closure->proto;
    
    if (out_name) {
        *out_name = proto->name ? XR_STRING_CHARS(proto->name) : "<anonymous>";
    }
    
    if (out_source) {
        *out_source = proto->source_file ? proto->source_file : "<unknown>";
    }
    
    if (out_line) {
        int pc_offset = (int)(frame->pc - PROTO_CODE_BASE(proto));
        int line_count = PROTO_LINE_COUNT(proto);
        if (pc_offset >= 0 && pc_offset < line_count) {
            *out_line = PROTO_LINE(proto, pc_offset);
        } else {
            *out_line = 1;
        }
    }
    
    return true;
}

// ============================================================================
// Variables
// ============================================================================

XrJsonValue *xdap_inspect_locals(XdapController *ctrl, XrCoroutine *coro,
                                  int frame_idx) {
    XrJsonValue *variables = xlsp_json_new_array();
    
    if (!ctrl || !coro) return variables;
    
    int actual_idx = coro->vm_ctx.frame_count - 1 - frame_idx;
    if (actual_idx < 0 || actual_idx >= coro->vm_ctx.frame_count) return variables;
    
    XrBcCallFrame *frame = &coro->vm_ctx.frames[actual_idx];
    if (!frame->closure || !frame->closure->proto) return variables;
    
    XrProto *proto = frame->closure->proto;
    XrValue *base = coro->vm_ctx.stack + frame->base_offset;
    int locvar_count = PROTO_LOCVAR_COUNT(proto);
    
    for (int i = 0; i < locvar_count; i++) {
        XrLocVar locvar = PROTO_LOCVAR(proto, i);
        XrValue val = base[i];
        
        XrJsonValue *var = xlsp_json_new_object();
        xlsp_json_object_set(var, "name", 
            xlsp_json_new_string(locvar.name ? locvar.name : "<unnamed>"));
        
        char *value_str = xr_value_to_debug_string(ctrl->isolate, val);
        xlsp_json_object_set(var, "value", xlsp_json_new_string(value_str));
        free(value_str);
        
        xlsp_json_object_set(var, "type", 
            xlsp_json_new_string(xr_value_type_name(val)));
        
        // Variable reference for expandable values (directly use debug API)
        int var_ref = 0;
        if (ctrl->isolate && xr_debug_value_is_expandable(ctrl->isolate, val)) {
            XdapVarRefType ref_type = XDAP_REF_INVALID;
            if (XR_IS_ARRAY(val)) ref_type = XDAP_REF_ARRAY;
            else if (XR_IS_MAP(val)) ref_type = XDAP_REF_MAP;
            else if (XR_IS_JSON(val)) ref_type = XDAP_REF_OBJECT;
            else if (XR_IS_PTR(val) && XR_HEAP_TYPE(val) == XR_TINSTANCE) {
                ref_type = XDAP_REF_INSTANCE;
            }
            
            if (ref_type != XDAP_REF_INVALID) {
                var_ref = xr_debug_create_var_ref(ctrl->isolate, ref_type, frame_idx, val);
            }
        }
        xlsp_json_object_set(var, "variablesReference", xlsp_json_new_number(var_ref));
        
        xlsp_json_array_push(variables, var);
    }
    
    return variables;
}

// Convert XdapVarInfo array from debug API to JSON array for DAP protocol
static XrJsonValue *var_info_to_json(XrayIsolate *isolate, int var_ref_id) {
    XdapVarInfo *vars = NULL;
    int count = xr_debug_get_var_children(isolate, var_ref_id, &vars);
    if (count <= 0 || !vars) return xlsp_json_new_array();
    
    XrJsonValue *result = xlsp_json_new_array();
    for (int i = 0; i < count; i++) {
        XrJsonValue *var = xlsp_json_new_object();
        xlsp_json_object_set(var, "name", xlsp_json_new_string(vars[i].name ? vars[i].name : "?"));
        xlsp_json_object_set(var, "value", xlsp_json_new_string(vars[i].value ? vars[i].value : "?"));
        xlsp_json_object_set(var, "type", xlsp_json_new_string(vars[i].type ? vars[i].type : "?"));
        xlsp_json_object_set(var, "variablesReference", xlsp_json_new_number(vars[i].var_ref));
        xlsp_json_array_push(result, var);
    }
    xr_debug_var_info_array_free(vars, count);
    return result;
}

XrJsonValue *xdap_inspect_variables(XdapController *ctrl, int var_ref) {
    if (!ctrl || !ctrl->isolate) return xlsp_json_new_array();
    
    XrDebugVarRef *ref = xr_debug_get_var_ref(ctrl->isolate, var_ref);
    if (!ref) return xlsp_json_new_array();
    
    switch (ref->type) {
        case XDAP_REF_SCOPE_LOCALS: {
            XrCoroutine *coro = ctrl->stopped_coro;
            return xdap_inspect_locals(ctrl, coro, ref->frame_idx);
        }
        
        case XDAP_REF_ARRAY:
        case XDAP_REF_MAP:
        case XDAP_REF_OBJECT:
        case XDAP_REF_INSTANCE:
            return var_info_to_json(ctrl->isolate, var_ref);
            
        default:
            break;
    }
    
    return xlsp_json_new_array();
}

// ============================================================================
// Expression Evaluation (delegates to full AST evaluator in xdap_debug.c)
// ============================================================================

char *xdap_inspect_evaluate(XdapController *ctrl, const char *expression,
                             int frame_idx) {
    if (!ctrl || !ctrl->isolate || !expression) {
        return strdup("<error: invalid context>");
    }
    
    return xr_debug_evaluate(ctrl->isolate, expression, frame_idx);
}

char *xdap_inspect_evaluate_ex(XdapController *ctrl, const char *expression,
                                int frame_idx, int *out_var_ref) {
    if (!ctrl || !ctrl->isolate || !expression) {
        if (out_var_ref) *out_var_ref = 0;
        return strdup("<error: invalid context>");
    }
    
    return xr_debug_evaluate_ex(ctrl->isolate, expression, frame_idx, out_var_ref);
}
