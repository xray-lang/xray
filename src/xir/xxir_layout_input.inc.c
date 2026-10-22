/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_layout_input.inc.c - Current physical readers for target layout
 */
typedef struct LayoutInputRequest {
    const XrXirCompileContext *context;
    const XirLayoutInput *input;
    const XrXirTarget *target;
} LayoutInputRequest;
typedef struct LayoutInputSlot {
    XrXirType type;
    XrXirPlaceKind place;
    bool phi;
} LayoutInputSlot;

static XrXirStatus layout_storage(const XrXirCompileContext *context,
    const void *memory, size_t count, size_t element, size_t alignment) {
    if (!count) return XR_XIR_OK;
    if ((uintptr_t)memory % alignment) return XR_XIR_BAD_STRUCTURE;
    if (count > SIZE_MAX / element) return XR_XIR_BUDGET;
    XrCompileResourceStatus status=xr_compile_resources_extent(context->resources,memory,count*element);
    return status==XR_COMPILE_RESOURCE_OK ? XR_XIR_OK :
        status==XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_BAD_STRUCTURE;
}
static XrXirStatus layout_raw_head(const XrXirCompileContext *context,
    const void *owner,uint32_t f,XirInputFunctionHead *output) {
    const XrXirModule *module=owner;
    if (!module || !output || !module->functions || f>=module->function_count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,sizeof(*output))) return XR_XIR_BUDGET;
    const XrXirFunction *function=&module->functions[f];
    *output=(XirInputFunctionHead){function->name,function->name_length,function->parameters,
        function->parameter_count,function->result,function->blocks,function->block_count,
        function->instruction_count,function->operands,function->operand_count,function->error_type};
    return XR_XIR_OK;
}
static XrXirStatus layout_raw_instruction(const XrXirCompileContext *context,
    const void *owner,uint32_t f,uint32_t i,XrXirInstruction *storage,const XrXirInstruction **output) {
    const XrXirModule *module=owner;
    (void)storage;
    if (!module || !output || !module->functions || f>=module->function_count ||
        !module->functions[f].instructions || i>=module->functions[f].instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
    *output=&module->functions[f].instructions[i];return XR_XIR_OK;
}
static XirLayoutInput layout_raw_input(const XrXirModule *module) {
    return (XirLayoutInput){xir_metadata_domain_from_module(module),module,
        layout_raw_head,layout_raw_instruction};
}
static XrXirStatus layout_read_slot(const LayoutInputRequest *request,
    const XirInputFunctionHead *head,uint32_t function,uint32_t slot,LayoutInputSlot *output) {
    if (!head || !output) return XR_XIR_BAD_STRUCTURE;
    if (slot<head->parameter_count) {
        if (!head->parameters) return XR_XIR_BAD_STRUCTURE;
        *output=(LayoutInputSlot){head->parameters[slot],XR_XIR_PLACE_NONE,false};return XR_XIR_OK;
    }
    XrXirInstruction storage;const XrXirInstruction *op=NULL;
    XrXirStatus status=request->input->instruction(request->context,request->input->owner,
        function,slot-head->parameter_count,&storage,&op);
    if (status!=XR_XIR_OK) return status;
    if (!op) return XR_XIR_BAD_STRUCTURE;
    *output=(LayoutInputSlot){op->op==XR_XIR_INVOKE || op->op==XR_XIR_INVOKE_INDIRECT ?
        XR_XIR_UNIT : op->type,xr_xir_place_op_kind(op->op),op->op==XR_XIR_PHI};
    return XR_XIR_OK;
}
static XrXirStatus layout_path_capacity(const LayoutInputRequest *request,
    const XirInputFunctionHead *head,uint32_t function,uint32_t *capacity) {
    *capacity=0;
    for (uint32_t i=0;i<head->instruction_count;++i) {
        if (!xir_compile_work(request->context,1)) return XR_XIR_BUDGET;
        XrXirInstruction storage;const XrXirInstruction *op=NULL;
        XrXirStatus status=request->input->instruction(request->context,request->input->owner,
            function,i,&storage,&op);
        if (status!=XR_XIR_OK) return status;
        if (!op) return XR_XIR_BAD_STRUCTURE;
        if (op->op==XR_XIR_FIELD_PLACE || op->op==XR_XIR_INDEX_PLACE ||
            xr_xir_operand_role(op->op,0)==XR_XIR_OPERAND_VALUE) continue;
        uint32_t id=op->args[0];
        if (xr_xir_op_uses_operand_table(op->op)) {
            if (!head->operands || id>=head->operand_count) return XR_XIR_BAD_VALUE;
            id=head->operands[id];
        }
        uint32_t depth=0;
        bool cell_root=false;
        for (;;) {
            LayoutInputSlot slot;
            status=layout_read_slot(request,head,function,id,&slot);
            if (status!=XR_XIR_OK) return status;
            if (slot.place!=XR_XIR_PLACE_FIELD && slot.place!=XR_XIR_PLACE_INDEX) {
                cell_root=slot.place==XR_XIR_PLACE_CELL;
                break;
            }
            if (!xir_compile_work(request->context,1)) return XR_XIR_BUDGET;
            if (depth==head->instruction_count) return XR_XIR_BAD_VALUE;
            ++depth;
            XrXirInstruction parent_storage;const XrXirInstruction *parent=NULL;
            status=request->input->instruction(request->context,request->input->owner,
                function,id-head->parameter_count,&parent_storage,&parent);
            if (status!=XR_XIR_OK) return status;
            if (!parent) return XR_XIR_BAD_STRUCTURE;
            id=parent->args[0];
        }
        /* A direct Cell uses the same implicit selector as a nested place. */
        if ((depth || cell_root) && (op->op==XR_XIR_ARRAY_GET || op->op==XR_XIR_ARRAY_SET || op->op==XR_XIR_STRUCT_SET)) {
            if (depth==UINT32_MAX) return XR_XIR_BUDGET;
            ++depth;
        }
        if (depth>*capacity) *capacity=depth;
    }
    return XR_XIR_OK;
}
