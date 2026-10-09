/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_views_native_call_boundary.h - Adaptive call storage boundaries
 *
 * KEY CONCEPT:
 *   Real dependency calls preserve owners and output at exact storage limits.
 */
/* A root initializer that has not returned cannot publish the root module.
 * Live call storage refusal is not lifetime exhaustion, but initialization
 * failure still remains sticky on this same Instance and epoch. */
static void views_initialization_limit(H1Runtime *t, unsigned i, uint64_t epoch) {
    H1Witness *w = &t->w[i]; XrXirInstance *instance = w->instance;
    uint32_t root = t->fixture.module, all = (UINT32_C(1) << w->output.modules) - 1;
    uint32_t root_bit = UINT32_C(1) << root;
    CHECK(w->output.modules == 18 && w->output.module == root);
    CHECK(w->output.begins == 1 && !w->output.ready && w->output.begun_mask == all);
    CHECK(w->output.ready_mask == (all ^ root_bit));
    CHECK(!w->output.groups && !w->output.writes && !w->output.length);
    CHECK(instance->state == XR_XIR_INSTANCE_FAILED && instance->epoch == epoch);
    CHECK(instance->cursor == 17 && instance->current_module == UINT32_MAX && !instance->publication_count);
    CHECK(!instance->budget.exhausted && !w->held.type && !w->alias.type);
    for (uint32_t m = 0; m < 18; ++m) CHECK(instance->ready[m] == (m == root ? 0u : 1u));
    h1r_sticky(t, i, XR_XIR_CALL_LIMIT);
    H1NrLedger before = h1nr_ledger(instance), peer = h1nr_ledger(t->w[1-i].instance);
    H1NrObservation output = w->output, peer_output = t->w[1-i].output;
    size_t attempts = runtime_attempts;
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        CHECK(xr_xir_instance_start(instance, t->fixture.run, NULL, 0) == XR_XIR_CALL_LIMIT);
        XrXirCallResult failure = {XR_XIR_CALL_READY,{0},0,{0}};
        CHECK(xr_xir_instance_copy_failure(instance, &failure) == XR_XIR_CALL_LIMIT);
        CHECK(failure.status == XR_XIR_CALL_LIMIT && xr_xir_call_result_valid(&failure));
        CHECK(!failure.value.type && !failure.value.reserved && !failure.value.payload && !failure.wake);
        xr_xir_call_result_drop(&failure);
        XrXirCallResult occupied = {XR_XIR_CALL_RETURNED,{XR_XIR_I64,0,101},0,{0}}, saved = occupied;
        CHECK(xr_xir_instance_copy_failure(instance, &occupied) == XR_XIR_CALL_BAD_ARGUMENT);
        CHECK(!memcmp(&occupied, &saved, sizeof(saved)));
        XrXirInstanceResult polled = xr_xir_instance_poll_bounded(instance, 1);
        CHECK(polled.epoch == epoch && polled.outcome.status == XR_XIR_CALL_LIMIT &&
            xr_xir_call_result_valid(&polled.outcome));
        h1r_failed_take(w);
        CHECK(instance->epoch == epoch && instance->state == XR_XIR_INSTANCE_FAILED && attempts == runtime_attempts);
        h1nr_unchanged(instance, &before); h1nr_unchanged(t->w[1-i].instance, &peer);
        h1nr_output_unchanged(&w->output, &output); h1nr_output_unchanged(&t->w[1-i].output, &peer_output);
    }
    printf("VIEWS_NATIVE_RESOURCE_INIT_LIMIT file=%s instance=%u epoch=%llu status=%u state=%u "
        "dependency_ready=17 root_ready=0 begun_mask=%u ready_mask=%u same_owners=1 retry=LIMIT copy_failure=LIMIT "
        "occupied_preserved=1 fees_unchanged=1\n", w->output.oracle->file, i, (unsigned long long)epoch, (unsigned)XR_XIR_CALL_LIMIT,
        (unsigned)XR_XIR_INSTANCE_FAILED, w->output.begun_mask, w->output.ready_mask);
}
static void views_call_tight(const RootParameterSourceOracle *oracle, const H1Expected *e) {
    CHECK(oracle->index == 4 && h1nr_native->entry_count == 37);
    bool deeper = !strcmp(h1nr_mode, "native-vm");
    const uint64_t minimum = deeper ? UINT64_C(10112) : UINT64_C(9728);
    for (unsigned i = 0; i < 2; ++i) for (unsigned minus = 0; minus < 2; ++minus) {
        H1Runtime t; h1r_prepare(&t, h1nr_build(oracle), oracle);
        H1Witness *w = &t.w[i]; unsigned peer = 1 - i;
        CHECK(e->axes[i][4] == 11008 && e->axes[i][1] == 25418);
        h1r_selector(w, 4, minimum - minus);
        CHECK(h1r_step(&t, peer, H1R_CREATE) == XR_XIR_CALL_READY);
        unsigned refused = deeper ? H1R_ENTRY_BODY : H1R_RUN_START;
        if (!minus) {
            h1r_forward(&t, i, H1R_CREATE, H1R_COPY);
            CHECK(w->selectors[4] == minimum);
            CHECK(w->selectors[1] == UINT64_C(25418) - 2 * (UINT64_C(11008) - minimum));
        } else {
            h1r_forward(&t, i, H1R_CREATE, refused - 1);
            XrXirInstance *identity = w->instance; XrXirDomain *domain = w->domain;
            const XrXirCallBudget *budget = &identity->budget;
            H1NrLedger before = h1nr_ledger(identity); uint64_t epoch = identity->epoch;
            h1r_audit(&t, i, "tight_S0");
            CHECK(h1r_step(&t, i, refused) == XR_XIR_CALL_LIMIT);
            CHECK(w->instance == identity && w->domain == domain && &identity->budget == budget);
            CHECK(identity->epoch == epoch && w->state == (deeper ? XR_XIR_INSTANCE_FAILED : XR_XIR_INSTANCE_READY));
            CHECK(w->output.groups == (deeper ? 0u : 1u) && w->output.writes == w->output.groups);
            CHECK((unsigned)budget->exhausted == (deeper ? 0u : 1u));
            h1r_no_refund(w, &before, domain, budget); h1r_audit(&t, i, "tight_S1");
            H1NrObservation published = w->output;
            if (deeper) {
                views_initialization_limit(&t, i, epoch);
                CHECK(identity->epoch == epoch && !w->held.type && !w->alias.type && !budget->exhausted);
            } else {
                CHECK(h1r_step(&t, i, H1R_RUN_START) == XR_XIR_CALL_LIMIT);
                CHECK(identity->epoch == epoch && budget->exhausted);
            }
            h1nr_output_unchanged(&w->output, &published);
            CHECK(w->instance == identity && w->domain == domain && &identity->budget == budget);
            h1r_no_refund(w, &before, domain, budget); h1r_audit(&t, i, "tight_S2");
        }
        h1r_program_release(&t); h1r_forward(&t, peer, H1R_ENTRY_START, H1R_COPY);
        unsigned close = minus && !deeper ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY;
        h1r_final(&t, i, close, close);
        printf("VIEWS_NATIVE_RESOURCE_CALL_TIGHT file=%s mode=%s instance=%u minus1=%u cap=%llu "
            "refused=%u retry=%u peak=%llu requested=%llu work=%llu physical=0/0,0/0\n",
            oracle->file, h1nr_mode, i, minus, (unsigned long long)(minimum - minus),
            minus ? refused : H1R_PHASES, minus ? XR_XIR_CALL_LIMIT : XR_XIR_CALL_READY,
            (unsigned long long)w->selectors[4], (unsigned long long)w->selectors[1],
            (unsigned long long)w->selectors[5]);
    }
}
