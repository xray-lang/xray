/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_cleanup_admission.h - Cleanup definition effects and control boundaries
 */
static void source_cleanup_admission(XrXirSourceRequest *request, const char *path) {
    const struct { const char *source; const char *code; } cases[] = {
        {"enum E{Bad}\nfn unused(){defer{throw E.Bad}}\n", "E0387"},
        {"fn unused<T:Error>(e:T){defer{throw e}}\n", "E0387"},
        {"enum E{Bad}\nfn fail(){throw E.Bad}\nfn unused(){defer{fail()}}\n", "E0387"},
        {"fn unused(){defer{Coro.yield()}}\n", "E0392"},
        {"fn pause(){Coro.yield()}\nfn unused(){defer{pause()}}\n", "E0392"},
        {"fn unused(g:fn()->i64){defer{const n=g()}}\n", "E0392"},
        {"fn unused(){defer{return}}\n", "E0395"},
        {"fn unused(){while(true){defer{break}}}\n", "E0395"},
        {"fn unused(){while(true){defer{continue}}}\n", "E0395"},
        {"enum E{Bad}\nfn unused(){defer{try{throw E.Bad}catch(e:E){print(1)}}}\n", NULL},
        {"fn unused(){defer{var n=0;while(n<2){n=n+1;if(n==1){continue};break}}}\n", NULL},
        {"fn unused(){defer{const f=fn()->i64{return 3};print(f())}}\n", "E0392"},
        {"fn unused(){defer{const f=fn()->i64{return 3}}}\n", NULL},
        {"fn unused<T>(v:T)->T{defer{const copy=v};return v}\n", NULL},
        {"struct C{value:i64;constructor(){defer{print(this.value)};this.value=2}}\n", "read requires storage initialized"},
        {"struct C{const value:i64=1;constructor(){defer{this.value=2}}}\n", "field access is not permitted"},
        {"struct C{private value:i64=1;constructor(){} }\nfn f(){const c=C();defer{print(c.value)}}\n", "field access is not permitted"},
        {"struct C{private const value:i64=1;constructor(){defer{print(this.value)}}}\n", NULL},
        {"struct C{value:i64}\nfn f(){const c=C{value:1};c.value+=1}\n", "field mutation requires"},
        {"struct C{const value:i64}\nfn f(){var c=C{value:1};c.value+=1}\n", "field access is not permitted"},
        {"struct C{private value:i64=1;constructor(){}}\nfn f(){var c=C();c.value+=1}\n", "field access is not permitted"},
        {"struct C{value:i64}\nfn f(c:C){c.value+=1}\n", "field mutation requires"},
        {"struct C{value:i64;constructor(){this.value+=1}}\n", "read requires storage initialized"},
        {"struct C{const value:i64=1;constructor(){this.value+=1}}\n", "field access is not permitted"},
        {"struct C{value:i8}\nfn f(){var c=C{value:1};c.value+=(1 as i64)}\n", "expression cannot satisfy its declared type"},
        {"struct C<T>{value:T}\nfn f<T>(v:T){var c=C<T>{value:v};c.value+=v}\n", "declared concrete operand contract"},
        {"struct C{value:i64;constructor(ok:bool){if(ok){this.value=1};return}}\n", "read requires storage initialized"},
        {"struct C{const value:i64;constructor(ok:bool){if(ok){this.value=1};this.value=2}}\n", "write may overwrite already initialized const storage"},
        {"struct C{const value:i64;constructor(ok:bool){while(ok){this.value=1};this.value=2}}\n", "write may overwrite already initialized const storage"},
        {"struct C{value:i64;constructor(){if(false){print(this.value)};this.value=1}}\n", "read requires storage initialized"},
        {"struct C{const value:i64;constructor(ok:bool){if(ok){this.value=1}else{this.value=2}}}\n", NULL},
        {"struct C{value:i64;constructor(){this.value=1;defer{this.value+=2};return}}\n", NULL}
    };
    unsigned failures = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        write_source(path, cases[i].source);
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_source_check(request, &result, &diagnostic);
        const XrXirSourceView *view = xr_xir_source_snapshot_view(result.snapshot);
        bool valid = cases[i].code == NULL;
        bool correct = valid ? status == XR_XIR_OK && result.checked && result.snapshot :
            status != XR_XIR_OK && !result.checked && view && !view->complete &&
            view->diagnostic.status == status && diagnostic.status == status &&
            view->diagnostic.line == diagnostic.line && view->diagnostic.column == diagnostic.column &&
            !strcmp(view->diagnostic.message, diagnostic.message) &&
            diagnostic.line > 0 && strstr(diagnostic.message, cases[i].code) != NULL;
        if (!correct) {
            fprintf(stderr, "cleanup source case %u: %u at %d:%d: %s\n", i, status,
                diagnostic.line, diagnostic.column, diagnostic.message);
            ++failures;
        }
        xr_xir_source_result_free(&result);
    }
    CHECK(!failures);
}
