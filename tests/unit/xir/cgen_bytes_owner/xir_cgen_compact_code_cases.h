/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cgen_compact_code_cases.h - Literal code whitespace and owner boundaries
 *
 * KEY CONCEPT:
 *   Fixed C spellings and suffix facts check actual canonical operations.
 *   These controls do not compact unknown text or construct a second emitter.
 */
#ifndef XIR_CGEN_COMPACT_CODE_CASES_H
#define XIR_CGEN_COMPACT_CODE_CASES_H

static const char compact_value[] = " XrXirValue value = {0};\n";
static const char compact_jump[] = " goto invalid;\n";
static const char compact_expected[] =
    "int compact_code(void) {\n"
    " XrXirValue value = {0};\n"
    " (void)value;\n"
    " goto invalid;\n"
    "invalid:\n"
    " return 42;\n"
    "}\n";

/* Full original table-prefix coverage remains in literal_all_table_prefixes.
 * This independent bounded set exercises the actual changed spellings at
 * every legal incoming state, then preserves admitted failure prefixes. */
static void compact_span_boundaries(void) {
    const char *const inputs[] = {compact_value, compact_jump};
    size_t cases = 0;
    for (size_t value = 0; value < sizeof(inputs)/sizeof(inputs[0]); ++value) {
        const EmitSpan *span = emit_literal_spans + literal_find(inputs[value]);
        CHECK(span->length == strlen(inputs[value]));
        CHECK(!memcmp(span->text, inputs[value], span->length));
        for (unsigned mask = 0; mask < 4; ++mask) {
            unsigned count0 = mask&1 ? 1 : 13, count1 = mask&2 ? 1 : 11;
            for (unsigned a = 0; a < count0; ++a) for (unsigned b = 0; b < count1; ++b) {
                for (unsigned measure = 0; measure < 2; ++measure) {
                    LiteralTrial trial = {span, {a,b}, {(mask&1)!=0,(mask&2)!=0},
                        measure!=0, span->length+1, 1000};
                    literal_trial(&trial);
                    ++cases;
                }
            }
        }
        for (unsigned measure = 0; measure < 2; ++measure) {
            const uint64_t work[] = {0,1,12,13,27,28,29,1000};
            const size_t caps[] = {0,1,span->length,span->length+1};
            for (size_t w = 0; w < sizeof(work)/sizeof(work[0]); ++w)
                for (size_t c = 0; c < sizeof(caps)/sizeof(caps[0]); ++c) {
                    LiteralTrial trial = {span,{8,0},{false,false},measure!=0,caps[c],work[w]};
                    literal_trial(&trial);
                    ++cases;
                }
        }
    }
    CHECK(cases == 800);
    printf("Compact code: %zu actual changed-span state/fee/partial/sentinel trials PASS\n", cases);
}
static void compact_reference_span(DecimalFacts *facts, const char *text, bool measuring) {
    size_t length = strlen(text);
    bool tracking = facts->tracking;
    facts->work += tracking ? 28 : 2;
    facts->work += measuring ? (length ? 1 : 0) : 2*length;
    facts->length += length;
    if (tracking) for (unsigned i = 0; i < 2; ++i)
        for (size_t at = 0; at < length; ++at)
            literal_suffix_byte(&facts->labels[i], literal_patterns[i], literal_lengths[i], text[at]);
    facts->tracking = !(facts->labels[0].found && facts->labels[1].found);
}
static DecimalFacts compact_reference(bool measuring) {
    DecimalFacts facts = {0};
    facts.labels[0] = literal_suffix(0,0,false);
    facts.labels[1] = literal_suffix(1,0,false);
    facts.tracking = true;
    decimal_literal_fee(&facts, "int compact_code(void) {\n", measuring);
    compact_reference_span(&facts, compact_value, measuring);
    decimal_literal_fee(&facts, " (void)value;\n", measuring);
    decimal_literal_fee(&facts, compact_jump, measuring);
    decimal_literal_fee(&facts, "invalid:\n return 42;\n}\n", measuring);
    return facts;
}
static void compact_actual(CBuffer *buffer) {
    EMIT_LITERAL(buffer, "int compact_code(void) {\n");
    EMIT_STATIC(buffer, es_2543e90d643759e3, " XrXirValue value = {0};\n");
    EMIT_LITERAL(buffer, " (void)value;\n");
    EMIT_LITERAL(buffer, " goto invalid;\n");
    EMIT_LITERAL(buffer, "invalid:\n return 42;\n}\n");
}
static void compact_facts(const CBuffer *buffer, const DecimalFacts *facts) {
    CHECK(buffer->length == facts->length && buffer->tracking == facts->tracking);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(buffer->label_used[i] == facts->labels[i].found &&
              buffer->label_match[i] == facts->labels[i].match);
}
/* Both measuring and writing use the same real owner. The independently
 * derived work total includes owner creation, one allocation and final NUL. */
static void compact_same_owner(void) {
    DecimalFacts measured = compact_reference(true), written = compact_reference(false);
    uint64_t exact = 2 + measured.work + written.work + 1;
    size_t length = sizeof(compact_expected)-1;
    CHECK(measured.length == length && written.length == length);
    for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,exact-minus};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
        CBuffer measure = {.limit=length+1,.status=XR_XIR_OK,.context=&context,
            .measuring=true,.tracking=true};
        compact_actual(&measure);
        CHECK(measure.status == XR_XIR_OK);
        compact_facts(&measure,&measured);
        void *memory = NULL;
        CHECK(xr_compile_resources_alloc(resources,length+2,&memory) == XR_COMPILE_RESOURCE_OK);
        char *storage = memory; memset(storage,'q',length+2);
        CBuffer writer = {.text=storage,.capacity=length+2,.limit=length+1,
            .status=XR_XIR_OK,.context=&context,.tracking=true};
        compact_actual(&writer);
        CHECK(writer.status == XR_XIR_OK);
        compact_facts(&writer,&written);
        CHECK(!memcmp(storage,compact_expected,length));
        CHECK(emit_finalize(&writer) == !minus);
        CHECK(writer.status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(storage[length] == (minus ? 'q' : '\0') && storage[length+1] == 'q');
        XrCompileResourceStats before,after;
        CHECK(xr_compile_resources_stats(resources,&before) == XR_COMPILE_RESOURCE_OK);
        CHECK(before.work == exact-minus);
        if (minus) {
            compact_actual(&writer);
            CHECK(xr_compile_resources_stats(resources,&after) == XR_COMPILE_RESOURCE_OK);
            CHECK(after.work == before.work && writer.length == length);
            CHECK(!memcmp(storage,compact_expected,length) && storage[length] == 'q');
        }
        xr_compile_resources_free(storage);xr_compile_resources_release(resources);
        CHECK(!runtime_live && !runtime_bytes);
    }
}
static unsigned compact_argument(unsigned *calls) { ++*calls; return 42; }
static void compact_typed_shape(void) {
    static const char expected[] = " state->pc = 42u;\n";
    DecimalInput input = {" state->pc = ","u;\n",42,{0,0},{false,false},false,false};
    DecimalFacts facts = decimal_normal_facts(&input,"42");
    XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,10000};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
    void *memory = NULL;
    CHECK(xr_compile_resources_alloc(resources,128,&memory) == XR_COMPILE_RESOURCE_OK);
    char *storage = memory;memset(storage,'q',128);
    CBuffer buffer = {.text=storage,.capacity=128,.limit=128,.status=XR_XIR_OK,
        .context=&context,.tracking=true};
    unsigned calls = 0;
    XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(resources,&before) == XR_COMPILE_RESOURCE_OK);
    EMIT_U_SHAPE(&buffer, et_d13fcec1f19e2337, " state->pc = %uu;\n",
        " state->pc = ", "u;\n", compact_argument(&calls));
    CHECK(emit_finalize(&buffer));
    CHECK(xr_compile_resources_stats(resources,&after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.work-before.work == facts.work+1 && calls == 1);
    CHECK(!strcmp(storage,expected));compact_facts(&buffer,&facts);
    buffer.status = XR_XIR_BUDGET;
    EMIT_U_SHAPE(&buffer, et_d13fcec1f19e2337, " state->pc = %uu;\n",
        " state->pc = ", "u;\n", compact_argument(&calls));
    XrCompileResourceStats rejected;
    CHECK(xr_compile_resources_stats(resources,&rejected) == XR_COMPILE_RESOURCE_OK);
    CHECK(calls == 2 && rejected.work == after.work && !strcmp(storage,expected));
    xr_compile_resources_free(storage);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static void compact_c11(void) {
    XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,10000};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
    CBuffer buffer = {.limit=1024,.status=XR_XIR_OK,.context=&context,.tracking=true};
    compact_actual(&buffer);
    CHECK(emit_finalize(&buffer) && !strcmp(buffer.text,compact_expected));
    CHECK(buffer.label_used[0] && !buffer.label_used[1] && buffer.tracking);
    XiCgenVerifyResult result;
    CHECK(xr_compile_cgen_verify_output(resources,buffer.text,buffer.length,&result) == XI_CGEN_VERIFY_PASSED);
    xr_compile_resources_free(buffer.text);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static void cgen_compact_code_cases(void) {
    compact_span_boundaries();
    compact_same_owner();
    compact_typed_shape();
    compact_c11();
    puts("Compact code: independent C bytes/typed/fees/partial/sticky/owner/exact-minus1/physical-zero PASS");
}
#endif // XIR_CGEN_COMPACT_CODE_CASES_H
