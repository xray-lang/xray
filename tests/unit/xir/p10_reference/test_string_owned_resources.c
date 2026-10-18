/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_string_owned_resources.c - Actual owned-kernel faults on finite ledgers
 */
#include "xir/xxir_type_arena_internal.h"
#include "xir/xxir_array.h"
#include "xir/xxir_value_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "../xir_source_fixture_owner.h"
#include "../xir_runtime_allocations.h"
typedef struct OwnedFixture {
    XrXirDomain *domain;
    uint64_t baseline;
    XrXirValue text,search,replacement;
    XrXirValueAdmission admission;
} OwnedFixture;
static void fixture_new(OwnedFixture *fixture,const XrXirTypeArena *arena,uint64_t requested) {
    const XrXirDomainBudgetControls controls={requested,1048576,128000000,1048576,1048576};
    CHECK(xr_xir_domain_new_budgeted(1048576,&controls,&fixture->domain)==XR_XIR_VALUE_OK);
    fixture->baseline=xr_xir_domain_stats(fixture->domain).live_bytes;
    CHECK(xr_xir_string_new(fixture->domain,"\xc3\xa9\0\xc3\xa9",5,&fixture->text)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(fixture->domain,"\xc3\xa9",2,&fixture->search)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(fixture->domain,"\xf0\x9f\x99\x82",4,&fixture->replacement)==XR_XIR_VALUE_OK);
    fixture->admission=(XrXirValueAdmission){arena,fixture->domain,NULL,NULL,128000000,1048576};
}
static void fixture_free(OwnedFixture *fixture) {
    xr_xir_value_drop(&fixture->replacement);xr_xir_value_drop(&fixture->search);xr_xir_value_drop(&fixture->text);
    CHECK(xr_xir_domain_stats(fixture->domain).live_bytes==fixture->baseline);xr_xir_domain_drop(fixture->domain);
}
static XrXirValueStatus construct(OwnedFixture *fixture,unsigned mode,XrXirValue *output) {
    if(!mode) return xr_xir_string_copy_bytes(&fixture->text,(XrXirType)256,&fixture->admission,output);
    return xr_xir_string_replace(&fixture->text,&fixture->search,&fixture->replacement,mode==2,&fixture->admission,output);
}
static void expected_value(OwnedFixture *fixture,unsigned mode,const XrXirValue *value) {
    static const unsigned char original[]={195,169,0,195,169};
    static const unsigned char first[]={240,159,153,130,0,195,169};
    static const unsigned char all[]={240,159,153,130,0,240,159,153,130};
    const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&fixture->text,&bytes,&length) && length==sizeof(original));
    CHECK(!memcmp(bytes,original,sizeof(original)));
    if(mode) {
        CHECK(xr_xir_string_view(value,&bytes,&length) && length==(mode==1?sizeof(first):sizeof(all)));
        CHECK(!memcmp(bytes,mode==1?first:all,length));
    } else {
        int64_t count=-1;CHECK(xr_xir_array_len(value,&fixture->admission,&count)==XR_XIR_VALUE_OK && count==5);
        for(int64_t i=0;i<count;++i) {
            XrXirValue item={0};XrXirFaultDetail fault={0};
            CHECK(xr_xir_array_get(value,i,&fixture->admission,&item,&fault)==XR_XIR_VALUE_OK);
            CHECK(item.type==XR_XIR_U8 && item.payload==original[i]);xr_xir_value_drop(&item);
        }
    }
}
static void allocation_faults(const XrXirTypeArena *arena,unsigned mode) {
    OwnedFixture fixture={0};fixture_new(&fixture,arena,67108864);
    XrXirValue output={0};size_t start=runtime_attempts;
    uint64_t work=fixture.admission.work;
    CHECK(construct(&fixture,mode,&output)==XR_XIR_VALUE_OK);
    size_t sites=runtime_attempts-start;work-=fixture.admission.work;
    CHECK(sites && sites<=8 && work);expected_value(&fixture,mode,&output);xr_xir_value_drop(&output);
    size_t live=runtime_live,bytes=runtime_bytes;
    for(size_t site=0;site<sites;++site) {
        XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(fixture.domain);
        uint64_t admission_before=fixture.admission.work;
        start=runtime_attempts;runtime_fail_at=start+site;
        CHECK(construct(&fixture,mode,&output)==XR_XIR_VALUE_OOM);
        runtime_fail_at=SIZE_MAX;
        CHECK(runtime_attempts-start==site+1 && !output.type && !output.reserved && !output.payload);
        CHECK(runtime_live==live && runtime_bytes==bytes && fixture.admission.work<admission_before);
        XrXirDomainBudgetStats failed=xr_xir_domain_budget_stats(fixture.domain);
        CHECK(failed.requested_bytes>before.requested_bytes && failed.work>=before.work);
        CHECK(construct(&fixture,mode,&output)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_domain_budget_stats(fixture.domain).requested_bytes>failed.requested_bytes);
        expected_value(&fixture,mode,&output);xr_xir_value_drop(&output);
        CHECK(runtime_live==live && runtime_bytes==bytes);
    }
    fixture.admission.work=work-1;
    CHECK(construct(&fixture,mode,&output)==XR_XIR_VALUE_LIMIT);
    CHECK(!output.type && !output.reserved && !output.payload && runtime_live==live && runtime_bytes==bytes);
    uint64_t remaining=fixture.admission.work;
    CHECK(construct(&fixture,mode,&output)==XR_XIR_VALUE_LIMIT && fixture.admission.work<=remaining);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    printf("owned kernel mode=%u actual allocation sites=%zu successful work=%llu; OOM retry and limit physical baseline\n",
        mode,sites,(unsigned long long)work);
    fixture_free(&fixture);
}
static void cumulative_limit(const XrXirTypeArena *arena,unsigned mode) {
    OwnedFixture fixture={0};fixture_new(&fixture,arena,4096);
    size_t live=runtime_live,bytes=runtime_bytes;unsigned successes=0;XrXirValue output={0};
    for(;;) {
        CHECK(successes<256);XrXirValueStatus status=construct(&fixture,mode,&output);
        if(status==XR_XIR_VALUE_LIMIT) break;
        CHECK(status==XR_XIR_VALUE_OK);++successes;xr_xir_value_drop(&output);
    }
    CHECK(successes && !output.type && !output.reserved && !output.payload);
    CHECK(runtime_live==live && runtime_bytes==bytes);
    XrXirDomainBudgetStats before=xr_xir_domain_budget_stats(fixture.domain);
    CHECK(before.bound && before.requested_bytes<=before.requested_limit);
    size_t attempts=runtime_attempts;
    CHECK(construct(&fixture,mode,&output)==XR_XIR_VALUE_LIMIT);
    XrXirDomainBudgetStats after=xr_xir_domain_budget_stats(fixture.domain);
    CHECK(after.requested_bytes==before.requested_bytes && after.work>=before.work && runtime_attempts==attempts);
    printf("owned kernel mode=%u cumulative request limit after %u drops: %llu/%llu; no refill\n",mode,successes,
        (unsigned long long)after.requested_bytes,(unsigned long long)after.requested_limit);
    fixture_free(&fixture);
}
static void retained_original(const XrXirTypeArena *arena) {
    size_t live=runtime_live,allocated=runtime_bytes;OwnedFixture fixture={0};fixture_new(&fixture,arena,67108864);
    XrXirValue output={0};
    CHECK(xr_xir_string_replace(&fixture.text,&fixture.replacement,&fixture.search,true,&fixture.admission,&output)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&fixture.text);xr_xir_value_drop(&fixture.search);xr_xir_value_drop(&fixture.replacement);
    xr_xir_domain_drop(fixture.domain);
    static const unsigned char expected[]={195,169,0,195,169};const char *bytes=NULL;size_t length=0;
    CHECK(xr_xir_string_view(&output,&bytes,&length) && length==sizeof(expected) && !memcmp(bytes,expected,length));
    xr_xir_value_drop(&output);CHECK(runtime_live==live && runtime_bytes==allocated);
}
int main(void) {
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    XrXirTypeNode node={.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_U8};XrXirTypes types={&node,1,NULL,NULL};
    XrXirTypeArena *arena=NULL;CHECK(xr_xir_compile_type_arena_new(&compiler.context,&types,&arena)==XR_XIR_VALUE_OK);
    for(unsigned mode=0;mode<3;++mode) {allocation_faults(arena,mode);cumulative_limit(arena,mode);}
    retained_original(arena);
    xr_xir_compile_type_arena_drop(arena);source_fixture_owner_free(&compiler);
    CHECK(!runtime_live && !runtime_bytes);puts("Owned String kernels: same-ledger actual OOM/limits and physical0 PASS");return 0;
}
