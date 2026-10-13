/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lsp_source_navigation_three_cases.h - Independent multi-entry reference union
 *
 * KEY CONCEPT:
 *   Two unsaved importers share one declaration; each source location occurs once.
 */
static const char lsp_three_extra_text[] =
    "import { value } from \"./lib\"\n"
    "export fn extra()->i64 { return value() + value() }\n";

static void lsp_three_locations(const XrJsonValue *output, const char *root_uri,
    const char *library_uri, const char *extra_uri, bool include_declaration) {
    CHECK(output && output->type == XR_JSON_ARRAY);
    CHECK(output->as.array.count == (include_declaration ? 4 : 3));
    unsigned seen = 0;
    for (int i = 0; i < output->as.array.count; ++i) {
        XrJsonValue *item = output->as.array.items[i];
        const char *uri = xjson_get_string(item, "uri");
        CHECK(uri);
        unsigned bit = 0;
        if (!strcmp(uri, root_uri)) {
            lsp_json_location_facts(item, root_uri, 1, 42, 47); bit = 1;
        } else if (!strcmp(uri, extra_uri)) {
            XrJsonValue *range = xjson_get_object(item, "range");
            XrJsonValue *start = xjson_get_object(range, "start");
            CHECK(start);
            int column = (int)xjson_get_int(start, "character");
            CHECK(column == 32 || column == 42);
            lsp_json_location_facts(item, extra_uri, 1, column, column + 5);
            bit = column == 32 ? 2u : 4u;
        } else {
            CHECK(include_declaration && !strcmp(uri, library_uri));
            lsp_json_location_facts(item, library_uri, 0, 19, 24); bit = 8;
        }
        CHECK(!(seen & bit)); seen |= bit;
    }
    CHECK(seen == (include_declaration ? 15u : 7u));
}

static XrXirStatus lsp_three_once(const char *root, XrCompileResourceLimits limits,
    XrCompileResourceStats *stats) {
    XrXirCompileContext context = {0}; XrCompilerSession *session = NULL;
    XlspSourceSnapshot *snapshots[3] = {0}; XrJsonValue *output = NULL;
    XrCompileResourceStatus made = xr_compile_resources_new(&limits, &context.resources);
    if (made != XR_COMPILE_RESOURCE_OK)
        return made == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    context.limits = xr_xir_compile_default_limits();
    XrCompilerSessionStatus created = xr_compile_session_new(context.resources, &session);
    XrXirStatus status = created == XR_COMPILER_SESSION_OK ? XR_XIR_OK :
        created == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
    char root_uri[XR_TEST_PATH_MAX], library_uri[XR_TEST_PATH_MAX], extra_uri[XR_TEST_PATH_MAX];
    lsp_uri(root_uri, sizeof(root_uri), root, "root.xr");
    lsp_uri(library_uri, sizeof(library_uri), root, "lib.xr");
    lsp_uri(extra_uri, sizeof(extra_uri), root, "extra.xr");
    char input_uris[3][XR_TEST_PATH_MAX];
    memcpy(input_uris[0], root_uri, strlen(root_uri) + 1);
    memcpy(input_uris[1], library_uri, strlen(library_uri) + 1);
    memcpy(input_uris[2], extra_uri, strlen(extra_uri) + 1);
    char root_text[sizeof(source_root_text)], library_text[sizeof(source_library_text)];
    char extra_text[sizeof(lsp_three_extra_text)];
    memcpy(root_text, source_root_text, sizeof(root_text));
    memcpy(library_text, source_library_text, sizeof(library_text));
    memcpy(extra_text, lsp_three_extra_text, sizeof(extra_text));
    XlspSourceDocument documents[3] = {
        {input_uris[0], root_text, strlen(input_uris[0]), sizeof(root_text) - 1, 1},
        {input_uris[1], library_text, strlen(input_uris[1]), sizeof(library_text) - 1, 7},
        {input_uris[2], extra_text, strlen(input_uris[2]), sizeof(extra_text) - 1, 2}};
    XrModuleIdentityAuthority authorities[3] = {
        {XR_MODULE_IDENTITY_SCRIPT, NULL, root}, {XR_MODULE_IDENTITY_SCRIPT, NULL, root},
        {XR_MODULE_IDENTITY_SCRIPT, NULL, root}};
    XrXirSourceRequest request = {session, NULL, &authorities[0], &context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    for (size_t i = 0; i < 3 && status == XR_XIR_OK; ++i)
        status = xlsp_source_snapshot_build_authorities(&request, documents, authorities, 3, i,
            &snapshots[i], NULL, NULL);
    memset(root_text, '?', sizeof(root_text)); memset(library_text, '?', sizeof(library_text));
    memset(extra_text, '?', sizeof(extra_text)); memset(input_uris, '?', sizeof(input_uris));
    memset(documents, 0, sizeof(documents)); memset(&request, 0, sizeof(request));
    xr_compile_session_free(session);
    /* Only an observation pin and immutable queries retain the original ledger. */
    CHECK(xr_compile_resources_retain(context.resources) == XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources);
    for (unsigned include = 0; include < 2 && status == XR_XIR_OK; ++include) {
        status = xlsp_source_navigation_many_json(context.resources, snapshots, 3,
            library_uri, strlen(library_uri), (XrLspPosition){0,20}, 1, include != 0, &output);
        if (status != XR_XIR_OK) {CHECK(!output); break;}
        if (!include) {
            lsp_three_locations(output, root_uri, library_uri, extra_uri, false);
            xjson_free(output); output = NULL;
        }
    }
    if (stats) CHECK(xr_compile_resources_stats(context.resources, stats) == XR_COMPILE_RESOURCE_OK);
    for (size_t i = 0; i < 3; ++i) xlsp_source_snapshot_free(snapshots[i]);
    xr_compile_resources_release(context.resources); lsp_zero();
    /* The complete JSON result remains readable after every Source owner dies. */
    if (output) lsp_three_locations(output, root_uri, library_uri, extra_uri, true);
    xjson_free(output); CHECK(!lsp_json_live && !lsp_json_bytes);
    return status;
}

static void lsp_navigation_three_cases(const char *root) {
    size_t source_sites = 0; XrCompileResourceStats required = {0};
    lsp_json_fail = SIZE_MAX; lsp_json_injected = false;
    for (size_t probe = 0; probe <= source_sites; ++probe) {
        source_fixture_compile_attempts = 0;
        source_fixture_compile_fail_at = probe ? probe - 1 : SIZE_MAX;
        source_fixture_compile_injected = false;
        XrXirStatus status = lsp_three_once(root, lsp_limits(), probe ? NULL : &required);
        if (!probe) {CHECK(status == XR_XIR_OK); source_sites = source_fixture_compile_attempts; CHECK(source_sites);}
        else CHECK(status == XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected);
        lsp_zero();
    }
    source_fixture_compile_fail_at = SIZE_MAX; source_fixture_compile_injected = false;
    for (unsigned axis = 0; axis < 3; ++axis) for (int delta = -1; delta <= 1; ++delta) {
        XrCompileResourceLimits limits = {required.allocated_bytes, required.peak_bytes, required.work};
        uint64_t *bound = axis == 0 ? &limits.allocated_bytes : axis == 1 ? &limits.live_bytes : &limits.work;
        CHECK(*bound && *bound < UINT64_MAX); *bound = (uint64_t)((int64_t)*bound + delta);
        CHECK(lsp_three_once(root, limits, NULL) == (delta < 0 ? XR_XIR_BUDGET : XR_XIR_OK)); lsp_zero();
    }
    size_t json_sites = 0;
    for (size_t probe = 0; probe <= json_sites; ++probe) {
        lsp_json_calls = 0; lsp_json_fail = probe ? probe - 1 : SIZE_MAX; lsp_json_injected = false;
        XrXirStatus status = lsp_three_once(root, lsp_limits(), NULL);
        if (!probe) {CHECK(status == XR_XIR_OK); json_sites = lsp_json_calls; CHECK(json_sites);}
        else CHECK(status == XR_XIR_OUT_OF_MEMORY && lsp_json_injected && lsp_json_calls == probe);
        lsp_zero();
    }
    lsp_json_fail = SIZE_MAX; lsp_json_injected = false;
    printf("LSP three-document reference union Source sites=%zu JSON sites=%zu; exact 3/4 locations; three axes; producer death; physical=0/0\n",
        source_sites, json_sites);
}
