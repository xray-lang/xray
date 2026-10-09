/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cgen_decimal_shape_cases.h - Certified decimal bytes and real owner fees
 *
 * KEY CONCEPT:
 *   Standard formatting and a bounded suffix reference supply independent
 *   bytes/facts. Actual helpers retain partial admissions and physical zero.
 */
#ifndef XIR_CGEN_DECIMAL_SHAPE_CASES_H
#define XIR_CGEN_DECIMAL_SHAPE_CASES_H

typedef struct DecimalInput {
    const char *first, *last;
    unsigned long long value;
    unsigned match[2];
    bool used[2], wide, measuring;
} DecimalInput;
typedef struct DecimalFacts {
    LiteralSuffix labels[2];
    size_t length;
    uint64_t work;
    bool tracking;
} DecimalFacts;
typedef struct DecimalBound {
    size_t length;
    uint64_t work, available;
    size_t limit;
    XrXirStatus status;
    bool measuring;
} DecimalBound;

static void decimal_call(CBuffer *buffer, const DecimalInput *input) {
    if (input->wide) emit_decimal_ull(buffer,input->value,input->first,strlen(input->first),
        input->last,strlen(input->last));
    else emit_decimal_u(buffer,(unsigned int)input->value,input->first,strlen(input->first),
        input->last,strlen(input->last));
}
static void decimal_number(const DecimalInput *input, char number[32]) {
    int count = input->wide ? snprintf(number,32,"%llu",input->value) :
        snprintf(number,32,"%u",(unsigned int)input->value);
    CHECK(count > 0 && count < 32);
}
/* Facts come from the existing independent bounded suffix reference. The
 * cost trace describes the unchanged detector's actual admitted operations. */
static void decimal_label(DecimalFacts *facts, char byte) {
    if (!facts->tracking) return;
    facts->work += 2;
    if (!facts->labels[0].match && !facts->labels[1].match && byte != 'g') {
        for (unsigned i = 0; i < 2; ++i)
            literal_suffix_byte(&facts->labels[i],literal_patterns[i],literal_lengths[i],byte);
        return;
    }
    for (unsigned i = 0; i < 2; ++i) {
        ++facts->work;
        if (facts->labels[i].found) continue;
        facts->work += 2;
        if (byte != literal_patterns[i][facts->labels[i].match]) ++facts->work;
        literal_suffix_byte(&facts->labels[i],literal_patterns[i],literal_lengths[i],byte);
        facts->work += 2;
        if (facts->labels[i].found) {
            facts->work += 2;
            if (facts->labels[0].found && facts->labels[1].found) {
                ++facts->work;
                facts->tracking = false;
                return;
            }
        }
    }
}
static void decimal_literal_fee(DecimalFacts *facts, const char *text, bool measuring) {
    size_t length = strlen(text);
    if (measuring && !facts->tracking) {
        if (length) ++facts->work;
        facts->length += length;
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        facts->work += 2;
        ++facts->length;
        decimal_label(facts,text[i]);
    }
}
static DecimalFacts decimal_normal_facts(const DecimalInput *input, const char *number) {
    DecimalFacts facts = {0};
    for (unsigned i = 0; i < 2; ++i)
        facts.labels[i] = literal_suffix(i,input->match[i],input->used[i]);
    facts.tracking = !(input->used[0] && input->used[1]);
    decimal_literal_fee(&facts,input->first,input->measuring);
    size_t digits = strlen(number);
    facts.work += digits;
    if (input->measuring && !facts.tracking) {
        ++facts.work;
        facts.length += digits;
    } else for (size_t i = 0; i < digits; ++i) {
        facts.work += 2;
        ++facts.length;
        decimal_label(&facts,number[i]);
    }
    decimal_literal_fee(&facts,input->last,input->measuring);
    return facts;
}
static void decimal_normal_trial(const DecimalInput *input) {
    char number[32],expected[192];
    decimal_number(input,number);
    int count = snprintf(expected,sizeof(expected),"%s%s%s",input->first,number,input->last);
    CHECK(count > 0 && (size_t)count < sizeof(expected));
    DecimalFacts facts = decimal_normal_facts(input,number);
    XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,100000};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
    void *memory = NULL;
    CHECK(xr_compile_resources_alloc(resources,256,&memory) == XR_COMPILE_RESOURCE_OK);
    char *storage = memory;memset(storage,'q',256);
    CBuffer buffer = {.text=storage,.capacity=256,.limit=256,.status=XR_XIR_OK,
        .context=&context,.measuring=input->measuring,.tracking=!(input->used[0]&&input->used[1]),
        .label_match={input->match[0],input->match[1]},.label_used={input->used[0],input->used[1]}};
    XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(resources,&before) == XR_COMPILE_RESOURCE_OK);
    decimal_call(&buffer,input);
    CHECK(buffer.status == XR_XIR_OK && buffer.length == (size_t)count && facts.length == (size_t)count);
    if (!input->measuring) { CHECK(emit_finalize(&buffer));++facts.work; }
    CHECK(xr_compile_resources_stats(resources,&after) == XR_COMPILE_RESOURCE_OK);
    CHECK(after.work-before.work == facts.work && after.live_bytes == before.live_bytes);
    CHECK(buffer.tracking == facts.tracking);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(buffer.label_match[i] == facts.labels[i].match && buffer.label_used[i] == facts.labels[i].found);
    if (!input->measuring) CHECK(!strcmp(storage,expected));
    for (size_t i = input->measuring ? 0 : buffer.length+1; i < 256; ++i) CHECK(storage[i] == 'q');
    xr_compile_resources_free(storage);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static void decimal_all_incoming(void) {
    static const char *const pairs[][2] = {{"",""},{"go","to invalid;goto limit;"},
        {"goto invalid;","goto limit;"},{"goto inv","alid;"},{"","goto invalid;goto limit;"},
        {"goto invalid;goto limit;",""}};
    size_t trials = 0;
    for (unsigned mask = 0; mask < 4; ++mask) {
        unsigned count0 = mask&1 ? 1 : 13, count1 = mask&2 ? 1 : 11;
        for (unsigned a = 0; a < count0; ++a) for (unsigned b = 0; b < count1; ++b) {
            for (unsigned wide = 0; wide < 2; ++wide) for (unsigned value = 0; value < 3; ++value) {
                unsigned long long number = value < 2 ? value : wide ? ULLONG_MAX : UINT_MAX;
                for (unsigned measuring = 0; measuring < 2; ++measuring)
                    for (size_t pair = 0; pair < sizeof(pairs)/sizeof(pairs[0]); ++pair) {
                        DecimalInput input = {pairs[pair][0],pairs[pair][1],number,{a,b},
                            {(mask&1)!=0,(mask&2)!=0},wide!=0,measuring!=0};
                        decimal_normal_trial(&input);++trials;
                    }
            }
        }
    }
    CHECK(trials == 12096);
    printf("Decimal shapes: %zu real typed/168-state/split-label byte and fee trials PASS\n",trials);
}
/* Decimal zero has one digit. Fixed milestones retain the unchanged original
 * mismatch publication: byte stored at3, invalid match stored at11, total17. */
static void decimal_fixed_partial(void) {
    static const unsigned fees[] = {1,1,1,2,1,2,1,2,1,2,1,2};
    for (unsigned wide = 0; wide < 2; ++wide) for (unsigned state = 0; state < 2; ++state) {
        unsigned initial = state ? 11 : 8;
        for (uint64_t available = 0; available <= 18; ++available) {
            uint64_t spent = 0;
            for (size_t i = 0; i < sizeof(fees)/sizeof(fees[0]); ++i) {
                if (fees[i] > available-spent) break;
                spent += fees[i];
            }
            XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,available+2};
            XrCompileResources *resources = NULL;
            CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
            XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
            void *memory = NULL;
            CHECK(xr_compile_resources_alloc(resources,4,&memory) == XR_COMPILE_RESOURCE_OK);
            char *storage = memory;memset(storage,'q',4);
            CBuffer buffer = {.text=storage,.capacity=4,.limit=4,.status=XR_XIR_OK,
                .context=&context,.tracking=true,.label_match={initial,0}};
            DecimalInput input = {"","",0,{initial,0},{false,false},wide!=0,false};
            decimal_call(&buffer,&input);
            XrCompileResourceStats stats;
            CHECK(xr_compile_resources_stats(resources,&stats) == XR_COMPILE_RESOURCE_OK);
            CHECK(stats.work == spent+2 && buffer.length == (spent>=3 ? 1u : 0u));
            CHECK(buffer.label_match[0] == (spent>=11 ? 0u : initial));
            CHECK(!buffer.label_match[1] && !buffer.label_used[0] && !buffer.label_used[1] && buffer.tracking);
            CHECK(buffer.status == (available>=17 ? XR_XIR_OK : XR_XIR_BUDGET));
            if (buffer.length) CHECK(storage[0] == '0');
            for (size_t i = buffer.length; i < 4; ++i) CHECK(storage[i] == 'q');
            xr_compile_resources_free(storage);xr_compile_resources_release(resources);
            CHECK(!runtime_live && !runtime_bytes);
        }
    }
}
static bool decimal_bound_debit(DecimalBound *bound) {
    if (bound->status != XR_XIR_OK) return false;
    if (bound->work == bound->available) { bound->status=XR_XIR_BUDGET;return false; }
    ++bound->work;return true;
}
static bool decimal_bound_byte(DecimalBound *bound) {
    if (!decimal_bound_debit(bound)) return false;
    if (bound->length+2 > bound->limit) { bound->status=XR_XIR_BUDGET;return false; }
    if (!decimal_bound_debit(bound)) return false;
    ++bound->length;return true;
}
static void decimal_bound_literal(DecimalBound *bound) {
    if (bound->status != XR_XIR_OK) return;
    if (!bound->measuring) { (void)decimal_bound_byte(bound);return; }
    if (bound->length+1 >= bound->limit) { bound->status=XR_XIR_BUDGET;return; }
    if (decimal_bound_debit(bound)) ++bound->length;
}
/* Independent p42s arithmetic schedules the two digit creations before the
 * first digit read. It has no table/format code or product output as oracle. */
static DecimalBound decimal_bound_model(bool measuring, size_t cap, uint64_t available) {
    DecimalBound bound = {0,0,available,cap,XR_XIR_OK,measuring};
    decimal_bound_literal(&bound);
    if (bound.status != XR_XIR_OK) return bound;
    for (unsigned digit = 0; digit < 2; ++digit) if (!decimal_bound_debit(&bound)) return bound;
    if (measuring) {
        if (bound.length >= cap || 2 >= cap-bound.length) bound.status=XR_XIR_BUDGET;
        else if (decimal_bound_debit(&bound)) bound.length+=2;
    } else for (unsigned digit = 0; digit < 2; ++digit)
        if (!decimal_bound_byte(&bound)) return bound;
    decimal_bound_literal(&bound);
    if (!measuring && bound.status == XR_XIR_OK) (void)decimal_bound_debit(&bound);
    return bound;
}
static void decimal_bounded_work_and_cap(void) {
    static const char expected[] = "p42s";
    for (unsigned wide = 0; wide < 2; ++wide) for (unsigned measuring = 0; measuring < 2; ++measuring) {
        for (size_t cap = 0; cap <= 5; ++cap) for (uint64_t available = 0; available <= 12; ++available) {
            DecimalBound expected_fee = decimal_bound_model(measuring!=0,cap,available);
            XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,available+2};
            XrCompileResources *resources = NULL;
            CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
            XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
            void *memory = NULL;
            CHECK(xr_compile_resources_alloc(resources,8,&memory) == XR_COMPILE_RESOURCE_OK);
            char *storage = memory;memset(storage,'q',8);
            CBuffer buffer = {.text=storage,.capacity=8,.limit=cap,.status=XR_XIR_OK,.context=&context,
                .measuring=measuring!=0,.label_used={true,true}};
            DecimalInput input = {"p","s",42,{0,0},{true,true},wide!=0,measuring!=0};
            decimal_call(&buffer,&input);
            if (!measuring && buffer.status == XR_XIR_OK) (void)emit_finalize(&buffer);
            XrCompileResourceStats stats;
            CHECK(xr_compile_resources_stats(resources,&stats) == XR_COMPILE_RESOURCE_OK);
            CHECK(stats.work == expected_fee.work+2 && buffer.status == expected_fee.status);
            CHECK(buffer.length == expected_fee.length && !buffer.tracking && buffer.label_used[0] && buffer.label_used[1]);
            if (!measuring) CHECK(!memcmp(storage,expected,buffer.length));
            size_t used = measuring ? 0 : buffer.length;
            if (!measuring && buffer.status == XR_XIR_OK) CHECK(storage[used++] == '\0');
            for (size_t i = used; i < 8; ++i) CHECK(storage[i] == 'q');
            xr_compile_resources_free(storage);xr_compile_resources_release(resources);
            CHECK(!runtime_live && !runtime_bytes);
        }
    }
}
static unsigned decimal_argument(unsigned *calls) { ++*calls;return UINT_MAX; }
static unsigned long long decimal_wide_argument(unsigned *calls) { ++*calls;return ULLONG_MAX; }
static CBuffer *decimal_buffer_argument(CBuffer *buffer, unsigned *calls) { ++*calls;return buffer; }
static void decimal_macro_single_call(void) {
    XrCompileResourceLimits limits = {UINT64_C(1)<<20,UINT64_C(1)<<20,10000};
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits,&resources) == XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context = {resources,xr_xir_compile_default_limits()};
    CBuffer buffer = {.limit=128,.status=XR_XIR_OK,.context=&context};
    unsigned calls = 0,buffer_calls = 0;
    char expected[48];
    CHECK(snprintf(expected,sizeof(expected),"%uu,",UINT_MAX) > 0);
    EMIT_U_SHAPE(decimal_buffer_argument(&buffer,&buffer_calls),et_84635448af4d156a,
        "%uu,","","u,",decimal_argument(&calls));
    CHECK(emit_finalize(&buffer) && calls == 1 && buffer_calls == 1 && !strcmp(buffer.text,expected));
    XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(resources,&before) == XR_COMPILE_RESOURCE_OK);
    buffer.status=XR_XIR_BUDGET;
    EMIT_U_SHAPE(decimal_buffer_argument(&buffer,&buffer_calls),et_84635448af4d156a,
        "%uu,","","u,",decimal_argument(&calls));
    CHECK(xr_compile_resources_stats(resources,&after) == XR_COMPILE_RESOURCE_OK);
    CHECK(calls == 2 && buffer_calls == 2 && after.work == before.work && !strcmp(buffer.text,expected));
    xr_compile_resources_free(buffer.text);
    buffer=(CBuffer){.limit=128,.status=XR_XIR_OK,.context=&context};
    CHECK(snprintf(expected,sizeof(expected),"INT64_C(%llu)",ULLONG_MAX) > 0);
    EMIT_ULL_SHAPE(decimal_buffer_argument(&buffer,&buffer_calls),et_42b80bce7e8ab13e,
        "INT64_C(%llu)","INT64_C(",")",decimal_wide_argument(&calls));
    CHECK(emit_finalize(&buffer) && calls == 3 && buffer_calls == 3 && !strcmp(buffer.text,expected));
    CHECK(xr_compile_resources_stats(resources,&before) == XR_COMPILE_RESOURCE_OK);
    buffer.status=XR_XIR_BUDGET;
    EMIT_ULL_SHAPE(decimal_buffer_argument(&buffer,&buffer_calls),et_42b80bce7e8ab13e,
        "INT64_C(%llu)","INT64_C(",")",decimal_wide_argument(&calls));
    CHECK(xr_compile_resources_stats(resources,&after) == XR_COMPILE_RESOURCE_OK);
    CHECK(calls == 4 && buffer_calls == 4 && after.work == before.work && !strcmp(buffer.text,expected));
    xr_compile_resources_free(buffer.text);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static const char decimal_long_first[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static void decimal_long_input(DecimalInput *input) {
    *input=(DecimalInput){decimal_long_first,"goto invalid;goto limit;",ULLONG_MAX,
        {0,0},{false,false},true,false};
}
static void decimal_check_initialized(const CBuffer *buffer, const DecimalInput *input, bool facts) {
    char number[32],expected[384];decimal_number(input,number);
    int count=snprintf(expected,sizeof(expected),"%s%s%s",input->first,number,input->last);
    CHECK(count>0 && (size_t)count<sizeof(expected) && buffer->length<=(size_t)count);
    CHECK(!buffer->length || !memcmp(buffer->text,expected,buffer->length));
    if (facts) for (unsigned i=0;i<2;++i) {
        LiteralSuffix ref=literal_suffix(i,0,false);
        for(size_t at=0;at<buffer->length;++at)
            literal_suffix_byte(&ref,literal_patterns[i],literal_lengths[i],expected[at]);
        CHECK(buffer->label_match[i]==ref.match && buffer->label_used[i]==ref.found);
    }
}
static void decimal_resize_faults(void) {
    DecimalInput input;decimal_long_input(&input);
    size_t sites=0;
    for(size_t fault=SIZE_MAX;;) {
        XrCompileResourceLimits limits={UINT64_C(1)<<26,UINT64_C(1)<<23,128000000};
        XrCompileResources *resources=NULL;
        CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
        XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
        XrCompileResourceStats before,after;
        CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
        CBuffer buffer={.limit=sizeof(decimal_long_first)+20+24,.context=&context,.status=XR_XIR_OK,.tracking=true};
        runtime_attempts=0;runtime_fail_at=fault;
        decimal_call(&buffer,&input);
        runtime_fail_at=SIZE_MAX;
        if(fault==SIZE_MAX) { CHECK(buffer.status==XR_XIR_OK && emit_finalize(&buffer));sites=runtime_attempts; }
        else CHECK(buffer.status==XR_XIR_OUT_OF_MEMORY);
        decimal_check_initialized(&buffer,&input,true);
        xr_compile_resources_free(buffer.text);
        CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==before.live_bytes && after.work>=before.work);
        xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
        if(fault==SIZE_MAX)fault=0;else if(++fault==sites)break;
    }
    CHECK(sites>1);
    printf("Decimal shapes: %zu actual resize OOM frontiers, physical zero PASS\n",sites);
}
static XrCompileResourceStats decimal_census(void) {
    DecimalInput input;decimal_long_input(&input);
    XrCompileResourceLimits limits={UINT64_C(1)<<26,UINT64_C(1)<<23,128000000};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    CBuffer buffer={.limit=sizeof(decimal_long_first)+20+24,.context=&context,.status=XR_XIR_OK,.tracking=true};
    decimal_call(&buffer,&input);CHECK(buffer.status==XR_XIR_OK && emit_finalize(&buffer));
    decimal_check_initialized(&buffer,&input,true);
    XrCompileResourceStats stats;
    CHECK(xr_compile_resources_stats(resources,&stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_free(buffer.text);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
    return stats;
}
static void decimal_axis_case(unsigned axis, unsigned minus, const XrCompileResourceStats *census) {
    DecimalInput input;decimal_long_input(&input);
    XrCompileResourceLimits limits={UINT64_C(1)<<26,UINT64_C(1)<<23,128000000};
    if(axis==0)limits.allocated_bytes=census->allocated_bytes-minus;
    else if(axis==1)limits.live_bytes=census->peak_bytes-minus;
    else limits.work=census->work-minus;
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompileResourceStats before,after;
    CHECK(xr_compile_resources_stats(resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(unsigned attempt=0;attempt<3;++attempt) {
        CBuffer buffer={.limit=sizeof(decimal_long_first)+20+24,.context=&context,.status=XR_XIR_OK,.tracking=true};
        decimal_call(&buffer,&input);
        if(buffer.status==XR_XIR_OK)(void)emit_finalize(&buffer);
        bool pass=!minus && (!attempt || axis==1);
        CHECK(buffer.status==(pass ? XR_XIR_OK : XR_XIR_BUDGET));
        decimal_check_initialized(&buffer,&input,true);
        xr_compile_resources_free(buffer.text);
        CHECK(xr_compile_resources_stats(resources,&after)==XR_COMPILE_RESOURCE_OK);
        CHECK(after.live_bytes==before.live_bytes && after.work>=before.work && after.allocated_bytes>=before.allocated_bytes);
        before=after;
    }
    xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
}
static void decimal_resource_axes(void) {
    XrCompileResourceStats census=decimal_census();
    for(unsigned axis=0;axis<3;++axis)for(unsigned minus=0;minus<2;++minus)
        decimal_axis_case(axis,minus,&census);
    printf("Decimal shapes: actual census allocated=%llu peak=%llu work=%llu, six axes and same-owner retries PASS\n",
        (unsigned long long)census.allocated_bytes,(unsigned long long)census.peak_bytes,(unsigned long long)census.work);
}
static void cgen_decimal_shape_cases(void) {
    decimal_all_incoming();
    decimal_fixed_partial();
    decimal_bounded_work_and_cap();
    decimal_macro_single_call();
    decimal_resize_faults();
    decimal_resource_axes();
    puts("Decimal shapes: typed bytes/fees/partial/sentinel/FI/three-axis/physical-zero PASS");
}
#endif // XIR_CGEN_DECIMAL_SHAPE_CASES_H
