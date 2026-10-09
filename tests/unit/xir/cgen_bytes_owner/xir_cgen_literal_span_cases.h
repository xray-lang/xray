/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cgen_literal_span_cases.h - Independent static-span and real fee controls
 *
 * KEY CONCEPT:
 *   A bounded suffix reference checks every immutable table prefix. Actual
 *   helper trials retain finite-owner fees, failure prefixes and physical zero.
 */
#ifndef XIR_CGEN_LITERAL_SPAN_CASES_H
#define XIR_CGEN_LITERAL_SPAN_CASES_H

typedef struct LiteralSuffix {
    char tail[13];
    unsigned length, match;
    bool found;
} LiteralSuffix;
typedef struct LiteralFixture {
    EmitSpan span;
    uint8_t tables[2][256];
} LiteralFixture;
typedef struct LiteralTrial {
    const EmitSpan *span;
    unsigned match[2];
    bool found[2], measuring;
    size_t limit;
    uint64_t work;
} LiteralTrial;
typedef struct LiteralExpected {
    size_t prefix;
    uint64_t charged;
    XrXirStatus status;
} LiteralExpected;
static const char *const literal_patterns[2] = {"goto invalid;", "goto limit;"};
static const unsigned literal_lengths[2] = {13, 11};

/* This reference retains a bounded suffix, independently of the product's
 * incremental match state and compressed metadata selection. */
static void literal_suffix_byte(LiteralSuffix *state, const char *pattern, unsigned length, char byte) {
    if (state->found) return;
    if (state->length == length) {
        memmove(state->tail, state->tail + 1, length - 1);
        --state->length;
    }
    state->tail[state->length++] = byte;
    if (state->length == length && !memcmp(state->tail, pattern, length)) {
        state->found = true;
        state->match = 0;
        return;
    }
    state->match = 0;
    for (unsigned count = state->length < length ? state->length : length - 1; count; --count) {
        if (!memcmp(state->tail + state->length - count, pattern, count)) {
            state->match = count;
            return;
        }
    }
}
static LiteralSuffix literal_suffix(unsigned pattern, unsigned match, bool found) {
    LiteralSuffix state = {{0}, match, match, found};
    CHECK(match < literal_lengths[pattern] && (!found || !match));
    memcpy(state.tail, literal_patterns[pattern], match);
    return state;
}
static uint8_t literal_reference(unsigned pattern, const char *text, size_t length, unsigned match) {
    LiteralSuffix state = literal_suffix(pattern, match, false);
    for (size_t i = 0; i < length; ++i)
        literal_suffix_byte(&state, literal_patterns[pattern], literal_lengths[pattern], text[i]);
    return state.found ? 128 : (uint8_t)state.match;
}
static void literal_fixture(LiteralFixture *fixture, const char *text) {
    size_t length = strlen(text);
    CHECK(length <= 40);
    fixture->span.text = text;
    fixture->span.length = length;
    for (unsigned pattern = 0; pattern < 2; ++pattern) {
        unsigned count = literal_lengths[pattern];
        size_t width = length + 1 < count ? length + 1 : count;
        CHECK(count * width + length + 1 <= sizeof(fixture->tables[pattern]));
        for (unsigned initial = 0; initial < count; ++initial)
            for (size_t prefix = 0; prefix < width; ++prefix)
                fixture->tables[pattern][initial * width + prefix] =
                    literal_reference(pattern, text, prefix, initial);
        for (size_t prefix = 0; prefix <= length; ++prefix)
            fixture->tables[pattern][count * width + prefix] = literal_reference(pattern, text, prefix, 0);
        fixture->span.table[pattern] = fixture->tables[pattern];
    }
}
static void literal_all_table_prefixes(void) {
    size_t checks = 0, expected = 0;
    for (size_t span = 0; span < EMIT_SPAN_COUNT; ++span) {
        const EmitSpan *value = &emit_literal_spans[span];
        expected += (value->length + 1) * 24;
        for (unsigned pattern = 0; pattern < 2; ++pattern) {
            unsigned count = literal_lengths[pattern];
            size_t width = value->length + 1 < count ? value->length + 1 : count;
            for (unsigned initial = 0; initial < count; ++initial) {
                LiteralSuffix state = literal_suffix(pattern, initial, false);
                for (size_t prefix = 0; prefix <= value->length; ++prefix) {
                    if (prefix) literal_suffix_byte(&state, literal_patterns[pattern], count,
                                                   value->text[prefix - 1]);
                    uint8_t short_value = value->table[pattern][initial * width +
                        (prefix < count ? prefix : count - 1)];
                    uint8_t zero_value = value->table[pattern][count * width + prefix];
                    uint8_t actual = prefix < count || (short_value & 128) ? short_value : zero_value;
                    CHECK(actual == (state.found ? 128 : state.match));
                    ++checks;
                }
            }
        }
    }
    CHECK(checks == expected);
    printf("Literal metadata: %zu independent incoming-state/prefix checks PASS\n", checks);
}
/* The expectation records actual operations at the admitted boundaries,
 * rather than treating the commit reservation as a per-byte scan. */
static LiteralExpected literal_fee_prefix(const LiteralTrial *trial, bool tracking) {
    LiteralExpected result = {0, 0, XR_XIR_OK};
    uint64_t begin = tracking ? 13 : 2;
    if (trial->work < begin) {
        result.charged = trial->work;
        result.status = XR_XIR_BUDGET;
        return result;
    }
    result.charged = begin;
    if (tracking) {
        if (trial->work - begin < 15) { result.status = XR_XIR_BUDGET; return result; }
        result.charged += 15;
    }
    if (trial->measuring) {
        size_t available = trial->limit ? trial->limit - 1 : 0;
        size_t advanced = trial->span->length < available ? trial->span->length : available;
        if (advanced) {
            if (result.charged == trial->work) { result.status = XR_XIR_BUDGET; return result; }
            ++result.charged;
            result.prefix = advanced;
        }
        if (result.prefix < trial->span->length) result.status = XR_XIR_BUDGET;
        return result;
    }
    while (result.prefix < trial->span->length) {
        if (result.charged == trial->work) { result.status = XR_XIR_BUDGET; break; }
        ++result.charged;
        if (result.prefix + 2 > trial->limit || result.charged == trial->work) {
            result.status = XR_XIR_BUDGET;
            break;
        }
        ++result.charged;
        ++result.prefix;
    }
    return result;
}
static void literal_check_labels(const CBuffer *buffer, const LiteralTrial *trial, size_t prefix) {
    bool tracking = !(trial->found[0] && trial->found[1]);
    bool found[2];
    for (unsigned i = 0; i < 2; ++i) {
        LiteralSuffix state = literal_suffix(i, trial->match[i], trial->found[i]);
        if (tracking)
            for (size_t p = 0; p < prefix; ++p)
                literal_suffix_byte(&state, literal_patterns[i], literal_lengths[i], trial->span->text[p]);
        found[i] = state.found;
        CHECK(buffer->label_match[i] == state.match && buffer->label_used[i] == state.found);
    }
    CHECK(buffer->tracking == !(found[0] && found[1]));
}
static void literal_trial(const LiteralTrial *trial) {
    XrCompileResourceLimits limits = {UINT64_C(1)<<20, UINT64_C(1)<<20, trial->work + 2};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources, xr_xir_compile_default_limits()};
    void *memory = NULL;
    CHECK(xr_compile_resources_alloc(resources, 128, &memory) == XR_COMPILE_RESOURCE_OK);
    char *storage = memory;
    memset(storage, 'q', 128);
    bool tracking = !(trial->found[0] && trial->found[1]);
    CBuffer buffer = {.text=storage, .capacity=128, .limit=trial->limit,
        .status=XR_XIR_OK, .context=&context, .measuring=trial->measuring, .tracking=tracking,
        .label_match={trial->match[0],trial->match[1]}, .label_used={trial->found[0],trial->found[1]}};
    XrCompileResourceStats before, after;
    CHECK(xr_compile_resources_stats(resources, &before) == XR_COMPILE_RESOURCE_OK);
    LiteralExpected expected = literal_fee_prefix(trial, tracking);
    emit_static_span(&buffer, trial->span);
    CHECK(xr_compile_resources_stats(resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.work - before.work == expected.charged);
    CHECK(buffer.status == expected.status && buffer.length == expected.prefix);
    CHECK(after.live_bytes == before.live_bytes && after.allocated_bytes == before.allocated_bytes);
    if (!trial->measuring) CHECK(!memcmp(storage, trial->span->text, expected.prefix));
    for (size_t p = trial->measuring ? 0 : expected.prefix; p < 128; ++p) CHECK(storage[p] == 'q');
    literal_check_labels(&buffer, trial, expected.prefix);
    xr_compile_resources_free(storage);
    xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static void literal_bounded_partials(void) {
    static const char *const inputs[] = {"", "go", "to inv", "alid;", "goto invalid;goto limit;"};
    static const unsigned states[][2] = {{0,0},{1,0},{8,0},{11,0},{12,10}};
    for (size_t value = 0; value < sizeof(inputs)/sizeof(inputs[0]); ++value) {
        LiteralFixture fixture;
        literal_fixture(&fixture, inputs[value]);
        for (size_t state = 0; state < sizeof(states)/sizeof(states[0]); ++state) {
            for (unsigned mask = 0; mask < 4; ++mask) {
                for (unsigned measuring = 0; measuring < 2; ++measuring) {
                    uint64_t complete = measuring ? (fixture.span.length ? 29 : 28) : 28 + 2*fixture.span.length;
                    uint64_t work[] = {0,1,2,12,13,14,27,28,29,complete-1,complete,complete+1};
                    size_t caps[] = {0,1,fixture.span.length,fixture.span.length+1};
                    for (size_t cap = 0; cap < sizeof(caps)/sizeof(caps[0]); ++cap) {
                        for (size_t budget = 0; budget < sizeof(work)/sizeof(work[0]); ++budget) {
                            LiteralTrial trial = {&fixture.span,
                                {(mask&1) ? 0 : states[state][0], (mask&2) ? 0 : states[state][1]},
                                {(mask&1)!=0,(mask&2)!=0},measuring!=0,caps[cap],work[budget]};
                            literal_trial(&trial);
                        }
                    }
                }
            }
        }
    }
}
static void literal_same_owner_exact(void) {
    LiteralFixture fixture;
    literal_fixture(&fixture, "goto invalid;goto limit;");
    size_t length = fixture.span.length;
    uint64_t exact = 1 + 29 + 1 + 28 + 2*length + 1;
    for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,exact-minus};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
        CBuffer measure = {.limit=length+1,.status=XR_XIR_OK,.context=&context,.measuring=true,.tracking=true};
        emit_static_span(&measure,&fixture.span);
        CHECK(measure.status == XR_XIR_OK && measure.length == length && !measure.tracking);
        void *memory = NULL;
        CHECK(xr_compile_resources_alloc(resources,length+2,&memory) == XR_COMPILE_RESOURCE_OK);
        char *storage = memory;
        memset(storage,'q',length+2);
        CBuffer writer = {.text=storage,.capacity=length+2,.limit=length+1,
            .status=XR_XIR_OK,.context=&context,.tracking=true};
        emit_static_span(&writer,&fixture.span);
        CHECK(writer.status == XR_XIR_OK && writer.length == length && !writer.tracking);
        CHECK(emit_finalize(&writer) == !minus);
        CHECK(writer.status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(storage[length] == (minus ? 'q' : '\0') && storage[length+1] == 'q');
        XrCompileResourceStats stats;
        CHECK(xr_compile_resources_stats(resources,&stats) == XR_COMPILE_RESOURCE_OK);
        CHECK(stats.work == exact-minus && !memcmp(storage,fixture.span.text,length));
        xr_compile_resources_free(storage);xr_compile_resources_release(resources);
        CHECK(!runtime_live && !runtime_bytes);
    }
}
/* Fixed operation milestones retain the original dynamic detector's partial
 * publication ordering, including a first emitted a/d that extends a match. */
static void literal_hex_partial(void) {
    static const unsigned fees[] = {1,1,1,1,2,1,2,2,1,2,1,2,1,1,2,1,2,1,2,1,2,1,2};
    for (unsigned which = 0; which < 2; ++which) {
        unsigned initial = which ? 11 : 8;
        const char *expected = which ? "d0" : "a0";
        for (uint64_t available = 0; available <= 34; ++available) {
            uint64_t spent = 0;
            for (size_t i = 0; i < sizeof(fees)/sizeof(fees[0]); ++i) {
                if (fees[i] > available-spent) break;
                spent += fees[i];
            }
            size_t prefix = spent >= 19 ? 2 : spent >= 4 ? 1 : 0;
            unsigned match = spent >= 27 ? 0 : spent >= 11 ? initial+1 : initial;
            XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,available+2};
            XrCompileResources *resources = NULL;
            CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
            XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
            void *memory = NULL;
            CHECK(xr_compile_resources_alloc(resources,4,&memory) == XR_COMPILE_RESOURCE_OK);
            char *storage = memory;memset(storage,'q',4);
            CBuffer buffer = {.text=storage,.capacity=4,.limit=4,.context=&context,
                .status=XR_XIR_OK,.tracking=true,.label_match={initial,0}};
            emit_unsigned(&buffer,which ? 0xd0 : 0xa0,16,2);
            XrCompileResourceStats stats;
            CHECK(xr_compile_resources_stats(resources,&stats) == XR_COMPILE_RESOURCE_OK);
            CHECK(stats.work == spent+2 && buffer.length == prefix && buffer.label_match[0] == match);
            CHECK(buffer.status == (available >= 33 ? XR_XIR_OK : XR_XIR_BUDGET));
            CHECK(buffer.label_match[1] == 0 && !buffer.label_used[0] && !buffer.label_used[1] && buffer.tracking);
            CHECK(!memcmp(storage,expected,prefix));
            for (size_t p = prefix; p < 4; ++p) CHECK(storage[p] == 'q');
            xr_compile_resources_free(storage);xr_compile_resources_release(resources);
            CHECK(!runtime_live && !runtime_bytes);
        }
    }
}
static int literal_argument_d(unsigned *calls) { ++*calls;return INT_MIN; }
static unsigned literal_argument_u(unsigned *calls) { ++*calls;return UINT_MAX; }
static unsigned long long literal_argument_llu(unsigned *calls) { ++*calls;return UINT64_MAX; }
static unsigned literal_argument_hex(unsigned *calls) { ++*calls;return 65536; }
static const char *literal_argument_s(unsigned *calls) { ++*calls;return "goto invalid;goto limit;"; }
static unsigned literal_find(const char *text) {
    unsigned i = 0;
    for (; i < EMIT_SPAN_COUNT; ++i)
        if (!strcmp(emit_literal_spans[i].text,text)) return i;
    CHECK(i < EMIT_SPAN_COUNT);
    return 0;
}
static void literal_typed_arguments(void) {
    EmitPart parts[] = {{EP_D,0},{EP_TEXT,0},{EP_U,0},{EP_TEXT,0},{EP_LLU,0},
        {EP_TEXT,0},{EP_HEX,0},{EP_TEXT,0},{EP_S,0}};
    for (size_t i = 1; i < 8; i += 2) parts[i].span = literal_find("\n");
    const EmitTemplate format = {parts,sizeof(parts)/sizeof(parts[0])};
    static const char expected[] = "-2147483648\n4294967295\n18446744073709551615\n10000\ngoto invalid;goto limit;";
    unsigned calls[5] = {0};
    XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,10000};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
    void *memory = NULL;
    CHECK(xr_compile_resources_alloc(resources,128,&memory) == XR_COMPILE_RESOURCE_OK);
    char *storage = memory;memset(storage,'q',128);
    CBuffer buffer = {.text=storage,.capacity=128,.limit=128,.context=&context,.status=XR_XIR_OK};
    XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(resources,&before) == XR_COMPILE_RESOURCE_OK);
    emit_template(&buffer,&format,literal_argument_d(calls),literal_argument_u(calls+1),
        literal_argument_llu(calls+2),literal_argument_hex(calls+3),literal_argument_s(calls+4));
    CHECK(emit_finalize(&buffer));
    CHECK(xr_compile_resources_stats(resources,&after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.work-before.work == 240 && buffer.length == strlen(expected) && !strcmp(storage,expected));
    for (unsigned i = 0; i < 5; ++i) CHECK(calls[i] == 1);
    buffer.status = XR_XIR_BUDGET;
    emit_template(&buffer,&format,literal_argument_d(calls),literal_argument_u(calls+1),
        literal_argument_llu(calls+2),literal_argument_hex(calls+3),literal_argument_s(calls+4));
    XrCompileResourceStats rejected;
    CHECK(xr_compile_resources_stats(resources,&rejected) == XR_COMPILE_RESOURCE_OK);
    CHECK(rejected.work == after.work && !strcmp(storage,expected));
    for (unsigned i = 0; i < 5; ++i) CHECK(calls[i] == 2);
    xr_compile_resources_free(storage);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static void literal_invalid_and_failed(void) {
    LiteralFixture fixture;literal_fixture(&fixture,"go");
    XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,10000};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
    for (unsigned kind = 0; kind < 4; ++kind) {
        CBuffer buffer = {.context=&context,.limit=32,.status=XR_XIR_OK,.tracking=true};
        buffer.label_match[0] = kind == 0 ? 13 : kind == 1 ? 1 : 0;
        buffer.label_match[1] = kind == 2 ? 11 : 0;
        buffer.label_used[0] = kind == 1;
        EmitSpan span = fixture.span;
        if (kind == 3) span.table[1] = NULL;
        emit_static_span(&buffer,&span);
        CHECK(buffer.status == XR_XIR_BAD_STRUCTURE && !buffer.length && !buffer.text);
        CHECK(buffer.label_match[0] == (kind == 0 ? 13u : kind == 1 ? 1u : 0u));
    }
    XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(resources,&before) == XR_COMPILE_RESOURCE_OK);
    CBuffer failed = {.context=&context,.status=XR_XIR_BUDGET,.tracking=true,.label_match={8,0}};
    emit_static_span(&failed,NULL);
    CHECK(xr_compile_resources_stats(resources,&after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.work == before.work && failed.label_match[0] == 8 && !failed.length && !failed.text);
    xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
}
static void literal_actual_resize_faults(void) {
    const EmitSpan *span = emit_literal_spans;
    for (size_t i = 1; i < EMIT_SPAN_COUNT; ++i)
        if (emit_literal_spans[i].length > span->length) span = emit_literal_spans+i;
    size_t sites = 0;
    for (size_t fault = SIZE_MAX;;) {
        XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,UINT64_C(1)<<25};
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
        XrCompileResourceStats before,after;
        CHECK(xr_compile_resources_stats(resources,&before) == XR_COMPILE_RESOURCE_OK);
        CBuffer buffer = {.limit=span->length+1,.context=&context,.status=XR_XIR_OK,.tracking=true};
        runtime_attempts=0;runtime_fail_at=fault;
        emit_static_span(&buffer,span);
        runtime_fail_at=SIZE_MAX;
        CHECK(xr_compile_resources_stats(resources,&after) == XR_COMPILE_RESOURCE_OK);
        CHECK(after.work >= before.work+28+2*buffer.length);
        LiteralTrial trial = {span,{0,0},{false,false},false,span->length+1,0};
        literal_check_labels(&buffer,&trial,buffer.length);
        CHECK(!buffer.length || !memcmp(buffer.text,span->text,buffer.length));
        if (fault == SIZE_MAX) {
            if (buffer.status != XR_XIR_OK || buffer.length != span->length)
                fprintf(stderr,"literal resize normal span=%zu bytes=%zu status=%u prefix=%zu capacity=%zu limit=%zu attempts=%zu header=%zu allocated=%llu peak=%llu work=%llu live=%llu label0=%u/%u label1=%u/%u tracking=%u\n",
                    (size_t)(span-emit_literal_spans),span->length,(unsigned)buffer.status,
                    buffer.length,buffer.capacity,buffer.limit,runtime_attempts,sizeof(CompileAllocation),
                    (unsigned long long)after.allocated_bytes,(unsigned long long)after.peak_bytes,
                    (unsigned long long)after.work,(unsigned long long)after.live_bytes,
                    buffer.label_match[0],(unsigned)buffer.label_used[0],
                    buffer.label_match[1],(unsigned)buffer.label_used[1],(unsigned)buffer.tracking);
            CHECK(buffer.status == XR_XIR_OK && buffer.length == span->length);
            sites=runtime_attempts;
            CHECK(emit_finalize(&buffer));
        } else CHECK(buffer.status == XR_XIR_OUT_OF_MEMORY);
        xr_compile_resources_free(buffer.text);
        CHECK(xr_compile_resources_stats(resources,&after) == XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes == before.live_bytes && after.work >= before.work);
        xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
        if (fault == SIZE_MAX) fault=0;else if (++fault == sites) break;
    }
    CHECK(sites > 1);
    printf("Literal spans: %zu actual resize OOM frontiers and physical zero PASS\n",sites);
}
static void literal_cross_call_c11(void) {
    LiteralFixture first,last;
    literal_fixture(&first,"int span_case(void) { ");
    literal_fixture(&last,"alid: return 1; limit: return 2; }\n");
    LiteralFixture middle;literal_fixture(&middle,"goto invalid;goto limit;inv");
    static const char expected[] = "int span_case(void) { goto invalid;goto limit;invalid: return 1; limit: return 2; }\n";
    XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,10000};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
    CBuffer buffer = {.limit=256,.context=&context,.status=XR_XIR_OK,.tracking=true};
    emit_static_span(&buffer,&first.span);
    append(&buffer,"%s",middle.span.text);
    emit_static_span(&buffer,&last.span);
    CHECK(emit_finalize(&buffer) && !strcmp(buffer.text,expected));
    CHECK(buffer.label_used[0] && buffer.label_used[1] && !buffer.tracking);
    XiCgenVerifyResult result;
    CHECK(xr_compile_cgen_verify_output(resources,buffer.text,buffer.length,&result) == XI_CGEN_VERIFY_PASSED);
    xr_compile_resources_free(buffer.text);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static void cgen_literal_span_cases(void) {
    literal_all_table_prefixes();
    literal_bounded_partials();
    literal_same_owner_exact();
    literal_hex_partial();
    literal_typed_arguments();
    literal_invalid_and_failed();
    literal_actual_resize_faults();
    literal_cross_call_c11();
    puts("Literal spans: finite fee/partial/sticky/exact-minus1/sentinel/typed controls PASS");
}
#endif // XIR_CGEN_LITERAL_SPAN_CASES_H
