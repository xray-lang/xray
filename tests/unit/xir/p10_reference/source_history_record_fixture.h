/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_history_record_fixture.h - Owned records carrying actual birth closures
 */
#ifndef SOURCE_HISTORY_RECORD_FIXTURE_H
#define SOURCE_HISTORY_RECORD_FIXTURE_H
static inline const char *source_history_record_input(unsigned generation,bool escaped) {
    static const char initial[]=
        "var value:{apply:fn()->i64,items:Array<i64>}=make()\n"
        "fn make()->{apply:fn()->i64,items:Array<i64>}{var count:i64=41;return {apply:fn()->i64{count+=1;return count},items:[1]}}\n"
        "fn fetch()->{apply:fn()->i64,items:Array<i64>}{return value}\n"
        "print(17)\nexport fn seed()->i64{return 0}";
    static const char *const returned[]={initial,
        "struct Padding{flag:bool}\nconst value=\"shadow\"\nexport fn answer()->fn()->i64{const saved=fetch();return saved.apply}",
        "struct Later{flag:bool}\nconst value=true\nexport fn answer()->fn()->i64{const saved=fetch();return saved.apply}",
        "const value=99\nexport fn fail()->fn()->i64{const saved=fetch();assert(false);return saved.apply}"};
    static const char *const called[]={initial,
        "struct Padding{flag:bool}\nconst value=\"shadow\"\n"
        "fn run(box:{apply:fn()->i64,items:Array<i64>})->i64{var snapshot=box;snapshot.items[0]=9;return snapshot.apply()+box.items[0]*10+snapshot.items[0]-19}\n"
        "export fn answer()->i64{return run(fetch())}",
        "struct Later{flag:bool}\nconst value=true\n"
        "fn run(box:{apply:fn()->i64,items:Array<i64>})->i64{var snapshot=box;snapshot.items[0]=9;return snapshot.apply()+box.items[0]*10+snapshot.items[0]-19}\n"
        "export fn answer()->i64{return run(fetch())}",
        "const value=99\nexport fn fail()->i64{const saved=fetch();const seen=saved.apply();assert(false);return seen}"};
    return generation>=1 && generation<=4 ? (escaped?returned:called)[generation-1] : NULL;
}
#endif // SOURCE_HISTORY_RECORD_FIXTURE_H
