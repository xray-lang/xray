/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_library_typed_native.c - Native and mixed resource owners
 */
#define main h1n_reference_main
#include "xir_library_typed_native_base.h"
#undef main
#include "xir/xxir_checked.h"
#include <ctype.h>
#include "xir_library_typed_native_resource_cases.h"

enum { H1C_OWNER,H1C_READ,H1C_SPECIALIZE,H1C_CHECK,H1C_WRITE,H1C_LOWER,H1C_LOWER_CHECK,
    H1C_ENTRIES,H1C_BINDINGS,H1C_BIND,H1C_SEAL,H1C_PHASES };
static const char *const h1c_names[H1C_PHASES] = {"owner","proof_read","specialize","closed_verify",
    "proof_write","lower","lowered_verify","entry_storage","binding_storage","provider_bind","Program_seal"};
static const XrCompileResourceLimits h1c_caps = {UINT64_C(67108864),UINT64_C(8388608),UINT64_C(128000000)};
static const char *h1nr_mode;
static const XrXirProgramSpec *h1nr_native;
static uint32_t h1nr_run;
static bool h1nr_run_native,h1nr_vm_only;
static H1NativeOwner h1nr_owner;

typedef struct H1NativeCompiler {
    XrXirCompileContext context;
    XrXirArtifact *checked,*closed;
    XrXirCheckedPacket packet;
    XrXirProgramSpec spec;
    H1NrFixture fixture;
    const RootParameterSourceOracle *oracle;
    size_t sites[H1C_PHASES], owner_blocks, owner_bytes;
    unsigned phase;
} H1NativeCompiler;

static XrXirStatus h1nc_status(XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return XR_XIR_OK;
    CHECK(status == XR_COMPILE_RESOURCE_BUDGET || status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
}
static XrCompileResourceStats h1nc_stats(const H1NativeCompiler *c) {
    XrCompileResourceStats stats = {0};
    if (c->context.resources) CHECK(xr_compile_resources_stats(c->context.resources,&stats) == XR_COMPILE_RESOURCE_OK);
    return stats;
}
static void h1nc_init(H1NativeCompiler *c, const RootParameterSourceOracle *oracle) {
    instance_compile_zero(); CHECK(!runtime_live && !runtime_bytes);
    CHECK(!h1nr_owner.lowered && !h1nr_owner.entries && !h1nr_owner.bindings);
    h1nr_owner = (H1NativeOwner){0}; h1nr_owner.count = h1nr_native->entry_count;
    h1nr_owner.run=h1nr_run;h1nr_owner.entry=h1nr_native->declarations->entry_function;
    CHECK(h1nr_owner.count && h1nr_run < h1nr_owner.count && !h1nr_native->code.owner && !h1nr_native->code.release);
    CHECK(h1nr_native->declarations && h1nr_native->declarations->module_count == oracle->modules);
    CHECK(h1nr_native->declarations->slot_count == (oracle->root ? 1u : 0u));
    *c = (H1NativeCompiler){0}; c->oracle = oracle; c->spec = *h1nr_native;
    c->fixture = (H1NrFixture){NULL,c->spec.declarations->entry_function,h1nr_run,
        c->spec.declarations->root_module,c->spec.declarations->slot_count};
}
static XrXirStatus h1nc_action(H1NativeCompiler *c, unsigned p, const XrCompileResourceLimits *caps) {
    XrXirStatus status = XR_XIR_OK;
    switch (p) {
    case H1C_OWNER:
        status = h1nc_status(xr_compile_resources_new(caps,&c->context.resources));
        if (status == XR_XIR_OK) {
            c->context.limits = xr_xir_compile_default_limits();
            c->owner_blocks = instance_compile_live; c->owner_bytes = instance_compile_bytes;
            CHECK(c->owner_blocks == 1);
        }
        else CHECK(!c->context.resources); break;
#ifdef H1_MIXED
    case H1C_READ:
        status = xr_xir_compile_checked_read(&c->context,h1nr_native->proof.bytes,h1nr_native->proof.length,&c->checked,NULL);
        if (status != XR_XIR_OK) CHECK(!c->checked); break;
    case H1C_SPECIALIZE:
        status = xr_xir_compile_specialize(c->checked,&c->closed,NULL);
        if (status == XR_XIR_OK) { xr_xir_compile_artifact_free(c->checked); c->checked = NULL; }
        else CHECK(!c->closed); break;
    case H1C_CHECK: status = xr_xir_compile_artifact_verify(c->closed,NULL); break;
    case H1C_WRITE:
        status = xr_xir_compile_checked_write(c->closed,&c->packet,NULL);
        if (status == XR_XIR_OK) {
            CHECK(c->packet.length == h1nr_native->proof.length && !memcmp(c->packet.bytes,h1nr_native->proof.bytes,c->packet.length));
            memset(c->packet.bytes,0xa5,c->packet.length); xr_xir_compile_checked_packet_free(&c->packet);
        } else CHECK(!c->packet.bytes && !c->packet.length); break;
    case H1C_LOWER:
        status = xr_xir_compile_lower(c->closed,&h1nr_native->target,&h1nr_owner.lowered,NULL);
        if (status == XR_XIR_OK) { xr_xir_compile_artifact_free(c->closed); c->closed = NULL; }
        else CHECK(!h1nr_owner.lowered); break;
    case H1C_LOWER_CHECK: {
        status = xr_xir_compile_artifact_verify(h1nr_owner.lowered,NULL);
        if (status != XR_XIR_OK) break;
        const XrXirModule *module = xr_xir_compile_artifact_module(h1nr_owner.lowered);
        CHECK(module && module->function_count == h1nr_owner.count);
        c->spec.proof = xr_xir_compile_program_proof(h1nr_owner.lowered);
        CHECK(c->spec.proof.length == h1nr_native->proof.length && !memcmp(c->spec.proof.bytes,h1nr_native->proof.bytes,c->spec.proof.length));
        CHECK(!memcmp(c->spec.proof.identity,h1nr_native->proof.identity,32));
        c->spec.declarations = module->declarations; c->spec.types = module->types; break;
    }
#else
    case H1C_READ: case H1C_SPECIALIZE: case H1C_CHECK: case H1C_WRITE: case H1C_LOWER: case H1C_LOWER_CHECK: break;
#endif
    case H1C_ENTRIES: {
        void *memory = NULL;
        status = h1nc_status(xr_compile_resources_calloc(c->context.resources,h1nr_owner.count,sizeof(*h1nr_owner.entries),&memory));
        if (status == XR_XIR_OK) h1nr_owner.entries = memory; else CHECK(!memory); break;
    }
    case H1C_BINDINGS: {
        void *memory = NULL;
        status = h1nc_status(xr_compile_resources_calloc(c->context.resources,h1nr_owner.count,sizeof(*h1nr_owner.bindings),&memory));
        if (status == XR_XIR_OK) h1nr_owner.bindings = memory; else CHECK(!memory); break;
    }
    case H1C_BIND:
        for (uint32_t i = 0; i < h1nr_owner.count; ++i) {
            H1NativeBinding *binding = &h1nr_owner.bindings[i];
            binding->owner = &h1nr_owner; binding->function = i; binding->native = true;
#ifdef H1_MIXED
            binding->native = !h1nr_vm_only && (i == h1nr_run ? h1nr_run_native : !h1nr_run_native);
            if (!binding->native) {
                XrXirVmBinding before = binding->vm; XrXirCallEntry empty = binding->actual;
                status = xr_xir_compile_vm_bind(h1nr_owner.lowered,i,&binding->vm,&binding->actual);
                if (status != XR_XIR_OK) {
                    CHECK(!memcmp(&before,&binding->vm,sizeof(before)) && !memcmp(&empty,&binding->actual,sizeof(empty)));
                    return status;
                }
                CHECK(binding->actual.environment == &binding->vm);
            } else
#endif
            { CHECK(!h1nr_native->entries[i].environment); binding->actual = h1nr_native->entries[i]; }
            CHECK(binding->actual.resume && binding->actual.release);
            h1nr_owner.entries[i] = binding->actual;
            h1nr_owner.entries[i].resume = h1n_resume; h1nr_owner.entries[i].release = h1n_release;
            h1nr_owner.entries[i].environment = &binding->vm;
        }
        for (uint32_t i = 0; i < h1nr_owner.count; ++i)
            printf("TYPED_NATIVE_RESOURCE_BINDING file=%s mode=%s function=%u native=%u is_run=%u\n",
                c->oracle->file, h1nr_mode, i, (unsigned)h1nr_owner.bindings[i].native, (unsigned)(i == h1nr_run));
        c->spec.entries = h1nr_owner.entries; c->spec.code = (XrXirCodeLease){&h1nr_owner,h1n_code_drop}; break;
    case H1C_SEAL:
        status = xr_xir_compile_program_seal(&c->context,&c->spec,&c->fixture.program);
        if (status == XR_XIR_OK) {
            CHECK(c->fixture.program && c->fixture.program->permissions && !h1nr_owner.code_releases);
            const XrXirProgramPermission *f = &c->fixture.program->permissions->entries[h1nr_run];
            CHECK(f->requires_root == c->oracle->root && f->unresolved == c->oracle->unresolved);
        } else CHECK(!c->fixture.program && !h1nr_owner.code_releases); break;
    default: CHECK(false); break;
    }
    return status;
}
static XrXirStatus h1nc_step(H1NativeCompiler *c, unsigned p, const XrCompileResourceLimits *caps) {
    XrXirArtifact *input = p == H1C_SPECIALIZE ? c->checked :
        p == H1C_CHECK || p == H1C_WRITE || p == H1C_LOWER ? c->closed :
        p >= H1C_LOWER_CHECK ? h1nr_owner.lowered : NULL;
    const XrXirModule *module = input ? xr_xir_compile_artifact_module(input) : NULL;
    XrXirModule module_before = {0};
    if (module) memcpy(&module_before,module,sizeof(module_before));
    XrXirProgramSpec spec_before; memcpy(&spec_before,&c->spec,sizeof(spec_before));
    size_t first = instance_compile_attempts;
    XrXirStatus status = h1nc_action(c,p,caps);
    if (status != XR_XIR_OK && module) CHECK(!memcmp(module,&module_before,sizeof(module_before)));
    if (p == H1C_SEAL) CHECK(!memcmp(&spec_before,&c->spec,sizeof(spec_before)));
    c->sites[p] = instance_compile_attempts-first; c->phase = p;
    XrCompileResourceStats s = h1nc_stats(c);
    printf("TYPED_NATIVE_COMPILER_STAGE file=%s mode=%s phase=%u name=%s sites=%zu status=%u allocated=%llu peak=%llu work=%llu live=%llu\n",
        c->oracle->file,h1nr_mode,p,h1c_names[p],c->sites[p],(unsigned)status,
        (unsigned long long)s.allocated_bytes,(unsigned long long)s.peak_bytes,(unsigned long long)s.work,(unsigned long long)s.live_bytes);
    return status;
}
static XrXirStatus h1nc_pipeline(H1NativeCompiler *c, unsigned first, const XrCompileResourceLimits *caps) {
    for (unsigned p = first; p < H1C_PHASES; ++p) {
        XrXirStatus status = h1nc_step(c,p,caps); if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static void h1nc_clear(H1NativeCompiler *c) {
    XrCompileResourceStats before = h1nc_stats(c);
    xr_xir_compile_program_drop(c->fixture.program); c->fixture.program = NULL;
    xr_xir_compile_artifact_free(c->checked); c->checked = NULL;
    xr_xir_compile_artifact_free(c->closed); c->closed = NULL;
    xr_xir_compile_checked_packet_free(&c->packet);
    xr_xir_compile_artifact_free(h1nr_owner.lowered); h1nr_owner.lowered = NULL;
    xr_compile_resources_free(h1nr_owner.entries); h1nr_owner.entries = NULL;
    xr_compile_resources_free(h1nr_owner.bindings); h1nr_owner.bindings = NULL;
    XrCompileResourceStats after = h1nc_stats(c);
    CHECK(after.allocated_bytes == before.allocated_bytes && after.peak_bytes == before.peak_bytes && after.work == before.work);
    CHECK(after.live_bytes == (c->context.resources ? sizeof(XrCompileResources) : 0));
    CHECK(instance_compile_live == c->owner_blocks && instance_compile_bytes == c->owner_bytes);
    CHECK(!runtime_live && !runtime_bytes);
    c->spec = *h1nr_native; h1nr_owner.code_releases = 0;
}
static void h1nr_provider_finish(bool both) {
    CHECK(h1nr_owner.code_releases == 1 && !h1nr_owner.lowered && !h1nr_owner.entries && !h1nr_owner.bindings);
    unsigned complete = 0;
    for (unsigned i = 0; i < 2; ++i) {
        bool observed = h1nr_owner.steps[i][1] && h1nr_owner.releases[i][1];
#ifdef H1_MIXED
        if(h1nr_vm_only){
            CHECK(!h1nr_owner.steps[i][1]&&!h1nr_owner.releases[i][1]&&!h1nr_owner.crossings[i]);
            observed=h1nr_owner.steps[i][0]&&h1nr_owner.releases[i][0];
        } else observed = observed && h1nr_owner.steps[i][0] && h1nr_owner.releases[i][0] && h1nr_owner.crossings[i];
#else
        CHECK(!h1nr_owner.steps[i][0] && !h1nr_owner.releases[i][0] && !h1nr_owner.crossings[i]);
#endif
        complete += observed;
    }
    CHECK(complete >= (both ? 2u : 1u));
    for (unsigned i = 0; i < 2; ++i)
        printf("TYPED_NATIVE_RESOURCE_PROVIDER_INSTANCE mode=%s witness=%u owner=0x%" PRIxPTR " nativeSteps=%llu vmSteps=%llu nativeReleases=%llu vmReleases=%llu crossings=%llu\n",
            h1nr_mode, i, h1nr_owner.instance_ids[i],
            (unsigned long long)h1nr_owner.steps[i][1], (unsigned long long)h1nr_owner.steps[i][0],
            (unsigned long long)h1nr_owner.releases[i][1], (unsigned long long)h1nr_owner.releases[i][0],
            (unsigned long long)h1nr_owner.crossings[i]);
    printf("TYPED_NATIVE_RESOURCE_PROVIDERS mode=%s fullWitnesses=%u codeReleases=%u physical=0/0\n",h1nr_mode,complete,h1nr_owner.code_releases);
}
static H1NrFixture h1nr_build(const RootParameterSourceOracle *oracle) {
    H1NativeCompiler c; h1nc_init(&c,oracle);
    CHECK(h1nc_pipeline(&c,0,&h1c_caps) == XR_XIR_OK);
    xr_compile_resources_release(c.context.resources); return c.fixture;
}
#include "xir_library_typed_native_resource_oracle.h"

static void h1nc_counts(H1NativeCompiler *c, const H1Expected *e) {
    XrCompileResourceStats s = h1nc_stats(c);
    for (unsigned p = 0; p < H1C_PHASES; ++p) CHECK(c->sites[p] == e->compiler_sites[p]);
    CHECK(s.allocated_bytes == e->compiler_axes[0] && s.peak_bytes == e->compiler_axes[1] && s.work == e->compiler_axes[2]);
}
static void h1nr_census(const RootParameterSourceOracle *oracle, const H1Expected *e) {
    H1NativeCompiler c; h1nc_init(&c,oracle); CHECK(h1nc_pipeline(&c,0,&h1c_caps) == XR_XIR_OK);
    if (e) h1nc_counts(&c,e);
    XrCompileResourceStats s = h1nc_stats(&c);
    printf("TYPED_NATIVE_RESOURCE_COMPILER_CENSUS file=%s mode=%s sites=",oracle->file,h1nr_mode);
    for (unsigned p = 0; p < H1C_PHASES; ++p) printf("%s%zu",p ? "," : "",c.sites[p]);
    printf(" allocated=%llu peak=%llu work=%llu\n",(unsigned long long)s.allocated_bytes,(unsigned long long)s.peak_bytes,(unsigned long long)s.work);
    xr_compile_resources_release(c.context.resources); c.context.resources = NULL;
    H1Runtime t; h1r_normal(&t,c.fixture,oracle);
    for(unsigned i=0;i<2;++i){
        CHECK(h1nr_owner.run_releases[i]==2 && h1nr_owner.entry_releases[i]==1);
        printf("TYPED_NATIVE_CALLS file=%s mode=%s instance=%u resultCalls=%llu entryCalls=%llu\n",
            oracle->file,h1nr_mode,i,(unsigned long long)h1nr_owner.run_releases[i],(unsigned long long)h1nr_owner.entry_releases[i]);
    }
    if (e) for (unsigned i = 0; i < 2; ++i) {
        for (unsigned p = 0; p < H1R_PHASES; ++p) CHECK(t.w[i].sites[p] == e->sites[i][p]);
        for (unsigned a = 0; a < H1R_AXES; ++a) CHECK(t.w[i].selectors[a] == e->axes[i][a]);
    }
}
static void h1nc_oom(const RootParameterSourceOracle *oracle, const H1Expected *e, unsigned stage, size_t first, size_t count) {
    CHECK(stage < H1C_PHASES && count && count <= 64 && first <= e->compiler_sites[stage] && count <= e->compiler_sites[stage]-first);
    for (size_t point = first; point < first+count; ++point) {
        H1NativeCompiler c; h1nc_init(&c,oracle);
        for (unsigned p = 0; p < stage; ++p) CHECK(h1nc_step(&c,p,&h1c_caps) == XR_XIR_OK);
        XrCompileResources *owner = c.context.resources; XrCompileResourceStats before = h1nc_stats(&c);
        size_t begin = instance_compile_attempts; CHECK(point < SIZE_MAX-begin);
        instance_compile_injected = false; instance_compile_fail_at = begin+point;
        XrXirStatus status = h1nc_step(&c,stage,&h1c_caps); instance_compile_fail_at = SIZE_MAX;
        CHECK(status == XR_XIR_OUT_OF_MEMORY && instance_compile_injected && instance_compile_attempts == begin+point+1);
        h1nc_clear(&c); XrCompileResourceStats failed = h1nc_stats(&c);
        CHECK(c.context.resources == owner && failed.work >= before.work && failed.allocated_bytes >= before.allocated_bytes);
        CHECK(h1nc_pipeline(&c,owner ? H1C_READ : H1C_OWNER,&h1c_caps) == XR_XIR_OK);
        XrCompileResourceStats retried = h1nc_stats(&c);
        CHECK(!owner || (c.context.resources == owner && retried.work >= failed.work && retried.allocated_bytes >= failed.allocated_bytes));
        xr_compile_resources_release(c.context.resources); c.context.resources = NULL;
        H1Runtime t; h1r_normal(&t,c.fixture,oracle);
        printf("TYPED_NATIVE_RESOURCE_COMPILER_OOM file=%s mode=%s phase=%u point=%zu denominator=%zu sameOwner=%u refund=0 physical=0/0\n",
            oracle->file,h1nr_mode,stage,point,e->compiler_sites[stage],(unsigned)(owner != NULL));
    }
    printf("TYPED_NATIVE_RESOURCE_COMPILER_RANGE file=%s mode=%s phase=%u first=%zu count=%zu denominator=%zu complete=1\n",
        oracle->file,h1nr_mode,stage,first,count,e->compiler_sites[stage]);
}
static void h1nc_axes(const RootParameterSourceOracle *oracle, const H1Expected *e) {
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits caps = h1c_caps; uint64_t amount = e->compiler_axes[axis]-minus;
        if (!axis) caps.allocated_bytes = amount; else if (axis == 1) caps.live_bytes = amount; else caps.work = amount;
        H1NativeCompiler c; h1nc_init(&c,oracle);
        CHECK(h1nc_pipeline(&c,0,&caps) == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        if (!minus) {
            h1nc_counts(&c,e); xr_compile_resources_release(c.context.resources); c.context.resources = NULL;
            H1Runtime t; h1r_normal(&t,c.fixture,oracle);
        } else {
            XrCompileResources *owner = c.context.resources; h1nc_clear(&c); XrCompileResourceStats before = h1nc_stats(&c);
            CHECK(h1nc_pipeline(&c,owner ? H1C_READ : H1C_OWNER,&caps) == XR_XIR_BUDGET);
            XrCompileResourceStats after = h1nc_stats(&c);
            CHECK(c.context.resources == owner && after.work >= before.work && after.allocated_bytes >= before.allocated_bytes);
            h1nc_clear(&c); xr_compile_resources_release(c.context.resources); instance_compile_zero();
        }
        printf("TYPED_NATIVE_RESOURCE_COMPILER_AXIS file=%s mode=%s axis=%u minus1=%u cap=%llu physical=0/0\n",
            oracle->file,h1nr_mode,axis,minus,(unsigned long long)amount);
    }
}
int main(int argc, char **argv) {
    CHECK(argc >= 4 && argc <= 9); (void)&h1nr_drive_pair;
    const XrXirProgramSpec *programs[] = {&h1typed_0_program,&h1typed_1_program,&h1typed_2_program,&h1typed_3_program,&h1typed_4_program,&h1typed_5_program,&h1typed_6_program,&h1typed_7_program,&h1typed_8_program};
    const uint32_t runs[] = {h1typed_0_run,h1typed_1_run,h1typed_2_run,h1typed_3_run,h1typed_4_run,h1typed_5_run,h1typed_6_run,h1typed_7_run,h1typed_8_run};
    size_t index = SIZE_MAX;
    for (size_t i = 0; i < H1_TYPED_CASES; ++i) if (!strcmp(argv[2],rps_oracles[i].file)) index = i;
    CHECK(index != SIZE_MAX); const RootParameterSourceOracle *oracle = &rps_oracles[index];
    h1nr_native = programs[index]; h1nr_run = runs[index]; h1nr_mode = argv[3];
    CHECK(h1nr_native->declarations && h1nr_run < h1nr_native->entry_count);
    const XrXirFunctionIdentity *identity = &h1nr_native->declarations->functions[h1nr_run];
    CHECK(identity->module == h1nr_native->declarations->root_module && identity->exported && !identity->nominal_owner);
    CHECK(h1nr_native->entries[h1nr_run].result == XR_XIR_I64 && !h1nr_native->entries[h1nr_run].parameter_count);

#ifdef H1_MIXED
    h1nr_vm_only=!strcmp(h1nr_mode,"vm");
    h1nr_run_native = !strcmp(h1nr_mode,"native-vm"); CHECK(h1nr_vm_only || h1nr_run_native || !strcmp(h1nr_mode,"vm-native"));
#else
    CHECK(!strcmp(h1nr_mode,"native")); h1nr_run_native = true; (void)h1nr_vm_only;
#endif
    if (!strcmp(argv[1],"--census")) { CHECK(argc == 4); h1nr_census(oracle,NULL); return 0; }
    if (!strcmp(argv[1],"--typed-failure")) {
        CHECK(argc == 5 && (!strcmp(argv[4],"error") || !strcmp(argv[4],"limit")));
        h1r_typed_failure(oracle,!strcmp(argv[4],"limit")); return 0;
    }
    CHECK(argc >= 5); H1Expected e = h1_oracle(argv[4],oracle); h1nr_census(oracle,&e);
    if (!strcmp(argv[1],"--strict")) CHECK(argc == 5);
    else if (!strcmp(argv[1],"--compiler-axes")) { CHECK(argc == 5); h1nc_axes(oracle,&e); }
    else if (!strcmp(argv[1],"--runtime-axes")) { CHECK(argc == 5); h1r_axes(oracle,&e); }
    else if (!strcmp(argv[1],"--same-quota")) { CHECK(argc == 5); h1r_same_quota(oracle,&e); }
    else if (!strcmp(argv[1],"--compiler-oom")) {
        CHECK(argc == 8); uint64_t p = h1_decimal(argv[5]), first = h1_decimal(argv[6]), count = h1_decimal(argv[7]);
        CHECK(p < H1C_PHASES && (uint64_t)(size_t)first == first && count && count <= 64);
        h1nc_oom(oracle,&e,(unsigned)p,(size_t)first,(size_t)count);
    } else if (!strcmp(argv[1],"--runtime-oom")) {
        CHECK(argc == 9); uint64_t i = h1_decimal(argv[5]), p = h1_decimal(argv[6]), first = h1_decimal(argv[7]), count = h1_decimal(argv[8]);
        CHECK(i < 2 && p < H1R_PHASES && (uint64_t)(size_t)first == first && count && count <= 64);
        h1r_oom(oracle,&e,(unsigned)i,(unsigned)p,(size_t)first,(size_t)count);
    } else CHECK(false);
    return 0;
}
