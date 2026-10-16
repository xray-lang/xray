/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_history_function_fixture.h - Actual escaped closures for every backend
 */
#ifndef SOURCE_HISTORY_FUNCTION_FIXTURE_H
#define SOURCE_HISTORY_FUNCTION_FIXTURE_H
static inline const char *source_history_function_input(unsigned generation,bool escaped) {
    static const char *const returned[]={
        "var value:fn()->i64=make()\nfn make()->fn()->i64{var count:i64=41;return fn()->i64{count+=1;return count}}\nfn fetch()->fn()->i64{return value}\nprint(17)\nexport fn seed()->i64{return 0}",
        "struct Padding{flag:bool}\nconst value=\"shadow\"\nexport fn answer()->fn()->i64{return fetch()}",
        "struct Later{flag:bool}\nconst value=true\nexport fn answer()->fn()->i64{return fetch()}",
        "const value=99\nexport fn fail()->fn()->i64{const saved=fetch();assert(false);return saved}"};
    static const char *const called[]={
        "var value:fn()->i64=make()\nfn make()->fn()->i64{var count:i64=41;return fn()->i64{count+=1;return count}}\nfn fetch()->fn()->i64{return value}\nprint(17)\nexport fn seed()->i64{return 0}",
        "struct Padding{flag:bool}\nconst value=\"shadow\"\nexport fn answer()->i64{return fetch()()}",
        "struct Later{flag:bool}\nconst value=true\nexport fn answer()->i64{return fetch()()}",
        "const value=99\nexport fn fail()->i64{const seen=fetch()();assert(false);return seen}"};
    return generation>=1 && generation<=4 ? (escaped?returned:called)[generation-1] : NULL;
}
#endif // SOURCE_HISTORY_FUNCTION_FIXTURE_H
