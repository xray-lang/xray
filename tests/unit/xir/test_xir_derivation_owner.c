/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_derivation_owner.c - Physical grant data lifetime and rollback
 */
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while(0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_derivation_internal.h"
static const XrCompileResourceLimits caps={67108864,8388608,128000000};
typedef struct GrantFixture {
    char module[8],file[24],field[24];
    XrXirType arguments[2];
    XrXirDerivationMember members[2];
    XrXirGeneratedOrigin origins[2];
    XrXirDerivationPolicy policies[2];
    XrXirDerivationGrant rows[2];
    XrXirDerivationTable table;
} GrantFixture;
static XrCompileResourceStats grant_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static XrXirCompileContext grant_context(const XrCompileResourceLimits *limits) {
    XrXirCompileContext context={0};CHECK(xr_compile_resources_new(limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();return context;
}
static void grant_fixture(GrantFixture *f) {
    memset(f,0,sizeof(*f));memcpy(f->module,"owner",6);memcpy(f->file,"owner/derive.xr",16);memcpy(f->field,"Record.secret",14);
    f->arguments[0]=XR_XIR_I64;f->arguments[1]=XR_XIR_STRING;
    f->members[0]=(XrXirDerivationMember){0,XR_XIR_DERIVE_DESCRIBE|XR_XIR_DERIVE_READ};
    f->members[1]=(XrXirDerivationMember){2,XR_XIR_DERIVE_WRITE};
    for(unsigned i=0;i<2;++i) {
        f->origins[i]=(XrXirGeneratedOrigin){{f->module,5},{f->file,15},{f->field,13},11+i,3};
        f->policies[i]=(XrXirDerivationPolicy){.grantor_module=1,
            .target=(XrXirType)(256+i),.protocol={0,f->arguments,2},.generator=5,
            .constructor=i?8:0,.members=f->members,.member_count=2,
            .origin=f->origins[0]};
        f->rows[i]=(XrXirDerivationGrant){.function=2+i,.policy=i,.source_row=i,.root=i,
            .target=(XrXirType)(256+i),.origins={f->origins[0],f->origins,2}};
    }
    f->table=(XrXirDerivationTable){f->policies,2,f->rows,2};
}
static void grant_owned(const XrXirDerivationTable *owner) {
    CHECK(owner && owner->count==2 && owner->records && owner->policy_count==2 && owner->policies);
    for(unsigned i=0;i<2;++i) {
        const XrXirDerivationGrant *row=&owner->records[i];
        CHECK(row->function==2+i && row->policy==i && row->source_row==i && row->root==i && row->target==(XrXirType)(256+i));
        const XrXirDerivationPolicy *policy=&owner->policies[row->policy];
        CHECK(policy->grantor_module==1 && policy->target==row->target);
        CHECK(policy->protocol.declaration==0 && policy->protocol.argument_count==2 && policy->protocol.arguments[0]==XR_XIR_I64 && policy->protocol.arguments[1]==XR_XIR_STRING);
        CHECK(policy->generator==5 && policy->constructor==(i?8u:0u));
        CHECK(policy->member_count==2 && policy->members[0].field==0 && policy->members[0].operations==3 && policy->members[1].field==2 && policy->members[1].operations==4);
        CHECK(row->origins.instruction_count==2 && row->origins.entry.line==11 && row->origins.entry.column==3);
        for(unsigned n=0;n<2;++n) {
            const XrXirGeneratedOrigin *site=&row->origins.instructions[n];
            CHECK(site->line==11+n && site->column==3 && site->module.length==5 && !memcmp(site->module.bytes,"owner",5));
            CHECK(site->file.length==15 && !memcmp(site->file.bytes,"owner/derive.xr",15));
            CHECK(site->field_path.length==13 && !memcmp(site->field_path.bytes,"Record.secret",13));
        }
    }
}
static void grant_ownership(void) {
    XrXirCompileContext context=grant_context(&caps);uint64_t live=grant_stats(&context).live_bytes;
    GrantFixture f;grant_fixture(&f);XrXirDerivationTable *owner=NULL,*second=NULL;
    f.table.records=NULL;f.table.count=0;
    CHECK(xir_derivations_copy(&context,&f.table,&owner)==XR_XIR_OK);memset(&f,0xa5,sizeof(f));
    CHECK(owner && !owner->count && !owner->records && owner->policy_count==2);
    CHECK(owner->policies[0].origin.field_path.length==13 &&
        !memcmp(owner->policies[0].origin.field_path.bytes,"Record.secret",13));
    CHECK(owner->policies[1].constructor==8 && owner->policies[1].generator==5);
    xir_derivations_free(owner);owner=NULL;grant_fixture(&f);
    CHECK(xir_derivations_copy(&context,&f.table,&owner)==XR_XIR_OK);memset(&f,0xa5,sizeof(f));grant_owned(owner);
    CHECK(xir_derivations_copy(&context,owner,&second)==XR_XIR_OK);xir_derivations_free(owner);grant_owned(second);
    XrCompileResourceStats before=grant_stats(&context);XrXirDerivationTable *saved=second;
    CHECK(xir_derivations_copy(&context,NULL,&second)==XR_XIR_BAD_STRUCTURE && second==saved);
    CHECK(grant_stats(&context).work==before.work && grant_stats(&context).allocated_bytes==before.allocated_bytes);
    xir_derivations_free(second);CHECK(grant_stats(&context).live_bytes==live);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void grant_invalid(void) {
    XrXirCompileContext context=grant_context(&caps);uint64_t live=grant_stats(&context).live_bytes;
    for(unsigned mode=0;mode<9;++mode) {
        GrantFixture f;grant_fixture(&f);XrXirDerivationTable *owner=NULL;
        if(mode==0){f.table.count=0;f.table.policy_count=0;}
        if(mode==1)f.table.records=NULL;
        if(mode==2)f.rows[1].function=2;
        if(mode==3)f.members[1].field=0;
        if(mode==4)f.members[0].operations=8;
        if(mode==5)f.origins[1].line=0;
        if(mode==6)f.origins[1].file=(XrXirGeneratedText){"\xff",1};
        if(mode==7)f.rows[0].origins.instructions=NULL;
        if(mode==8)f.policies[0].protocol.arguments=NULL;
        CHECK(xir_derivations_copy(&context,&f.table,&owner)==XR_XIR_BAD_STRUCTURE && !owner);
        CHECK(grant_stats(&context).live_bytes==live);
        XrCompileResourceStats failed=grant_stats(&context);grant_fixture(&f);
        CHECK(xir_derivations_copy(&context,&f.table,&owner)==XR_XIR_OK);grant_owned(owner);xir_derivations_free(owner);
        CHECK(grant_stats(&context).live_bytes==live && grant_stats(&context).work>=failed.work && grant_stats(&context).allocated_bytes>=failed.allocated_bytes);
    }
    XrCompileResourceStats before=grant_stats(&context);XrXirDerivationTable *empty=NULL;
    CHECK(xir_derivations_copy(&context,NULL,&empty)==XR_XIR_OK && !empty && grant_stats(&context).work==before.work);
    xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
}
static void grant_budgets(void) {
    XrXirCompileContext context=grant_context(&caps);GrantFixture f;grant_fixture(&f);XrXirDerivationTable *owner=NULL;
    CHECK(xir_derivations_copy(&context,&f.table,&owner)==XR_XIR_OK);XrCompileResourceStats cost=grant_stats(&context);
    xir_derivations_free(owner);xr_compile_resources_release(context.resources);
    for(unsigned axis=0;axis<3;++axis)for(unsigned exact=0;exact<2;++exact) {
        XrCompileResourceLimits limits=caps;uint64_t amount=(axis==0?cost.allocated_bytes:axis==1?cost.peak_bytes:cost.work)-!exact;
        if(axis==0)limits.allocated_bytes=amount;else if(axis==1)limits.live_bytes=amount;else limits.work=amount;
        context=grant_context(&limits);uint64_t live=grant_stats(&context).live_bytes;owner=NULL;
        CHECK(xir_derivations_copy(&context,&f.table,&owner)==(exact?XR_XIR_OK:XR_XIR_BUDGET));
        if(exact)grant_owned(owner);else CHECK(!owner);
        xir_derivations_free(owner);CHECK(grant_stats(&context).live_bytes==live);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
    printf("grant data owner allocated=%llu peak=%llu work=%llu exact-minus-one=PASS full-FI=NOT_RUN authority-checker=NOT_WIRED\n",
        (unsigned long long)cost.allocated_bytes,(unsigned long long)cost.peak_bytes,(unsigned long long)cost.work);
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    if(!strcmp(argv[1],"ownership"))grant_ownership();else if(!strcmp(argv[1],"invalid"))grant_invalid();
    else { CHECK(!strcmp(argv[1],"budgets"));grant_budgets(); }
    return 0;
}
