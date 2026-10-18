/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_ctfe_import.inc.c - Independent bounded receiving of data arguments
 *
 * KEY CONCEPT:
 *   A source carrier pins its authentic immutable type pool. Every actual data
 *   node is copied into the current compiler owner; no foreign identity enters.
 */
#include "xxir_ctfe_import_types.inc.c"
static XrXirCtfeStatus ctfe_external_argument_admit(const XrXirArtifact *artifact,
    XrXirType target,const XrXirValue *value,const XrXirTypeArena **source) {
    if (ctfe_parameter_type(target))
        return xr_xir_value_argument(value,NULL,target)?XR_XIR_CTFE_OK:XR_XIR_CTFE_BAD_ARGUMENT;
    if (target==XR_XIR_UNIT) return XR_XIR_CTFE_NOT_SUPPORTED;
    XrXirCtfeTypeShape shape={0};
    XrXirCtfeStatus status=ctfe_type_info(&artifact->context,artifact->module.types,target,false,&shape);
    if (status!=XR_XIR_CTFE_OK) return status;
    const XrXirTypeArena *arena=NULL;
    status=ctfe_value_status(xr_xir_compile_value_arena_borrow(artifact->context.resources,value,&arena));
    if (status!=XR_XIR_CTFE_OK) return status==XR_XIR_CTFE_BAD_ARTIFACT?XR_XIR_CTFE_BAD_ARGUMENT:status;
    const XrXirTypes *types=xr_xir_compile_type_arena_types(arena);
    status=ctfe_type_info(&artifact->context,types,(XrXirType)value->type,false,&shape);
    if (status!=XR_XIR_CTFE_OK) return status;
    status=ctfe_import_types_equal(&artifact->context,types,(XrXirType)value->type,artifact->module.types,target);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (source) *source=arena;return XR_XIR_CTFE_OK;
}
typedef struct CtfeImportFrame {
    XrXirValue source,array;
    XrXirType target;
    XrXirValue *fields;
    uint64_t next,count;
    uint32_t begin,variant;
    bool source_owned;
} CtfeImportFrame;
typedef struct CtfeImportStack {
    CtfeImportFrame *frames;
    uint32_t depth,capacity;
    uint64_t bytes,maximum;
} CtfeImportStack;
static XrXirCtfeStatus ctfe_import_source_work(ScalarRun *run,CtfeOwnedState *state,
    const XrXirTypeArena *arena,XrXirType type) {
    XrXirCtfeTypeShape shape={0};
    XrXirCtfeStatus status=ctfe_type_info(state->context,xr_xir_compile_type_arena_types(arena),type,false,&shape);
    if (status!=XR_XIR_CTFE_OK) return status;
    if (shape.work>(UINT64_MAX-64)/16 || !scalar_compile_work(run,64+shape.work*16)) return XR_XIR_CTFE_BUDGET;
    return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_import_push(ScalarRun *run,CtfeImportStack *stack,CtfeImportFrame frame) {
    if (stack->depth==stack->capacity) {
        uint64_t count=stack->capacity?(uint64_t)stack->capacity*2:1,bytes=count*sizeof(*stack->frames);
        if (count>UINT32_MAX || bytes>SIZE_MAX || stack->bytes>stack->maximum || bytes>stack->maximum-stack->bytes)
            return XR_XIR_CTFE_FRAME_LIMIT;
        void *memory=stack->frames;
        XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(
            xr_compile_resources_resize(run->compile_resources,&memory,(size_t)bytes)));
        if (status!=XR_XIR_CTFE_OK) return status;
        stack->bytes+=bytes-(uint64_t)stack->capacity*sizeof(*stack->frames);
        stack->frames=memory;stack->capacity=(uint32_t)count;
    }
    stack->frames[stack->depth++]=frame;return XR_XIR_CTFE_OK;
}
static XrXirCtfeStatus ctfe_import_fields(ScalarRun *run,CtfeImportStack *stack,CtfeImportFrame *frame) {
    uint64_t bytes=frame->count*sizeof(*frame->fields);
    if (frame->count>SIZE_MAX/sizeof(*frame->fields) || bytes>stack->maximum-stack->bytes) return XR_XIR_CTFE_FRAME_LIMIT;
    if (!bytes) return XR_XIR_CTFE_OK;
    void *memory=NULL;
    XrXirCtfeStatus status=ctfe_compile_status(xir_compile_resource_status(
        xr_compile_resources_calloc(run->compile_resources,1,(size_t)bytes,&memory)));
    if (status==XR_XIR_CTFE_OK) { frame->fields=memory;stack->bytes+=bytes; }
    return status;
}
static XrXirCtfeStatus ctfe_import_child(ScalarRun *run,CtfeOwnedState *state,
    const XrXirTypeArena *arena,CtfeImportFrame *frame,XrXirValue *output,XrXirType *target) {
    XrXirCtfeStatus status=ctfe_import_source_work(run,state,arena,(XrXirType)frame->source.type);
    if (status!=XR_XIR_CTFE_OK) return status;
    const XrXirTypeNode *node=xr_xir_type_node(run->module->types,frame->target);
    uint32_t index=(uint32_t)frame->next;
    *target=node->kind==XR_XIR_TYPE_ARRAY || node->kind==XR_XIR_TYPE_NULLABLE?node->element:
        node->kind==XR_XIR_TYPE_TUPLE?node->parameters[index].type:node->nominal.fields[frame->begin+index];
    XrXirValueAdmission admission=ctfe_value_admission(state);admission.arena=arena;
    return ctfe_value_status(xr_xir_compile_value_observe(run->compile_resources,&frame->source,frame->next,&admission,output));
}
static XrXirCtfeStatus ctfe_import_finish(ScalarRun *run,CtfeOwnedState *state,
    CtfeImportFrame *frame,XrXirValue *output) {
    if (frame->array.type) { *output=frame->array;frame->array=(XrXirValue){0};return XR_XIR_CTFE_OK; }
    XrXirCtfeStatus status=ctfe_validation_work(run,state,frame->target);
    for (uint64_t i=0;status==XR_XIR_CTFE_OK && i<frame->count;++i) status=ctfe_admission_work(run,state,&frame->fields[i]);
    if (status!=XR_XIR_CTFE_OK) return status;
    XrXirValueAdmission admission=ctfe_value_admission(state);
    const XrXirTypeNode *node=xr_xir_type_node(run->module->types,frame->target);
    if (node->kind==XR_XIR_TYPE_TUPLE)
        return ctfe_value_status(xr_xir_tuple_new(frame->target,frame->fields,(uint32_t)frame->count,&admission,output));
    if (node->kind==XR_XIR_TYPE_NULLABLE)
        return ctfe_value_status(xr_xir_nullable_new(frame->target,frame->count?frame->fields:NULL,&admission,output));
    if (xr_xir_type_is_struct(run->module->types,frame->target))
        return ctfe_value_status(xr_xir_struct_new(frame->target,frame->fields,(uint32_t)frame->count,&admission,output));
    return ctfe_value_status(xr_xir_enum_new(frame->target,frame->variant,frame->fields,(uint32_t)frame->count,&admission,output));
}
static void ctfe_import_frame_drop(CtfeImportStack *stack,CtfeImportFrame *frame) {
    if (frame->source_owned) xr_xir_value_drop(&frame->source);
    xr_xir_value_drop(&frame->array);
    if (frame->fields) {
        for (uint64_t i=0;i<frame->count;++i) xr_xir_value_drop(&frame->fields[i]);
        xr_compile_resources_free(frame->fields);stack->bytes-=frame->count*sizeof(*frame->fields);
    }
}
static XrXirCtfeStatus ctfe_external_argument_store(ScalarRun *run,uint32_t offset,
    XrXirType target,const XrXirValue *input,CtfeOwnedState *state) {
    if (ctfe_parameter_type(target)) return ctfe_argument_store(run,offset,input,state,true);
    const XrXirTypeArena *arena=NULL;
    XrXirCtfeStatus status=ctfe_external_argument_admit(state->artifact,target,input,&arena);
    if (status!=XR_XIR_CTFE_OK) return status;
    uint64_t original_spare=state->frame_spare;
    uint64_t fixed=sizeof(CtfeImportStack)+2*sizeof(CtfeImportFrame)+3*sizeof(XrXirValue);
    if (fixed>ctfe_frame_spare(state)) return XR_XIR_CTFE_FRAME_LIMIT;
    uint64_t import_spare=original_spare-fixed;state->frame_spare=import_spare;
    CtfeImportStack stack={.maximum=ctfe_frame_spare(state)};
    XrXirValue current=*input,result={0};bool current_owned=false;
    for (;;) {
        state->frame_spare=import_spare-stack.bytes;
        if (!scalar_compile_work(run,sizeof(CtfeImportFrame)+sizeof(result))) { status=XR_XIR_CTFE_BUDGET;break; }
        status=ctfe_import_source_work(run,state,arena,(XrXirType)current.type);
        if (status!=XR_XIR_CTFE_OK) break;
        if (target==XR_XIR_UNIT) {
            if (current.type || current.reserved || current.payload) { status=XR_XIR_CTFE_BAD_ARGUMENT;break; }
        } else if (!xr_xir_value_argument(&current,current.type>=XR_XIR_CONSTRUCTED_TYPE_BASE?arena:NULL,(XrXirType)current.type)) {
            status=XR_XIR_CTFE_BAD_ARGUMENT;break;
        }
        if (ctfe_scalar_type(target)) {
            if (current.type!=(uint32_t)target) { status=XR_XIR_CTFE_BAD_ARGUMENT;break; }
            result=current;
        }
        else if (target==XR_XIR_STRING) {
            const char *bytes=NULL;size_t length=0;
            if (!xr_xir_string_view(&current,&bytes,&length)) { status=XR_XIR_CTFE_BAD_ARGUMENT;break; }
            if (!scalar_compile_work(run,(uint64_t)length)) { status=XR_XIR_CTFE_BUDGET;break; }
            status=ctfe_value_status(xr_xir_string_new(state->domain,bytes,length,&result));
            if (status!=XR_XIR_CTFE_OK) break;
        } else if (target!=XR_XIR_UNIT) {
            for (uint32_t i=0;i<stack.depth;++i) {
                if (!scalar_compile_work(run,sizeof(XrXirValue))) { status=XR_XIR_CTFE_BUDGET;break; }
                if (stack.frames[i].source.type==current.type && stack.frames[i].source.payload==current.payload) {
                    status=XR_XIR_CTFE_NOT_SUPPORTED;break;
                }
            }
            if (status!=XR_XIR_CTFE_OK) break;
            const XrXirTypeNode *node=xr_xir_type_node(run->module->types,target);
            CtfeImportFrame frame={.source=current,.target=target,.source_owned=current_owned};
            if (!node) { status=XR_XIR_CTFE_BAD_ARGUMENT;break; }
            if (node->kind==XR_XIR_TYPE_ARRAY) {
                XrXirValueAdmission admission=ctfe_value_admission(state);admission.arena=arena;
                int64_t length=0,capacity=0;
                status=ctfe_value_status(xr_xir_array_len(&current,&admission,&length));
                if (status==XR_XIR_CTFE_OK) status=ctfe_value_status(xr_xir_array_capacity(&current,&admission,&capacity));
                if (status!=XR_XIR_CTFE_OK) break;
                frame.count=(uint64_t)length;
                status=ctfe_array_bytes_work(run,state,target,(uint64_t)capacity);
                if (status!=XR_XIR_CTFE_OK) break;
                admission=ctfe_value_admission(state);
                status=ctfe_value_status(xr_xir_array_with_capacity(target,capacity,&admission,&frame.array));
                if (status!=XR_XIR_CTFE_OK) break;
            } else if (node->kind==XR_XIR_TYPE_NULLABLE) {
                bool some=false;const XrXirValue *payload=NULL;
                if (!xr_xir_nullable_view(&current,&some,&payload)) { status=XR_XIR_CTFE_BAD_ARGUMENT;break; }
                frame.count=some?1:0;
            } else if (node->kind==XR_XIR_TYPE_TUPLE) frame.count=node->parameter_count;
            else if (xr_xir_type_is_struct(run->module->types,target)) frame.count=node->nominal.field_count;
            else {
                status=ctfe_value_status(xr_xir_enum_variant(&current,&frame.variant));
                const XrXirNominalVariant *variant=status==XR_XIR_CTFE_OK?ctfe_enum_variant_info(run->module->types,node,frame.variant):NULL;
                if (!variant) { if (status==XR_XIR_CTFE_OK) status=XR_XIR_CTFE_BAD_ARGUMENT;break; }
                frame.begin=variant->field_begin;frame.count=variant->field_count;
            }
            status=ctfe_import_push(run,&stack,frame);
            if (status!=XR_XIR_CTFE_OK) { xr_xir_value_drop(&frame.array);break; }
            current=(XrXirValue){0};current_owned=false;
            frame=stack.frames[stack.depth-1];
            if (!frame.array.type) status=ctfe_import_fields(run,&stack,&stack.frames[stack.depth-1]);
            if (status!=XR_XIR_CTFE_OK) break;
            state->frame_spare=import_spare-stack.bytes;
            if (frame.count) {
                status=ctfe_import_child(run,state,arena,&stack.frames[stack.depth-1],&current,&target);
                if (status!=XR_XIR_CTFE_OK) break;current_owned=true;continue;
            }
            status=ctfe_import_finish(run,state,&stack.frames[stack.depth-1],&result);
            if (status!=XR_XIR_CTFE_OK) break;
            ctfe_import_frame_drop(&stack,&stack.frames[--stack.depth]);
        }
        if (current_owned) xr_xir_value_drop(&current);current=(XrXirValue){0};current_owned=false;
        for (;;) {
            if (!stack.depth) goto done;
            state->frame_spare=import_spare-stack.bytes;
            CtfeImportFrame *frame=&stack.frames[stack.depth-1];
            if (frame->array.type) {
                status=ctfe_admission_work(run,state,&frame->array);
                if (status==XR_XIR_CTFE_OK) status=ctfe_admission_work(run,state,&result);
                if (status!=XR_XIR_CTFE_OK) goto done;
                XrXirValueAdmission admission=ctfe_value_admission(state);
                XrXirValuePlace place={frame->target,&frame->array.payload};
                status=ctfe_value_status(xr_xir_array_push(&place,&result,&admission));
                if (status!=XR_XIR_CTFE_OK) goto done;
                xr_xir_value_drop(&result);
            } else { frame->fields[frame->next]=result;result=(XrXirValue){0}; }
            if (++frame->next<frame->count) {
                status=ctfe_import_child(run,state,arena,frame,&current,&target);
                if (status!=XR_XIR_CTFE_OK) goto done;current_owned=true;break;
            }
            status=ctfe_import_finish(run,state,frame,&result);
            if (status!=XR_XIR_CTFE_OK) goto done;
            ctfe_import_frame_drop(&stack,frame);--stack.depth;
        }
    }
done:
    if (current_owned) xr_xir_value_drop(&current);
    while (stack.depth) ctfe_import_frame_drop(&stack,&stack.frames[--stack.depth]);
    xr_compile_resources_free(stack.frames);state->frame_spare=original_spare;
    if (status==XR_XIR_CTFE_OK) xr_xir_owned_slot_move(run->frame,offset,&result);
    xr_xir_value_drop(&result);return status;
}
