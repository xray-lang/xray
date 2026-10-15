/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_history_class_fixture.h - Shared real Class generation source inputs
 */
#ifndef SOURCE_HISTORY_CLASS_FIXTURE_H
#define SOURCE_HISTORY_CLASS_FIXTURE_H
static inline const char *source_history_class_input(unsigned generation,unsigned kind) {
    static const char *const inputs[3][4]={
        {"final class Counter{n:i64 constructor(n:i64){this.n=n} add()->i64{this.n+=1;return this.n}}\nvar value=Counter(41)\nfn old()->i64{return value.add()}\nprint(17)\nexport fn seed()->i64{return 0}","const value=\"shadow\"\nexport fn answer()->i64{return old()}","const value=true\nexport fn answer()->i64{return old()}","const value=99\nexport fn fail()->i64{const seen=old();assert(false);return seen}"},
        {"final class Counter{n:i64 constructor(n:i64){this.n=n} add()->i64{this.n+=1;return this.n}}\nvar value=Counter(41)\nfn old()->i64{return value.add()}\nprint(17)\nexport fn seed()->i64{return 0}","final class Padding{x:bool constructor(x:bool){this.x=x}}\nconst padding=Padding(true)\nconst value=\"shadow\"\nexport fn answer()->i64{return old()}","final class Other{x:string constructor(x:string){this.x=x}}\nconst padding=Other(\"gap\")\nconst value=true\nexport fn answer()->i64{return old()}","const value=99\nexport fn fail()->i64{const seen=old();assert(false);return seen}"},
        {"final class Counter{n:i64 constructor(n:i64){this.n=n} add()->i64{this.n+=1;return this.n}}\nvar value=Counter(41)\nfn old()->i64{return value.add()}\nprint(17)\nexport fn seed()->i64{return 0}","final class Padding{x:bool constructor(x:bool){this.x=x}}\nvar padding=Padding(true)\nconst value=\"shadow\"\nexport fn answer()->i64{return old()}","final class Other{x:string constructor(x:string){this.x=x}}\nvar padding=Other(\"gap\")\nconst value=true\nexport fn answer()->i64{return old()}","const value=99\nexport fn fail()->i64{const seen=old();assert(false);return seen}"},
    };
    CHECK(generation>=1 && generation<=4 && kind<3);
    return inputs[kind][generation-1];
}
#endif // SOURCE_HISTORY_CLASS_FIXTURE_H
