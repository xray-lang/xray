/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_catch_cases.h - Handler checking is independent of runtime reachability
 *
 * A rejected case may name a diagnostic fragment so that it fails for the
 * intended rule rather than for an unrelated mistake in the probe source.
 */
static void source_catch_cases(XrXirSourceRequest *request, const char *path, const char *library) {
    AdmissionCase case_owner={0};
    write_source(library,
        "export enum Shared<T> { Failed { message:T }, Empty }\n"
        "enum Hidden { Bad }\n"
        "export fn raise(message:string) { throw Shared<string>.Failed{message:message} }\n");
    const struct { const char *source; bool valid; const char *diagnostic; } cases[] = {
        {"fn run() -> i64 { try { } catch (e) { return 1 }; return 2 }\nrun()\n",true,NULL},
        {"enum Failure { Bad }\nfn run() -> i64 { try { throw Failure.Bad } catch (e) { return 7 } }\nrun()\n",true,NULL},
        {"enum Failure { Bad }\nfn fail() -> i64 { throw Failure.Bad }\nfn run() -> i64 { try { return fail() } catch (e) { return 7 } }\nrun()\n",true,NULL},
        {"enum Failure { Bad }\nfn run() -> i64 { try { try { throw Failure.Bad } catch (e) { throw e } } catch (outer) { return 9 } }\nrun()\n",true,NULL},
        {"fn run() { try { } catch (e) { const x = missing } }\nrun()\n",false,NULL},
        {"fn run() { try { } catch (e) { var text: string = 9 } }\nrun()\n",false,NULL},
        {"fn run() -> i64 { try { } catch (e) { return true }; return 2 }\nrun()\n",false,NULL},
        {"enum Failure { Bad }\nfn run() { try { throw Failure.Bad } catch (e) { }; const x = e }\nrun()\n",false,NULL},
        {"struct C { const value:string\n constructor() { try {} catch(e) { const bad=this.value }; this.value=\"ready\" } }\nconst c=C()\n",false,NULL},
        {"struct C { const value:string=\"ready\"\n constructor() { try {} catch(e) { this.value=\"again\" } } }\nconst c=C()\n",false,NULL},
        {"const f=fn() { try {} catch(e) { return true }; return 2 }\nf()\n",false,NULL},
        {"const f=fn() { try {} catch(e) { return 1 }; return 2 }\nf()\n",true,NULL},
        {"struct C { const value:string=\"ready\"\n constructor() { try {} catch(e) { const good=this.value } } }\nconst c=C()\n",true,NULL},
        {"struct C { const value:string\n constructor() { try {} catch(e) { this.value=\"handler\" }; this.value=\"normal\" } }\nconst c=C()\n",true,NULL},
        {"struct C { const value:string\n constructor(flag:bool) { try {} catch(e) { if(flag){this.value=\"left\"}else{this.value=\"right\"}; const good=this.value }; this.value=\"normal\" } }\nconst c=C(true)\n",true,NULL},
        {"struct C { const value:string\n constructor(flag:bool) { try {} catch(e) { if(flag){this.value=\"left\"}; const bad=this.value }; this.value=\"normal\" } }\nconst c=C(true)\n",false,NULL},
        {"struct C { const value:string\n constructor(flag:bool) { try {} catch(e) { while(flag){this.value=\"again\"} }; this.value=\"normal\" } }\nconst c=C(true)\n",false,NULL},
        {"struct C { const value:string\n constructor(flag:bool) { try {} catch(e) { while(flag){this.value=\"once\"; break}; const bad=this.value }; this.value=\"normal\" } }\nconst c=C(true)\n",false,NULL},
        {"struct C { const value:string\n constructor() { try {} catch(e) { this.value=\"handler\"; try {} catch(inner) { const good=this.value } }; this.value=\"normal\" } }\nconst c=C()\n",true,NULL},
        {"struct C { const value:string\n constructor() { try {} catch(e) { try {this.value=\"inner\"} catch(inner) { const bad=this.value } }; this.value=\"normal\" } }\nconst c=C()\n",false,NULL},
        {"struct C { const value:string\n constructor() { try {this.value=\"normal\"} catch(e) { const bad=this.value } } }\nconst c=C()\n",false,NULL},
        {"struct C { const value:string\n constructor() { try {} catch(e) { return }; this.value=\"normal\" } }\nconst c=C()\n",false,NULL},
        {"struct C { const value:string\n constructor(flag:bool) { while(flag) { try {} catch(e) { break }; break }; this.value=\"normal\" } }\nconst c=C(true)\n",true,NULL},
        {"fn run() {try {} catch(e) { const f=fn() {throw e} }}\nrun()\n",true,NULL},
        {"struct C { const value:string\n constructor(flag:bool) { try {} catch(e) { if(flag){this.value=\"unused\"} }; this.value=\"normal\" } }\nconst c=C(true)\n",true,NULL},
        {"enum Failure { Bad }\nfn run() {try {throw Failure.Bad} catch(e) {e=e}}\nrun()\n",false,NULL},
        {"enum Failure { Bad }\nfn run() {try {throw Failure.Bad} catch(e) {var saved=e; saved=e; throw saved}}\nrun()\n",true,NULL},
        {"struct C { const value:string\n constructor() { try {} catch(e) { const f=fn()->string{return this.value} }; this.value=\"normal\" } }\nconst c=C()\n",false,NULL},
        {"enum Failure { Bad }\nfn run(e:Error)->Error {try {throw e} catch(inner) {return inner}}\nconst result=run(Failure.Bad)\n",true,NULL},
        {"enum E { Bad }\nfn run()->i64 {try {throw E.Bad} catch(e:E){return 1}}\nrun()\n",true,NULL},
        {"enum E { Bad }\nfn run(){try {} catch(e:E){const x=missing}}\nrun()\n",false,NULL},
        {"fn run(){try {} catch(e:i64){}}\nrun()\n",false,NULL},
        {"enum E { Bad }\nfn run(){try {throw E.Bad} catch(e:Error){}}\nrun()\n",false,NULL},
        {"enum E { Bad }\nfn run(){try {throw E.Bad} catch(e:E){e=E.Bad}}\nrun()\n",false,NULL},
        {"enum E<T> { Bad {value:T} }\nfn run<T>(v:T)->T {try {throw E<T>.Bad{value:v}} catch(e:E<T>){return match(e){E.Bad{value:x}->x}}}\nrun<i64>(1)\n",true,NULL},
        {"enum E { Bad }\nfn run(){try {throw E.Bad} catch(e){} catch(later:E){const x=missing}}\nrun()\n",false,NULL},
        {"enum E<T:Sendable> { Bad }\nfn unused<T>() {try {} catch(e:E<T>){}}\n",false,NULL},
        {"enum E<T:Sendable> { Bad }\nfn unused<T:Sendable>() {try {} catch(e:E<T>){}}\n",true,NULL},
        {"enum E<T:Sendable> { Bad }\nfn unused() {try {} catch(e:E<Error>){}}\n",false,NULL},
        /* Enum variant pattern handlers. */
        {"enum E { A, B }\nfn run()->i64 {try {throw E.B} catch(E.A){return 1} catch(E.B){return 2}}\nrun()\n",true,NULL},
        {"enum E { Bad {code:i64} }\nfn run()->i64 {try {throw E.Bad{code:3}} catch(E.Bad{code}){return code}}\nrun()\n",true,NULL},
        {"enum E { Bad {code:i64} }\nfn run()->i64 {try {throw E.Bad{code:9}} catch(E.Bad{code:1..=5}){return 1} catch(E.Bad{code:x}){return x}}\nrun()\n",true,NULL},
        {"enum E { Bad {code:i64}, Other }\nfn run(flag:bool)->i64 {try {if(flag){throw E.Bad{code:9}}} catch(E.Bad{}){return 1}; return 2}\nrun(true)\n",true,NULL},
        {"enum E { A {code:i64}, B {code:i64}, C }\nfn run()->i64 {try {throw E.B{code:4}} catch(E.A{code}, E.B{code}){return code}}\nrun()\n",true,NULL},
        {"enum E<T> { Bad {value:T} }\nfn run<T>(v:T)->T {try {throw E<T>.Bad{value:v}} catch(E<T>.Bad{value}){return value}}\nrun<i64>(1)\n",true,NULL},
        {"enum E<T> { A {value:T}, B {value:T} }\nfn run(v:i64)->i64 {try {throw E<i64>.B{value:v}} catch(E<i64>.A{value}, E.B{value}){return value}}\nrun(1)\n",true,NULL},
        {"enum I { X {code:i64}, Y }\nenum O { Wrap {inner:I} }\nfn run()->i64 {try {throw O.Wrap{inner:I.X{code:4}}} catch(O.Wrap{inner:I.X{code}}){return code}}\nrun()\n",true,NULL},
        {"enum E { Bad {code:i64} }\nfn run()->i64 {try {} catch(E.Bad{code}){return code}; return 2}\nrun()\n",true,NULL},
        {"enum E { Bad {code:i64} }\nfn run()->i64 {try {throw E.Bad{code:1}} catch(e){return 1} catch(E.Bad{code}){return code}}\nrun()\n",true,NULL},
        {"enum E { Bad {text:string} }\nfn run()->string {try {throw E.Bad{text:\"x\"}} catch(E.Bad{text}){const f=fn()->string{return text}; return f()}}\nrun()\n",true,NULL},
        {"enum E { Bad {code:i64}, Other }\nfn run()->i64 {try {try {throw E.Bad{code:1}} catch(E.Bad{code}){throw E.Other}} catch(E.Other){return 5}}\nrun()\n",true,NULL},
        {"enum E { Bad {code:i64} }\nfn run()->i64 {try {throw E.Bad{code:1}} catch(E.Bad{code}){const code=2; return code}}\nrun()\n",true,NULL},
        {"enum E { A }\nfn run(){try {} catch(E.Missing){}}\nrun()\n",false,"pattern variant or payload syntax mismatch"},
        {"enum E { A }\nfn run(){try {} catch(E.A{}){}}\nrun()\n",false,"pattern variant or payload syntax mismatch"},
        {"enum E { Bad {code:i64} }\nfn run(){try {} catch(E.Bad){}}\nrun()\n",false,"pattern variant or payload syntax mismatch"},
        {"enum E { A }\nenum F { B }\nfn run(){try {throw E.A} catch(E.A, F.B){}}\nrun()\n",false,"pattern must name the scrutinee enum declaration"},
        {"enum E<T> { Bad {value:T} }\nfn run(){try {throw E<i64>.Bad{value:1}} catch(E.Bad{value}){}}\nrun()\n",false,"must state the enum's type arguments"},
        {"enum E<T> { A {value:T}, B {value:T} }\nfn run(){try {} catch(E<i64>.A{value}, E<string>.B{value}){}}\nrun()\n",false,"pattern type arguments differ from the scrutinee"},
        {"enum E { Bad {code:i64} }\nfn run(){try {} catch(E.Bad{missing}){}}\nrun()\n",false,"unknown or duplicate pattern field"},
        {"enum E { Bad {code:i64} }\nfn run(){try {} catch(E.Bad{code, code:x}){}}\nrun()\n",false,"unknown or duplicate pattern field"},
        {"enum E { P {a:i64, b:i64} }\nfn run(){try {} catch(E.P{a:x, b:x}){}}\nrun()\n",false,"duplicate pattern binding"},
        {"enum E { A {code:i64}, B {code:i64} }\nfn run(){try {} catch(E.A{code}, E.B{code:other}){}}\nrun()\n",false,"alternatives require the same binding names"},
        {"enum E { A {code:i64}, B {code:string} }\nfn run(){try {} catch(E.A{code}, E.B{code}){}}\nrun()\n",false,"alternatives require the same binding names"},
        {"enum E { A {code:i64}, B }\nfn run(){try {} catch(E.A{code}, E.B){}}\nrun()\n",false,"alternative is missing a common binding"},
        {"enum E { Bad {code:i64} }\nfn run(){try {throw E.Bad{code:1}} catch(E.Bad{code}){code=2}}\nrun()\n",false,NULL},
        {"enum E { Bad {code:i64} }\nfn run(){try {throw E.Bad{code:1}} catch(E.Bad{code}){}; const x=code}\nrun()\n",false,NULL},
        {"enum E { Bad {code:i64} }\nfn run(){try {} catch(E.Bad{code}){const x=missing}}\nrun()\n",false,NULL},
        {"enum E { Bad {code:i64} }\nfn run(){try {throw E.Bad{code:1}} catch(e){} catch(E.Bad{code}){var s:string=code}}\nrun()\n",false,NULL},
        {"enum E<T:Sendable> { Bad }\nfn unused<T>() {try {} catch(E<T>.Bad){}}\n",false,"definition-time type proof"},
        {"enum E<T:Sendable> { Bad }\nfn unused<T:Sendable>() {try {} catch(E<T>.Bad){}}\n",true,NULL},
        {"enum E<T:Sendable> { Bad }\nfn unused() {try {} catch(E<Error>.Bad){}}\n",false,"definition-time type proof"},
        {"enum E { Bad {code:i64} }\nfn run(){try {} catch(E.Bad{code:\"x\"}){}}\nrun()\n",false,"literal pattern type differs from its field"},
        {"struct S { x:i64 }\nfn run(){try {} catch(S.x){}}\nrun()\n",false,"catch pattern must name a visible enum variant"},
        {"fn run(){const E=1; try {} catch(E.A){}}\nrun()\n",false,"catch pattern must name a visible enum variant"},
        {"enum E { Bad {code:i64} }\nconst f=fn() { try {} catch(E.Bad{code}) { return true }; return 2 }\nf()\n",false,NULL},
        {"enum E { Bad {text:string} }\nstruct C { const value:string\n constructor() { try {} catch(E.Bad{text}) { this.value=text }; this.value=\"normal\" } }\nconst c=C()\n",true,NULL},
        {"enum E { Bad {text:string} }\nstruct C { const value:string\n constructor() { try {} catch(E.Bad{text}) { const bad=this.value }; this.value=\"normal\" } }\nconst c=C()\n",false,NULL},
        {"import \"./lib\" as lib\nfn run()->string {try {lib.raise(\"x\")} catch(lib.Shared<string>.Failed{message}){return message}; return \"none\"}\nrun()\n",true,NULL},
        {"import { Shared } from \"./lib\"\nfn run()->i64 {try {throw Shared<i64>.Empty} catch(Shared<i64>.Empty){return 1}}\nrun()\n",true,NULL},
        {"import \"./lib\" as lib\nfn run(){try {} catch(lib.Hidden.Bad){}}\nrun()\n",false,"import requires an exported declaration"},
        {"import \"./lib\" as lib\nfn run(){try {} catch(lib.Shared.Empty){}}\nrun()\n",false,"must state the enum's type arguments"},
        {"enum E { Bad {text:string} }\nfn accept(text:string)->string{return text}\nstruct C { const value:string\n constructor(){try{}catch(E.Bad{text}){this.value=accept(text)};this.value=\"normal\"}}\nconst c=C()\n",true,NULL},
        {"enum E { Bad {text:string} }\nstruct C { const value:string\n constructor(){try{}catch(E.Bad{text}){if(true){this.value=text}else{this.value=\"other\"};const good=this.value};this.value=\"normal\"}}\nconst c=C()\n",true,NULL},
        {"enum E { Bad {text:string} }\nstruct C { const value:string\n constructor(){try{}catch(E.Bad{text}){const bad=this.value;this.value=text};this.value=\"normal\"}}\nconst c=C()\n",false,"read requires storage initialized on every incoming path"},
        {"enum E { Bad {text:string} }\nstruct C { const value:string=\"before\"\n constructor(){try{}catch(E.Bad{text}){this.value=text}}}\nconst c=C()\n",false,"write may overwrite already initialized const storage"},
    };
    uint32_t failures = 0;
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        write_source(path,cases[i].source);
        XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0};
        admission_case_begin(request,&case_owner);
        XrXirStatus status = xr_xir_compile_source_check(&case_owner.request,&result,&diagnostic,NULL);
        bool wrong_reason = !cases[i].valid && cases[i].diagnostic && status != XR_XIR_OK &&
            !strstr(diagnostic.message,cases[i].diagnostic);
        if ((status == XR_XIR_OK) != cases[i].valid || wrong_reason)
            fprintf(stderr,"catch case %u: %u at %d:%d: %s\n",i,status,
                diagnostic.line,diagnostic.column,diagnostic.message);
        failures += ((status == XR_XIR_OK) != cases[i].valid);
        failures += ((result.checked != NULL) != cases[i].valid);
        failures += wrong_reason;
        CHECK(status==XR_XIR_OK || (!result.checked && !result.snapshot));
        xr_xir_compile_source_result_free(&result);
    }
    CHECK(failures == 0);
    admission_case_end(&case_owner);
}
