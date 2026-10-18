/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_effect_derivation_snapshot.c - Receiving ownership and complete field correspondence
 */
#define main original_derivation_main
#include "test_xir_source_derivation.c"
#undef main
#include "xir/xxir_effects.c"

typedef struct SnapshotFixture {
    char name[8],texts[4][3][16];
    XrXirSourceModule module;
    XrXirFunctionIdentity function;
    XrXirType argument;
    XrXirDerivationMember members[2];
    XrXirGeneratedOrigin origins[4];
    XrXirDerivationPolicy policy;
    XrXirDerivationGrant grant;
    XrXirDerivationTable table;
    XrXirImplementation implementation;
    XrXirImplementationTable implementations;
    XrXirDeclarations declarations;
} SnapshotFixture;

/* These literal data rows test copying and comparison only. They are not an
 * admitted module, a derivation grant proof or an execution permission. */
static void snapshot_fixture(SnapshotFixture *f) {
    memset(f,0,sizeof(*f));memcpy(f->name,"owner",6);
    f->module=(XrXirSourceModule){.name=f->name,.name_length=5,.lexical_scope=1};
    for (uint32_t i=0;i<4;++i) {
        memcpy(f->texts[i][0],"unit",5);memcpy(f->texts[i][1],"test.xr",8);
        memcpy(f->texts[i][2],"Record.value",13);
        f->origins[i]=(XrXirGeneratedOrigin){.module={f->texts[i][0],4},
            .file={f->texts[i][1],7},.field_path={f->texts[i][2],12},.line=i+1,.column=i+2};
    }
    f->argument=XR_XIR_I64;
    f->members[0]=(XrXirDerivationMember){0,XR_XIR_DERIVE_DESCRIBE|XR_XIR_DERIVE_READ};
    f->members[1]=(XrXirDerivationMember){3,XR_XIR_DERIVE_DESCRIBE|XR_XIR_DERIVE_WRITE};
    f->policy=(XrXirDerivationPolicy){.target=(XrXirType)256,
        .protocol={.arguments=&f->argument,.argument_count=1},.constructor=1,
        .members=f->members,.member_count=2,.origin=f->origins[0]};
    f->grant=(XrXirDerivationGrant){.target=(XrXirType)256,
        .origins={.entry=f->origins[1],.instructions=&f->origins[2],.instruction_count=2}};
    f->table=(XrXirDerivationTable){.policies=&f->policy,.policy_count=1,.records=&f->grant,.count=1};
    f->implementation=(XrXirImplementation){.derivation_policy=1};
    f->implementations=(XrXirImplementationTable){.records=&f->implementation,.count=1};
    f->declarations=(XrXirDeclarations){.modules=&f->module,.module_count=1,
        .functions=&f->function,.derivations=&f->table,.implementations=&f->implementations};
}

static void snapshot_origin_change(XrXirGeneratedOrigin *origin,unsigned field) {
    if (!field) ++origin->line;
    else if (field==1) ++origin->column;
    else if (field==2) ((char *)origin->module.bytes)[0]='x';
    else if (field==3) ((char *)origin->file.bytes)[0]='x';
    else ((char *)origin->field_path.bytes)[0]='x';
}

static void snapshot_change(SnapshotFixture *f,unsigned mode) {
    if (!mode) ++f->policy.grantor_module;
    else if (mode==1) ++f->policy.target;
    else if (mode==2) ++f->policy.generator;
    else if (mode==3) ++f->policy.constructor;
    else if (mode==4) ++f->policy.protocol.declaration;
    else if (mode==5) f->argument=XR_XIR_BOOL;
    else if (mode==6) ++f->members[1].field;
    else if (mode==7) f->members[0].operations=XR_XIR_DERIVE_READ;
    else if (mode<13) snapshot_origin_change(&f->policy.origin,mode-8);
    else if (mode==13) ++f->grant.function;
    else if (mode==14) ++f->grant.policy;
    else if (mode==15) ++f->grant.source_row;
    else if (mode==16) ++f->grant.root;
    else if (mode==17) ++f->grant.target;
    else if (mode<23) snapshot_origin_change(&f->grant.origins.entry,mode-18);
    else if (mode<33) snapshot_origin_change(&f->origins[2+(mode-23)/5],(mode-23)%5);
    else if (mode==33) f->grant.origins.instruction_count=1;
    else if (mode==34) f->policy.member_count=1;
    else if (mode==35) {f->policy.protocol.argument_count=0;f->policy.protocol.arguments=NULL;}
    else if (mode==36) {f->table.policy_count=0;f->table.policies=NULL;}
    else if (mode==37) {f->table.count=0;f->table.records=NULL;}
    else if (mode==38) f->declarations.derivations=NULL;
    else if (mode<51) {
        unsigned n=(mode-39)/3,field=(mode-39)%3;
        XrXirGeneratedOrigin *o=n==0?&f->policy.origin:n==1?&f->grant.origins.entry:&f->origins[n];
        if (!field) --o->module.length;else if (field==1) --o->file.length;else --o->field_path.length;
    }
    else if (mode==51) ++f->members[0].field;
    else if (mode==52) f->members[1].operations=XR_XIR_DERIVE_WRITE;
    else ++f->implementation.derivation_policy;
}

static void snapshot_fields(void) {
    const XrXirCompileContext *c=source_fixture_source_owner(67108864,128000000);
    SnapshotFixture input,expected;snapshot_fixture(&input);snapshot_fixture(&expected);
    XrXirDeclarations *copy=NULL;
    CHECK(effect_invocation_declarations_clone(c,&input.declarations,1,&copy)==XR_XIR_OK && copy);
    CHECK(copy->derivations && copy->derivations!=&input.table &&
        copy->derivations->policies!=&input.policy && copy->derivations->records!=&input.grant);
    CHECK(copy->derivations->policies[0].protocol.arguments!=&input.argument &&
        copy->derivations->policies[0].members!=input.members &&
        copy->derivations->policies[0].origin.module.bytes!=input.policy.origin.module.bytes &&
        copy->derivations->records[0].origins.instructions!=input.grant.origins.instructions);
    memset(&input,0xa5,sizeof(input));
    CHECK(effect_body_declarations_match(c,copy,&expected.declarations,1,NULL)==XR_XIR_OK);
    for (unsigned mode=0;mode<54;++mode) {
        snapshot_fixture(&expected);snapshot_change(&expected,mode);bool same=true;
        XrCompileResourceStats before={0},after={0};
        CHECK(xr_compile_resources_stats(c->resources,&before)==XR_COMPILE_RESOURCE_OK);
        size_t attempts=source_fixture_compile_attempts;
        CHECK(effect_body_declarations_match(c,copy,&expected.declarations,1,&same)==XR_XIR_OK && !same);
        CHECK(effect_body_declarations_match(c,copy,&expected.declarations,1,NULL)==XR_XIR_BAD_STRUCTURE);
        CHECK(xr_compile_resources_stats(c->resources,&after)==XR_COMPILE_RESOURCE_OK &&
            source_fixture_compile_attempts==attempts && after.allocated_bytes==before.allocated_bytes &&
            after.live_bytes==before.live_bytes);
    }
    snapshot_fixture(&expected);expected.policy.members=NULL;bool same=true;
    CHECK(effect_body_declarations_match(c,copy,&expected.declarations,1,&same)==XR_XIR_BAD_STRUCTURE && same);
    effect_invocation_declarations_free(copy);source_fixture_source_owner_close(c);
    source_fixture_source_owners_free();
}

static XrXirStatus snapshot_trial(XrCompileResourceLimits caps,size_t fail_at,bool compare,
    XrCompileResourceStats *usage,size_t *sites) {
    XrXirCompileContext c={0};
    CHECK(xr_compile_resources_new(&caps,&c.resources)==XR_COMPILE_RESOURCE_OK);
    c.limits=xr_xir_compile_default_limits();XrCompileResourceStats baseline={0};
    CHECK(xr_compile_resources_stats(c.resources,&baseline)==XR_COMPILE_RESOURCE_OK);
    SnapshotFixture input;snapshot_fixture(&input);
    SnapshotFixture before=input;XrXirDeclarations *copy=NULL;
    source_fixture_compile_attempts=0;source_fixture_compile_injected=false;source_fixture_compile_fail_at=fail_at;
    XrXirStatus status=effect_invocation_declarations_clone(&c,&input.declarations,1,&copy);
    if (sites) *sites=source_fixture_compile_attempts;
    if (fail_at!=SIZE_MAX) CHECK(status==XR_XIR_OUT_OF_MEMORY && source_fixture_compile_injected &&
        source_fixture_compile_attempts>fail_at && !copy);
    source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(!memcmp(&before,&input,sizeof(input)));
    if (status==XR_XIR_OK && compare) status=effect_body_declarations_match(&c,copy,&input.declarations,1,NULL);
    if (usage) CHECK(xr_compile_resources_stats(c.resources,usage)==XR_COMPILE_RESOURCE_OK);
    effect_invocation_declarations_free(copy);XrCompileResourceStats end={0};
    CHECK(xr_compile_resources_stats(c.resources,&end)==XR_COMPILE_RESOURCE_OK && end.live_bytes==baseline.live_bytes);
    xr_compile_resources_release(c.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    return status;
}

static void snapshot_resources(void) {
    XrCompileResourceLimits caps={67108864,8388608,128000000};
    XrCompileResourceStats usage={0},matched={0};size_t sites=0;
    CHECK(snapshot_trial(caps,SIZE_MAX,false,&usage,&sites)==XR_XIR_OK && sites);
    for (size_t i=0;i<sites;++i) {
        CHECK(snapshot_trial(caps,i,false,NULL,NULL)==XR_XIR_OUT_OF_MEMORY);
        CHECK(snapshot_trial(caps,SIZE_MAX,false,NULL,NULL)==XR_XIR_OK);
    }
    caps=(XrCompileResourceLimits){usage.allocated_bytes,usage.peak_bytes,usage.work};
    CHECK(caps.allocated_bytes && caps.live_bytes && caps.work);
    CHECK(snapshot_trial(caps,SIZE_MAX,false,NULL,NULL)==XR_XIR_OK);
    for (unsigned axis=0;axis<3;++axis) {
        XrCompileResourceLimits less=caps;
        if (!axis) --less.allocated_bytes;else if (axis==1) --less.live_bytes;else --less.work;
        CHECK(snapshot_trial(less,SIZE_MAX,false,NULL,NULL)==XR_XIR_BUDGET);
    }
    caps=(XrCompileResourceLimits){67108864,8388608,128000000};
    CHECK(snapshot_trial(caps,SIZE_MAX,true,&matched,NULL)==XR_XIR_OK && matched.work>usage.work);
    caps.work=matched.work;CHECK(snapshot_trial(caps,SIZE_MAX,true,NULL,NULL)==XR_XIR_OK);
    --caps.work;CHECK(snapshot_trial(caps,SIZE_MAX,true,NULL,NULL)==XR_XIR_BUDGET);
    printf("Owned derivation metadata:54 independent field changes, %zu compiler allocation faults, three exact/minus-one axes, matcher work boundary and physical0\n",sites);
}

int main(void) {
    snapshot_fields();snapshot_resources();
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes && !runtime_live && !runtime_bytes);
    return 0;
}
