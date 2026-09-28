/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_catch_cases.h - Handler checking is independent of runtime reachability
 */
static void source_catch_cases(XrXirSourceRequest *request, const char *path) {
    const struct { const char *source; bool valid; } cases[] = {
        {"fn run() -> i64 { try { } catch (e) { return 1 }; return 2 }\nrun()\n",true},
        {"enum Failure { Bad }\nfn run() -> i64 { try { throw Failure.Bad } catch (e) { return 7 } }\nrun()\n",true},
        {"enum Failure { Bad }\nfn fail() -> i64 { throw Failure.Bad }\nfn run() -> i64 { try { return fail() } catch (e) { return 7 } }\nrun()\n",true},
        {"enum Failure { Bad }\nfn run() -> i64 { try { try { throw Failure.Bad } catch (e) { throw e } } catch (outer) { return 9 } }\nrun()\n",true},
        {"fn run() { try { } catch (e) { const x = missing } }\nrun()\n",false},
        {"fn run() { try { } catch (e) { var text: string = 9 } }\nrun()\n",false},
        {"fn run() -> i64 { try { } catch (e) { return true }; return 2 }\nrun()\n",false},
        {"enum Failure { Bad }\nfn run() { try { throw Failure.Bad } catch (e) { }; const x = e }\nrun()\n",false},
        {"struct C { const value:string\n constructor() { try {} catch(e) { const bad=this.value }; this.value=\"ready\" } }\nconst c=C()\n",false},
        {"struct C { const value:string=\"ready\"\n constructor() { try {} catch(e) { this.value=\"again\" } } }\nconst c=C()\n",false},
        {"const f=fn() { try {} catch(e) { return true }; return 2 }\nf()\n",false},
        {"const f=fn() { try {} catch(e) { return 1 }; return 2 }\nf()\n",true},
        {"struct C { const value:string=\"ready\"\n constructor() { try {} catch(e) { const good=this.value } } }\nconst c=C()\n",true},
        {"struct C { const value:string\n constructor() { try {} catch(e) { this.value=\"handler\" }; this.value=\"normal\" } }\nconst c=C()\n",true},
        {"struct C { const value:string\n constructor(flag:bool) { try {} catch(e) { if(flag){this.value=\"left\"}else{this.value=\"right\"}; const good=this.value }; this.value=\"normal\" } }\nconst c=C(true)\n",true},
        {"struct C { const value:string\n constructor(flag:bool) { try {} catch(e) { if(flag){this.value=\"left\"}; const bad=this.value }; this.value=\"normal\" } }\nconst c=C(true)\n",false},
        {"struct C { const value:string\n constructor(flag:bool) { try {} catch(e) { while(flag){this.value=\"again\"} }; this.value=\"normal\" } }\nconst c=C(true)\n",false},
        {"struct C { const value:string\n constructor(flag:bool) { try {} catch(e) { while(flag){this.value=\"once\"; break}; const bad=this.value }; this.value=\"normal\" } }\nconst c=C(true)\n",false},
        {"struct C { const value:string\n constructor() { try {} catch(e) { this.value=\"handler\"; try {} catch(inner) { const good=this.value } }; this.value=\"normal\" } }\nconst c=C()\n",true},
        {"struct C { const value:string\n constructor() { try {} catch(e) { try {this.value=\"inner\"} catch(inner) { const bad=this.value } }; this.value=\"normal\" } }\nconst c=C()\n",false},
        {"struct C { const value:string\n constructor() { try {this.value=\"normal\"} catch(e) { const bad=this.value } } }\nconst c=C()\n",false},
        {"struct C { const value:string\n constructor() { try {} catch(e) { return }; this.value=\"normal\" } }\nconst c=C()\n",false},
        {"struct C { const value:string\n constructor(flag:bool) { while(flag) { try {} catch(e) { break }; break }; this.value=\"normal\" } }\nconst c=C(true)\n",true},
        {"fn run() {try {} catch(e) { const f=fn() {throw e} }}\nrun()\n",true},
        {"struct C { const value:string\n constructor(flag:bool) { try {} catch(e) { if(flag){this.value=\"unused\"} }; this.value=\"normal\" } }\nconst c=C(true)\n",true},
        {"enum Failure { Bad }\nfn run() {try {throw Failure.Bad} catch(e) {e=e}}\nrun()\n",false},
        {"enum Failure { Bad }\nfn run() {try {throw Failure.Bad} catch(e) {var saved=e; saved=e; throw saved}}\nrun()\n",true},
        {"struct C { const value:string\n constructor() { try {} catch(e) { const f=fn()->string{return this.value} }; this.value=\"normal\" } }\nconst c=C()\n",false},
        {"enum Failure { Bad }\nfn run(e:Error)->Error {try {throw e} catch(inner) {return inner}}\nconst result=run(Failure.Bad)\n",true}
    };
    uint32_t failures = 0;
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        write_source(path,cases[i].source);
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
        XrXirStatus status = xr_xir_source_check(request,&result,&diagnostic);
        if ((status == XR_XIR_OK) != cases[i].valid)
            fprintf(stderr,"catch case %u: %u at %d:%d: %s\n",i,status,
                diagnostic.line,diagnostic.column,diagnostic.message);
        failures += ((status == XR_XIR_OK) != cases[i].valid);
        failures += ((result.checked != NULL) != cases[i].valid);
        xr_xir_source_result_free(&result);
    }
    CHECK(failures == 0);
}
