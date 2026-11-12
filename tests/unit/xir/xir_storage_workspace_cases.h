/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_storage_workspace_cases.h - Fixed storage-table ownership and rollback
 */
#ifndef XIR_STORAGE_WORKSPACE_CASES_H
#define XIR_STORAGE_WORKSPACE_CASES_H

typedef struct StorageWorkspaceFixture {
    XrXirType parameters[2];
    XrXirBlock blocks[2];
    XrXirInstruction instructions[4];
    XrXirFunction function;
    XrXirModule module;
    EffectCheckedRoots execution;
} StorageWorkspaceFixture;

static void storage_workspace_fixture(StorageWorkspaceFixture *fixture, bool empty) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->parameters[0] = fixture->parameters[1] = XR_XIR_I64;
    fixture->blocks[0] = (XrXirBlock){.count=2};
    fixture->blocks[1] = (XrXirBlock){.first=2,.count=2};
    fixture->instructions[0] = (XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=7};
    fixture->instructions[1] = (XrXirInstruction){.op=XR_XIR_JUMP,.targets={1}};
    fixture->instructions[2] = (XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=9};
    fixture->instructions[3] = (XrXirInstruction){.op=XR_XIR_RETURN,.args={4}};
    fixture->function = (XrXirFunction){.parameters=empty ? NULL : fixture->parameters,
        .parameter_count=empty ? 0 : 2,.blocks=empty ? NULL : fixture->blocks,
        .block_count=empty ? 0 : 2,.instructions=empty ? NULL : fixture->instructions,
        .instruction_count=empty ? 0 : 4,.result=XR_XIR_I64,.error_type=XR_XIR_NEVER};
    fixture->module = (XrXirModule){.functions=&fixture->function,.function_count=1};
}

static void storage_workspace_constructor(bool empty) {
    StorageWorkspaceFixture fixture;
    storage_workspace_fixture(&fixture, empty);
    EffectMark baseline = effect_mark();
    XirStorageFlow *flow = NULL;
    attempts = 0;
    CHECK(xir_storage_flow_new(&effect_context,&fixture.module,&fixture.execution,NULL,&flow) == XR_XIR_OK);
    size_t sites = attempts;
    CHECK(sites == 4);
    XirStorageFlowView view = {0};
    CHECK(xir_storage_flow_view(flow,&view) == XR_XIR_OK && view.function_count == 1);
    CHECK(view.functions[0].parameter_count == (empty ? 0u : 2u) &&
        view.functions[0].physical_values == (empty ? 0u : 6u));
    if (!empty) {
        memset(fixture.parameters,0xCE,sizeof(fixture.parameters));
        memset(fixture.blocks,0xCE,sizeof(fixture.blocks));
        for (uint32_t i = 0; i < fixture.function.instruction_count; ++i)
            CHECK(xir_storage_flow_observe(flow,0,i,&fixture.instructions[i]) == XR_XIR_OK);
        CHECK(xir_storage_flow_finish_local(flow) == XR_XIR_OK);
    }
    xir_storage_flow_free(flow);
    effect_mark_check(baseline);
    storage_workspace_fixture(&fixture, empty);
    for (size_t site = 0; site < sites; ++site) {
        flow = NULL; attempts = 0; injected = false; fail_at = site;
        CHECK(xir_storage_flow_new(&effect_context,&fixture.module,&fixture.execution,NULL,&flow) ==
            XR_XIR_OUT_OF_MEMORY && injected && !flow);
        fail_at = SIZE_MAX;
        effect_mark_check(baseline);
    }
}

static void storage_workspace_invalid_headers(void) {
    StorageWorkspaceFixture fixture;
    storage_workspace_fixture(&fixture, false);
    EffectMark baseline = effect_mark();
    XrXirFunction valid = fixture.function;
    for (uint32_t variant = 0; variant < 6; ++variant) {
        fixture.function = valid;
        switch (variant) {
        case 0: fixture.function.parameter_count = UINT32_MAX; break;
        case 1: fixture.function.instruction_count = UINT32_MAX; break;
        case 2: fixture.function.block_count = UINT32_MAX; break;
        case 3: fixture.function.parameters = NULL; break;
        case 4: fixture.function.blocks = NULL; break;
        default: fixture.blocks[1].first = 5; break;
        }
        XirStorageFlow *flow = NULL;
        CHECK(xir_storage_flow_new(&effect_context,&fixture.module,&fixture.execution,NULL,&flow) ==
            XR_XIR_BAD_STRUCTURE && !flow);
        effect_mark_check(baseline);
    }
}

static void storage_workspace_ownership(void) {
    storage_workspace_constructor(true);
    storage_workspace_constructor(false);
    storage_workspace_invalid_headers();
    puts("storage workspace: zero tables, retired inputs, four-site rollback and invalid geometry PASS");
}

#endif // XIR_STORAGE_WORKSPACE_CASES_H
