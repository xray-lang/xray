/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "old_headers/xray_native_state.h"
#include "old_headers/xray_native_boxed.h"
#include "old_producer.h"
#include <stdint.h>
#include <string.h>
/* No inclusion of the new metadata header is allowed in this producer TU. */
static const XrtCallableTypeSpec old_types[] = {
    {.kind=XRT_CALLABLE_SIGNATURE, .result={XRT_CALLABLE_SCALAR,XR_POD_I64}},
    {.kind=XRT_CALLABLE_SIGNATURE, .result={XRT_CALLABLE_SCALAR,XR_POD_I64}}
};
static const XrtCallableEntrySpec old_entries[] = {{0,NULL,0}};
static const XrtCallableImage old_image = {old_types,2,old_entries,1};
static const XrtCallableImage poison_image = {(const XrtCallableTypeSpec *)(uintptr_t)4096,2,
    (const XrtCallableEntrySpec *)(uintptr_t)4096,1};
static const XrtBoxedImage old_boxed = {old_types,2};
static const XrtBoxedImage poison_boxed = {(const XrtCallableTypeSpec *)(uintptr_t)4096,2};
const void *old_metadata_image(bool poison) { return poison?&poison_image:&old_image; }
void old_metadata_invoke(uint32_t action, bool poison, void *state) {
    const XrtCallableImage *image=poison?&poison_image:&old_image;
    const XrtBoxedImage *boxed=poison?&poison_boxed:&old_boxed;
    XrtCallableScope scope;
    const XrtCallable *invalid_function=(const XrtCallable *)(uintptr_t)4096;
    const XrtCapturedCell *invalid_cell=(const XrtCapturedCell *)(uintptr_t)4096;
    switch(action) {
    case OLD_SCOPE_ENTER:xrt_callable_scope_enter(&scope,image,0);break;
    case OLD_CAPTURE:(void)xrt_callable_capture(image,invalid_function,0,0,0);break;
    case OLD_ENTRY:(void)xrt_callable_entry(image,invalid_function,0);break;
    case OLD_CELL_READ:(void)xrt_callable_cell_i64_read(image,invalid_cell,0);break;
    case OLD_CELL_WRITE:xrt_callable_cell_i64_write(image,(XrtCapturedCell *)invalid_cell,1,0);break;
    case OLD_CALLABLE_STATE:xrt_callable_state_prepare(state,image,0);break;
    case OLD_BOXED_NEW:(void)xrt_boxed_new(boxed,0,NULL,0,0);break;
    case OLD_BOXED_STATE:xrt_boxed_state_prepare(state,boxed,0);break;
    default:break;
    }
}
uint32_t old_metadata_layout(bool poison, bool *output_untouched) {
    static const XrtModuleSpec module={0,0};
    XrtModuleLayoutSpec spec={.modules=&module,.module_count=1,.payload_alignment=1,
        .byte_limit=UINT64_MAX,.callable_types=old_types,.callable_type_count=2};
    if(poison) {
        spec.types=(const XrtModuleTypeSpec *)(uintptr_t)4096;spec.type_count=1;
        spec.modules=(const XrtModuleSpec *)(uintptr_t)4096;
        spec.callable_types=(const XrtCallableTypeSpec *)(uintptr_t)4096;
    }
    XrtModuleLayout *layout=NULL;
    uint32_t status=xrt_module_layout_new(&spec,&layout);
    *output_untouched=layout==NULL;
    if(layout)xrt_module_layout_drop(&layout);
    return status;
}

void old_metadata_scope_invoke(uint32_t action, bool poison, void *opaque) {
    XrtCallableScope *scope=opaque;
    scope->image=poison?&poison_image:&old_image;
    XrtCallable *invalid=(XrtCallable *)(uintptr_t)4096;
    switch(action) {
    case OLD_SCOPE_NEW:(void)xrt_callable_scope_new(scope,0,NULL,0,0);break;
    case OLD_SCOPE_COPY:(void)xrt_callable_scope_copy(scope,invalid,0);break;
    case OLD_SCOPE_BIND:(void)xrt_callable_scope_bind(scope,0,NULL,0);break;
    case OLD_SCOPE_LEAVE:(void)xrt_callable_scope_leave(scope,NULL,0);break;
    case OLD_CELL_NEW:(void)xrt_callable_cell_scope_new(scope,0,0,0);break;
    case OLD_CELL_MAP_NEW:(void)xrt_callable_cell_scope_map_new(scope,0,NULL,0);break;
    case OLD_CELL_BIND:(void)xrt_callable_cell_scope_bind(scope,0,(XrtCapturedCell *)invalid,0);break;
    default:break;
    }
}

uint32_t old_metadata_view(const void *layout, bool *output_untouched) {
    struct { XrtModuleLayoutSpec spec; unsigned char canary[64]; } output;
    memset(&output,0,sizeof(output));memset(output.canary,0x5a,sizeof(output.canary));
    unsigned char before[sizeof(output)];memcpy(before,&output,sizeof(output));
    uint32_t status=xrt_module_layout_view(layout,&output.spec);
    *output_untouched=memcmp(before,&output,sizeof(output))==0;
    return status;
}
