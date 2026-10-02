/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_decimal_work.c - Decimal semantics and independent work boundaries
 */
#include "shared/xr_decimal_float.h"
#include "xir/xxir_float.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); abort(); } } while (0)
#include "xir_decimal_cases.h"
#include "xir_float_format_cases.h"
typedef struct DecimalLedger { uint64_t limit, used, calls; } DecimalLedger;
static bool decimal_charge(void *context, uint64_t units) {
    DecimalLedger *ledger=context; ++ledger->calls;
    if (units>ledger->limit-ledger->used) return false;
    ledger->used+=units;return true;
}
static void fixed_boundaries(void) {
    _Static_assert(sizeof(XrDecimalInput)==792,"Independent decimal workspace layout");
    /* Parse/scan/encode/publication, two byte reads, and actual 792-byte clear. */
    for (uint64_t limit=0;limit<=798;++limit) {
        DecimalLedger ledger={limit,0,0};XrDecimalWork work={&ledger,decimal_charge,false};
        uint64_t out=UINT64_C(0x1122334455667788);
        CHECK(xr_decimal_float_parse_work(&work,"0",1,64,&out)==(limit==798?XR_DECIMAL_OK:XR_DECIMAL_WORK_LIMIT));
        CHECK(out==(limit==798?0:UINT64_C(0x1122334455667788)));
        CHECK(limit!=798 || ledger.used==798);
        if (work.failed) {
            ledger.limit=UINT64_MAX;uint64_t calls=ledger.calls;
            CHECK(xr_decimal_float_parse_work(&work,(const char *)(uintptr_t)1,1,64,&out)==XR_DECIMAL_WORK_LIMIT);
            CHECK(ledger.calls==calls);
        }
    }
    DecimalLedger ledger={2,0,0};XrDecimalWork work={&ledger,decimal_charge,false};
    XrDecimalInteger n={{UINT32_MAX},1};
    CHECK(xr_decimal_multiply(&work,&n,10,9) && ledger.used==2 && n.count==2 && n.words[0]==UINT32_MAX && n.words[1]==9);
    ledger=(DecimalLedger){6,0,0};work.failed=false;n=(XrDecimalInteger){{1},1};
    CHECK(xr_decimal_shift(&work,&n,32) && ledger.used==6 && n.count==2 && !n.words[0] && n.words[1]==1);
    ledger=(DecimalLedger){3,0,0};work.failed=false;
    CHECK(xr_decimal_compare(&work,&n,&n)==0 && ledger.used==3 && !work.failed);
    ledger=(DecimalLedger){5,0,0};work.failed=false;n=(XrDecimalInteger){{8},1};
    CHECK(xr_decimal_bits(&work,&n)==4 && ledger.used==5);
}
static void parse_failures(void) {
    const char *texts[]={"1.25","0.1","1.000000059604644775390625","5e-324","1_234.5e-2"};
    const uint64_t expected[]={UINT64_C(0x3ff4000000000000),UINT64_C(0x3fb999999999999a),
        UINT64_C(0x3ff0000010000000),UINT64_C(1),UINT64_C(0x4028b0a3d70a3d71)};
    for (size_t i=0;i<sizeof(texts)/sizeof(*texts);++i) {
        DecimalLedger ledger={UINT64_MAX,0,0};XrDecimalWork work={&ledger,decimal_charge,false};
        uint64_t out=0;
        CHECK(xr_decimal_float_parse_work(&work,texts[i],strlen(texts[i]),64,&out)==XR_DECIMAL_OK);
        CHECK(out==expected[i]);uint64_t needed=ledger.used;
        for (uint64_t limit=0;limit<needed;limit+=needed-limit>257?257:1) {
            ledger=(DecimalLedger){limit,0,0};work.failed=false;out=UINT64_MAX;
            CHECK(xr_decimal_float_parse_work(&work,texts[i],strlen(texts[i]),64,&out)==XR_DECIMAL_WORK_LIMIT && out==UINT64_MAX);
        }
        printf("Decimal %s work %llu, distributed cutoffs and exact boundary\n",texts[i],(unsigned long long)needed);
    }
    DecimalLedger ledger={0,0,0};XrDecimalWork work={&ledger,decimal_charge,false};uint64_t out=37;
    CHECK(xr_decimal_float_parse_work(&work,(const char *)(uintptr_t)1,1,64,&out)==XR_DECIMAL_WORK_LIMIT && out==37);
    ledger=(DecimalLedger){UINT64_MAX,0,0};work.failed=false;
    CHECK(xr_decimal_float_parse_work(&work,"?",1,64,&out)==XR_DECIMAL_INVALID && out==37);
    CHECK(ledger.used==796 && !work.failed);
    CHECK(xr_decimal_float_parse_work(NULL,"0",1,64,&out)==XR_DECIMAL_INVALID && out==37);
    CHECK(xr_decimal_float_parse_work(&work,"0",1,16,&out)==XR_DECIMAL_INVALID && out==37);
}
int main(void) { fixed_boundaries();parse_failures();decimal_cases();float_format_cases();return 0; }
