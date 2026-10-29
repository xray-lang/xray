/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * nominal_projection_fixture.h - Complete wide Source shared by real clients
 */
#ifndef NOMINAL_PROJECTION_FIXTURE_H
#define NOMINAL_PROJECTION_FIXTURE_H
#include <stdarg.h>
static void nominal_fixture_put(char *text,size_t capacity,size_t *used,const char *format,...) {
    va_list arguments;va_start(arguments,format);
    int n=vsnprintf(text+*used,capacity-*used,format,arguments);va_end(arguments);
    CHECK(n>0 && (size_t)n<capacity-*used);*used+=(size_t)n;
}
static void nominal_fixture_name(unsigned index,char *name,size_t capacity) {
    if(!index){CHECK(capacity>4617);memcpy(name,"retained_",9);memset(name+9,'a',4608);name[4617]=0;}
    else {
        int n=snprintf(name,capacity,"field_%03u_metadata_name_metadata_name_metadata_name_metadata_name_",index);
        CHECK(n==66 && (size_t)n<capacity);
    }
}
static size_t nominal_fixture(char *text,size_t capacity) {
    size_t used=0;char name[4618];
    nominal_fixture_put(text,capacity,&used,"enum WideRecord {\n    Packed {\n");
    for(unsigned i=0;i<300;++i){nominal_fixture_name(i,name,sizeof(name));nominal_fixture_put(text,capacity,&used,"        %s: i64%s\n",name,i<299?",":"");}
    nominal_fixture_put(text,capacity,&used,"    }\n}\nfn answer() -> i64 {\n    var value = WideRecord.Packed {\n");
    for(unsigned i=0;i<300;++i){nominal_fixture_name(i,name,sizeof(name));nominal_fixture_put(text,capacity,&used,"        %s: %u%s\n",name,i?i:42,i<299?",":"");}
    nominal_fixture_put(text,capacity,&used,"    }\n    return match (value) {\n        WideRecord.Packed {\n");
    for(unsigned i=0;i<300;++i){nominal_fixture_name(i,name,sizeof(name));nominal_fixture_put(text,capacity,&used,"            %s: %s%s\n",name,i?"_":"selected",i<299?",":"");}
    nominal_fixture_put(text,capacity,&used,"        } -> selected\n    }\n}\nprint(answer())\n");
    CHECK(used==87251);
    const char *record=getenv("XR_NOMINAL_SOURCE_RECORD");
    if(record){FILE *file=fopen(record,"wb");CHECK(file && fwrite(text,1,used,file)==used && !fclose(file));}
    return used;
}
#endif // NOMINAL_PROJECTION_FIXTURE_H
