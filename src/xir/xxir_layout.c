/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_layout.c - Target-bound scalar layout authority
 *
 * KEY CONCEPT:
 *   Physical consumers read frozen offsets; only this service chooses layouts.
 */

#include "xxir_internal.h"
#include "xxir_layout_input_internal.h"
#include "xxir_compile_memory.h"
#include "xxir_types.h"
#include "xxir_operand_roles.h"
#include "xxir_value_place.h"
#include "../base/xmalloc.h"

static bool layout_equal(XrXirLayout left, XrXirLayout right) {
    return left.size == right.size && left.alignment == right.alignment;
}

#include "xxir_layout_input.inc.c"


static XrXirStatus layout_function_bounds(const XrXirModule *module, const XrXirCompileContext *context) {
    for (uint32_t f = 0; f < module->function_count; ++f) {
        if (!xir_compile_work(context, 1)) return XR_XIR_BUDGET;
        const XrXirFunction *function = &module->functions[f];
        uint64_t slots = (uint64_t)function->parameter_count + function->instruction_count;
        if (slots > UINT32_MAX / 2 || slots > SIZE_MAX / (sizeof(uint32_t) * 2)) return XR_XIR_BUDGET;
    }
    return XR_XIR_OK;
}

static XrXirStatus layout_structure(const XrXirModule *module, const XrXirCompileContext *context) {
    XrXirStatus status = xr_xir_compile_types_structure_verify(context, module->types);
    if (status == XR_XIR_OK) status = xr_xir_compile_declarations_verify(context,
        module->declarations, module->types, module->function_count, module->linkage_kind);
    return status == XR_XIR_OK ? layout_function_bounds(module, context) : status;
}

static XrXirStatus function_layout(const LayoutInputRequest *request,uint32_t index,
    XrXirFunctionLayout *layout,bool create) {
    const XrXirCompileContext *budget=request->context;
    XrXirStatus allocation_status=XR_XIR_OK;
    XirInputFunctionHead head;
    XrXirStatus head_status=request->input->head(budget,request->input->owner,index,&head);
    if (head_status!=XR_XIR_OK) return head_status;
    const XirInputFunctionHead *function=&head;
    uint64_t maximum=(uint64_t)head.parameter_count+head.instruction_count;
    if (maximum>UINT32_MAX/2 || maximum>SIZE_MAX/(2*sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t slots=(uint32_t)maximum;
    if (create) {
        layout->slot_count = slots;
        layout->offsets = xir_compile_calloc(budget, slots, sizeof(*layout->offsets), &allocation_status);
        layout->owned_offsets = xir_compile_calloc(budget, (size_t) slots * 2, sizeof(*layout->owned_offsets), &allocation_status);
        if (!layout->offsets || !layout->owned_offsets)
            return allocation_status;
        if (function->parameter_count) {
            layout->parameters = xir_compile_calloc(budget, function->parameter_count, sizeof(*layout->parameters), &allocation_status);
            if (!layout->parameters)
                return allocation_status;
        }
    } else if (layout->slot_count != slots || !layout->offsets || !layout->owned_offsets ||
               (function->parameter_count && !layout->parameters) ||
               (!function->parameter_count && layout->parameters)) {
        return XR_XIR_BAD_LAYOUT;
    }
    if (!create) {
        XrXirStatus status=layout_storage(budget,layout->offsets,slots,sizeof(uint32_t),_Alignof(uint32_t));
        if (status==XR_XIR_OK) status=layout_storage(budget,layout->owned_offsets,(size_t)slots*2,
            sizeof(uint32_t),_Alignof(uint32_t));
        if (status==XR_XIR_OK) status=layout_storage(budget,layout->parameters,head.parameter_count,
            sizeof(XrXirLayout),_Alignof(XrXirLayout));
        if (status!=XR_XIR_OK) return status;
    }
    uint32_t bytes = 0, owned = 0;
    for (uint32_t slot = 0; slot < slots; ++slot) {
        if (!xir_compile_work(budget, 1)) return XR_XIR_BUDGET;
        LayoutInputSlot field;
        XrXirStatus reading=layout_read_slot(request,function,index,slot,&field);
        if (reading!=XR_XIR_OK) return reading;
        XrXirPlaceKind place=field.place;
        if (place == XR_XIR_PLACE_CELL || place == XR_XIR_PLACE_SLOT ||
            place == XR_XIR_PLACE_FIELD || place == XR_XIR_PLACE_INDEX || place == XR_XIR_PLACE_OBJECT) {
            if (create) ((uint32_t *) layout->offsets)[slot] = UINT32_MAX;
            else if (layout->offsets[slot] != UINT32_MAX) return XR_XIR_BAD_LAYOUT;
            continue;
        }
        XrXirLayout physical;
        XrXirType type = field.type;
        XrXirStatus status = xr_xir_compile_layout(budget, request->input->domain.types, type, request->target, XR_XIR_LAYOUT_FRAME, &physical);
        if (status != XR_XIR_OK)
            return status;
        bool phi = field.phi;
        uint32_t offset = physical.size ? bytes : UINT32_MAX;
        uint32_t stride = physical.size;
        if (phi) physical.size *= 2;
        if (physical.size > UINT32_MAX - bytes ||
            (uint64_t) bytes + physical.size > budget->limits.frame_bytes)
            return XR_XIR_BUDGET;
        bytes += physical.size;
        if (create)
            ((uint32_t *) layout->offsets)[slot] = offset;
        else if (layout->offsets[slot] != offset)
            return XR_XIR_BAD_LAYOUT;
        if (xr_xir_type_is_owned(request->input->domain.types, type)) {
            if (create) ((uint32_t *) layout->owned_offsets)[owned] = offset;
            else if (owned >= layout->owned_count || layout->owned_offsets[owned] != offset)
                return XR_XIR_BAD_LAYOUT;
            ++owned;
            if (phi) {
                if (create) ((uint32_t *) layout->owned_offsets)[owned] = offset + stride;
                else if (owned >= layout->owned_count || layout->owned_offsets[owned] != offset + stride)
                    return XR_XIR_BAD_LAYOUT;
                ++owned;
            }
        }
        if (slot < function->parameter_count) {
            status = xr_xir_compile_layout(budget, request->input->domain.types, type, request->target, XR_XIR_LAYOUT_PARAMETER, &physical);
            if (status != XR_XIR_OK)
                return status;
            if (create)
                ((XrXirLayout *) layout->parameters)[slot] = physical;
            else if (!layout_equal(layout->parameters[slot], physical))
                return XR_XIR_BAD_LAYOUT;
        }
    }
    XrXirLayout result;
    uint32_t outgoing = 0;
    for (uint32_t i = 0; i < function->instruction_count; ++i) {
        if (!xir_compile_work(budget, 1)) return XR_XIR_BUDGET;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        XrXirStatus reading=request->input->instruction(budget,request->input->owner,index,i,&storage,&op);
        if (reading!=XR_XIR_OK) return reading;
        if (!op) return XR_XIR_BAD_STRUCTURE;
        uint32_t count = op->op == XR_XIR_OUTPUT || op->op == XR_XIR_WRITE_STREAM ? 1 :
            xr_xir_op_references_function(op->op) || op->op == XR_XIR_INVOKE_INDIRECT || op->op == XR_XIR_CALL_INDIRECT ||
            op->op == XR_XIR_PRINT || op->op == XR_XIR_ARRAY_NEW || op->op == XR_XIR_STRUCT_NEW ||
            op->op == XR_XIR_ENUM_NEW || op->op == XR_XIR_CLASS_NEW || op->op == XR_XIR_SLOT_GROUP_INIT ? op->args[1] : 0;
        if (op->op==XR_XIR_TUPLE_NEW) {
            const XrXirTypeNode *tuple=xr_xir_tuple_signature(request->input->domain.types,op->type);
            if (!tuple) return XR_XIR_BAD_TYPE;
            count=tuple->parameter_count;
        }
        if (count > outgoing) outgoing = count;
    }
    uint32_t paths = 0;
    XrXirStatus path_status = layout_path_capacity(request,function,index,&paths);
    if (path_status != XR_XIR_OK) return path_status;
    uint64_t physical_bytes = bytes + (uint64_t) outgoing * sizeof(XrXirValue) +
        (uint64_t) paths * sizeof(XrXirValuePathStep);
    if (physical_bytes > UINT32_MAX || physical_bytes > budget->limits.frame_bytes) return XR_XIR_BUDGET;
    XrXirStatus status = xr_xir_compile_layout(budget, request->input->domain.types, function->result, request->target, XR_XIR_LAYOUT_RESULT, &result);
    if (status != XR_XIR_OK)
        return status;
    if (create) {
        layout->result = result;
        layout->frame_bytes = bytes;
        layout->owned_count = owned;
        layout->outgoing_count = outgoing;
        layout->path_count = paths;
    } else if (!layout_equal(layout->result, result) || layout->frame_bytes != bytes ||
               layout->owned_count != owned || layout->outgoing_count != outgoing || layout->path_count != paths) {
        return XR_XIR_BAD_LAYOUT;
    }
    return XR_XIR_OK;
}

XrXirStatus xr_xir_compile_layout_build(XrXirArtifact *artifact) {
    XrXirStatus allocation_status = XR_XIR_OK;
    if (!artifact) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = artifact->context;
    XrXirCompileContext *budget = &compile_state;
    XrXirStatus status = layout_structure(&artifact->module, budget);
    if (status != XR_XIR_OK)
        return status;
    artifact->layouts = xir_compile_calloc(&artifact->context, artifact->module.function_count, sizeof(*artifact->layouts), &allocation_status);
    if (!artifact->layouts)
        return allocation_status;
    XirLayoutInput input=layout_raw_input(&artifact->module);
    LayoutInputRequest request={budget,&input,&artifact->target};
    for (uint32_t f = 0; f < artifact->module.function_count; ++f) {
        status = function_layout(&request,f,&artifact->layouts[f],true);
        if (status != XR_XIR_OK)
            return status;
    }
    return XR_XIR_OK;
}

XR_FUNC XrXirStatus xir_layout_input_match(const XrXirCompileContext *context,
    const XirLayoutInput *input,const XrXirTarget *target,XrXirFunctionLayout *layouts) {
    if (!xir_compile_context_valid(context) || !input || !input->owner || !input->head ||
        !input->instruction || !target || (input->domain.function_count && !layouts)) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=layout_storage(context,layouts,input->domain.function_count,
        sizeof(XrXirFunctionLayout),_Alignof(XrXirFunctionLayout));
    if (status!=XR_XIR_OK) return status;
    LayoutInputRequest request={context,input,target};
    for (uint32_t f=0;f<input->domain.function_count;++f) {
        status=function_layout(&request,f,&layouts[f],false);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static XrXirStatus layout_match(const XrXirArtifact *artifact,const XrXirCompileContext *budget) {
    XirLayoutInput input=layout_raw_input(&artifact->module);
    return xir_layout_input_match(budget,&input,&artifact->target,artifact->layouts);
}

/* The calling artifact checker has just completed all metadata admission.
 * This matcher retains no caller proof and never crosses a request boundary. */
XR_FUNC XrXirStatus xr_xir_compile_layout_match_verified(const XrXirArtifact *artifact) {
    if (!artifact) return XR_XIR_BAD_STRUCTURE;
    if (artifact->module.stage != XR_XIR_LOWERED)
        return (!artifact->layouts && !artifact->target.architecture && !artifact->target.abi_version) ? XR_XIR_OK : XR_XIR_BAD_LAYOUT;
    if (!artifact->layouts) return XR_XIR_BAD_LAYOUT;
    XrXirCompileContext budget = artifact->context;
    XrXirStatus status = layout_function_bounds(&artifact->module, &budget);
    return status == XR_XIR_OK ? layout_match(artifact, &budget) : status;
}

XrXirStatus xr_xir_compile_layout_verify(const XrXirArtifact *artifact) {
    if (!artifact) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = artifact->context;
    XrXirCompileContext *budget = &compile_state;
    if (artifact->module.stage != XR_XIR_LOWERED)
        return (!artifact->layouts && !artifact->target.architecture && !artifact->target.abi_version)
            ? XR_XIR_OK : XR_XIR_BAD_LAYOUT;
    if (!artifact->layouts)
        return XR_XIR_BAD_LAYOUT;
    XrXirStatus status = layout_structure(&artifact->module, budget);
    if (status != XR_XIR_OK)
        return status;
    return layout_match(artifact, budget);
}
