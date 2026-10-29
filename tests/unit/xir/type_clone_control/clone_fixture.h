/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * clone_fixture.h - Whole original programs and separate wide signature/name consumers.
 */
#ifndef TYPE_CLONE_FIXTURE_H
#define TYPE_CLONE_FIXTURE_H
#include "../implementation_control_fixture.h"
#include "../nominal_projection_fixture.h"
static size_t tc_source(unsigned kind,char *text,size_t capacity) {
    if(kind==1)return nominal_fixture(text,capacity);
    if(kind==3) {
        char original[8000];size_t length=implementation_control_fixture(original,sizeof(original)),used=0;
        for(size_t i=0;i<length;) {
            if(i+3<=length&&!memcmp(original+i,"Box",3)) {CHECK(capacity-used>4617);memcpy(text+used,"retained_",9);memset(text+used+9,'a',4608);used+=4617;i+=3;}
            else {CHECK(used+1<capacity);text[used++]=original[i++];}
        }
        text[used]=0;CHECK(used==16886);return used;
    }
    size_t used=implementation_control_fixture(text,capacity);
    if(!kind)return used;
    CHECK(kind==2||kind==4);
#define TC_APPEND(...) do {int n=snprintf(text+used,capacity-used,__VA_ARGS__);CHECK(n>=0&&(size_t)n<capacity-used);used+=(size_t)n;}while(0)
    TC_APPEND("fn wide(");for(unsigned i=0;i<600;++i)TC_APPEND("%sp%03u:i64",i?",":"",i);
    TC_APPEND(")->i64{return p000+p599}\nprint(wide(");
    for(unsigned i=0;i<600;++i)TC_APPEND("%s%u",i?",":"",i==0?41u:i==599?1u:0u);
    TC_APPEND("))\n");
    if(kind==4) {
        TC_APPEND("var callableWide=wide\nprint(callableWide(");
        for(unsigned i=0;i<600;++i)TC_APPEND("%s%u",i?",":"",i==0?41u:i==599?1u:0u);
        TC_APPEND("))\n");
    }
#undef TC_APPEND
    CHECK(used==(kind==4?15548u:14304u));return used;
}
static const char *tc_expected(unsigned kind) {return kind==4?"42\n42\n42\n":kind==2?"42\n42\n":"42\n";}
#endif // TYPE_CLONE_FIXTURE_H
