/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "xray_native_boxed.h"
#include "xray_native_state.h"
#include "runtime/native/xnative_callable.h"
#include "runtime/native/xnative_execution.h"
#include "runtime/native/xnative_module_state.h"
#include "old_producer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);} }while(0)

typedef struct MetadataContext {
    XrtValueRoot root;
    XrtExecutionFailure failure;
    XrtModuleState *state;
    const void *image;
    uint32_t action;
    bool old, poison, reached, scope_action;
} MetadataContext;
static void rejected_root(void *opaque) {
    MetadataContext *context=opaque;
    if(context->scope_action) {
        static const XrtCallableImage current={.metadata=XRT_CALLABLE_IMAGE_HEADER};
        XrtCallableScope scope;
        xrt_callable_scope_enter(&scope,&current,0);
        old_metadata_scope_invoke(context->action,context->poison,&scope);
    } else if(context->old) old_metadata_invoke(context->action,context->poison,context->state);
    else if(context->action==OLD_BOXED_NEW)
        (void)xrt_boxed_new(context->image,0,NULL,0,0);
    else if(context->action==OLD_BOXED_STATE)
        xrt_boxed_state_prepare(context->state,context->image,0);
    else if(context->action==OLD_CALLABLE_STATE)
        xrt_callable_state_prepare(context->state,context->image,0);
    else {
        XrtCallableScope scope;
        xrt_callable_scope_enter(&scope,context->image,0);
    }
    context->reached=true;
}
static void require_rejected(XrtLocationOwner *locations,MetadataContext *context) {
    context->reached=false;
    CHECK(xrt_run_embedded_owned(locations,&context->failure,rejected_root,context,&context->root)==XRT_EXECUTION_FAILURE);
    CHECK(!context->reached&&context->failure.kind==XRT_FAILURE_VALUE&&
        context->failure.detail.value.status==XR_XIR_VALUE_BAD_ARGUMENT&&xrt_value_root_empty(&context->root));
    xrt_execution_failure_drop(&context->failure);
}
static XrtModuleState *empty_state(void) {
    XrtModuleSpec module={0};
    XrtModuleLayoutSpec spec={.metadata=XRT_MODULE_LAYOUT_HEADER,.modules=&module,.module_count=1,
        .payload_alignment=1,.byte_limit=UINT64_MAX};
    XrtModuleLayout *layout=NULL;XrtModuleState *state=NULL;
    CHECK(xrt_module_layout_new(&spec,&layout)==XR_XIR_VALUE_OK);
    CHECK(xrt_module_state_new(layout,UINT64_MAX,&state)==XR_XIR_VALUE_OK);
    xrt_module_layout_drop(&layout);return state;
}
static void legacy_rejected(XrtLocationOwner *locations) {
    MetadataContext context={.old=true,.state=empty_state()};
    for(unsigned poison=0;poison<2;++poison) {
        context.poison=poison!=0;
        for(context.action=0;context.action<OLD_ACTION_COUNT;++context.action)
            require_rejected(locations,&context);
        context.scope_action=true;
        for(context.action=0;context.action<OLD_SCOPE_ACTION_COUNT;++context.action)
            require_rejected(locations,&context);
        context.scope_action=false;
        const XrtCallableImage *old=old_metadata_image(context.poison);
        XrXirTypeArena *arena=NULL;uint64_t bytes=17;
        CHECK(xrt_callable_types_new_accounted(old,UINT64_MAX,&arena,&bytes)==XR_XIR_VALUE_BAD_ARGUMENT&&!arena&&bytes==17);
        CHECK(xrt_callable_types_new(old,UINT64_MAX,&arena)==XR_XIR_VALUE_BAD_ARGUMENT&&!arena);
        CHECK(!xrt_callable_specs_equal(old,old));
        CHECK(!xrt_module_state_callables(context.state,old));
        bool untouched=false;
        CHECK(old_metadata_layout(context.poison,&untouched)==XR_XIR_VALUE_BAD_ARGUMENT&&untouched);
    }
    xrt_module_state_drop(&context.state);
    puts("pre-cutover producer TU rejected before real48-byte or unreadable rows");
}
static void corrupt(XrtNativeMetadataHeader *header,unsigned field) {
    switch(field) {
    case 0:header->magic^=1u;break;
    case 1:++header->schema;break;
    case 2:++header->kind;break;
    case 3:--header->descriptor_size;break;
    case 4:--header->type_size;break;
    case 5:--header->parameter_size;break;
    case 6:--header->entry_size;break;
    case 7:header->reserved=1;break;
    case 8:--header->prefix_size;break;
    default:header->schema=0;break;
    }
}
static void malformed_rejected(XrtLocationOwner *locations) {
    MetadataContext context={.state=empty_state()};
    for(unsigned field=0;field<10;++field) {
        XrtCallableImage callable={.metadata=XRT_CALLABLE_IMAGE_HEADER,
            .types=(const XrtCallableTypeSpec *)(uintptr_t)4096,.type_count=1,
            .entries=(const XrtCallableEntrySpec *)(uintptr_t)4096,.entry_count=1};
        corrupt(&callable.metadata,field);context.image=&callable;
        context.action=OLD_SCOPE_ENTER;require_rejected(locations,&context);
        context.action=OLD_CALLABLE_STATE;require_rejected(locations,&context);
        XrXirTypeArena *arena=NULL;uint64_t bytes=17;
        CHECK(xrt_callable_types_new_accounted(&callable,UINT64_MAX,&arena,&bytes)==XR_XIR_VALUE_BAD_ARGUMENT&&!arena&&bytes==17);
        CHECK(!xrt_callable_specs_equal(&callable,&callable));
        XrtBoxedImage boxed={.metadata=XRT_BOXED_IMAGE_HEADER,
            .types=(const XrtCallableTypeSpec *)(uintptr_t)4096,.count=1};
        corrupt(&boxed.metadata,field);context.image=&boxed;
        context.action=OLD_BOXED_NEW;require_rejected(locations,&context);
        context.action=OLD_BOXED_STATE;require_rejected(locations,&context);
        XrtModuleLayoutSpec spec={.metadata=XRT_MODULE_LAYOUT_HEADER,
            .types=(const XrtModuleTypeSpec *)(uintptr_t)4096,.type_count=1,
            .modules=(const XrtModuleSpec *)(uintptr_t)4096,.module_count=1,
            .callable_types=(const XrtCallableTypeSpec *)(uintptr_t)4096,.callable_type_count=1};
        corrupt(&spec.metadata,field);XrtModuleLayout *layout=NULL;
        CHECK(xrt_module_layout_new(&spec,&layout)==XR_XIR_VALUE_BAD_ARGUMENT&&!layout);
    }
    /* A wrong first word needs only eight readable bytes. ASAN guards its tail. */
    uint64_t *short_old=malloc(sizeof(*short_old));CHECK(short_old);*short_old=0;
    context.image=short_old;context.action=OLD_SCOPE_ENTER;require_rejected(locations,&context);
    context.action=OLD_BOXED_NEW;require_rejected(locations,&context);
    XrtModuleLayout *layout=NULL;
    CHECK(xrt_module_layout_new((const XrtModuleLayoutSpec *)short_old,&layout)==XR_XIR_VALUE_BAD_ARGUMENT&&!layout);
    memset(short_old,0xa5,sizeof(*short_old));free(short_old);
    xrt_module_state_drop(&context.state);
    puts("exact schema/kind/size/reserved and eight-byte refusal precede row reads");
}
static void current_root(void *opaque) {
    MetadataContext *context=opaque;
    static const XrtCallableTypeSpec type={.kind=XRT_CALLABLE_SIGNATURE,.result={XRT_CALLABLE_SCALAR,XR_POD_I64}};
    static const XrtCallableEntrySpec entry={0,NULL,0};
    static const XrtCallableImage image={.metadata=XRT_CALLABLE_IMAGE_HEADER,.types=&type,.type_count=1,
        .entries=&entry,.entry_count=1};
    XrtCallableScope scope;
    xrt_callable_scope_enter(&scope,&image,0);
    XrtCallable *function=xrt_callable_scope_new(&scope,0,NULL,0,0);
    CHECK(xrt_callable_entry(&image,function,0)==0);
    CHECK(!xrt_callable_scope_leave(&scope,NULL,0));
    static const XrtCallableParameterSpec field={.value={XRT_CALLABLE_SCALAR,XR_POD_I64},.name="score",.name_length=5};
    static const XrtCallableTypeSpec record={.kind=XRT_CALLABLE_RECORD,.count=1,.parameters=&field,
        .parameter_count=1,.module="test",.module_length=4,.name="Score",.name_length=5};
    static const XrtBoxedImage boxed={.metadata=XRT_BOXED_IMAGE_HEADER,.types=&record,.count=1};
    uint64_t value=42;XrtBoxed *box=xrt_boxed_new(&boxed,0,&value,1,0);
    CHECK(xrt_boxed_field(box,0,0)==42);xrt_boxed_drop(box,0);
    context->reached=true;
}
static void current_receiving(void) {
    char *text=malloc(14);CHECK(text);memcpy(text,"testScorevalue",14);
    XrtCallableParameterSpec field={.value={XRT_CALLABLE_SCALAR,XR_POD_I64},.name=text+9,.name_length=5};
    XrtCallableTypeSpec type={.kind=XRT_CALLABLE_RECORD,.count=1,.parameters=&field,.parameter_count=1,
        .module=text,.module_length=4,.name=text+4,.name_length=5};
    XrtModuleSpec module={0};
    XrtModuleLayoutSpec spec={.metadata=XRT_MODULE_LAYOUT_HEADER,.modules=&module,.module_count=1,
        .payload_alignment=1,.byte_limit=UINT64_MAX,.callable_types=&type,.callable_type_count=1};
    XrtModuleLayout *layout=NULL;CHECK(xrt_module_layout_new(&spec,&layout)==XR_XIR_VALUE_OK);
    bool untouched=false;
    CHECK(old_metadata_view(layout,&untouched)==XR_XIR_VALUE_BAD_ARGUMENT&&untouched);
    for(unsigned field_index=0;field_index<10;++field_index) {
        XrtModuleLayoutSpec output={.metadata=XRT_MODULE_LAYOUT_HEADER};
        corrupt(&output.metadata,field_index);
        unsigned char before[sizeof(output)];memcpy(before,&output,sizeof(output));
        CHECK(xrt_module_layout_view(layout,&output)==XR_XIR_VALUE_BAD_ARGUMENT&&!memcmp(before,&output,sizeof(output)));
    }
    uint64_t *short_output=malloc(sizeof(*short_output));CHECK(short_output);*short_output=0;
    CHECK(xrt_module_layout_view(layout,(XrtModuleLayoutSpec *)short_output)==XR_XIR_VALUE_BAD_ARGUMENT&&*short_output==0);
    free(short_output);
    memset(text,0xa5,14);free(text);memset(&type,0xa5,sizeof(type));memset(&field,0xa5,sizeof(field));
    XrtModuleLayoutSpec view={.metadata=XRT_MODULE_LAYOUT_HEADER};CHECK(xrt_module_layout_view(layout,&view)==XR_XIR_VALUE_OK);
    CHECK(view.metadata.schema==XRT_NATIVE_METADATA_SCHEMA&&view.callable_type_count==1);
    CHECK(view.callable_types[0].module_length==4&&!memcmp(view.callable_types[0].module,"test",4));
    CHECK(!memcmp(view.callable_types[0].name,"Score",5));
    CHECK(!memcmp(view.callable_types[0].parameters[0].name,"value",5));
    xrt_module_layout_drop(&layout);CHECK(!layout);
}
int main(void) {
    XrtLocationOwner *locations=NULL;CHECK(xrt_locations_create(NULL,0,&locations)==XRT_LOCATION_OK);
    legacy_rejected(locations);malformed_rejected(locations);current_receiving();
    MetadataContext context={0};
    CHECK(xrt_run_embedded_owned(locations,&context.failure,current_root,&context,&context.root)==XRT_EXECUTION_COMPLETED);
    CHECK(context.reached&&xrt_value_root_empty(&context.root));
    xrt_locations_release(locations);puts("current metadata ownership and native calls pass");return 0;
}
