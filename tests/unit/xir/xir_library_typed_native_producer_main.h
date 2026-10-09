/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_typed_native_producer_main.h
 */
#ifndef XIR_LIBRARY_TYPED_NATIVE_PRODUCER_MAIN_H
#define XIR_LIBRARY_TYPED_NATIVE_PRODUCER_MAIN_H
/* External denominators are audited independently before strict execution. */
typedef struct H1ProducerExpected { size_t sites[H1C_PHASES]; uint64_t axes[3]; } H1ProducerExpected;
static H1ProducerExpected h1p_oracle(const char *path,const RootParameterSourceOracle *oracle) {
    FILE *file = fopen(path,"rb"); CHECK(file); H1ProducerExpected e = {0};
    h1_token(file,"TYPED_NATIVE_PRODUCER_R1"); CHECK(h1_number(file) == XR_XIR_CHECKED_SCHEMA);
    CHECK(h1_number(file) == XR_XIR_CHECKED_CONTRACT && h1_number(file) == XR_XIR_VALUE_ABI_VERSION);
    CHECK(h1_number(file) == XR_XIR_CALL_ABI_VERSION && h1_number(file) == XR_XIR_PROGRAM_ABI_VERSION);
    h1_token(file,oracle->file); h1_token(file,"sites");
    for (unsigned p = 0; p < H1C_PHASES; ++p) {
        uint64_t n = h1_number(file); CHECK(n <= 65536 && (uint64_t)(size_t)n == n); e.sites[p] = (size_t)n;
    }
    CHECK(e.sites[H1C_OWNER] == 1); h1_token(file,"axes");
    const uint64_t caps[3] = {h1c_caps.allocated_bytes,h1c_caps.live_bytes,h1c_caps.work};
    for (unsigned a = 0; a < 3; ++a) { e.axes[a] = h1_number(file); CHECK(e.axes[a] > 1 && e.axes[a] <= caps[a]); }
    h1_token(file,"END"); int b; while ((b = fgetc(file)) != EOF) CHECK(isspace((unsigned char)b));
    CHECK(!ferror(file) && !fclose(file)); return e;
}
static void h1p_counts(H1Compiler *c,const H1ProducerExpected *e) {
    XrCompileResourceStats s = h1c_stats(c);
    for (unsigned p = 0; p < H1C_PHASES; ++p) CHECK(c->sites[p] == e->sites[p]);
    CHECK(s.allocated_bytes == e->axes[0] && s.peak_bytes == e->axes[1] && s.work == e->axes[2]);
}
static void h1p_census(const RootParameterSourceOracle *oracle,const H1ProducerExpected *e) {
    H1Compiler c; h1c_init(&c,oracle); CHECK(h1c_pipeline(&c,0,&h1c_caps) == XR_XIR_OK);
    if (e) h1p_counts(&c,e);
    XrCompileResourceStats s = h1c_stats(&c);
    printf("TYPED_NATIVE_PRODUCER_CENSUS file=%s sites=",oracle->file);
    for (unsigned p = 0; p < H1C_PHASES; ++p) printf("%s%zu",p ? "," : "",c.sites[p]);
    printf(" allocated=%llu peak=%llu work=%llu bytes=%zu finalNul=1\n",
        (unsigned long long)s.allocated_bytes,(unsigned long long)s.peak_bytes,(unsigned long long)s.work,c.generated.length);
    h1c_drop(&c);
    printf("TYPED_NATIVE_PRODUCER_PHYSICAL file=%s compiler_blocks=%zu compiler_bytes=%zu\n",
        oracle->file, instance_compile_live, instance_compile_bytes);
}
static void h1p_oom(const RootParameterSourceOracle *oracle,const H1ProducerExpected *e,unsigned stage,size_t first,size_t count) {
    CHECK(stage < H1C_PHASES && count && count <= 64 && first <= e->sites[stage] && count <= e->sites[stage]-first);
    for (size_t point = first; point < first+count; ++point) {
        H1Compiler c; h1c_init(&c,oracle);
        for (unsigned p = 0; p < stage; ++p) CHECK(h1c_step(&c,p,&h1c_caps) == XR_XIR_OK);
        XrCompileResources *owner = c.context.resources; XrCompileResourceStats s0 = h1c_stats(&c);
        size_t begin = instance_compile_attempts; CHECK(point < SIZE_MAX-begin);
        instance_compile_injected = false; instance_compile_fail_at = begin+point;
        XrXirStatus status = h1c_step(&c,stage,&h1c_caps); instance_compile_fail_at = SIZE_MAX;
        CHECK(status == XR_XIR_OUT_OF_MEMORY && instance_compile_injected && instance_compile_attempts == begin+point+1);
        h1c_clear(&c); XrCompileResourceStats s1 = h1c_stats(&c);
        CHECK(c.context.resources == owner && s1.work >= s0.work && s1.allocated_bytes >= s0.allocated_bytes);
        CHECK(!c.generated.text && !c.generated.length);
        CHECK(h1c_pipeline(&c,owner ? H1C_IO : H1C_OWNER,&h1c_caps) == XR_XIR_OK);
        XrCompileResourceStats s2 = h1c_stats(&c);
        CHECK(!owner || (c.context.resources == owner && s2.work >= s1.work && s2.allocated_bytes >= s1.allocated_bytes));
        h1c_drop(&c);
        printf("TYPED_NATIVE_PRODUCER_OOM file=%s phase=%u point=%zu denominator=%zu sameOwner=%u refund=0 physical=0/0\n",
            oracle->file,stage,point,e->sites[stage],(unsigned)(owner != NULL));
    }
    printf("TYPED_NATIVE_PRODUCER_RANGE file=%s phase=%u first=%zu count=%zu denominator=%zu complete=1\n",
        oracle->file,stage,first,count,e->sites[stage]);
}
static void h1p_axes(const RootParameterSourceOracle *oracle,const H1ProducerExpected *e) {
    for (unsigned a = 0; a < 3; ++a) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits caps = h1c_caps; uint64_t amount = e->axes[a]-minus;
        if (!a) caps.allocated_bytes = amount; else if (a == 1) caps.live_bytes = amount; else caps.work = amount;
        H1Compiler c; h1c_init(&c,oracle);
        CHECK(h1c_pipeline(&c,0,&caps) == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        if (!minus) h1p_counts(&c,e);
        else {
            XrCompileResources *owner = c.context.resources; h1c_clear(&c); XrCompileResourceStats s0 = h1c_stats(&c);
            CHECK(h1c_pipeline(&c,owner ? H1C_IO : H1C_OWNER,&caps) == XR_XIR_BUDGET);
            XrCompileResourceStats s1 = h1c_stats(&c);
            CHECK(c.context.resources == owner && s1.work >= s0.work && s1.allocated_bytes >= s0.allocated_bytes);
        }
        h1c_drop(&c);
        printf("TYPED_NATIVE_PRODUCER_AXIS file=%s axis=%u minus1=%u cap=%llu physical=0/0\n",oracle->file,a,minus,(unsigned long long)amount);
    }
}
int main(int argc,char **argv) {
    CHECK(argc >= 3 && argc <= 7); (void)&h1_unsigned;
    const RootParameterSourceOracle *oracle = NULL;
    for (size_t i = 0; i < H1_TYPED_CASES; ++i) if (!strcmp(argv[2],rps_oracles[i].file)) oracle = &rps_oracles[i];
    CHECK(oracle);
    if (!strcmp(argv[1],"--census")) { CHECK(argc == 3); h1p_census(oracle,NULL); return 0; }
    CHECK(argc >= 4); H1ProducerExpected e = h1p_oracle(argv[3],oracle); h1p_census(oracle,&e);
    if (!strcmp(argv[1],"--strict")) CHECK(argc == 4);
    else if (!strcmp(argv[1],"--axes")) { CHECK(argc == 4); h1p_axes(oracle,&e); }
    else if (!strcmp(argv[1],"--oom")) {
        CHECK(argc == 7); uint64_t p = h1_decimal(argv[4]), first = h1_decimal(argv[5]), count = h1_decimal(argv[6]);
        CHECK(p < H1C_PHASES && (uint64_t)(size_t)first == first && count && count <= 64);
        h1p_oom(oracle,&e,(unsigned)p,(size_t)first,(size_t)count);
    } else CHECK(false);
    return 0;
}

#endif /* XIR_LIBRARY_TYPED_NATIVE_PRODUCER_MAIN_H */
