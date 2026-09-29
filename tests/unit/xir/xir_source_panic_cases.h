/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_panic_cases.h - Panic binding, channel and initialization admission
 */
static void source_panic_cases(XrXirSourceRequest *request, const char *path) {
    const struct { const char *source; bool valid; } cases[] = {
        {"fn run()->i64 {try{return 10/0}catch panic{return 17}}\nrun()\n", true},
        {"fn run()->i64 {try{return 10/0}catch panic(p){return p.code}}\nrun()\n", true},
        {"fn run()->string {try{const n=10/0}catch panic(p:PanicInfo){return p.message};return \"ok\"}\nrun()\n", true},
        {"fn id<T>(v:T)->T{return v}\nfn run()->PanicInfo {try{const n=10/0}catch panic(p){return id<PanicInfo>(p)};return run()}\nrun()\n", true},
        {"fn run()->i64 {try{try{return 1/0}catch panic(p){return 1%0}}catch panic(q){return q.code}}\nrun()\n", true},
        {"fn run()->i64 {var x=3;try{x=5;return 1/0}catch panic{return x}}\nrun()\n", true},
        {"enum E{Bad}\nfn run()->i64 {try{try{throw E.Bad}catch panic{return 0}}catch(e:E){return 23}}\nrun()\n", true},
        {"enum E{Bad}\nfn run()->i64 {try{return 1/0}catch(e:E){return 0}catch panic(p){return p.code}}\nrun()\n", true},
        {"fn run()->string {try{const n=1/0}catch panic(p){Coro.yield();return p.message};return \"ok\"}\nrun()\n", true},
        {"fn run(){try{}catch panic(p){const f=fn()->i64{return p.code};f()}}\nrun()\n", true},
        {"fn run(){try{}catch panic(p){const xs=[p];const n=xs[0].code}}\nrun()\n", true},
        {"struct Box<T>{value:T}\nfn run(){try{}catch panic(p){const b=Box<PanicInfo>{value:p};const n=b.value.code}}\nrun()\n", true},
        {"fn run(){try{}catch panic(p:i64){}}\nrun()\n", false},
        {"fn run(){try{}catch panic(p:Error){}}\nrun()\n", false},
        {"fn run(){try{}catch panic{}catch panic{}}\nrun()\n", false},
        {"fn run(){try{}catch panic(p){p=p}}\nrun()\n", false},
        {"fn run(){try{}catch panic(p){};const n=p.code}\nrun()\n", false},
        {"fn run(){try{}catch panic(p){throw p}}\nrun()\n", false},
        {"fn send<T:Sendable>(v:T){}\nfn run(){try{}catch panic(p){send<PanicInfo>(p)}}\nrun()\n", false},
        {"fn send<T:Sendable>(v:T){}\nfn run(){try{}catch panic(p){send<Array<PanicInfo>>([p])}}\nrun()\n", false},
        {"fn read<T>(v:T)->i64{return v.code}\nfn run(){try{}catch panic(p){read<PanicInfo>(p)}}\nrun()\n", false},
        {"fn run(){try{}catch panic(p){const n=p.stack}}\nrun()\n", false},
        {"fn run(){try{}catch panic(p){const n=p.cause}}\nrun()\n", false},
        {"fn run(){try{}catch panic(p){const n=p.data}}\nrun()\n", false},
        {"fn run(){try{}catch panic(p){p.code=1}}\nrun()\n", false},
        {"fn run(){try{}catch panic(p){p.toString()}}\nrun()\n", false},
        {"fn run(){const p=PanicInfo()}\nrun()\n", false},
        {"fn run(){try{}catch panic(p){const n=unknown}}\nrun()\n", false},
        {"fn run()->i64 {try{}catch panic{return true};return 1}\nrun()\n", false},
        {"struct C{const value:string\nconstructor(){try{this.value=\"x\"}catch panic{const n=this.value}}}\nconst c=C()\n", false},
        {"struct C{const value:string=\"x\"\nconstructor(){try{}catch panic{const n=this.value}}}\nconst c=C()\n", true}
    };
    unsigned failures = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        write_source(path, cases[i].source);
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_source_check(request, &result, &diagnostic);
        if ((status == XR_XIR_OK) != cases[i].valid || (result.checked != NULL) != cases[i].valid) {
            fprintf(stderr, "panic source case %u: %u at %d:%d: %s\n", i, status,
                diagnostic.line, diagnostic.column, diagnostic.message);
            ++failures;
        }
        xr_xir_source_result_free(&result);
    }
    CHECK(!failures);
}
