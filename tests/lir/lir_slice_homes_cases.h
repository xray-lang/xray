/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_slice_homes_cases.h - Live borrowed windows and separate keeper publication
 */
#include "lir/xlir_internal.h"
#include "lir/xlir_view.h"
static XrLirValue slice_role(CArrayFixture *c, XrLirBlock block, XrLirSliceOperation role,
    XrLirType result, const XrLirValue *args, uint32_t count) {
    uint32_t id = 0;
    XrLirType binding = 0;
    if (role == XR_LIR_SLICE_OP_NEW) binding = c->module->functions[args[0].function - 1].values[args[0].id - 1].type;
    OK(xr_lir_slice_operation(c->module, role, c->element, binding, &id));
    XrLirInstruction op = {.op=XR_LIR_CALL_INTRINSIC,.type=result,.intrinsic=id,.operands=args,.operand_count=count};
    XrLirValue value = {0};
    if (result == XR_LIR_UNIT) OK(xr_lir_emit_effect(c->module, block, &op));
    else OK(xr_lir_emit_value(c->module, block, &op, &value));
    return value;
}
static void slice_write(CArrayFixture *c, XrLirBlock block, XrLirValue binding, bool alias) {
    if (alias) binding = ca_value(c, block, XR_LIR_ARRAY_BORROW, XR_LIR_ARRAY_REF, &binding, 1);
    XrLirType type = c->module->functions[binding.function - 1].values[binding.id - 1].type; uint32_t id = 0;
    OK(xr_lir_array_mutator(c->module, c->element, type, XR_LIR_ARRAY_OP_SET, &id));
    XrLirValue args[] = {binding, ca_constant(c, block, 0), number(c, block, c->element, 99)};
    XrLirInstruction write = {.op=XR_LIR_CALL_INTRINSIC,.type=XR_LIR_UNIT,.intrinsic=id,.operands=args,.operand_count=3};
    OK(xr_lir_emit_effect(c->module, block, &write));
}
static XrLirModule *slice_fixture(XrLirType element, uint32_t invalid) {
    CArrayFixture c = {.element=element}; OK(xr_lir_module_create(NULL, &c.module));
    OK(xr_lir_slice_catalog_install(c.module)); XrLirType slice = 0;
    OK(xr_lir_slice_type(c.module, element, &slice));
    XrLirType parameter = XR_LIR_ARRAY_CELL; XrLirFunction fn = {0};
    XrLirFunctionSpec signature = {.name="slice",.name_length=5,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &signature, &fn)); OK(xr_lir_define_function(c.module, fn));
    XrLirValue binding = {0}; OK(xr_lir_function_parameter(c.module, fn, 0, &binding));
    OK(xr_lir_set_array_shape(c.module, binding, &(XrLirArrayShape){element}));
    XrLirBlock entry = {0}, join = {0}; XrLirType parameters[] = {slice, slice};
    OK(xr_lir_add_block(c.module, fn, NULL, 0, &entry)); OK(xr_lir_set_entry(c.module, fn, entry));
    OK(xr_lir_add_block(c.module, fn, parameters, 2, &join));
    XrLirValue args[] = {binding, ca_constant(&c, entry, 0), ca_constant(&c, entry, 3)};
    XrLirValue original = slice_role(&c, entry, XR_LIR_SLICE_OP_NEW, slice, args, 3), copies[] = {original, original};
    XrLirInstruction branch = {.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,.edges={{join,copies,2}}};
    OK(xr_lir_set_terminator(c.module, entry, &branch));
    XrLirValue left = {0}, right = {0}; OK(xr_lir_block_parameter(c.module, join, 0, &left));
    OK(xr_lir_block_parameter(c.module, join, 1, &right));
    (void)slice_role(&c, join, XR_LIR_SLICE_OP_LEN, XR_LIR_I64, &original, 1);
    (void)slice_role(&c, join, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &original, 1);
    XrLirValue bound = slice_role(&c, join, XR_LIR_SLICE_OP_BIND, slice, &left, 1);
    if (invalid == 2 || invalid == 3) slice_write(&c, join, binding, invalid == 3);
    if (invalid == 5) {
        XrLirInstruction cleanup = {.op=XR_LIR_SLICE_CLEANUP,.type=XR_LIR_UNIT,.bits=bound.id};
        OK(xr_lir_emit_effect(c.module, join, &cleanup));
    }
    XrLirValue length = slice_role(&c, join, XR_LIR_SLICE_OP_LEN, XR_LIR_I64, invalid == 1 ? &original : &bound, 1);
    if (invalid != 4) {
        (void)slice_role(&c, join, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &left, 1);
        (void)slice_role(&c, join, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &right, 1);
        (void)slice_role(&c, join, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &bound, 1);
    }
    ca_return(&c, join, length);
    return c.module;
}
static XrLirModule *slice_state_fixture(XrLirType element, uint32_t mode) {
    CArrayFixture c = {.element=element}; OK(xr_lir_module_create(NULL, &c.module)); OK(xr_lir_slice_catalog_install(c.module));
    XrLirType slice = 0; OK(xr_lir_slice_type(c.module, element, &slice));
    XrLirStateModule module = {0}; XrLirStateSlot slots[2] = {
        {.payload=XR_LIR_ARRAY,.array_element=element,.mutable=true,.size=8,.alignment=8},
        {.payload=XR_LIR_ARRAY,.array_element=element,.mutable=true,.offset=8,.size=8,.alignment=8}};
    XrLirStateLayout layout = {.modules=&module,.module_count=1,.slots=slots,.slot_count=2,.payload_size=16,.payload_alignment=8};
    OK(xr_lir_set_state_layout(c.module, &layout)); XrLirType parameter = XR_LIR_STATE; XrLirFunction fn = {0};
    XrLirFunctionSpec signature = {.name="state_slice",.name_length=11,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &signature, &fn)); OK(xr_lir_set_state_entry(c.module, fn));
    OK(xr_lir_define_function(c.module, fn)); XrLirValue state = {0}; OK(xr_lir_function_parameter(c.module, fn, 0, &state));
    XrLirBlock block = {0}; OK(xr_lir_add_block(c.module, fn, NULL, 0, &block)); OK(xr_lir_set_entry(c.module, fn, block));
    XrLirInstruction ref = {.op=XR_LIR_STATE_REFERENCE,.type=XR_LIR_STATE_REF,.operands=&state,.operand_count=1};
    XrLirValue reference = {0}; OK(xr_lir_emit_value(c.module, block, &ref, &reference));
    XrLirValue binding = ca_value(&c, block, XR_LIR_ARRAY_BORROW, XR_LIR_ARRAY_REF, &reference, 1);
    XrLirValue args[] = {binding, ca_constant(&c, block, 0), ca_constant(&c, block, 3)};
    XrLirValue value = slice_role(&c, block, XR_LIR_SLICE_OP_NEW, slice, args, 3);
    XrLirValue length = slice_role(&c, block, XR_LIR_SLICE_OP_LEN, XR_LIR_I64, &value, 1);
    if (mode == 2) (void)slice_role(&c, block, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &value, 1);
    ref.bits = mode == 0 ? 1 : 0; OK(xr_lir_emit_value(c.module, block, &ref, &reference));
    binding = ca_value(&c, block, XR_LIR_ARRAY_BORROW, XR_LIR_ARRAY_REF, &reference, 1);
    slice_write(&c, block, binding, false);
    if (mode != 2) (void)slice_role(&c, block, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &value, 1);
    ca_return(&c, block, length); return c.module;
}
static void slice_conditional_cleanup(XrLirType element, bool malformed) {
    CArrayFixture c = {.element=element}; OK(xr_lir_module_create(NULL, &c.module));
    OK(xr_lir_slice_catalog_install(c.module)); XrLirType slice = 0;
    OK(xr_lir_slice_type(c.module, element, &slice)); XrLirFunction fn = {0};
    XrLirType parameter = XR_LIR_ARRAY_CELL;
    XrLirFunctionSpec signature = {.name="cleanup",.name_length=7,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &signature, &fn)); OK(xr_lir_define_function(c.module, fn));
    XrLirValue binding = {0}; OK(xr_lir_function_parameter(c.module, fn, 0, &binding));
    OK(xr_lir_set_array_shape(c.module, binding, &(XrLirArrayShape){element}));
    XrLirBlock entry = {0}, loan = {0}, skip = {0}, join = {0};
    OK(xr_lir_add_block(c.module, fn, NULL, 0, &entry)); OK(xr_lir_set_entry(c.module, fn, entry));
    OK(xr_lir_add_block(c.module, fn, NULL, 0, &loan)); OK(xr_lir_add_block(c.module, fn, NULL, 0, &skip));
    OK(xr_lir_add_block(c.module, fn, NULL, 0, &join));
    XrLirInstruction truth = {.op=XR_LIR_CONST,.type=XR_LIR_I1,.bits=1}; XrLirValue condition = {0};
    OK(xr_lir_emit_value(c.module, entry, &truth, &condition));
    XrLirInstruction branch = {.op=XR_LIR_CONDBR,.type=XR_LIR_UNIT,.operands=&condition,.operand_count=1,
        .edge_count=2,.edges={{loan,NULL,0},{skip,NULL,0}}};
    OK(xr_lir_set_terminator(c.module, entry, &branch));
    XrLirValue args[] = {binding, ca_constant(&c, loan, 0), ca_constant(&c, loan, 3)};
    XrLirValue value = slice_role(&c, loan, XR_LIR_SLICE_OP_NEW, slice, args, 3);
    branch = (XrLirInstruction){.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,.edges={{join,NULL,0}}};
    OK(xr_lir_set_terminator(c.module, loan, &branch)); OK(xr_lir_set_terminator(c.module, skip, &branch));
    XrLirInstruction cleanup = {.op=XR_LIR_SLICE_CLEANUP,.type=XR_LIR_UNIT,.bits=value.id};
    if (malformed) {
        cleanup.edge_count = 1; cleanup.edges[0].target = join;
        CHECK(xr_lir_emit_effect(c.module, join, &cleanup) == XR_LIR_VERIFY_ERROR);
        xr_lir_module_destroy(c.module); return;
    }
    OK(xr_lir_emit_effect(c.module, join, &cleanup));
    slice_write(&c, join, binding, false); ca_return(&c, join, ca_constant(&c, join, 42));
    XrLirSliceCleanupInsertion insertions[] = {{.block=join,.ordinal=0,.target=value},
        {.block=loan,.ordinal=0,.target=value}};
    OK(xr_lir_internal_slice_cleanup_batch(c.module, fn, insertions, 2));
    OK(xr_lir_module_finish(c.module, NULL));
    XrLirInstructionView *sealed = &c.module->functions[fn.id - 1].blocks[join.id - 1].instructions[0].view;
    sealed->edge_count = 1; sealed->edges[0].target = join.id;
    CHECK(xr_lir_verify(c.module, NULL) == XR_LIR_VERIFY_ERROR);
    sealed->edge_count = 0; memset(sealed->edges, 0, sizeof(sealed->edges)); OK(xr_lir_verify(c.module, NULL));
    XrLirValueData *target = &c.module->functions[fn.id - 1].values[value.id - 1];
    uint32_t definition = target->instruction; target->instruction = UINT32_MAX;
    CHECK(xr_lir_verify(c.module, NULL) == XR_LIR_VERIFY_ERROR);
    target->instruction = definition; OK(xr_lir_verify(c.module, NULL));
    xr_lir_module_destroy(c.module);

}
static void slice_homes(void) {
    const XrLirType elements[] = {XR_LIR_I64, XR_LIR_U8};
    for (size_t family = 0; family < 2; ++family) {
        slice_conditional_cleanup(elements[family], false);
        slice_conditional_cleanup(elements[family], true);
        XrLirModule *module = slice_fixture(elements[family], 0); OK(xr_lir_module_finish(module, NULL));
        XrtBillingOwner *billing = NULL; MANAGED(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
        XrLirType slice = 0;
        for (uint32_t n = 0; n < xr_lir_constructed_type_count(module); ++n) {
            XrLirTypeView shape; OK(xr_lir_type_view(module, XR_LIR_TYPE_COUNT + n, &shape));
            if (shape.kind == XR_LIR_TYPE_SLICE && shape.element == elements[family]) slice = XR_LIR_TYPE_COUNT + n;
        }
        CHECK(slice); XrLirValueLayoutKey roots[] = {{XR_LIR_ARRAY,elements[family],0},{slice,0,0},{XR_LIR_I64,0,0}};
        XrLirValueLayouts *set = NULL; HOME(xr_lir_value_layouts_create(module, roots, 3, billing, &set));
        XrLirOwnershipPlan *plan = NULL; OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=module}, &plan, NULL));
        XrLirValueHomesRequest request = {module,{module,1},set,plan}; XrLirValueHomes *homes = NULL;
        attempts = 0; HOME(xr_lir_value_homes_create(&request, &homes)); size_t allocations = attempts;
        CHECK(homes && homes->workspace_count == 4 && homes->frame.slot_count == 8 && homes->transfer_count == 2);
        CHECK(homes->owned_frame_needed && !(homes->unmodeled & XR_LIR_OWNERSHIP_UNMODELED_CONSTRUCTED));
        for (size_t n = 0; n < homes->workspace_count; ++n) {
            XrLirHomeWorkspace workspace = homes->workspaces[n]; const XrLirValueHome *home = &homes->values[workspace.value];
            CHECK(workspace.kind == XR_LIR_HOME_WORKSPACE_VIEW_HOLD && home->view_workspace == n + 1);
            CHECK(home->size == sizeof(XrtReadonlyArraySlice) && !home->owner_count && home->first_owner == SIZE_MAX &&
                home->temporary_first_owner == SIZE_MAX && home->offset == workspace.offset && home->temporary_offset == workspace.temporary_offset);
            CHECK(homes->frame.owned_offsets[workspace.first_owner] == workspace.offset + offsetof(XrtArraySliceBundle,hold));
            CHECK(homes->frame.owned_offsets[workspace.temporary_first_owner] == workspace.temporary_offset + offsetof(XrtArraySliceBundle,hold));
        }
        CHECK(homes->transfers[0].source == homes->transfers[1].source && homes->transfers[0].target != homes->transfers[1].target);
        for (size_t n = 0; n < homes->transfer_count; ++n) CHECK(homes->transfers[n].transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_BORROW &&
            homes->transfers[n].hold_transfer_mode == XR_LIR_OWNERSHIP_TRANSFER_COPY && homes->transfers[n].source_workspace && homes->transfers[n].target_workspace);
        xr_lir_value_homes_destroy(homes); homes = NULL; CHECK(!live);
        for (size_t n = 1; n <= allocations; ++n) {
            attempts = 0; fail_at = n;
            CHECK(xr_lir_value_homes_create(&request, &homes) == XR_LIR_VALUE_LAYOUT_OOM && !homes && !live);
        }
        fail_at = SIZE_MAX; xr_lir_ownership_plan_destroy(plan); xr_lir_value_layouts_destroy(set);
        xr_lir_module_destroy(module); CHECK(xrt_billing_owner_stats(billing).blocks == 1); xrt_billing_owner_drop(billing);
        for (uint32_t invalid = 1; invalid <= 3; ++invalid) {
            module = slice_fixture(elements[family], invalid); XrLirDiagnostic diagnostic = {0};
            CHECK(xr_lir_module_finish(module, &diagnostic) == XR_LIR_VERIFY_ERROR && strcmp(diagnostic.message,
                invalid == 1 ? "Slice use is outside its live loan" : "Array binding write conflicts with a live readonly Slice loan") == 0);
            xr_lir_module_destroy(module);
        }
        for (uint32_t mode = 0; mode < 3; ++mode) {
            module = slice_state_fixture(elements[family], mode); XrLirDiagnostic diagnostic = {0};
            XrLirStatus status = xr_lir_module_finish(module, &diagnostic);
            if (mode == 1) CHECK(status == XR_LIR_VERIFY_ERROR && strcmp(diagnostic.message,
                "Array binding write conflicts with a live readonly Slice loan") == 0);
            else CHECK(status == XR_LIR_OK);
            xr_lir_module_destroy(module);
        }
        module = slice_fixture(elements[family], 4); OK(xr_lir_module_finish(module, NULL));
        xr_lir_module_destroy(module);
        module = slice_fixture(elements[family], 5); XrLirDiagnostic ended = {0};
        CHECK(xr_lir_module_finish(module, &ended) == XR_LIR_VERIFY_ERROR &&
            strcmp(ended.message, "Slice use is outside its live loan") == 0);
        xr_lir_module_destroy(module);
    }
}
