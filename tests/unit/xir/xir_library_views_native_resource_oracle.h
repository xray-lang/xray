/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_views_native_resource_oracle.h
 */
static uint64_t h1_decimal(const char *text) {
    CHECK(text && *text); uint64_t value = 0;
    for (const char *p = text; *p; ++p) {
        CHECK(*p >= '0' && *p <= '9'); uint64_t digit = (uint64_t)(*p - '0');
        CHECK(value <= (UINT64_MAX - digit) / 10); value = value * 10 + digit;
    }
    return value;
}
static uint64_t h1_number(FILE *file) {
    char text[32] = {0}; CHECK(fscanf(file, "%31s", text) == 1); return h1_decimal(text);
}
static void h1_token(FILE *file, const char *expected) {
    char text[64] = {0}; CHECK(fscanf(file, "%63s", text) == 1 && !strcmp(text, expected));
}
static unsigned h1_unsigned(FILE *file) {
    uint64_t value = h1_number(file); CHECK(value <= UINT32_MAX); return (unsigned)value;
}
static H1Expected h1_oracle(const char *path, const RootParameterSourceOracle *oracle) {
    FILE *file = fopen(path, "rb"); CHECK(file); H1Expected e = {0};
    h1_token(file, "VIEWS_NATIVE_RESOURCE_R1"); CHECK(h1_number(file) == XR_XIR_CHECKED_SCHEMA);
    CHECK(h1_number(file) == XR_XIR_CHECKED_CONTRACT && h1_number(file) == XR_XIR_VALUE_ABI_VERSION);
    CHECK(h1_number(file) == XR_XIR_CALL_ABI_VERSION && h1_number(file) == XR_XIR_PROGRAM_ABI_VERSION);
    h1_token(file, oracle->file); h1_token(file, h1nr_mode); h1_token(file, "compiler_sites");
    for (unsigned p = 0; p < H1C_PHASES; ++p) {
        uint64_t v = h1_number(file); CHECK((uint64_t)(size_t)v == v && v <= 65536); e.compiler_sites[p] = (size_t)v;
    }
    CHECK(e.compiler_sites[H1C_OWNER] == 1); h1_token(file, "compiler_axes");
    const uint64_t caps[3] = {h1c_caps.allocated_bytes, h1c_caps.live_bytes, h1c_caps.work};
    for (unsigned a = 0; a < 3; ++a) { e.compiler_axes[a] = h1_number(file); CHECK(e.compiler_axes[a] > 1 && e.compiler_axes[a] <= caps[a]); }
    const uint64_t limits[H1R_AXES] = {UINT64_C(67108864), UINT64_C(67108864), 65536, 65536, 65536, UINT64_C(128000000)};
    for (unsigned i = 0; i < 2; ++i) {
        h1_token(file, "instance"); CHECK(h1_number(file) == i); h1_token(file, "sites");
        for (unsigned p = 0; p < H1R_PHASES; ++p) {
            uint64_t v = h1_number(file); CHECK((uint64_t)(size_t)v == v && v <= 65536); e.sites[i][p] = (size_t)v;
        }
        CHECK(e.sites[i][H1R_CREATE] && e.sites[i][H1R_ENTRY_START] && e.sites[i][H1R_RUN_START]);
        CHECK(!e.sites[i][H1R_ENTRY_TAKE] && !e.sites[i][H1R_RUN_TAKE]);
        for (unsigned p = H1R_COPY; p < H1R_PHASES; ++p) CHECK(!e.sites[i][p]);
        h1_token(file, "selectors");
        for (unsigned a = 0; a < H1R_AXES; ++a) { e.axes[i][a] = h1_number(file); CHECK(e.axes[i][a] > 1 && e.axes[i][a] <= limits[a]); }
        h1_token(file, "minus_frontiers");
        for (unsigned a = 0; a < H1R_AXES; ++a) {
            H1Minus *m = &e.minus[i][a]; m->phase = h1_unsigned(file); m->state = h1_unsigned(file);
            m->groups = h1_unsigned(file); m->writes = h1_unsigned(file); m->exhausted = h1_unsigned(file);
            m->stop = h1_unsigned(file); m->free = h1_unsigned(file);
            m->retry_phase = h1_unsigned(file); m->retry_status = h1_unsigned(file);
            bool adaptive = oracle->index == 4 && a == 4 && m->phase == H1R_PHASES;
            CHECK(adaptive || (m->phase <= H1R_FREE && m->phase != H1R_ENTRY_TAKE && m->phase != H1R_RUN_TAKE && m->phase != H1R_COPY));
            if (adaptive) CHECK(m->state == XR_XIR_INSTANCE_DRAINING && m->groups == 1 && m->writes == 1 &&
                !m->exhausted && !m->stop && !m->free && m->retry_phase == H1R_PHASES && !m->retry_status);
            CHECK(m->state <= XR_XIR_INSTANCE_DRAINING && m->groups <= 1 && m->writes <= m->groups && m->exhausted <= 1);
            CHECK(m->stop == XR_XIR_CALL_READY || m->stop == XR_XIR_CALL_LIMIT);
            CHECK(m->free == XR_XIR_CALL_READY || m->free == XR_XIR_CALL_LIMIT);
            CHECK(m->retry_phase == H1R_PHASES || m->retry_phase == H1R_RUN_START || m->retry_phase == H1R_RUN_BODY);
            if (m->retry_phase != H1R_PHASES) CHECK(m->retry_status == XR_XIR_CALL_LIMIT || m->retry_status == XR_XIR_CALL_BAD_STATE);
        }
        h1_token(file, "same_quota"); H1Minus *q = &e.quota[i];
        q->phase = h1_unsigned(file); q->state = h1_unsigned(file); q->groups = h1_unsigned(file);
        q->writes = h1_unsigned(file); q->exhausted = h1_unsigned(file); q->stop = h1_unsigned(file); q->free = h1_unsigned(file);
        CHECK(q->phase >= H1R_RUN_START && q->phase <= H1R_FREE && q->state <= XR_XIR_INSTANCE_DRAINING);
        CHECK(q->groups == 1 && q->writes == 1 && q->exhausted <= 1);
        CHECK(q->stop == XR_XIR_CALL_READY || q->stop == XR_XIR_CALL_LIMIT);
        CHECK(q->free == XR_XIR_CALL_READY || q->free == XR_XIR_CALL_LIMIT);
        h1_token(file, "exact_close");
        for (unsigned a = 0; a < H1R_AXES; ++a) {
            e.exact_stop[i][a] = h1_unsigned(file); e.exact_free[i][a] = h1_unsigned(file);
            CHECK(e.exact_stop[i][a] == XR_XIR_CALL_READY && e.exact_free[i][a] == XR_XIR_CALL_READY);
        }
    }
    h1_token(file, "END"); int b; while ((b = fgetc(file)) != EOF) CHECK(isspace((unsigned char)b));
    CHECK(!ferror(file) && !fclose(file)); return e;
}
