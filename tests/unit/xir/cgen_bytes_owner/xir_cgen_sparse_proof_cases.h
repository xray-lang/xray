/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cgen_sparse_proof_cases.h - Independent bounded proof emission controls
 *
 * KEY CONCEPT:
 *   Fixed raw bytes and an independent C11 decoder verify complete proof
 *   storage, finite fees, byte/work rejection and physical release.
 */
#ifndef XIR_CGEN_SPARSE_PROOF_CASES_H
#define XIR_CGEN_SPARSE_PROOF_CASES_H

typedef struct SparseProofCase {
    const char *name;
    const uint8_t *bytes;
    size_t length;
} SparseProofCase;
typedef struct SparseProofOracle {
    size_t bytes, elements;
    uint64_t measure_work, write_work;
} SparseProofOracle;

static const uint8_t sparse_one_zero[] = {0};
static const uint8_t sparse_all_zero[32] = {0};
static const uint8_t sparse_one_nonzero[] = {255};
static const uint8_t sparse_first_zero[32] = {[30]=255, 1};
static const uint8_t sparse_tail_zero[55] = {88,82,67,72,75};
static const uint8_t sparse_short_zero[] = {1,0,0,2};
static const uint8_t sparse_long_zero[35] = {1,[32]=255,2,3};
static const uint8_t sparse_alternating[] = {
    0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,
    0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,
    0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,
    0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255,
    0,255,0,255,0,255,0,255,0,255,0,255,0,255,0,255
};
static const uint8_t sparse_consecutive[] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,
    81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,
    113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,
    129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,
    145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,
    161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,
    177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,
    193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,208,
    209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,
    225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,
    241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
};
static const uint8_t sparse_decimal_boundary[999] = {2,[98]=3,[998]=4};
static const uint8_t sparse_binary_nul[] = {
    88,82,67,72,75,0,0,0,25,0,0,0,70,0,0,0
};
static const SparseProofCase sparse_cases[] = {
    {"one-zero",sparse_one_zero,sizeof(sparse_one_zero)},
    {"all-zero",sparse_all_zero,sizeof(sparse_all_zero)},
    {"one-nonzero",sparse_one_nonzero,sizeof(sparse_one_nonzero)},
    {"first-zero",sparse_first_zero,sizeof(sparse_first_zero)},
    {"tail-zero",sparse_tail_zero,sizeof(sparse_tail_zero)},
    {"short-zero",sparse_short_zero,sizeof(sparse_short_zero)},
    {"long-zero",sparse_long_zero,sizeof(sparse_long_zero)},
    {"alternating",sparse_alternating,sizeof(sparse_alternating)},
    {"consecutive",sparse_consecutive,sizeof(sparse_consecutive)},
    {"decimal-boundary",sparse_decimal_boundary,sizeof(sparse_decimal_boundary)},
    {"binary-nul",sparse_binary_nul,sizeof(sparse_binary_nul)}
};
_Static_assert(sizeof(sparse_all_zero)==32,"Complete zero span");
_Static_assert(sizeof(sparse_alternating)==80,"Complete alternating span");
_Static_assert(sizeof(sparse_consecutive)==255,"All nonzero byte values");
_Static_assert(sizeof(sparse_decimal_boundary)==999,"Complete index span");
_Static_assert(sizeof(sparse_binary_nul)==16,"Binary span has no extra NUL");

static unsigned sparse_digits(size_t value) {
    unsigned digits=1;
    while(value>=10) {value/=10;++digits;}
    return digits;
}
/* This declarative oracle visits nonzero indices, not the emitter's pending
 * zero scan. Output length and all actual private fees are frozen from its
 * grammar and scalar operations before calling the product helper. */
static void sparse_oracle_element(SparseProofOracle *oracle,size_t index,
    unsigned byte,bool designator) {
    unsigned digits=sparse_digits(byte);
    if(!(oracle->elements%16)) {
        oracle->bytes+=5;oracle->measure_work+=1;oracle->write_work+=10;
    }
    if(designator) {
        unsigned at=sparse_digits(index);
        oracle->bytes+=at+3;
        oracle->measure_work+=at+3;oracle->write_work+=3*at+5;
    }
    oracle->bytes+=digits+1;
    oracle->measure_work+=digits+3;oracle->write_work+=3*digits+2;
    ++oracle->elements;
}
static SparseProofOracle sparse_oracle(const SparseProofCase *input) {
    const size_t format=sizeof("static const uint8_t %s_checked[%llu] = {")-1;
    unsigned length_digits=sparse_digits(input->length);
    SparseProofOracle oracle={0};
    oracle.bytes=format+length_digits;
    oracle.measure_work=2*format+length_digits+9;
    oracle.write_work=2*format+3*length_digits+8;
    oracle.measure_work+=2*input->length;oracle.write_work+=2*input->length;
    size_t previous=0;bool found=false;
    for(size_t index=0;index<input->length;++index) {
        unsigned byte=input->bytes[index];
        if(!byte)continue;
        size_t gap=found ? index-previous-1 : index;
        bool designator=false;
        if(gap) {
            unsigned at=sparse_digits(index);
            oracle.measure_work+=at+1;oracle.write_work+=at+1;
            designator=gap>(size_t)(at+3)/2;
            if(!designator) {
                oracle.measure_work+=gap;oracle.write_work+=gap;
                for(size_t position=index-gap;position<index;++position)
                    sparse_oracle_element(&oracle,position,0,false);
            }
        }
        sparse_oracle_element(&oracle,index,byte,designator);
        previous=index;found=true;
    }
    if(!found) {++oracle.bytes;++oracle.measure_work;oracle.write_work+=2;}
    /* The caller's complete C11 array closing text and one final NUL. */
    oracle.bytes+=4;oracle.measure_work+=1;oracle.write_work+=9;
    return oracle;
}
static size_t sparse_parse_number(const char **cursor,const char *end) {
    CHECK(*cursor<end && **cursor>='0' && **cursor<='9');
    size_t value=0;
    while(*cursor<end && **cursor>='0' && **cursor<='9') {
        unsigned digit=(unsigned)(**cursor-'0');
        CHECK(value<=(SIZE_MAX-digit)/10);
        value=value*10+digit;++*cursor;
    }
    return value;
}
/* Standard C11 zero initialization and sequential designated elements are
 * decoded independently. Every assigned index is checked once and bounded. */
static void sparse_decode(const CBuffer *buffer,const SparseProofCase *input) {
    static const char prefix[]="static const uint8_t sparse_checked[";
    CHECK(input->length<=1024 && buffer->length>=sizeof(prefix));
    const char *cursor=buffer->text,*end=cursor+buffer->length;
    CHECK(memcmp(cursor,prefix,sizeof(prefix)-1)==0);cursor+=sizeof(prefix)-1;
    CHECK(sparse_parse_number(&cursor,end)==input->length);
    CHECK(end-cursor>=5 && memcmp(cursor,"] = {",5)==0);cursor+=5;
    uint8_t decoded[1024]={0};bool assigned[1024]={false};size_t index=0;
    for(;;) {
        while(cursor<end && (*cursor==' ' || *cursor=='\n'))++cursor;
        CHECK(cursor<end);
        if(*cursor=='}')break;
        if(*cursor=='[') {
            ++cursor;index=sparse_parse_number(&cursor,end);
            CHECK(end-cursor>=2 && memcmp(cursor,"]=",2)==0);cursor+=2;
        }
        size_t byte=sparse_parse_number(&cursor,end);
        CHECK(index<input->length && byte<=255 && !assigned[index]);
        assigned[index]=true;decoded[index++]=(uint8_t)byte;
        if(cursor<end && *cursor==',') {++cursor;continue;}
        while(cursor<end && (*cursor==' ' || *cursor=='\n'))++cursor;
        CHECK(cursor<end && *cursor=='}');break;
    }
    CHECK(end-cursor==3 && memcmp(cursor,"};\n",3)==0);
    CHECK(memcmp(decoded,input->bytes,input->length)==0);
    CHECK(buffer->text[buffer->length]=='\0');
}
static void sparse_emit_closed(CBuffer *buffer,const SparseProofCase *input) {
    emit_checked_proof_bytes(buffer,input->bytes,input->length,"sparse");
    EMIT_LITERAL(buffer,"\n};\n");
}
static void sparse_normal(const SparseProofCase *input) {
    SparseProofOracle oracle=sparse_oracle(input);
    XrCompileResourceLimits limits={UINT64_C(1)<<26,UINT64_C(1)<<23,128000000};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompileResourceStats start,measured,allocated,written,verified,closed;
    xr_compile_resources_stats(resources,&start);
    CBuffer measure={.limit=oracle.bytes+1,.status=XR_XIR_OK,.context=&context,.measuring=true};
    sparse_emit_closed(&measure,input);xr_compile_resources_stats(resources,&measured);
    CHECK(measure.status==XR_XIR_OK && measure.length==oracle.bytes);
    CHECK(measured.work-start.work==oracle.measure_work && measured.live_bytes==start.live_bytes);
    void *allocation=NULL;
    CHECK(xr_compile_resources_alloc(resources,oracle.bytes+1,&allocation)==XR_COMPILE_RESOURCE_OK);
    char *storage=allocation;
    xr_compile_resources_stats(resources,&allocated);CHECK(allocated.work-measured.work==1);
    CBuffer output={.text=storage,.capacity=oracle.bytes+1,.limit=oracle.bytes+1,.status=XR_XIR_OK,.context=&context};
    sparse_emit_closed(&output,input);CHECK(emit_finalize(&output));
    xr_compile_resources_stats(resources,&written);
    CHECK(output.length==oracle.bytes && written.work-allocated.work==oracle.write_work);
    sparse_decode(&output,input);
    XiCgenVerifyResult result={0};
    CHECK(xr_compile_cgen_verify_output(resources,output.text,output.length,&result)==XI_CGEN_VERIFY_PASSED);
    xr_compile_resources_stats(resources,&verified);CHECK(verified.work>written.work);
    xr_compile_resources_free(output.text);xr_compile_resources_stats(resources,&closed);
    CHECK(closed.live_bytes==start.live_bytes && closed.work==verified.work);
    xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
}
static void sparse_bytecap(const SparseProofCase *input) {
    SparseProofOracle oracle=sparse_oracle(input);
    XrCompileResourceLimits limits={UINT64_C(1)<<26,UINT64_C(1)<<23,128000000};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    void *allocation=NULL;
    CHECK(xr_compile_resources_alloc(resources,oracle.bytes+1,&allocation)==XR_COMPILE_RESOURCE_OK);
    char *storage=allocation;
    memset(storage,'q',oracle.bytes+1);
    CBuffer measure={.limit=oracle.bytes,.status=XR_XIR_OK,.context=&context,.measuring=true};
    CBuffer output={.text=storage,.capacity=oracle.bytes+1,.limit=oracle.bytes,.status=XR_XIR_OK,.context=&context};
    sparse_emit_closed(&measure,input);sparse_emit_closed(&output,input);
    CHECK(measure.status==XR_XIR_BUDGET && output.status==XR_XIR_BUDGET);
    CHECK(measure.length==oracle.bytes-1 && output.length==measure.length);
    CHECK(storage[oracle.bytes-1]=='q' && storage[oracle.bytes]=='q');
    CHECK(!emit_finalize(&output) && storage[oracle.bytes-1]=='q');
    xr_compile_resources_free(output.text);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static void sparse_workcap(const SparseProofCase *input,unsigned minus) {
    SparseProofOracle oracle=sparse_oracle(input);
    XrCompileResourceLimits limits={UINT64_C(1)<<26,UINT64_C(1)<<23,
        2+oracle.measure_work+oracle.write_work-minus};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    CBuffer measure={.limit=oracle.bytes+1,.status=XR_XIR_OK,.context=&context,.measuring=true};
    sparse_emit_closed(&measure,input);CHECK(measure.status==XR_XIR_OK);
    void *allocation=NULL;
    CHECK(xr_compile_resources_alloc(resources,oracle.bytes+1,&allocation)==XR_COMPILE_RESOURCE_OK);
    char *storage=allocation;
    memset(storage,'q',oracle.bytes+1);
    CBuffer output={.text=storage,.capacity=oracle.bytes+1,.limit=oracle.bytes+1,.status=XR_XIR_OK,.context=&context};
    sparse_emit_closed(&output,input);CHECK(output.status==XR_XIR_OK && output.length==oracle.bytes);
    CHECK(storage[oracle.bytes]=='q');
    CHECK(emit_finalize(&output)==!minus);
    CHECK(output.status==(minus ? XR_XIR_BUDGET : XR_XIR_OK));
    CHECK(storage[oracle.bytes]==(minus ? 'q' : '\0'));
    XrCompileResourceStats stats;xr_compile_resources_stats(resources,&stats);
    CHECK(stats.work==limits.work);
    xr_compile_resources_free(output.text);xr_compile_resources_release(resources);
    CHECK(!runtime_live && !runtime_bytes);
}
static size_t sparse_fault_trial(size_t fault) {
    const SparseProofCase *input=&sparse_cases[8];
    XrCompileResourceLimits limits={UINT64_C(1)<<26,UINT64_C(1)<<23,128000000};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompileResourceStats baseline,closed;xr_compile_resources_stats(resources,&baseline);
    CBuffer output={.limit=1<<20,.status=XR_XIR_OK,.context=&context};
    runtime_attempts=0;runtime_fail_at=fault;
    sparse_emit_closed(&output,input);(void)emit_finalize(&output);
    runtime_fail_at=SIZE_MAX;size_t sites=runtime_attempts;
    CHECK(output.status==(fault==SIZE_MAX ? XR_XIR_OK : XR_XIR_OUT_OF_MEMORY));
    if(fault==SIZE_MAX)sparse_decode(&output,input);
    xr_compile_resources_free(output.text);xr_compile_resources_stats(resources,&closed);
    CHECK(closed.live_bytes==baseline.live_bytes && closed.work>=baseline.work);
    xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
    return sites;
}
static void sparse_cross_call_labels(void) {
    static const char expected[]="void sparse_labels(void) {\n    goto invalid;\n    goto limit;\ninvalid:;\nlimit:;\n}\n";
    XrCompileResourceLimits limits={UINT64_C(1)<<26,UINT64_C(1)<<23,128000000};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    for(unsigned pass=0;pass<2;++pass) {
        CBuffer buffer={.limit=sizeof(expected),.status=XR_XIR_OK,.context=&context,
            .measuring=pass==0,.tracking=true};
        if(pass) {
            void *allocation=NULL;
            CHECK(xr_compile_resources_alloc(resources,sizeof(expected),&allocation)==XR_COMPILE_RESOURCE_OK);
            buffer.text=allocation;
            buffer.capacity=sizeof(expected);
        }
        EMIT_LITERAL(&buffer,"void sparse_labels(void) {\n    go");
        append(&buffer,"%s","to inv");EMIT_LITERAL(&buffer,"alid;\n    go");
        append(&buffer,"%s","to li");EMIT_LITERAL(&buffer,"mit;\ninvalid:;\nlimit:;\n}\n");
        CHECK(buffer.status==XR_XIR_OK && buffer.length==sizeof(expected)-1);
        CHECK(buffer.label_used[0] && buffer.label_used[1] && !buffer.tracking);
        CHECK(!buffer.label_match[0] && !buffer.label_match[1]);
        if(pass) {
            CHECK(emit_finalize(&buffer) && memcmp(buffer.text,expected,sizeof(expected))==0);
            XiCgenVerifyResult result={0};
            CHECK(xr_compile_cgen_verify_output(resources,buffer.text,buffer.length,&result)==XI_CGEN_VERIFY_PASSED);
        }
        xr_compile_resources_free(buffer.text);
    }
    xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
}
static void sparse_invalid_spans(void) {
    XrCompileResourceLimits limits={UINT64_C(1)<<26,UINT64_C(1)<<23,128000000};
    XrCompileResources *resources=NULL;
    CHECK(xr_compile_resources_new(&limits,&resources)==XR_COMPILE_RESOURCE_OK);
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    XrCompileResourceStats before,after;xr_compile_resources_stats(resources,&before);
    for(unsigned which=0;which<3;++which) {
        char sentinel='q';
        CBuffer buffer={.text=&sentinel,.capacity=1,.limit=1,.status=XR_XIR_OK,.context=&context};
        const uint8_t *bytes=which==2 ? sparse_one_zero : NULL;
        size_t length=which==1 ? 1 : 0;
        emit_checked_proof_bytes(&buffer,bytes,length,"sparse");
        CHECK(buffer.status==XR_XIR_BAD_STRUCTURE && !buffer.length);
        CHECK(buffer.text==&sentinel && sentinel=='q');
    }
    xr_compile_resources_stats(resources,&after);
    CHECK(after.work==before.work && after.live_bytes==before.live_bytes);
    xr_compile_resources_release(resources);CHECK(!runtime_live && !runtime_bytes);
}
/* One inclusion and one call from the existing private emitter owner TU. */
static void cgen_sparse_proof_cases(void) {
    CHECK(!runtime_live && !runtime_bytes && runtime_fail_at==SIZE_MAX);
    for(size_t i=0;i<sizeof(sparse_cases)/sizeof(sparse_cases[0]);++i) {
        sparse_normal(&sparse_cases[i]);sparse_bytecap(&sparse_cases[i]);
        sparse_workcap(&sparse_cases[i],0);sparse_workcap(&sparse_cases[i],1);
    }
    sparse_cross_call_labels();sparse_invalid_spans();
    size_t sites=sparse_fault_trial(SIZE_MAX);CHECK(sites>1);
    for(size_t fault=0;fault<sites;++fault)(void)sparse_fault_trial(fault);
    CHECK(!runtime_live && !runtime_bytes);
    puts("Sparse proof: 11 raw-byte oracles, complete C11/W1-W4, same-ledger exact/minus-one, split labels and actual FI/physical zero PASS");
}
#endif
