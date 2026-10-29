/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * declarations_control_fixture.h - Original complete declaration workload
 */
#ifndef DECLARATIONS_CONTROL_FIXTURE_H
#define DECLARATIONS_CONTROL_FIXTURE_H
static size_t declarations_control_fixture(char *text,size_t capacity) {
    size_t used=0;
    for(unsigned i=0;i<300;++i) {
        int n=snprintf(text+used,capacity-used,"fn pad%u(value:i64=1)->i64{return value}\n",i);
        CHECK(n>0 && (size_t)n<capacity-used);used+=(size_t)n;
    }
    const char tail[]="fn add(left:i64=40,right:i64=2)->i64{return left+right}\nexport fn answer()->i64{return add()}\n";
    CHECK(sizeof(tail)<=capacity-used);memcpy(text+used,tail,sizeof(tail));used+=sizeof(tail)-1;CHECK(used==12584);
    const char *record=getenv("XR_DECLARATIONS_SOURCE_RECORD");
    if(record){FILE *file=fopen(record,"wb");CHECK(file&&fwrite(text,1,used,file)==used&&!fclose(file));}
    return used;
}
#endif // DECLARATIONS_CONTROL_FIXTURE_H
