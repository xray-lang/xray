/* Type permission cases with complete enum projections and actual generic scopes. */
typedef struct MarkerTypes {
    XrXirNominalDeclaration declarations[6]; XrXirNominalTable table;
    XrXirNominalField safe_fields[2], generic_field, bad_field, phantom_field;
    XrXirNominalVariant empty_variant, safe_variants[2], bad_variants[2], generic_variant;
    XrXirConstraint bound, caller_bound; XrXirGeneric generic;
    XrXirType generic_argument, scalar_argument, bad_argument, safe_projection[2], generic_projection,
        scalar_projection, bad_projection, phantom_projection;
    XrXirCallableParameter tuple_fields[2];
    XrXirTypeNode nodes[12]; XrXirTypes types; XrXirFunction function; XrXirModule module;
} MarkerTypes;
static void marker_types(MarkerTypes *f) {
    *f = (MarkerTypes){0};
    f->safe_fields[0] = (XrXirNominalField){{"number", 6}, XR_XIR_I64, 0};
    f->safe_fields[1] = (XrXirNominalField){{"message", 7}, XR_XIR_STRING, 0};
    f->generic_field = (XrXirNominalField){{"value", 5}, (XrXirType)XR_XIR_TYPE_PARAMETER_BASE, 0};
    f->bad_field = (XrXirNominalField){{"hidden", 6}, (XrXirType)258, 0};
    f->phantom_field = (XrXirNominalField){{"value", 5}, XR_XIR_I64, 0};
    f->empty_variant = (XrXirNominalVariant){{"Empty", 5}, 0, 0};
    f->safe_variants[0] = (XrXirNominalVariant){{"Number", 6}, 0, 1};
    f->safe_variants[1] = (XrXirNominalVariant){{"Message", 7}, 1, 1};
    f->bad_variants[0] = (XrXirNominalVariant){{"Empty", 5}, 0, 0};
    f->bad_variants[1] = (XrXirNominalVariant){{"Unsafe", 6}, 0, 1};
    f->generic_variant = (XrXirNominalVariant){{"Value", 5}, 0, 1};
    f->bound.markers = XR_XIR_CONSTRAINT_SENDABLE; f->caller_bound = f->bound;
    f->generic = (XrXirGeneric){.constraints = &f->caller_bound, .parameter_count = 1};
    f->declarations[0] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"EmptyError", 10},
        .kind = XR_XIR_NOMINAL_ENUM, .variants = &f->empty_variant, .variant_count = 1};
    f->declarations[1] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"ScalarError", 11},
        .kind = XR_XIR_NOMINAL_ENUM, .variants = f->safe_variants, .variant_count = 2,
        .fields = f->safe_fields, .field_count = 2};
    f->declarations[2] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"LocalClass", 10},
        .kind = XR_XIR_NOMINAL_CLASS, .flags = XR_XIR_NOMINAL_FINAL};
    f->declarations[3] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"UnsafeError", 11},
        .kind = XR_XIR_NOMINAL_ENUM, .variants = f->bad_variants, .variant_count = 2,
        .fields = &f->bad_field, .field_count = 1};
    f->declarations[4] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"GenericError", 12},
        .kind = XR_XIR_NOMINAL_ENUM, .variants = &f->generic_variant, .variant_count = 1,
        .fields = &f->generic_field, .field_count = 1, .constraints = &f->bound, .parameter_count = 1};
    f->declarations[5] = (XrXirNominalDeclaration){.module = {"m", 1}, .name = {"PhantomError", 12},
        .kind = XR_XIR_NOMINAL_ENUM, .variants = &f->generic_variant, .variant_count = 1,
        .fields = &f->phantom_field, .field_count = 1, .constraints = &f->bound, .parameter_count = 1};
    f->table = (XrXirNominalTable){f->declarations, 6, NULL};
    f->safe_projection[0] = XR_XIR_I64; f->safe_projection[1] = XR_XIR_STRING;
    f->generic_argument = f->generic_projection = (XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    f->scalar_argument = f->scalar_projection = f->phantom_projection = XR_XIR_I64;
    f->bad_argument = f->bad_projection = (XrXirType)258;
    for (uint32_t i = 0; i < 4; ++i)
        f->nodes[i] = (XrXirTypeNode){.kind = XR_XIR_TYPE_NOMINAL, .nominal = {.declaration = i}};
    f->nodes[1].nominal.fields = f->safe_projection; f->nodes[1].nominal.field_count = 2;
    f->nodes[3].nominal.fields = &f->bad_projection; f->nodes[3].nominal.field_count = 1;
    f->nodes[4] = (XrXirTypeNode){.kind = XR_XIR_TYPE_NOMINAL, .parameter_span = 1,
        .nominal = {4, &f->generic_argument, 1, &f->generic_projection, 1}};
    f->nodes[5] = (XrXirTypeNode){.kind = XR_XIR_TYPE_NOMINAL,
        .nominal = {4, &f->scalar_argument, 1, &f->scalar_projection, 1}};
    f->tuple_fields[0] = (XrXirCallableParameter){XR_XIR_UNIT, 0};
    f->tuple_fields[1] = (XrXirCallableParameter){(XrXirType)257, 0};
    f->nodes[6] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TUPLE, .parameters = f->tuple_fields, .parameter_count = 2};
    f->nodes[7] = (XrXirTypeNode){.kind = XR_XIR_TYPE_ARRAY, .element = (XrXirType)262};
    f->nodes[8] = (XrXirTypeNode){.kind = XR_XIR_TYPE_TASK, .element = (XrXirType)263, .result = XR_XIR_NEVER};
    f->nodes[9] = (XrXirTypeNode){.kind = XR_XIR_TYPE_NOMINAL,
        .nominal = {5, &f->bad_argument, 1, &f->phantom_projection, 1}};
    f->types = (XrXirTypes){f->nodes, 10, &f->table, NULL};
    f->module = (XrXirModule){.functions = &f->function, .function_count = 1, .types = &f->types, .generics = &f->generic};
}

static void marker_case(unsigned mode) {
    MarkerTypes fixture;marker_types(&fixture);XrXirType type=(XrXirType)256;
    XrXirStatus expected=XR_XIR_OK;bool malformed=false;XrXirType late_argument=XR_XIR_I64,late_projection=XR_XIR_I64;
    switch(mode) {
    case 0: break;
    case 1: type=(XrXirType)257;break;
    case 2: type=(XrXirType)259;expected=XR_XIR_BAD_TYPE;break;
    case 3: type=(XrXirType)260;break;
    case 4: type=(XrXirType)261;break;
    case 5: type=(XrXirType)265;expected=XR_XIR_BAD_TYPE;break;
    case 6: type=(XrXirType)260;fixture.caller_bound.markers=XR_XIR_CONSTRAINT_ERROR;expected=XR_XIR_BAD_TYPE;break;
    case 7: type=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;fixture.caller_bound.markers|=XR_XIR_CONSTRAINT_ERROR;break;
    case 8: type=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;fixture.caller_bound.markers=XR_XIR_CONSTRAINT_ERROR;expected=XR_XIR_BAD_TYPE;break;
    case 9: type=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;fixture.caller_bound.markers=XR_XIR_CONSTRAINT_ERROR|XR_XIR_CONSTRAINT_ATOMIC_VALUE;break;
    case 10: type=(XrXirType)266;late_argument=late_projection=(XrXirType)263;
        fixture.nodes[10]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={4,&late_argument,1,&late_projection,1}};fixture.types.count=11;break;
    case 11: type=(XrXirType)267;fixture.nodes[10]=(XrXirTypeNode){.kind=XR_XIR_TYPE_ATOMIC,.element=XR_XIR_I64};
        late_argument=late_projection=(XrXirType)266;fixture.nodes[11]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={4,&late_argument,1,&late_projection,1}};
        fixture.types.count=12;break;
    case 12: type=(XrXirType)257;fixture.safe_fields[0].type=fixture.safe_projection[0]=(XrXirType)257;expected=XR_XIR_BAD_TYPE;break;
    case 13: type=(XrXirType)261;fixture.scalar_projection=XR_XIR_STRING;malformed=true;break;
    case 14: type=(XrXirType)257;fixture.safe_variants[1].field_begin=0;malformed=true;break;
    case 15: type=(XrXirType)267;late_argument=late_projection=(XrXirType)266;
        fixture.nodes[10]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)264};
        fixture.nodes[11]=(XrXirTypeNode){.kind=XR_XIR_TYPE_NOMINAL,.nominal={4,&late_argument,1,&late_projection,1}};fixture.types.count=12;break;
    default: CHECK(false);
    }
    XrXirCompileContext context={0};XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context.resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirStatus shape=xr_xir_compile_types_structure_verify(&context,&fixture.types);
    XrXirStatus expected_shape=mode==14?XR_XIR_BAD_STRUCTURE:
        (malformed || mode==12)?XR_XIR_BAD_TYPE:XR_XIR_OK;
    if (shape!=expected_shape)
        fprintf(stderr,"marker type mode%u shape%u\n",mode,(unsigned)shape);
    CHECK(shape==expected_shape);
    if (!malformed) {
        XrXirProofContext proof={&fixture.module,{XR_XIR_CONTEXT_FUNCTION,0,0}};
        CHECK(xr_xir_compile_type_markers_prove(&context,&proof,type,
            XR_XIR_CONSTRAINT_ERROR|XR_XIR_CONSTRAINT_SENDABLE)==expected);
    }
    CHECK(xr_compile_resources_stats(context.resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(before.held_bytes==after.held_bytes);xr_compile_resources_release(context.resources);
}
