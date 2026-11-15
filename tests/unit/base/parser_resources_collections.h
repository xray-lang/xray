/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Argument labels and collection spans survive their parser and input owners. */
static const char collection_metadata_source[] =
    "const call = target(1, 2, 3, 4, 5, right: ref r, left: move l)\n"
    "const map = {...base, makeKey(): makeValue(), \"a\\u{0}b\": 3}\n"
    "const point = Point{...base, x: 3, ...extra}\n"
    "const nested = \"${target(outer: \"${inner(value: 1)}\")}\"\n"
    "const array = [1 + 2, \"x\", ...rest]\n";

static void collection_metadata_facts(const AstNode *program) {
    CHECK(program && program->type == AST_PROGRAM && program->as.program.count == 5);
    const AstNode *call = program->as.program.statements[0]->as.var_decl.initializer;
    CHECK(call && call->type == AST_CALL_EXPR && call->as.call_expr.arg_count == 7);
    const XrCallArgLabel *labels = call->as.call_expr.arg_labels;
    CHECK(labels && !strcmp(labels[5].name, "right") && !strcmp(labels[6].name, "left"));
    CHECK(!labels[0].name && labels[5].span.line == 1);
    CHECK(labels[5].span.column == (int)strlen("const call = target(1, 2, 3, 4, 5, ") + 1);
    CHECK(call->as.call_expr.arg_accesses[5] == XR_CALL_ARG_REF);
    CHECK(call->as.call_expr.arg_accesses[6] == XR_CALL_ARG_MOVE);
    const AstNode *map = program->as.program.statements[1]->as.var_decl.initializer;
    CHECK(map && map->type == AST_MAP_LITERAL && map->as.map_literal.count == 3);
    CHECK(!map->as.map_literal.keys[0] && map->as.map_literal.values[0]->type == AST_SPREAD_EXPR);
    CHECK(map->as.map_literal.entry_spans[0].key.line == 2);
    CHECK(map->as.map_literal.entry_spans[0].key.column == 14);
    CHECK(map->as.map_literal.entry_spans[1].value_end.line==2 && map->as.map_literal.entry_spans[1].value_end.column==
        (int)strlen("const map = {...base, makeKey(): makeValue()")+1);
    const AstNode *key = map->as.map_literal.keys[2];
    CHECK(key->type == AST_LITERAL_STRING && key->as.literal.string_length == 3);
    CHECK(!memcmp(key->as.literal.raw_value.string_val, "a\0b", 3));
    const AstNode *array=program->as.program.statements[4]->as.var_decl.initializer;
    CHECK(array && array->type==AST_ARRAY_LITERAL && array->as.array_literal.count==3);
    CHECK(array->as.array_literal.element_spans && array->as.array_literal.element_spans[0].start.line==5 &&
        array->as.array_literal.element_spans[0].start.column==(int)strlen("const array = [")+1 &&
        array->as.array_literal.element_spans[0].end.column==(int)strlen("const array = [1 + 2")+1 &&
        array->as.array_literal.element_spans[2].start.column==(int)strlen("const array = [1 + 2, \"x\", ")+1 &&
        array->as.array_literal.element_spans[2].end.column==(int)strlen("const array = [1 + 2, \"x\", ...rest")+1);
    const AstNode *point = program->as.program.statements[2]->as.var_decl.initializer;
    CHECK(point && point->type == AST_STRUCT_LITERAL && point->as.struct_literal.field_count == 3);
    CHECK(!point->as.struct_literal.field_names[0] && !point->as.struct_literal.field_names[2]);
    CHECK(!strcmp(point->as.struct_literal.field_names[1], "x"));
    CHECK(point->as.struct_literal.field_name_spans[1].line == 3);
    CHECK(point->as.struct_literal.field_name_spans[1].column ==
          (int)strlen("const point = Point{...base, ") + 1);
    const AstNode *nested = program->as.program.statements[3]->as.var_decl.initializer;
    CHECK(nested && nested->type == AST_TEMPLATE_STRING && nested->as.template_str.part_count == 1);
    call = nested->as.template_str.parts[0];
    CHECK(call && call->type == AST_CALL_EXPR && !strcmp(call->as.call_expr.arg_labels[0].name, "outer"));
    nested = call->as.call_expr.arguments[0];
    CHECK(nested && nested->type == AST_TEMPLATE_STRING && nested->as.template_str.part_count == 1);
    call = nested->as.template_str.parts[0];
    CHECK(call && call->type == AST_CALL_EXPR && !strcmp(call->as.call_expr.arg_labels[0].name, "value"));
    const char *line = strstr(collection_metadata_source, "const nested");
    CHECK(call->as.call_expr.arg_labels[0].span.line == 4);
    CHECK(call->as.call_expr.arg_labels[0].span.column == (int)(strstr(line, "value:") - line) + 1);
}

static XrParseStatus collection_metadata_pipeline(const XrCompileResourceLimits *limits,
                                                 XrCompileResourceStats *stats) {
    XrCompileResources *resources = NULL;
    XrCompilerSession *session = NULL;
    AstNode *program = NULL;
    char input[sizeof(collection_metadata_source)];
    memcpy(input, collection_metadata_source, sizeof(input));
    XrCompileResourceStatus made = xr_compile_resources_new(limits, &resources);
    if (made != XR_COMPILE_RESOURCE_OK)
        return made == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_PARSE_MEMORY_LIMIT : XR_PARSE_OUT_OF_MEMORY;
    made = xr_compile_session_new(resources, &session);
    XrParseStatus status = made == XR_COMPILE_RESOURCE_OK ? XR_PARSE_OK :
        made == XR_COMPILE_RESOURCE_MEMORY_LIMIT ? XR_PARSE_MEMORY_LIMIT : XR_PARSE_OUT_OF_MEMORY;
    if (status == XR_PARSE_OK) {
        status = xr_compile_parse(session, input, &program);
        if (status == XR_PARSE_OK) collection_metadata_facts(program);
        else {
            CHECK(!program && (status == XR_PARSE_OUT_OF_MEMORY || status == XR_PARSE_MEMORY_LIMIT));
            size_t attempts = attempt_count;
            CHECK(xr_compile_parse(session, (const char *)(uintptr_t)1, &program) == status);
            CHECK(attempt_count == attempts && !program);
        }
    }
    OK(xr_compile_resources_stats(resources, stats));
    CHECK(stats->allocated_bytes == physical_total && stats->peak_bytes == physical_peak);
    CHECK(stats->held_bytes == physical_live);
    memset(input, '?', sizeof(input));
    xr_compile_session_free(session);
    xr_compile_resources_release(resources);
    if (program) { collection_metadata_facts(program); xr_program_destroy(program); }
    CHECK(!physical_live && !allocation_count);
    return status;
}

static void collection_metadata_ownership(void) {
    reset();
    XrCompileResourceStats exact = {0}, stats = {0};
    CHECK(collection_metadata_pipeline(&unlimited, &exact) == XR_PARSE_OK);
    size_t sites = attempt_count;
    CHECK(sites && exact.peak_bytes && exact.allocation_count);
    for (size_t point = 0; point < sites; ++point) {
        reset(); fail_at = point;
        CHECK(collection_metadata_pipeline(&unlimited, &stats) == XR_PARSE_OUT_OF_MEMORY);
        CHECK(attempt_count == point + 1);
    }
    for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = {XR_COMPILE_MEMORY_FINITE, exact.peak_bytes, true};
        CHECK(limits.memory_limit < UINT64_MAX);
        limits.memory_limit = (uint64_t)((int64_t)limits.memory_limit + delta);
        reset();
        CHECK(collection_metadata_pipeline(&limits, &stats) == (delta < 0 ? XR_PARSE_MEMORY_LIMIT : XR_PARSE_OK));
    }
    printf("parser labels/Map/struct metadata: allocations=%zu full FI/held boundary/producer death/physical zero passed\n", sites);
}
