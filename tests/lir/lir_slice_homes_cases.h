/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_slice_homes_cases.h - Live borrowed windows and separate keeper publication
 */
static XrLirValue slice_role(CArrayFixture *c, XrLirBlock block, XrLirSliceOperation role,
    XrLirType result, const XrLirValue *args, uint32_t count) {
    uint32_t id = 0;
    OK(xr_lir_slice_operation(c->module, role, c->element,
        role == XR_LIR_SLICE_OP_NEW ? XR_LIR_ARRAY_CELL : 0, &id));
    XrLirInstruction op = {.op=XR_LIR_CALL_INTRINSIC,.type=result,.intrinsic=id,.operands=args,.operand_count=count};
    XrLirValue value = {0};
    if (result == XR_LIR_UNIT) OK(xr_lir_emit_effect(c->module, block, &op));
    else OK(xr_lir_emit_value(c->module, block, &op, &value));
    return value;
}
static XrLirModule *slice_fixture(XrLirType element, bool stale) {
    CArrayFixture c = {.element=element}; OK(xr_lir_module_create(NULL, &c.module));
    OK(xr_lir_slice_catalog_install(c.module)); XrLirType slice = 0;
    OK(xr_lir_slice_type(c.module, element, &slice));
    XrLirType parameter = XR_LIR_ARRAY_CELL; XrLirFunction fn = {0};
    XrLirFunctionSpec signature = {.name="slice",.name_length=5,.parameters=&parameter,.parameter_count=1,.result=XR_LIR_I64};
    OK(xr_lir_declare_function(c.module, &signature, &fn)); OK(xr_lir_define_function(c.module, fn));
    XrLirValue binding = {0}; OK(xr_lir_function_parameter(c.module, fn, 0, &binding));
    OK(xr_lir_set_array_shape(c.module, binding, &(XrLirArrayShape){element}));
    XrLirBlock entry = {0}, join = {0}; XrLirType parameters[] = {slice, slice};
    OK(xr_lir_create_block(c.module, fn, NULL, 0, &entry)); OK(xr_lir_set_entry(c.module, fn, entry));
    OK(xr_lir_create_block(c.module, fn, parameters, 2, &join));
    XrLirValue args[] = {binding, ca_constant(&c, entry, 0), ca_constant(&c, entry, 3)};
    XrLirValue original = slice_role(&c, entry, XR_LIR_SLICE_OP_NEW, slice, args, 3), copies[] = {original, original};
    XrLirInstruction branch = {.op=XR_LIR_BR,.type=XR_LIR_UNIT,.edge_count=1,.edges={{join,copies,2}}};
    OK(xr_lir_set_terminator(c.module, entry, &branch));
    XrLirValue left = {0}, right = {0}; OK(xr_lir_block_parameter(c.module, join, 0, &left));
    OK(xr_lir_block_parameter(c.module, join, 1, &right));
    (void)slice_role(&c, join, XR_LIR_SLICE_OP_LEN, XR_LIR_I64, &original, 1);
    (void)slice_role(&c, join, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &original, 1);
    XrLirValue bound = slice_role(&c, join, XR_LIR_SLICE_OP_BIND, slice, &left, 1);
    XrLirValue length = slice_role(&c, join, XR_LIR_SLICE_OP_LEN, XR_LIR_I64, stale ? &original : &bound, 1);
    (void)slice_role(&c, join, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &left, 1);
    (void)slice_role(&c, join, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &right, 1);
    (void)slice_role(&c, join, XR_LIR_SLICE_OP_END, XR_LIR_UNIT, &bound, 1);
    ca_return(&c, join, length);
    return c.module;
}
static void slice_homes(void) {
    const XrLirType elements[] = {XR_LIR_I64, XR_LIR_U8};
    for (size_t family = 0; family < 2; ++family) {
        XrLirModule *module = slice_fixture(elements[family], false); OK(xr_lir_module_finish(module, NULL));
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
        module = slice_fixture(elements[family], true); XrLirDiagnostic diagnostic = {0};
        CHECK(xr_lir_module_finish(module, &diagnostic) == XR_LIR_VERIFY_ERROR &&
            strcmp(diagnostic.message, "Slice use is outside its live loan") == 0);
        xr_lir_module_destroy(module);
    }
}
