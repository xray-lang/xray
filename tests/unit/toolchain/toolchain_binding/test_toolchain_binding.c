/* Independent identity and physical-allocation tests for the compiler owners. */
#include "toolchain/xr_toolchain_binding.h"
#include "aot/program/xr_xir_native_artifact.h"
#include "base/xchecks.h"
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static const XrCompileResourceLimits unlimited = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
typedef struct Record { void *pointer; size_t size; } Record;
static Record records[16];
static size_t attempts, fail_at = SIZE_MAX, physical, peak, total, active;
static void *observed_alloc(size_t size) {
    if (attempts++ == fail_at) return NULL;
    void *p = xr_malloc(size);
    CHECK(p);
    size_t i = 0;
    while (i < 16 && records[i].pointer) ++i;
    CHECK(i < 16);
    records[i] = (Record){p, size};
    physical += size; total += size; ++active;
    if (physical > peak) peak = physical;
    return p;
}
static void observed_free(void *p) {
    if (!p) return;
    size_t i = 0;
    while (i < 16 && records[i].pointer != p) ++i;
    CHECK(i < 16);
    physical -= records[i].size; --active; records[i] = (Record){0};
    xr_free(p);
}
#undef xr_malloc
#undef xr_free
#define xr_malloc(size) observed_alloc(size)
#define xr_free(p) observed_free(p)
#include "base/xcompile_resources.c"

static void reset(void) {
    CHECK(!physical && !active);
    attempts = total = peak = 0; fail_at = SIZE_MAX;
}
static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats s;
    CHECK(xr_compile_resources_stats(resources, &s) == XR_COMPILE_RESOURCE_OK);
    CHECK(s.live_bytes == physical && s.peak_bytes == peak && s.allocated_bytes == total);
    return s;
}
static XrCompileResources *owner(uint64_t work) {
    XrCompileResources *r = NULL;
    XrCompileResourceLimits limits = unlimited;
    limits.work = work;
    CHECK(xr_compile_resources_new(&limits, &r) == XR_COMPILE_RESOURCE_OK);
    return r;
}
static XrFingerprint digest(const char *text) {
    XrFingerprint result;
    xr_sha256((const uint8_t *)text, strlen(text), result.bytes);
    return result;
}
static XrToolchainInput request(void) {
    XrToolchainInput result = {0};
    result.schema_version = 2; result.provider = XR_TOOLCHAIN_PROVIDER_MSVC;
    result.provider_version = "cl-19.44"; result.target_triple = "windows-x86_64";
    result.codegen_options = "opt=2;debug=0";
    result.sysroot_id = digest("sysroot"); result.runtime_sdk_id = digest("sdk");
    result.target_profile_id = digest("target");
    return result;
}

static const uint8_t native_bytes[] = {0x4d, 0x5a, 0, 0xff, 0x80, 1, 2, 3};
static uint64_t outer_work(void) { return 1 + 21 + 2*8 + 6*32 + 33; }
static uint64_t binding_work(void) {
    /* Header copy, identity reads, actual zero/copy, framed text and publication. */
    return 8 + 3*32 + sizeof(XrToolchainBinding) + 8 + 3*32 +
        (43 + 34 + 2*8) + (43 + 31 + 2*14) + (43 + 33 + 2*13) +
        outer_work() + sizeof(XrToolchainBinding);
}
static uint64_t validation_work(void) { return 8 + 6*32 + outer_work() + 64; }
static uint64_t input_hash_work(void) { return 1 + 24 + 10*8 + 6*32 + 33; }
static uint64_t input_fields_work(void) { return 40 + 5*32 + validation_work(); }
static uint64_t input_seal_work(void) { return input_fields_work() + 2*sizeof(XrXirNativeInput) + input_hash_work(); }
static uint64_t input_valid_work(void) { return input_fields_work() + input_hash_work() + 64; }
static uint64_t artifact_hash_work(void) { return 1 + 27 + 2*4 + 2*8 + 2*32 + 33; }
static uint64_t digest_work(void) { return 1 + sizeof(native_bytes) + 33; }
static size_t native_owner_header(void) {
    return sizeof(XrXirCompileContext) + sizeof(size_t) + sizeof(XrXirNativeArtifactView);
}
static uint64_t artifact_seal_work(void) {
    return input_valid_work() + 1 + native_owner_header() + sizeof(XrXirCompileContext) +
        sizeof(XrXirNativeInput) + sizeof(native_bytes) + 4 + sizeof(void*) + 2*sizeof(size_t) +
        digest_work() + artifact_hash_work() + sizeof(void*);
}
static uint64_t artifact_verify_work(void) {
    return offsetof(XrXirNativeArtifactView, input) + 2*input_valid_work() + 3*64 +
        digest_work() + artifact_hash_work();
}
static XrXirNativeInput native_request(XrToolchainBinding binding) {
    XrXirNativeInput in = {0};
    in.schema_version = XR_XIR_NATIVE_INPUT_SCHEMA;
    in.checked_schema = XR_XIR_CHECKED_SCHEMA; in.checked_contract = XR_XIR_CHECKED_CONTRACT;
    in.value_abi = XR_XIR_VALUE_ABI_VERSION; in.call_abi = XR_XIR_CALL_ABI_VERSION;
    in.program_abi = XR_XIR_PROGRAM_ABI_VERSION; in.architecture = XR_XIR_ARCH_X86_64;
    in.entry = 1; in.function_count = 3; in.module_count = 2;
    in.source_checked_id = digest("source"); in.closed_checked_id = digest("closed");
    in.lowered_layout_id = digest("layout"); in.codegen_policy_id = digest("policy");
    in.generated_digest = digest("generated"); in.toolchain = binding;
    return in;
}
static XrXirCompileContext context(XrCompileResources *r) {
    XrXirCompileContext c = {r, {0}};
    return c;
}
static void print_id(const char *name, const XrFingerprint *fp) {
    printf("%s=", name);
    for (unsigned i=0; i<32; ++i) printf("%02x", fp->bytes[i]);
    putchar('\n');
}
static void fixed_vectors(void) {
    reset(); XrCompileResources *r = owner(UINT64_MAX);
    XrToolchainInput in = request(); XrToolchainBinding binding;
    CHECK(xr_compile_toolchain_binding_build(r, &in, &binding) == XR_TOOLCHAIN_BINDING_OK);
    CHECK(stats(r).work == 1 + binding_work());
    CHECK(xr_compile_toolchain_binding_validate(r, &binding) == XR_TOOLCHAIN_BINDING_OK);
    CHECK(stats(r).work == 1 + binding_work() + validation_work());
    bool same = false;
    CHECK(xr_compile_toolchain_binding_equal(r, &binding, &binding, &same) == XR_TOOLCHAIN_BINDING_OK && same);
    uint64_t before = stats(r).work;
    CHECK(before == 1 + binding_work() + 3*validation_work() + 64 + sizeof(bool));
    XrXirCompileContext ctx = context(r);
    XrXirNativeInput raw = native_request(binding), sealed;
    CHECK(xr_compile_native_input_seal(&ctx, &raw, &sealed) == XR_XIR_OK);
    CHECK(stats(r).work == before + input_seal_work());
    XrXirNativeArtifact *artifact = NULL;
    CHECK(xr_compile_native_artifact_seal(&ctx, &sealed, native_bytes, sizeof(native_bytes), sizeof(native_bytes), &artifact) == XR_XIR_OK);
    CHECK(stats(r).work == before + input_seal_work() + artifact_seal_work());
    CHECK(stats(r).allocation_count == 2 && attempts == 2);
    CHECK(physical == sizeof(XrCompileResources) + sizeof(CompileAllocation) + native_owner_header() + sizeof(native_bytes));
    CHECK(xr_compile_native_artifact_verify(artifact, &sealed, sizeof(native_bytes)) == XR_XIR_OK);
    CHECK(stats(r).work == before + input_seal_work() + artifact_seal_work() + artifact_verify_work());
    const XrXirNativeArtifactView *v = xr_compile_native_artifact_view(artifact);
    CHECK(v && v->bytes != native_bytes && v->size == sizeof(native_bytes) && !memcmp(v->bytes, native_bytes, v->size));
    print_id("toolchain", &binding.id); print_id("input", &sealed.id);
    print_id("native", &v->native_digest); print_id("artifact", &v->id);
    printf("abi=%u,%u,%u,%u,%u\n", raw.checked_schema, raw.checked_contract, raw.value_abi, raw.call_abi, raw.program_abi);
    printf("work=%llu,%llu,%llu,%llu,%llu\n", (unsigned long long)binding_work(),
        (unsigned long long)validation_work(), (unsigned long long)input_seal_work(),
        (unsigned long long)artifact_seal_work(), (unsigned long long)artifact_verify_work());
    /* The input, stack context and producer reference disappear first. */
    XrXirNativeInput expected = sealed;
    memset(&raw, 0, sizeof(raw)); memset(&sealed, 0, sizeof(sealed)); memset(&ctx, 0, sizeof(ctx));
    xr_compile_resources_release(r);
    CHECK(xr_compile_native_artifact_verify(artifact, &expected, sizeof(native_bytes)) == XR_XIR_OK);
    CHECK(!memcmp(v->bytes, native_bytes, v->size));
    xr_compile_native_artifact_free(artifact); CHECK(!physical && !active);
}
static void binding_boundaries(void) {
    XrToolchainInput in = request(); XrToolchainBinding golden;
    reset(); XrCompileResources *r = owner(UINT64_MAX);
    CHECK(xr_compile_toolchain_binding_build(r, &in, &golden) == XR_TOOLCHAIN_BINDING_OK);
    xr_compile_resources_release(r);
    for (uint64_t i=0; i<=binding_work(); ++i) {
        reset(); r = owner(1+i);
        XrToolchainBinding out, unchanged; memset(&out, 0xa5, sizeof(out)); unchanged = out;
        XrToolchainBindingStatus s = xr_compile_toolchain_binding_build(r, &in, &out);
        CHECK(s == (i==binding_work() ? XR_TOOLCHAIN_BINDING_OK : XR_TOOLCHAIN_BINDING_BUDGET));
        CHECK(i==binding_work() ? !memcmp(&out, &golden, sizeof(out)) : !memcmp(&out, &unchanged, sizeof(out)));
        CHECK(stats(r).allocation_count == 1 && attempts == 1 && stats(r).work<=1+i);
        xr_compile_resources_release(r); CHECK(!physical);
    }
    const uint64_t equal_work = 2*validation_work()+64+sizeof(bool);
    for (uint64_t i=0; i<=equal_work; ++i) {
        reset(); r = owner(1+i); bool out = false;
        XrToolchainBindingStatus s = xr_compile_toolchain_binding_equal(r, &golden, &golden, &out);
        CHECK(s == (i==equal_work ? XR_TOOLCHAIN_BINDING_OK : XR_TOOLCHAIN_BINDING_BUDGET));
        CHECK(out == (i==equal_work)); xr_compile_resources_release(r);
    }
    reset(); r = owner(UINT64_MAX);
    bool eq = true;
    XrToolchainBinding other = golden;
    other.schema_version = 1;
    CHECK(xr_compile_toolchain_binding_equal(r, &golden, &other, &eq) == XR_TOOLCHAIN_BINDING_BAD_STRUCTURE && eq);
    CHECK(xr_compile_toolchain_binding_build(NULL, &in, &other) == XR_TOOLCHAIN_BINDING_BAD_ARGUMENT);
    CHECK(xr_compile_toolchain_binding_equal(r, &golden, &golden, NULL) == XR_TOOLCHAIN_BINDING_BAD_ARGUMENT);
    XrToolchainInput old=in;
    old.schema_version=1;
    old.provider_version=old.target_triple=old.codegen_options=(const char *)(uintptr_t)1;
    other=golden;
    uint64_t rejected_before=stats(r).work;
    CHECK(xr_compile_toolchain_binding_build(r,&old,&other)==XR_TOOLCHAIN_BINDING_BAD_STRUCTURE);
    CHECK(stats(r).work==rejected_before+8 && !memcmp(&other,&golden,sizeof(other)));
    for (unsigned field=0; field<10; ++field) {
        other = golden;
        if (field==0) other.schema_version=1;
        else if (field==1) other.provider=0;
        else if (field==2) other.provider=5;
        else if (field==3) other.reserved8[1]=1;
        else {
            XrFingerprint *ids[] = {&other.provider_version_id,&other.target_triple_id,&other.codegen_options_id,
                &other.sysroot_id,&other.runtime_sdk_id,&other.target_profile_id};
            memset(ids[field-4], 0, sizeof(XrFingerprint));
        }
        CHECK(xr_compile_toolchain_binding_validate(r, &other) == XR_TOOLCHAIN_BINDING_BAD_STRUCTURE);
    }
    for (unsigned field=0; field<7; ++field) {
        other = golden;
        XrFingerprint *ids[] = {&other.provider_version_id,&other.target_triple_id,&other.codegen_options_id,
            &other.sysroot_id,&other.runtime_sdk_id,&other.target_profile_id,&other.id};
        ids[field]->bytes[31]^=1;
        CHECK(xr_compile_toolchain_binding_validate(r, &other) == XR_TOOLCHAIN_BINDING_BAD_STRUCTURE);
    }
    const char *strings[] = {"clang 22", "linux-x86_64", "", NULL};
    for (unsigned field=0; field<6; ++field) {
        XrToolchainInput modified = in;
        if (field==0) modified.provider_version=strings[0];
        if (field==1) modified.target_triple=strings[1];
        if (field==2) modified.codegen_options=strings[2];
        if (field==3) modified.sysroot_id.bytes[0]^=1;
        if (field==4) modified.runtime_sdk_id.bytes[0]^=1;
        if (field==5) modified.target_profile_id.bytes[0]^=1;
        CHECK(xr_compile_toolchain_binding_build(r, &modified, &other) == XR_TOOLCHAIN_BINDING_OK);
        eq=true;
        CHECK(xr_compile_toolchain_binding_equal(r, &golden, &other, &eq) == XR_TOOLCHAIN_BINDING_OK && !eq);
    }
    for (unsigned which=0; which<3; ++which) {
        XrToolchainInput bad = in;
        if (which==0) bad.provider_version=strings[3];
        if (which==1) bad.provider_version="";
        if (which==2) bad.target_triple="";
        other=golden;
        CHECK(xr_compile_toolchain_binding_build(r, &bad, &other) == XR_TOOLCHAIN_BINDING_BAD_STRUCTURE);
        CHECK(!memcmp(&other, &golden, sizeof(other)));
    }
    char long_text[XR_TOOLCHAIN_TEXT_LIMIT+2]; memset(long_text, 'a', sizeof(long_text));
    long_text[XR_TOOLCHAIN_TEXT_LIMIT]=0;
    XrToolchainInput large=in; large.provider_version=long_text;
    CHECK(xr_compile_toolchain_binding_build(r, &large, &other) == XR_TOOLCHAIN_BINDING_OK);
    long_text[XR_TOOLCHAIN_TEXT_LIMIT]='a'; long_text[XR_TOOLCHAIN_TEXT_LIMIT+1]=0;
    other=golden;
    CHECK(xr_compile_toolchain_binding_build(r, &large, &other) == XR_TOOLCHAIN_BINDING_BAD_STRUCTURE);
    CHECK(!memcmp(&other,&golden,sizeof(other)));
    xr_compile_resources_release(r); CHECK(!physical);
}
static XrXirStatus pipeline(const XrCompileResourceLimits *limits, XrCompileResourceStats *out) {
    XrCompileResources *r=NULL;
    XrCompileResourceStatus rs=xr_compile_resources_new(limits,&r);
    if (rs!=XR_COMPILE_RESOURCE_OK) return rs==XR_COMPILE_RESOURCE_OUT_OF_MEMORY ? XR_XIR_OUT_OF_MEMORY : XR_XIR_BUDGET;
    XrToolchainInput in=request(); XrToolchainBinding b;
    XrToolchainBindingStatus bs=xr_compile_toolchain_binding_build(r,&in,&b);
    XrXirStatus status=bs==XR_TOOLCHAIN_BINDING_OK ? XR_XIR_OK : XR_XIR_BUDGET;
    XrXirCompileContext ctx=context(r);
    XrXirNativeInput raw={0}, sealed, sentinel;
    memset(&sealed,0x7c,sizeof(sealed)); sentinel=sealed;
    XrXirNativeArtifact *artifact=NULL;
    if (status==XR_XIR_OK) {raw=native_request(b); status=xr_compile_native_input_seal(&ctx,&raw,&sealed);}
    if (status!=XR_XIR_OK) CHECK(!memcmp(&sealed,&sentinel,sizeof(sealed)));
    if (status==XR_XIR_OK) status=xr_compile_native_artifact_seal(&ctx,&sealed,native_bytes,sizeof(native_bytes),sizeof(native_bytes),&artifact);
    if (status!=XR_XIR_OK) CHECK(!artifact);
    if (status==XR_XIR_OK) status=xr_compile_native_artifact_verify(artifact,&sealed,sizeof(native_bytes));
    *out=stats(r);
    xr_compile_native_artifact_free(artifact);
    CHECK(stats(r).work==out->work && stats(r).allocated_bytes==out->allocated_bytes && active==1);
    xr_compile_resources_release(r); CHECK(!physical);
    return status;
}
static void resources_and_oom(void) {
    reset(); XrCompileResourceStats measured={0}; CHECK(pipeline(&unlimited,&measured)==XR_XIR_OK);
    const uint64_t all_work=1+binding_work()+input_seal_work()+artifact_seal_work()+artifact_verify_work();
    CHECK(measured.work==all_work && attempts==2 && measured.allocation_count==2);
    CHECK(measured.allocated_bytes==measured.peak_bytes);
    XrCompileResourceLimits exact={measured.allocated_bytes,measured.peak_bytes,all_work};
    reset(); XrCompileResourceStats again={0}; CHECK(pipeline(&exact,&again)==XR_XIR_OK);
    CHECK(!memcmp(&measured,&again,sizeof(again)));
    for (unsigned field=0; field<3; ++field) {
        reset(); XrCompileResourceLimits smaller=exact;
        if (field==0) --smaller.allocated_bytes;
        if (field==1) --smaller.live_bytes;
        if (field==2) --smaller.work;
        CHECK(pipeline(&smaller,&again)==XR_XIR_BUDGET);
    }
    for (size_t i=0; i<2; ++i) {
        reset(); fail_at=i; again=(XrCompileResourceStats){0};
        CHECK(pipeline(&unlimited,&again)==XR_XIR_OUT_OF_MEMORY && attempts==i+1 && !physical);
        CHECK(again.work==(i ? 1+binding_work()+input_seal_work()+input_valid_work()+1 : 0));
    }
    for (uint64_t i=1; i<all_work; ++i) {
        reset(); XrCompileResourceLimits limited=unlimited; limited.work=i;
        CHECK(pipeline(&limited,&again)==XR_XIR_BUDGET && again.work<=i);
    }
    /* Failure does not reset cumulative quota, even after physically freeing bytes. */
    reset(); XrCompileResources *r=owner(1+binding_work());
    XrToolchainInput in=request(); XrToolchainBinding b;
    CHECK(xr_compile_toolchain_binding_build(r,&in,&b)==XR_TOOLCHAIN_BINDING_OK);
    uint64_t used=stats(r).work;
    CHECK(xr_compile_toolchain_binding_validate(r,&b)==XR_TOOLCHAIN_BINDING_BUDGET && stats(r).work==used);
    xr_compile_resources_release(r); CHECK(!physical);
}
static void native_rejections(void) {
    reset(); XrCompileResources *r=owner(UINT64_MAX); XrXirCompileContext ctx=context(r);
    XrToolchainInput tc=request(); XrToolchainBinding b;
    CHECK(xr_compile_toolchain_binding_build(r,&tc,&b)==XR_TOOLCHAIN_BINDING_OK);
    XrXirNativeInput raw=native_request(b), sealed;
    CHECK(xr_compile_native_input_seal(&ctx,&raw,&sealed)==XR_XIR_OK);
    XrXirNativeArtifact *artifact=NULL;
    CHECK(xr_compile_native_artifact_seal(&ctx,&sealed,native_bytes,sizeof(native_bytes),sizeof(native_bytes),&artifact)==XR_XIR_OK);
    XrXirNativeArtifact *saved=artifact;
    CHECK(xr_compile_native_artifact_seal(&ctx,&sealed,native_bytes,sizeof(native_bytes),sizeof(native_bytes),&artifact)==XR_XIR_BAD_STRUCTURE && artifact==saved);
    XrXirNativeArtifact *sentinel=(XrXirNativeArtifact *)(uintptr_t)1;
    CHECK(xr_compile_native_artifact_seal(&ctx,&sealed,native_bytes,sizeof(native_bytes),sizeof(native_bytes),&sentinel)==XR_XIR_BAD_STRUCTURE && sentinel==(XrXirNativeArtifact *)(uintptr_t)1);
    CHECK(xr_compile_native_artifact_verify(artifact,&sealed,sizeof(native_bytes)-1)==XR_XIR_BUDGET);
    XrXirNativeArtifact *empty=NULL;
    CHECK(xr_compile_native_artifact_seal(&ctx,&sealed,native_bytes,sizeof(native_bytes),sizeof(native_bytes)-1,&empty)==XR_XIR_BUDGET && !empty);
    CHECK(xr_compile_native_artifact_seal(NULL,&sealed,native_bytes,sizeof(native_bytes),sizeof(native_bytes),&empty)==XR_XIR_BAD_STRUCTURE && !empty);
    XrXirNativeInput bad=sealed, untouched; memset(&untouched,0x69,sizeof(untouched));
    XrXirNativeInput out=untouched;
    for (unsigned field=0; field<10; ++field) {
        bad=sealed;
        uint32_t *fields[]={&bad.schema_version,&bad.checked_schema,&bad.checked_contract,&bad.value_abi,
            &bad.call_abi,&bad.program_abi,&bad.architecture,&bad.entry,&bad.function_count,&bad.module_count};
        *fields[field]=field==7 ? bad.function_count : 0;
        if (field==0) *fields[field]=1;
        out=untouched;
        CHECK(xr_compile_native_input_seal(&ctx,&bad,&out)==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&out,&untouched,sizeof(out)));
        CHECK(xr_compile_native_artifact_verify(artifact,&bad,sizeof(native_bytes))==XR_XIR_BAD_STRUCTURE);
    }
    for (unsigned field=0; field<5; ++field) {
        bad=sealed;
        XrFingerprint *ids[]={&bad.source_checked_id,&bad.closed_checked_id,&bad.lowered_layout_id,&bad.codegen_policy_id,&bad.generated_digest};
        memset(ids[field],0,sizeof(XrFingerprint)); out=untouched;
        CHECK(xr_compile_native_input_seal(&ctx,&bad,&out)==XR_XIR_BAD_STRUCTURE && !memcmp(&out,&untouched,sizeof(out)));
    }
    for (unsigned field=0; field<5; ++field) {
        bad=sealed;
        uint32_t *versions[]={&bad.checked_schema,&bad.checked_contract,&bad.value_abi,&bad.call_abi,&bad.program_abi};
        --*versions[field]; out=untouched;
        CHECK(xr_compile_native_input_seal(&ctx,&bad,&out)==XR_XIR_BAD_STRUCTURE);
        CHECK(!memcmp(&out,&untouched,sizeof(out)));
    }
    bad=sealed; bad.id.bytes[0]^=1;
    CHECK(xr_compile_native_artifact_verify(artifact,&bad,sizeof(native_bytes))==XR_XIR_BAD_STRUCTURE);
    CHECK(xr_compile_native_artifact_seal(&ctx,&bad,native_bytes,sizeof(native_bytes),sizeof(native_bytes),&empty)==XR_XIR_BAD_STRUCTURE && !empty);
    /* Valid reseals of every independently variable input cannot replace expected. */
    for (unsigned field=0; field<15; ++field) {
        bad=raw;
        if (field==0) bad.entry=0;
        else if (field==1) ++bad.function_count;
        else if (field==2) ++bad.module_count;
        else if (field<8) {
            XrFingerprint *ids[]={&bad.source_checked_id,&bad.closed_checked_id,&bad.lowered_layout_id,&bad.codegen_policy_id,&bad.generated_digest};
            ids[field-3]->bytes[0]^=1;
        } else {
            XrToolchainInput changed=tc;
            if (field==8) changed.provider=XR_TOOLCHAIN_PROVIDER_CLANG;
            if (field==9) changed.provider_version="another";
            if (field==10) changed.target_triple="another";
            if (field==11) changed.codegen_options="another";
            if (field==12) changed.sysroot_id.bytes[0]^=1;
            if (field==13) changed.runtime_sdk_id.bytes[0]^=1;
            if (field==14) changed.target_profile_id.bytes[0]^=1;
            CHECK(xr_compile_toolchain_binding_build(r,&changed,&bad.toolchain)==XR_TOOLCHAIN_BINDING_OK);
        }
        CHECK(xr_compile_native_input_seal(&ctx,&bad,&out)==XR_XIR_OK);
        CHECK(xr_compile_native_artifact_verify(artifact,&out,sizeof(native_bytes))==XR_XIR_BAD_STRUCTURE);
        XrXirNativeArtifact *attack=NULL;
        CHECK(xr_compile_native_artifact_seal(&ctx,&out,native_bytes,sizeof(native_bytes),sizeof(native_bytes),&attack)==XR_XIR_OK);
        CHECK(xr_compile_native_artifact_verify(attack,&sealed,sizeof(native_bytes))==XR_XIR_BAD_STRUCTURE);
        xr_compile_native_artifact_free(attack);
    }
    XrXirNativeArtifactView *view=(XrXirNativeArtifactView *)xr_compile_native_artifact_view(artifact);
    XrXirNativeArtifactView original=*view;
    view->schema_version=1;
    CHECK(xr_compile_native_artifact_verify(artifact,&sealed,sizeof(native_bytes))==XR_XIR_BAD_STRUCTURE); *view=original;
    view->reserved=1;
    CHECK(xr_compile_native_artifact_verify(artifact,&sealed,sizeof(native_bytes))==XR_XIR_BAD_STRUCTURE); *view=original;
    view->size=SIZE_MAX;
    CHECK(xr_compile_native_artifact_verify(artifact,&sealed,SIZE_MAX)==XR_XIR_BAD_STRUCTURE); *view=original;
    view->bytes=(const uint8_t *)(uintptr_t)1;
    CHECK(xr_compile_native_artifact_verify(artifact,&sealed,sizeof(native_bytes))==XR_XIR_BAD_STRUCTURE); *view=original;
    uint8_t *owned=(uint8_t *)view->bytes; owned[3]^=1;
    CHECK(xr_compile_native_artifact_verify(artifact,&sealed,sizeof(native_bytes))==XR_XIR_BAD_STRUCTURE); owned[3]^=1;
    view->native_digest.bytes[0]^=1;
    CHECK(xr_compile_native_artifact_verify(artifact,&sealed,sizeof(native_bytes))==XR_XIR_BAD_STRUCTURE); *view=original;
    view->id.bytes[0]^=1;
    CHECK(xr_compile_native_artifact_verify(artifact,&sealed,sizeof(native_bytes))==XR_XIR_BAD_STRUCTURE); *view=original;
    xr_compile_native_artifact_free(artifact); xr_compile_resources_release(r); CHECK(!physical);
    CHECK(xr_compile_native_artifact_view(NULL)==NULL);
    xr_compile_native_artifact_free(NULL);
}
#if defined(XR_OS_WINDOWS)
#include <windows.h>
static void guard_pages(void) {
    SYSTEM_INFO info; GetSystemInfo(&info);
    size_t page=info.dwPageSize;
    uint8_t *pages=VirtualAlloc(NULL,page*3,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(pages); DWORD old;
    CHECK(VirtualProtect(pages+2*page,page,PAGE_NOACCESS,&old));
    char *empty=(char *)(pages+2*page-1); *empty=0;
    XrToolchainInput in=request(); in.provider_version=empty;
    reset(); XrCompileResources *r=owner(UINT64_MAX); XrToolchainBinding b;
    CHECK(xr_compile_toolchain_binding_build(r,&in,&b)==XR_TOOLCHAIN_BINDING_BAD_STRUCTURE);
    xr_compile_resources_release(r);
    in.provider_version=(const char *)(pages+2*page);
    /* The limit stops immediately before the very first unreadable text byte. */
    reset(); r=owner(1+8+3*32+sizeof(b)+8+3*32);
    CHECK(xr_compile_toolchain_binding_build(r,&in,&b)==XR_TOOLCHAIN_BINDING_BUDGET);
    xr_compile_resources_release(r);
    char *long_text=(char *)(pages+2*page-(XR_TOOLCHAIN_TEXT_LIMIT+1));
    memset(long_text,'a',XR_TOOLCHAIN_TEXT_LIMIT+1); in.provider_version=long_text;
    reset(); r=owner(UINT64_MAX);
    CHECK(xr_compile_toolchain_binding_build(r,&in,&b)==XR_TOOLCHAIN_BINDING_BAD_STRUCTURE);
    xr_compile_resources_release(r);
    CHECK(VirtualFree(pages,0,MEM_RELEASE)); CHECK(!physical);
}
#endif
int main(void) {
    fixed_vectors(); binding_boundaries(); resources_and_oom(); native_rejections();
#if defined(XR_OS_WINDOWS)
    guard_pages();
#endif
    puts("toolchain owner tests passed");
    return 0;
}
