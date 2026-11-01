/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * move_runtime.h - Actual consuming calls and physical owner retirement
 *
 * KEY CONCEPT:
 *   Both backends execute the same cold checked functions and restore the
 *   actual Program frame after each Instance, without a synthetic owner.
 */
#ifndef MOVE_RUNTIME_H
#define MOVE_RUNTIME_H
#include "xir/xxir_output.h"
#define xr_xir_instance_cell_new_take_at move_test_cell_new_take_at_real
#define xr_xir_instance_move_arguments_at move_test_arguments_at_real
#define xr_xir_instance_cell_write_take_at move_test_write_take_at_real
#define xr_xir_instance_return_take_at move_test_return_take_at_real
#include "xir_runtime_allocations.h"
#undef xr_xir_instance_cell_new_take_at
#undef xr_xir_instance_move_arguments_at
#undef xr_xir_instance_cell_write_take_at
#undef xr_xir_instance_return_take_at
static unsigned move_test_transfers,move_test_prepare_failures;
static unsigned move_test_writes,move_test_returns;
static int64_t move_test_expected_payload;
static uint32_t move_test_expected_references;
static bool move_test_selected(const XrXirFunction *function) {
    return (function->name_length==5&&!memcmp(function->name,"moved",5))||
        (function->name_length==8&&!memcmp(function->name,"constant",8))||
        (function->name_length==8&&!memcmp(function->name,"indirect",8));
}
XR_FUNC XrXirCallStatus xr_xir_instance_move_arguments_at(XrXirCallView *view,
    uint32_t instruction,void *frame,const XrXirValue *arguments,uint32_t count) {
    XrXirInstance *instance=view_instance(view);uint32_t entry=xr_xir_call_current_entry(view->activation);
    const XrXirModule *module=&instance->program->permissions->input->artifact->module;
    bool selected=move_test_selected(&module->functions[entry]);
    if(selected) {
        CHECK(count==1);move_test_expected_payload=arguments[0].payload;
        move_test_expected_references=atomic_load(&object_pointer(arguments)->references);
        CHECK(move_test_expected_references>0);
    }
    XrXirCallStatus status=move_test_arguments_at_real(view,instruction,frame,arguments,count);
    if(selected)CHECK(status==XR_XIR_CALL_READY&&arguments[0].payload==move_test_expected_payload&&
        atomic_load(&object_pointer(arguments)->references)==move_test_expected_references);
    return status;
}
/* The normal adapter always executes. This observation chooses real fixture
 * entries only to check reference counts, never to authorize a transfer. */
XR_FUNC XrXirCallStatus xr_xir_instance_cell_new_take_at(XrXirCallView *view,
    uint32_t instruction,void *frame,XrXirValue *output) {
    XrXirInstance *instance=view_instance(view);uint32_t entry=xr_xir_call_current_entry(view->activation);
    const XrXirModule *module=&instance->program->permissions->input->artifact->module;
    const XrXirFunction *function=&module->functions[entry];
    CallFrame *active=view->activation->top,*parent=active->parent;
    const XrXirFunction *caller=parent?&module->functions[parent->entry-parent->metadata->entries]:NULL;
    bool observe=function->name_length==7&&!memcmp(function->name,"consume",7)&&caller&&move_test_selected(caller);
    if(!observe)return move_test_cell_new_take_at_real(view,instruction,frame,output);
    const XrXirFunctionLayout *layout=&instance->program->permissions->layouts[entry];
    const XrXirInstruction *op=&function->instructions[instruction];
    void *word=(unsigned char *)frame+layout->offsets[op->args[0]];int64_t payload=0;
    memcpy(&payload,word,sizeof(payload));XrXirValue value=xr_xir_value_load(view->arena,function->parameters[0],payload);
    XirObject *object=object_pointer(&value);CHECK(payload==move_test_expected_payload&&
        atomic_load(&object->references)==move_test_expected_references);
    CHECK(!active->arguments[0].type&&!active->arguments[0].payload);
    XrXirValue refused={0};
    CHECK(move_test_cell_new_take_at_real(view,instruction,(unsigned char *)view->state+active->entry->state_bytes,&refused)==XR_XIR_CALL_BAD_ARGUMENT);
    CHECK(unit_value(&refused));
    if(!move_test_prepare_failures) {
        size_t blocks=runtime_live,bytes=runtime_bytes;XrCompileResourceStats before,after;
        CHECK(xr_compile_resources_stats(instance->program->context.resources,&before)==XR_COMPILE_RESOURCE_OK);
        runtime_fail_at=runtime_attempts;
        XrXirCallStatus failed=move_test_cell_new_take_at_real(view,instruction,frame,&refused);
        runtime_fail_at=SIZE_MAX;CHECK(failed==XR_XIR_CALL_OOM&&unit_value(&refused));
        CHECK(runtime_live==blocks&&runtime_bytes==bytes&&atomic_load(&object->references)==move_test_expected_references);
        CHECK(xr_compile_resources_stats(instance->program->context.resources,&after)==XR_COMPILE_RESOURCE_OK&&before.held_bytes==after.held_bytes);
        int64_t retained=0;memcpy(&retained,word,sizeof(retained));CHECK(retained==payload);++move_test_prepare_failures;
    }
    XrXirCallStatus status=move_test_cell_new_take_at_real(view,instruction,frame,output);
    CHECK(status==XR_XIR_CALL_READY&&cell_owner(output)->value.payload==payload&&
        atomic_load(&object->references)==move_test_expected_references);
    int64_t empty=1;memcpy(&empty,word,sizeof(empty));CHECK(!empty);++move_test_transfers;return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_cell_write_take_at(XrXirCallView *view,
    uint32_t instruction,void *frame) {
    XrXirInstance *instance=view_instance(view);uint32_t entry=xr_xir_call_current_entry(view->activation);
    const XrXirModule *module=&instance->program->permissions->input->artifact->module;
    const XrXirFunction *function=&module->functions[entry];
    if(function->name_length!=12||memcmp(function->name,"destinations",12))
        return move_test_write_take_at_real(view,instruction,frame);
    const XrXirFunctionLayout *layout=&instance->program->permissions->layouts[entry];
    const XrXirInstruction *op=&function->instructions[instruction];int64_t source=0,handle=0;
    void *word=(unsigned char *)frame+layout->offsets[op->args[1]];
    memcpy(&source,word,sizeof(source));memcpy(&handle,(unsigned char *)frame+layout->offsets[op->args[0]],sizeof(handle));
    XrXirValue value=xr_xir_value_load(view->arena,xr_xir_operand_type(function,op->args[1]),source);
    XrXirValue cell=xr_xir_value_load(view->arena,xr_xir_operand_type(function,op->args[0]),handle);
    XirCell *owner=cell_owner(&cell);int64_t previous=owner->value.payload;
    uint32_t references=atomic_load(&object_pointer(&value)->references);
    size_t blocks=runtime_live,bytes=runtime_bytes;uint64_t work=view->activation->config.admission.work;
    CHECK(move_test_write_take_at_real(view,instruction,(unsigned char *)view->state+
        view->activation->top->entry->state_bytes)==XR_XIR_CALL_BAD_ARGUMENT);
    view->activation->config.admission.work=0;
    CHECK(move_test_write_take_at_real(view,instruction,frame)==XR_XIR_CALL_LIMIT);
    view->activation->config.admission.work=work;
    int64_t retained=0;memcpy(&retained,word,sizeof(retained));
    CHECK(retained==source&&owner->value.payload==previous&&runtime_live==blocks&&runtime_bytes==bytes&&
        atomic_load(&object_pointer(&value)->references)==references);
    XrXirCallStatus status=move_test_write_take_at_real(view,instruction,frame);
    memcpy(&retained,word,sizeof(retained));
    CHECK(status==XR_XIR_CALL_READY&&!retained&&cell_owner(&cell)==owner&&owner->value.payload==source&&
        atomic_load(&object_pointer(&value)->references)==references);++move_test_writes;return status;
}
XR_FUNC XrXirCallStatus xr_xir_instance_return_take_at(XrXirCallView *view,
    uint32_t instruction,void *frame,XrXirAction *output) {
    XrXirInstance *instance=view_instance(view);uint32_t entry=xr_xir_call_current_entry(view->activation);
    const XrXirModule *module=&instance->program->permissions->input->artifact->module;
    const XrXirFunction *function=&module->functions[entry];
    if(function->name_length!=4||memcmp(function->name,"echo",4))
        return move_test_return_take_at_real(view,instruction,frame,output);
    const XrXirFunctionLayout *layout=&instance->program->permissions->layouts[entry];
    void *word=(unsigned char *)frame+layout->offsets[function->instructions[instruction].args[0]];
    int64_t source=0;memcpy(&source,word,sizeof(source));
    XrXirValue value=xr_xir_value_load(view->arena,function->result,source);XrXirAction refused={0};
    uint32_t references=atomic_load(&object_pointer(&value)->references);
    size_t blocks=runtime_live,bytes=runtime_bytes;uint64_t work=view->activation->config.admission.work;
    CHECK(move_test_return_take_at_real(view,instruction,(unsigned char *)view->state+
        view->activation->top->entry->state_bytes,&refused)==XR_XIR_CALL_BAD_ARGUMENT);
    view->activation->config.admission.work=0;
    CHECK(move_test_return_take_at_real(view,instruction,frame,&refused)==XR_XIR_CALL_LIMIT);
    view->activation->config.admission.work=work;
    int64_t retained=0;memcpy(&retained,word,sizeof(retained));
    CHECK(retained==source&&!refused.kind&&!refused.value.payload&&!view->activation->top->return_staged&&
        runtime_live==blocks&&runtime_bytes==bytes&&atomic_load(&object_pointer(&value)->references)==references);
    XrXirCallStatus status=move_test_return_take_at_real(view,instruction,frame,output);
    memcpy(&retained,word,sizeof(retained));
    CHECK(status==XR_XIR_CALL_READY&&!retained&&output->kind==XR_XIR_ACTION_RETURN&&output->value.payload==source&&
        atomic_load(&object_pointer(&value)->references)==references);++move_test_returns;return status;
}
typedef struct MoveTestOutput {char bytes[256];size_t count;} MoveTestOutput;
static XrXirOutputStatus move_test_write(void *data,XrXirOutputStream stream,const char *bytes,size_t count) {
    MoveTestOutput *out=data;CHECK(stream==XR_XIR_STDOUT&&count<=sizeof(out->bytes)-out->count);
    memcpy(out->bytes+out->count,bytes,count);out->count+=count;return XR_XIR_OUTPUT_OK;
}
static void move_test_run(XrXirProgram *program,uint32_t entry,const XrXirCompileContext *context) {
    size_t blocks=runtime_live,bytes=runtime_bytes;XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    MoveTestOutput output={0};XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,move_test_write,&output,sizeof(output.bytes)};
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirInstanceResult result;unsigned ticks=0;
    do {CHECK(++ticks<100000);result=xr_xir_instance_poll_bounded(instance,4096);}while(result.outcome.status==XR_XIR_CALL_READY);
    fprintf(stderr,"MOVE_RUN entry%u status%u ticks%u\n",entry,result.outcome.status,ticks);
    CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    CHECK(value.type==XR_XIR_I64&&!value.payload);xr_xir_value_drop(&value);
    static const char expected[]="42 2 42 42 42 42 20 text! 42 42 42 42\n";
    CHECK(output.count==sizeof(expected)-1&&!memcmp(output.bytes,expected,output.count));
    CHECK(move_test_transfers==3&&move_test_prepare_failures==1);
    CHECK(move_test_writes==1&&move_test_returns==1);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==blocks&&runtime_bytes==bytes);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK&&after.held_bytes==before.held_bytes);
    xr_xir_compile_program_drop(program);
}
#endif /* MOVE_RUNTIME_H */
