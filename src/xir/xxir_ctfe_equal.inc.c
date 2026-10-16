/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_equal.inc.c - Original-ledger prepayment for shared owned equality
 *
 * KEY CONCEPT:
 *   Authentic bounded getters expose active operands. This walk pays work;
 *   only the existing Value equality operation determines the boolean result.
 */
typedef struct CtfeEqualFrame {
    XrXirValue left,right;
    uint64_t next,count;
    bool left_owned,right_owned;
} CtfeEqualFrame;
static bool ctfe_owned_equal_opcode(const ScalarRun *run,const XrXirInstruction *op) {
    if (op->op!=XR_XIR_EQUAL) return false;
    XrXirType type=xr_xir_operand_type(run->function,op->args[0]);
    return xr_xir_type_is_array(run->module->types,type) || xr_xir_type_is_nullable(run->module->types,type);
}
static XrXirCtfeStatus ctfe_owned_equal_work(ScalarRun *run,CtfeOwnedState *state,
    XrXirValue left,XrXirValue right) {
    CtfeEqualFrame *stack=NULL;uint32_t depth=0,capacity=0;bool left_owned=false,right_owned=false;
    XrXirCtfeStatus status=XR_XIR_CTFE_OK;
    for (;;) {
        if (!scalar_compile_work(run,64)) { status=XR_XIR_CTFE_BUDGET;break; }
        status=ctfe_validation_work(run,state,(XrXirType)left.type);if (status!=XR_XIR_CTFE_OK) break;
        bool array=xr_xir_type_is_array(run->module->types,(XrXirType)left.type);
        if (xr_xir_type_is_nullable(run->module->types,(XrXirType)left.type)) {
            bool a=false,b=false;const XrXirValue *first=NULL,*second=NULL;
            if (!xr_xir_nullable_view(&left,&a,&first) || !xr_xir_nullable_view(&right,&b,&second)) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
            if (a && b) {
                XrXirValue next_left={0},next_right={0};
                status=ctfe_value_status(xr_xir_value_copy(first,&next_left));
                if (status==XR_XIR_CTFE_OK) status=ctfe_value_status(xr_xir_value_copy(second,&next_right));
                if (status!=XR_XIR_CTFE_OK) { xr_xir_value_drop(&next_left);xr_xir_value_drop(&next_right);break; }
                if (left_owned) xr_xir_value_drop(&left);if (right_owned) xr_xir_value_drop(&right);
                left=next_left;right=next_right;left_owned=right_owned=true;continue;
            }
        } else if (array) {
            XrXirValueAdmission admission={.arena=state->arena,.domain=state->domain,.work=UINT64_MAX};int64_t a=0,b=0;
            status=ctfe_value_status(xr_xir_array_len(&left,&admission,&a));
            if (status==XR_XIR_CTFE_OK) status=ctfe_value_status(xr_xir_array_len(&right,&admission,&b));
            if (status!=XR_XIR_CTFE_OK) break;
            if (a==b && a) {
                if (depth==capacity) {
                    uint64_t next=capacity?(uint64_t)capacity*2:1,size=next*sizeof(*stack),old=(uint64_t)capacity*sizeof(*stack);
                    if (next>UINT32_MAX || size>SIZE_MAX || old>state->frame_spare || size>state->frame_spare-old) { status=XR_XIR_CTFE_FRAME_LIMIT;break; }
                    if (!scalar_compile_work(run,(uint64_t)depth*sizeof(*stack)+sizeof(*stack))) { status=XR_XIR_CTFE_BUDGET;break; }
                    void *memory=stack;status=ctfe_compile_status(xir_compile_resource_status(xr_compile_resources_resize(run->compile_resources,&memory,(size_t)size)));
                    if (status!=XR_XIR_CTFE_OK) break;stack=memory;capacity=(uint32_t)next;
                }
                stack[depth++]=(CtfeEqualFrame){left,right,0,(uint64_t)a,left_owned,right_owned};
                left=right=(XrXirValue){0};left_owned=right_owned=false;
                goto next_child;
            }
        } else if (left.type==XR_XIR_STRING) {
            const char *a=NULL,*b=NULL;size_t first=0,second=0;
            if (!xr_xir_string_view(&left,&a,&first) || !xr_xir_string_view(&right,&b,&second)) { status=XR_XIR_CTFE_BAD_ARTIFACT;break; }
            if ((uint64_t)second>UINT64_MAX-(uint64_t)first || !scalar_compile_work(run,(uint64_t)first+(uint64_t)second)) { status=XR_XIR_CTFE_BUDGET;break; }
        } else if (xr_xir_float_bits((XrXirType)left.type) && !scalar_compile_work(run,1024)) { status=XR_XIR_CTFE_BUDGET;break; }
        if (left_owned) xr_xir_value_drop(&left);if (right_owned) xr_xir_value_drop(&right);
        left=right=(XrXirValue){0};left_owned=right_owned=false;
next_child:
        while (depth && stack[depth-1].next==stack[depth-1].count) {
            CtfeEqualFrame *frame=&stack[--depth];
            if (frame->left_owned) xr_xir_value_drop(&frame->left);if (frame->right_owned) xr_xir_value_drop(&frame->right);
        }
        if (!depth) break;
        CtfeEqualFrame *frame=&stack[depth-1];
        status=ctfe_admission_work(run,state,&frame->left);
        if (status==XR_XIR_CTFE_OK) status=ctfe_admission_work(run,state,&frame->right);
        if (status!=XR_XIR_CTFE_OK) break;
        XrXirValueAdmission admission={.arena=state->arena,.domain=state->domain,.work=UINT64_MAX,
            .scratch_bytes=state->frame_spare-(uint64_t)capacity*sizeof(*stack)};XrXirFaultDetail fault={0};
        status=ctfe_value_status(xr_xir_array_get(&frame->left,(int64_t)frame->next,&admission,&left,&fault));
        if (status!=XR_XIR_CTFE_OK) break;left_owned=true;
        status=ctfe_value_status(xr_xir_array_get(&frame->right,(int64_t)frame->next++,&admission,&right,&fault));
        if (status!=XR_XIR_CTFE_OK) break;right_owned=true;
    }
    if (left_owned) xr_xir_value_drop(&left);if (right_owned) xr_xir_value_drop(&right);
    while (depth) {
        CtfeEqualFrame *frame=&stack[--depth];
        if (frame->left_owned) xr_xir_value_drop(&frame->left);if (frame->right_owned) xr_xir_value_drop(&frame->right);
    }
    xr_compile_resources_free(stack);return status;
}
static XrXirCtfeStatus ctfe_owned_equal_step(ScalarRun *run,uint32_t *cursor,CtfeOwnedState *state) {
    const XrXirInstruction *op=&run->function->instructions[*cursor];
    XrXirValue left=ctfe_array_operand(run,op->args[0]),right=ctfe_array_operand(run,op->args[1]);bool equal=false;
    XrXirCtfeStatus status=ctfe_owned_equal_work(run,state,left,right);if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValueAdmission admission={.arena=state->arena,.domain=state->domain,.work=UINT64_MAX,.scratch_bytes=state->frame_spare};
    status=ctfe_value_status(xr_xir_value_equal(&left,&right,(XrXirType)left.type,&admission,&equal));
    if (status!=XR_XIR_CTFE_OK) return status;
    xr_xir_scalar_store(run->frame,run->layout->offsets[run->function->parameter_count+*cursor],op->immediate?!equal:equal);
    ++*cursor;return XR_XIR_CTFE_OK;
}
