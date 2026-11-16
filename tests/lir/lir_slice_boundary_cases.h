/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_slice_boundary_cases.h - Slotless formals and atomic receiving geometry
 */
#include "lir_slice_boundary_fixture.h"
static void slice_boundary_projection(SliceBoundaryFixture *c) {
    XrtBillingOwner *billing = NULL; MANAGED(xrt_billing_owner_new(NULL,SIZE_MAX,&billing));
    XrLirValueLayoutKey roots[] = {{c->slice,0,0},{XR_LIR_I64,0,0}};
    XrLirValueLayouts *set = NULL; HOME(xr_lir_value_layouts_create(c->module,roots,2,billing,&set));
    XrLirOwnershipPlan *plan = NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=c->module},&plan,NULL));
    XrLirFunction functions[] = {c->read,c->fanout,c->relay};
    for (size_t f = 0; f < 3; ++f) {
        XrLirValueHomesRequest request = {c->module,functions[f],set,plan}; XrLirValueHomes *homes = NULL;
        attempts = 0; HOME(xr_lir_value_homes_create(&request,&homes)); size_t sites = attempts;
        CHECK(homes); uint32_t formal = f == 2 ? 2u : 1u;
        const XrLirValueHome *borrow = &homes->values[formal];
        CHECK(borrow->view_storage == XR_LIR_SLICE_STORAGE_BORROWED_FORMAL &&
            borrow->view_parameter == formal - 1 && !borrow->view_workspace && !borrow->owner_count &&
            borrow->offset == SIZE_MAX && borrow->temporary_offset == SIZE_MAX && borrow->first_owner == SIZE_MAX);
        bool cleanup = false, returned = false;
        for (size_t e = 0; e < homes->event_count; ++e) {
            const XrLirHomeEvent *event = &homes->events[e];
            if (event->view_operation == XR_LIR_SLICE_OP_FORMAL_CLEANUP) {
                CHECK(event->view_storage == XR_LIR_SLICE_STORAGE_BORROWED_FORMAL && !event->view_workspace &&
                    !event->transfer_mode && !event->hold_transfer_mode && event->source == formal); cleanup = true;
            }
            if (event->view_operation == XR_LIR_SLICE_OP_RETURN) {
                CHECK(event->kind == XR_LIR_OWNERSHIP_RETURN_TRANSFER && event->view_parameter == 1 &&
                    event->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_BORROW &&
                    event->hold_transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_COPY && event->view_workspace); returned = true;
            }
        }
        CHECK(cleanup);
        if (!f) CHECK(!homes->owned_frame_needed && !homes->frame.slot_count && !homes->frame.payload_size &&
            !homes->workspace_count && homes->view_result_parameter == UINT32_MAX);
        else {
            CHECK(homes->call_count == 1 && homes->calls[0].view_result_workspace &&
                homes->calls[0].view_result_source_argument == formal &&
                homes->calls[0].view_result_parameter == (f == 1 ? 1u : 0u));
            if (f == 2) CHECK(returned && homes->view_result_parameter == 1);
        }
        if (f == 1) {
            CHECK(homes->transfer_count == 2 && homes->frame.slot_count == 8);
            for (size_t t = 0; t < homes->transfer_count; ++t) {
                const XrLirHomeTransfer *transfer = &homes->transfers[t];
                CHECK(transfer->source == formal && !transfer->source_workspace && transfer->target_workspace &&
                    transfer->source_storage == XR_LIR_SLICE_STORAGE_BORROWED_FORMAL &&
                    transfer->transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_BORROW &&
                    transfer->hold_transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_COPY &&
                    transfer->borrowed_owner_workspace < homes->workspace_count);
                const XrLirHomeWorkspace *stage = &homes->workspaces[transfer->borrowed_owner_workspace];
                const XrLirHomeWorkspace *target = &homes->workspaces[transfer->target_workspace - 1];
                CHECK(stage->kind == XR_LIR_HOME_WORKSPACE_VIEW_BORROW_STAGE && stage->value == formal &&
                    stage->first_owner != target->first_owner && stage->first_owner != target->temporary_first_owner &&
                    stage->temporary_offset == SIZE_MAX && stage->temporary_first_owner == SIZE_MAX &&
                    homes->frame.owned_offsets[stage->first_owner] == stage->offset);
            }
            CHECK(homes->transfers[0].borrowed_owner_workspace != homes->transfers[1].borrowed_owner_workspace);
        }
        xr_lir_value_homes_destroy(homes); homes = NULL; CHECK(!live);
        for (size_t n = 1; n <= sites; ++n) {
            attempts = 0; fail_at = n;
            CHECK(xr_lir_value_homes_create(&request,&homes) == XR_LIR_VALUE_LAYOUT_OOM && !homes && !live);
        }
        fail_at = SIZE_MAX;
    }
    xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(set);
    CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
}
static void slice_boundary_rejections(XrLirType element) {
    SliceBoundaryFixture c = slice_boundary_fixture(element,true); XrLirDiagnostic diagnostic = {0};
    CHECK(xr_lir_module_finish(c.module,&diagnostic) == XR_LIR_VERIFY_ERROR &&
        strcmp(diagnostic.message,"Slice use is outside its live loan") == 0);
    xr_lir_module_destroy(c.module);
    c = slice_boundary_fixture(element,false); OK(xr_lir_module_finish(c.module,NULL));
    XrLirInstructionView *cleanup = &c.module->functions[c.leaf.id - 1].blocks[0].instructions[1].view;
    CHECK(cleanup->op == XR_LIR_SLICE_FORMAL_CLEANUP);
    cleanup->op = XR_LIR_SLICE_CLEANUP; CHECK(xr_lir_verify(c.module,NULL) == XR_LIR_VERIFY_ERROR);
    cleanup->op = XR_LIR_SLICE_FORMAL_CLEANUP; cleanup->edge_count = 1;
    CHECK(xr_lir_verify(c.module,NULL) == XR_LIR_VERIFY_ERROR); cleanup->edge_count = 0;
    uint64_t target = cleanup->bits; cleanup->bits = 2;
    CHECK(xr_lir_verify(c.module,NULL) == XR_LIR_VERIFY_ERROR); cleanup->bits = target;
    XrLirSliceFactsView view; OK(xr_lir_slice_facts_view(c.module->slice_facts,&view));
    XrLirSliceFunctionFact *row = &((XrLirSliceFunctionFact *)view.functions)[c.leaf.id];
    row->available = false; CHECK(xr_lir_verify(c.module,NULL) == XR_LIR_UNSUPPORTED); row->available = true;
    size_t count = row->result_origin.count; row->result_origin.count = SIZE_MAX;
    CHECK(xr_lir_verify(c.module,NULL) == XR_LIR_VERIFY_ERROR); row->result_origin.count = count;
    OK(xr_lir_verify(c.module,NULL)); xr_lir_module_destroy(c.module);
}
static void slice_boundary_write(XrLirType element, uint32_t slot, bool by_parameter, bool available) {
    CArrayFixture c = {.element=element}; OK(xr_lir_module_create(NULL,&c.module));
    OK(xr_lir_slice_catalog_install(c.module)); XrLirType slice = 0; OK(xr_lir_slice_type(c.module,element,&slice));
    XrLirStateModule module = {0}; XrLirStateSlot slots[] = {
        {.payload=XR_LIR_ARRAY,.array_element=element,.mutable=true,.size=8,.alignment=8},
        {.payload=XR_LIR_ARRAY,.array_element=element,.mutable=true,.size=8,.alignment=8,.offset=8}};
    OK(xr_lir_set_state_layout(c.module,&(XrLirStateLayout){.modules=&module,.module_count=1,.slots=slots,
        .slot_count=2,.payload_size=16,.payload_alignment=8}));
    XrLirFunction writer = {0}, root = {0}; XrLirType parameter = by_parameter ? XR_LIR_ARRAY_REF : XR_LIR_STATE;
    XrLirFunctionSpec spec = {.name="write",.name_length=5,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module,&spec,&writer)); OK(xr_lir_define_function(c.module,writer));
    XrLirValue input = {0}; OK(xr_lir_function_parameter(c.module,writer,0,&input));
    XrLirBlock block = {0}; OK(xr_lir_add_block(c.module,writer,NULL,0,&block)); OK(xr_lir_set_entry(c.module,writer,block));
    XrLirValue binding = input;
    if (by_parameter) OK(xr_lir_set_array_shape(c.module,input,&(XrLirArrayShape){element}));
    else {
        XrLirInstruction ref = {.op=XR_LIR_STATE_REFERENCE,.type=XR_LIR_STATE_REF,.bits=slot,.operands=&input,.operand_count=1};
        XrLirValue reference = {0}; OK(xr_lir_emit_value(c.module,block,&ref,&reference));
        binding = ca_value(&c,block,XR_LIR_ARRAY_BORROW,XR_LIR_ARRAY_REF,&reference,1);
    }
    slice_write(&c,block,binding,false); ca_return(&c,block,ca_constant(&c,block,99));
    parameter = XR_LIR_STATE; spec.name = "root"; spec.name_length = 4;
    OK(xr_lir_declare_function(c.module,&spec,&root)); OK(xr_lir_set_state_entry(c.module,root));
    OK(xr_lir_define_function(c.module,root)); XrLirValue state = {0};
    OK(xr_lir_function_parameter(c.module,root,0,&state)); block = (XrLirBlock){0};
    OK(xr_lir_add_block(c.module,root,NULL,0,&block)); OK(xr_lir_set_entry(c.module,root,block));
    XrLirInstruction ref = {.op=XR_LIR_STATE_REFERENCE,.type=XR_LIR_STATE_REF,.operands=&state,.operand_count=1};
    XrLirValue reference = {0}; OK(xr_lir_emit_value(c.module,block,&ref,&reference));
    binding = ca_value(&c,block,XR_LIR_ARRAY_BORROW,XR_LIR_ARRAY_REF,&reference,1);
    XrLirValue args[] = {binding,ca_constant(&c,block,0),ca_constant(&c,block,1)};
    XrLirValue value = slice_role(&c,block,XR_LIR_SLICE_OP_NEW,slice,args,3), actual = state;
    if (by_parameter) {
        ref.bits = slot; reference = (XrLirValue){0}; OK(xr_lir_emit_value(c.module,block,&ref,&reference));
        actual = ca_value(&c,block,XR_LIR_ARRAY_BORROW,XR_LIR_ARRAY_REF,&reference,1);
    }
    XrLirInstruction call = {.op=XR_LIR_CALL,.type=XR_LIR_I64,.callee=writer,.operands=&actual,.operand_count=1};
    XrLirValue ignored = {0}; OK(xr_lir_emit_value(c.module,block,&call,&ignored));
    (void)slice_role(&c,block,XR_LIR_SLICE_OP_LEN,XR_LIR_I64,&value,1);
    (void)slice_role(&c,block,XR_LIR_SLICE_OP_END,XR_LIR_UNIT,&value,1);
    ca_return(&c,block,ca_constant(&c,block,42));
    XrLirSliceFunctionFact rows[3] = {{0}};
    rows[writer.id] = (XrLirSliceFunctionFact){.function=writer.id,.parameter_count=1,
        .result_parameter=UINT32_MAX,.available=available,.write={0,1}};
    rows[root.id] = (XrLirSliceFunctionFact){.function=root.id,.parameter_count=1,
        .result_parameter=UINT32_MAX,.available=true,.write={1,1}};
    XrLirSliceEffect terms[] = {{by_parameter ? XR_LIR_SLICE_EFFECT_PARAMETER : XR_LIR_SLICE_EFFECT_SLOT,
        by_parameter ? 0u : slot},{XR_LIR_SLICE_EFFECT_SLOT,slot}};
    OK(xr_lir_internal_slice_effects_set(c.module,(XrLirSliceFactsView){.functions=rows,.function_count=2,.terms=terms,.term_count=2}));
    XrLirDiagnostic diagnostic = {0}; XrLirStatus status = xr_lir_module_finish(c.module,&diagnostic);
    if (available && slot) OK(status);
    else CHECK(status == XR_LIR_VERIFY_ERROR && strcmp(diagnostic.message,
        "Array binding write conflicts with a live readonly Slice loan") == 0);
    xr_lir_module_destroy(c.module);
}
static void slice_boundaries(void) {
    for (unsigned family = 0; family < 2; ++family) {
        XrLirType element = family ? XR_LIR_U8 : XR_LIR_I64;
        SliceBoundaryFixture c = slice_boundary_fixture(element,false); OK(xr_lir_module_finish(c.module,NULL));
        slice_boundary_projection(&c);
        XrLirModule *copy = NULL; OK(xr_lir_rebuild(c.module,&copy)); xr_lir_module_destroy(c.module); c.module = copy;
        copy = NULL; OK(xr_lir_inline(c.module,&copy)); xr_lir_module_destroy(c.module); c.module = copy;
        /* Both actual CALLs keep their independent formal death boundary. */
        size_t calls = 0;
        for (uint32_t f = 0; f < c.module->function_count; ++f)
            for (uint32_t b = 0; b < c.module->functions[f].block_count; ++b)
                for (uint32_t i = 0; i < c.module->functions[f].blocks[b].instruction_count; ++i)
                    calls += c.module->functions[f].blocks[b].instructions[i].view.op == XR_LIR_CALL;
        CHECK(calls == 2); OK(xr_lir_verify(c.module,NULL)); xr_lir_module_destroy(c.module);
        slice_boundary_rejections(element);
        for (unsigned by_parameter = 0; by_parameter < 2; ++by_parameter) {
            slice_boundary_write(element,0,by_parameter != 0,true);
            slice_boundary_write(element,1,by_parameter != 0,true);
        }
        slice_boundary_write(element,1,false,false);
    }
}
