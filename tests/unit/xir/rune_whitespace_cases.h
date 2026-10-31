/* Independent current-kernel whitespace boundaries, shared by VM and C tests. */
#ifndef RUNE_WHITESPACE_CASES_H
#define RUNE_WHITESPACE_CASES_H
#include "xir/xxir_scalar.h"
static bool whitespace_expected(uint32_t code) {
    return (code>=9 && code<=13) || code==32 || code==0xa0 || code==0x1680 ||
        (code>=0x2000 && code<=0x200a) || code==0x2028 || code==0x2029 ||
        code==0x202f || code==0x205f || code==0x3000;
}
typedef XrXirRunStatus (*WhitespaceRun)(void *,XrXirRunContext *,const XrXirValue *,XrXirValue *);
static void whitespace_cases(WhitespaceRun invoke,void *owner) {
    const int64_t values[]={0,8,9,10,11,12,13,14,31,32,33,0x7f,0x85,0x9f,0xa0,0xa1,
        0x167f,0x1680,0x1681,0x1fff,0x2000,0x2005,0x200a,0x200b,0x2027,0x2028,0x2029,
        0x202a,0x202e,0x202f,0x2030,0x205e,0x205f,0x2060,0x2fff,0x3000,0x3001,
        0xd7ff,0xe000,0xfeff,0x1f600,0x10ffff};
    for(size_t i=0;i<sizeof(values)/sizeof(*values);++i) {
        XrXirValue input={XR_XIR_RUNE,0,values[i]},output={0};
        XrXirRunContext run={1024,65536,0,0,0,0};
        CHECK(invoke(owner,&run,&input,&output)==XR_XIR_RUN_OK);
        CHECK(output.type==XR_XIR_BOOL && !output.reserved && output.payload==whitespace_expected((uint32_t)values[i]));
        CHECK(!run.live_bytes && run.allocations==run.frees);
    }
    const XrXirValue invalid[]={{XR_XIR_RUNE,0,-1},{XR_XIR_RUNE,0,0xd800},
        {XR_XIR_RUNE,0,0x110000},{XR_XIR_RUNE,1,32},{XR_XIR_I64,0,32},{XR_XIR_BOOL,0,1}};
    for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);++i) {
        XrXirValue output={0};XrXirRunContext run={1024,65536,0,0,0,0};
        CHECK(invoke(owner,&run,&invalid[i],&output)==XR_XIR_RUN_BAD_ARGUMENT);
        CHECK(!output.type && !output.reserved && !output.payload && !run.live_bytes);
        CHECK(run.allocations==run.frees);
    }
}
#endif // RUNE_WHITESPACE_CASES_H
