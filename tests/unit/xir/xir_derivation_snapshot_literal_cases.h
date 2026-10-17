/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_derivation_snapshot_literal_cases.h - Independent declaration field mutations
 */
#ifndef XIR_DERIVATION_SNAPSHOT_LITERAL_CASES_H
#define XIR_DERIVATION_SNAPSHOT_LITERAL_CASES_H
typedef struct SnapshotDerivationFixture {
    char modules[7][16],files[7][16],paths[7][16];
    XrXirGeneratedOrigin origins[7];
    XrXirType arguments[2];
    XrXirDerivationMember members[2];
    XrXirDerivationPolicy policy;
    XrXirDerivationGrant grants[2];
    XrXirDerivationTable table;
} SnapshotDerivationFixture;
static unsigned snapshot_cases;
static void snapshot_fixture(SnapshotDerivationFixture *f) {
    memset(f,0,sizeof(*f));
    for(unsigned n=0;n<7;++n) {
        memcpy(f->modules[n],"snapshot",8);memcpy(f->files[n],"field.xr",8);memcpy(f->paths[n],"secret",6);
        f->origins[n]=(XrXirGeneratedOrigin){.module={f->modules[n],8},.file={f->files[n],8},
            .field_path={f->paths[n],6},.line=7+n,.column=3+n};
    }
    f->arguments[0]=XR_XIR_I64;f->arguments[1]=XR_XIR_BOOL;
    f->members[0]=(XrXirDerivationMember){1,XR_XIR_DERIVE_READ};
    f->members[1]=(XrXirDerivationMember){5,XR_XIR_DERIVE_WRITE};
    f->policy=(XrXirDerivationPolicy){.target=XR_XIR_I64,.grantor_module=2,
        .protocol={.declaration=3,.arguments=f->arguments,.argument_count=2},.generator=4,.constructor=5,
        .members=f->members,.member_count=2,.origin=f->origins[0]};
    for(unsigned n=0;n<2;++n)f->grants[n]=(XrXirDerivationGrant){.function=3+2*n,.policy=1,
        .source_row=n,.root=2,.target=XR_XIR_I64,
        .origins={.entry=f->origins[1+3*n],.instructions=&f->origins[2+3*n],.instruction_count=2}};
    f->table=(XrXirDerivationTable){.policies=&f->policy,.policy_count=1,.records=f->grants,.count=2};
}
static void snapshot_case(const XrXirCompileContext *context,const XrXirDerivationTable *left,
    const XrXirDerivationTable *right,bool changed,bool malformed) {
    XrXirDeclarations a={.derivations=left},b={.derivations=right};
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    size_t blocks=snapshot_live,bytes=snapshot_bytes;
    XrXirStatus strict=effect_body_declarations_match(context,&a,&b,0,NULL);
    bool same=true;XrXirStatus status=effect_body_declarations_match(context,&a,&b,0,&same);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(strict==((changed || malformed)?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
    CHECK(status==(malformed?XR_XIR_BAD_STRUCTURE:XR_XIR_OK) && same==(malformed || !changed));
    CHECK(after.live_bytes==before.live_bytes && after.allocated_bytes==before.allocated_bytes && after.work>before.work);
    CHECK(snapshot_live==blocks && snapshot_bytes==bytes);++snapshot_cases;
}
#define SNAPSHOT_CHANGE(change) do {snapshot_fixture(&right);change;snapshot_case(context,&left.table,&right.table,true,false);}while(0)
#define SNAPSHOT_MALFORMED(change) do {snapshot_fixture(&right);change;snapshot_case(context,&left.table,&right.table,false,true);}while(0)
static void snapshot_literal_matrix(const XrXirCompileContext *context) {
    SnapshotDerivationFixture left,right;snapshot_fixture(&left);snapshot_fixture(&right);
    snapshot_case(context,&left.table,&right.table,false,false);
    snapshot_case(context,&left.table,NULL,true,false);snapshot_case(context,NULL,&right.table,true,false);
    SNAPSHOT_CHANGE(right.table.count=1);
    SNAPSHOT_CHANGE(right.table.policy_count=0;right.table.policies=NULL);
    SNAPSHOT_CHANGE(right.policy.grantor_module=9);
    SNAPSHOT_CHANGE(right.policy.target=XR_XIR_BOOL);
    SNAPSHOT_CHANGE(right.policy.protocol.declaration=9);
    SNAPSHOT_CHANGE(right.policy.protocol.argument_count=1);
    SNAPSHOT_CHANGE(right.arguments[1]=XR_XIR_I64);
    SNAPSHOT_CHANGE(right.policy.generator=9);
    SNAPSHOT_CHANGE(right.policy.constructor=9);
    SNAPSHOT_CHANGE(right.policy.member_count=1);
    SNAPSHOT_CHANGE(right.members[0].field=2);
    SNAPSHOT_CHANGE(right.members[0].operations=XR_XIR_DERIVE_DESCRIBE);
    SNAPSHOT_CHANGE(right.members[1].field=6);
    SNAPSHOT_CHANGE(right.members[1].operations=XR_XIR_DERIVE_DESCRIBE);
    SNAPSHOT_CHANGE(right.policy.origin.line=8);
    SNAPSHOT_CHANGE(right.policy.origin.column=4);
    SNAPSHOT_CHANGE(right.modules[0][0]='S');SNAPSHOT_CHANGE(right.files[0][0]='F');SNAPSHOT_CHANGE(right.paths[0][0]='S');
    SNAPSHOT_CHANGE(right.policy.origin.module.length=7);
    SNAPSHOT_CHANGE(right.policy.origin.file.length=7);
    SNAPSHOT_CHANGE(right.policy.origin.field_path.length=5);
    SNAPSHOT_CHANGE(right.grants[0].function=4);
    SNAPSHOT_CHANGE(right.grants[0].target=XR_XIR_BOOL);
    SNAPSHOT_CHANGE(right.grants[1].function=6);
    SNAPSHOT_CHANGE(right.grants[1].policy=2);
    SNAPSHOT_CHANGE(right.grants[1].source_row=9);
    SNAPSHOT_CHANGE(right.grants[1].root=9);
    SNAPSHOT_CHANGE(right.grants[1].target=XR_XIR_BOOL);
    SNAPSHOT_CHANGE(right.grants[1].origins.instruction_count=1);
    SNAPSHOT_CHANGE(right.grants[1].origins.entry.line=20);
    SNAPSHOT_CHANGE(right.grants[1].origins.entry.column=20);
    SNAPSHOT_CHANGE(right.modules[4][0]='S');SNAPSHOT_CHANGE(right.files[4][0]='F');SNAPSHOT_CHANGE(right.paths[4][0]='S');
    SNAPSHOT_CHANGE(right.grants[1].origins.entry.module.length=7);
    SNAPSHOT_CHANGE(right.grants[1].origins.entry.file.length=7);
    SNAPSHOT_CHANGE(right.grants[1].origins.entry.field_path.length=5);
    SNAPSHOT_CHANGE(right.grants[0].origins.entry.line=20);
    SNAPSHOT_CHANGE(right.origins[3].column=20);
    SNAPSHOT_CHANGE(right.origins[6].line=20);SNAPSHOT_CHANGE(right.origins[6].column=20);
    SNAPSHOT_CHANGE(right.modules[6][0]='S');SNAPSHOT_CHANGE(right.files[6][0]='F');SNAPSHOT_CHANGE(right.paths[6][0]='S');
    SNAPSHOT_CHANGE(right.origins[6].module.length=7);
    SNAPSHOT_CHANGE(right.origins[6].file.length=7);
    SNAPSHOT_CHANGE(right.origins[6].field_path.length=5);
    SNAPSHOT_MALFORMED(right.table.policies=NULL);
    SNAPSHOT_MALFORMED(right.table.records=NULL);
    SNAPSHOT_MALFORMED(right.policy.members=NULL);
    SNAPSHOT_MALFORMED(right.members[1].operations=0);
    SNAPSHOT_MALFORMED(right.members[1].field=0);
    SNAPSHOT_MALFORMED(right.grants[1].origins.instructions=NULL);
    SNAPSHOT_MALFORMED(right.grants[1].origins.instruction_count=0);
    SNAPSHOT_MALFORMED(right.policy.origin.line=0);
    SNAPSHOT_MALFORMED(right.policy.origin.module.bytes=NULL);
    snapshot_fixture(&right);XrXirDeclarations a={.derivations=&left.table},b={.derivations=&right.table};
    bool same=false;CHECK(effect_body_declarations_match(context,&a,&b,0,&same)==XR_XIR_OK && !same);
    XrXirImplementation records[2]={{.derivation_policy=0},{.derivation_policy=1}};
    XrXirImplementationTable tables[2]={{.records=&records[0],.count=1},{.records=&records[1],.count=1}};
    a.implementations=&tables[0];b.implementations=&tables[1];same=true;
    CHECK(effect_body_declarations_match(context,&a,&b,0,NULL)==XR_XIR_BAD_STRUCTURE);
    CHECK(effect_body_declarations_match(context,&a,&b,0,&same)==XR_XIR_OK && !same);
    printf("Derivation snapshot: %u independent literal equal/different/malformed cases, implementation policy and preserved prior change; zero allocations\n",snapshot_cases);
}
#undef SNAPSHOT_CHANGE
#undef SNAPSHOT_MALFORMED
static void snapshot_work_bounds(void) {
    SnapshotDerivationFixture left,right;snapshot_fixture(&left);snapshot_fixture(&right);
    XrXirDeclarations a={.derivations=&left.table},b={.derivations=&right.table};
    uint64_t exact=0;
    for(unsigned n=0;n<3;++n) {
        XrCompileResourceLimits caps={8388608,4194304,n?exact-(n==2):4000000};XrXirCompileContext context={0};
        size_t blocks=snapshot_live,bytes=snapshot_bytes;
        CHECK(xr_compile_resources_new(&caps,&context.resources)==XR_COMPILE_RESOURCE_OK);
        context.limits=xr_xir_compile_default_limits();
        XrCompileResourceStats entry={0};CHECK(xr_compile_resources_stats(context.resources,&entry)==XR_COMPILE_RESOURCE_OK);
        XrXirStatus status=effect_body_declarations_match(&context,&a,&b,0,NULL);
        XrCompileResourceStats spent={0};CHECK(xr_compile_resources_stats(context.resources,&spent)==XR_COMPILE_RESOURCE_OK);
        if(!n){exact=spent.work;CHECK(exact>1 && exact<=4000000);}
        CHECK(status==(n==2?XR_XIR_BUDGET:XR_XIR_OK) &&
            spent.live_bytes==entry.live_bytes && spent.allocated_bytes==entry.allocated_bytes);
        xr_compile_resources_release(context.resources);CHECK(snapshot_live==blocks && snapshot_bytes==bytes);
    }
    printf("Derivation snapshot same complete operation work%llu exact/minus-one and physical baseline PASS\n",(unsigned long long)exact);
}
#endif // XIR_DERIVATION_SNAPSHOT_LITERAL_CASES_H
