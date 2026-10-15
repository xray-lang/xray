/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_source_comptime.c - Genuine two-construction Source constants
 *
 * KEY CONCEPT:
 *   Caller death precedes complete packet receiving and real VM execution.
 *   Allocation injection observes the sole finite compiler ledger implementation.
 */
#include "base/xmalloc.h"
#include "base/xsha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1); } } while (0)
#include "xir_source_compile_owner.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_ctfe.h"
static const XrCompileResourceLimits source_comptime_test_limits={
    UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
static const char source_comptime_cleanup_text[]=
    "fn inc(x:ref i64) { x=x+1 }\n"
    "fn twice(x:ref i64) { inc(ref x); inc(ref x) }\n"
    "fn work(x:ref i64) { defer { twice(ref x) } }\n"
    "fn answer()->i64 { var x=40; work(ref x); return x }\n"
    "export fn run()->i64 { return comptime answer() }\n";
static const char source_comptime_output_text[]=
    "fn table()->Array<i8>{var xs:Array<i8>=[0,1];var i=2;while(i<10){xs.push(xs[i-1]+xs[i-2]);i=i+1};return xs}\n"
    "fn pair()->(i64,i64){return (21,21)}\n"
    "fn table_constant()->Array<i8>{return comptime table()}\n"
    "fn pair_constant()->(i64,i64){return comptime pair()}\n"
    "export fn run()->i64{return comptime {const xs=table_constant();const p=pair_constant();xs[9] as i64+p.0+p.1-34}}\n";
static const char source_comptime_class_text[]=
    "class Box{items:Array<i64>;constructor(items:Array<i64>){this.items=items}}\n"
    "fn inc(x:ref i64){defer{x=x+2}}\n"
    "fn answer()->i64{const b=Box([40]);inc(ref b.items[0]);return b.items[0]}\n"
    "export fn run()->i64{return comptime answer()}\n";
static const char source_comptime_dynamic_loan_text[]=
    "class Box{items:Array<(i64,i64)>;constructor(items:Array<(i64,i64)>){this.items=items}}\n"
    "fn add(xs:ref Array<(i64,i64)>){xs.push((1,1))}\n"
    "fn middle(xs:ref Array<(i64,i64)>){add(ref xs);xs[1]=(20,20);add(ref xs)}\n"
    "fn answer()->i64{const b=Box([(0,0)]);middle(ref b.items);return b.items[1].0+b.items[1].1+b.items[2].0+b.items[2].1}\n"
    "export fn run()->i64{return comptime answer()}\n";
static const char source_comptime_inactive_loan_text[]=
    "class Box{items:Array<(i64,i64)>;next:Box?;constructor(items:Array<(i64,i64)>,next:Box?){this.items=items;this.next=next}}\n"
    "fn add(xs:ref Array<(i64,i64)>){xs.push((1,1))}\n"
    "fn middle(xs:ref Array<(i64,i64)>){add(ref xs);xs[1]=(20,20);add(ref xs)}\n"
    "fn answer()->i64{const b=Box([(0,0)],null);middle(ref b.items);return b.items[1].0+b.items[1].1+b.items[2].0+b.items[2].1}\n"
    "export fn run()->i64{return comptime answer()}\n";
static const char source_comptime_array_identity_text[]=
    "class Box{value:i64;constructor(value:i64){this.value=value}}\n"
    "fn grow(xs:ref Array<Box>){xs.push(Box(41));xs[0]=Box(1)}\n"
    "fn answer()->i64{var xs=[Box(0)];grow(ref xs);return xs[0].value+xs[1].value}\n"
    "export fn run()->i64{return comptime answer()}\n";
static const char source_comptime_ref_closure_text[]=
    "fn answer()->i64{var n=2;var x=38;"
    "const f:fn(ref i64)->i64=fn(value)->i64{value+=n;defer{value+=2};return value};f(ref x);return x}\n"
    "export fn run()->i64{return comptime answer()}\n";
static const char source_comptime_struct_identity_text[]=
    "class Box{value:i64;constructor(value:i64){this.value=value}}\n"
    "struct Wrap{item:Box;label:string}\n"
    "fn replace(value:ref Wrap){value=Wrap{item:Box(0),label:\"changed\"}}\n"
    "fn own()->string{var w=Wrap{item:Box(42),label:\"ab\"};defer{replace(ref w)};return w.label}\n"
    "fn answer()->i64{return len(own())+40}\n"
    "export fn run()->i64{return comptime answer()}\n";
static const char source_comptime_panic_text[]=
    "fn fail(x:ref i64)->i64{defer{x=x+2};assert(false,\"ab\");return 0}\n"
    "fn answer()->i64{var xs=[40];try{return fail(ref xs[0])}catch panic(p){return p.code==445 && p.message==\"ab\" ? xs[0] : 0}}\n"
    "export fn run()->i64{return comptime answer()}\n";
static const char source_comptime_error_text[]=
    "enum E{Bad}\n"
    "fn fail(x:ref i64)->i64{defer{x=x+2};throw E.Bad}\n"
    "fn answer()->i64{var xs=[40];try{return fail(ref xs[0])}catch(e:E){return xs[0]}}\n"
    "export fn run()->i64{return comptime answer()}\n";
static const char source_comptime_projection_text[]=
    "fn inc(x:ref i64){defer{x=x+2}}\n"
    "fn answer()->i64{var xs=[[40]];inc(ref xs[0][0]);return xs[0][0]}\n"
    "export fn run()->i64{return comptime answer()}\n";
static const char source_comptime_test_text[]=
    "fn leaf(x:i64)->i64 { return x+1 }\n"
    "fn middle(x:i64)->i64 { return leaf(x)+1 }\n"
    "export fn run()->i64 { return comptime middle(40) }\n";
static const char source_comptime_local_text[]=
    "fn positive(x:i64)->bool { return x>0 }\n"
    "fn admitted(x:i64)->bool { return positive(x) && (x==6 || x==7) }\n"
    "fn answer()->i64 { return admitted(6) ? 42 : 0 }\n"
    "export fn run()->i64 { return comptime answer() }\n";
static const char source_comptime_unit_text[]=
    "fn noop() {}\n"
    "fn answer()->i64 { noop(); return 42 }\n"
    "export fn run()->i64 { comptime {}; comptime { const x=6; const y=7 }; comptime noop(); return comptime answer() }\n";
static const char source_comptime_rune_text[]=
    "fn same(a:rune,b:rune)->bool { return a==b }\n"
    "fn answer()->i64 { const c=rune(128512); return same(c,rune(128512)) && i64(c)==128512 ? 42 : 0 }\n"
    "export fn run()->i64 { comptime 'A'; return comptime answer() }\n";
static const char source_comptime_cell_text[]=
    "fn answer()->i64 { var i=0; var total=0; while (i<6) { total=total+7; i=i+1 }; return total }\n"
    "export fn run()->i64 { comptime { var u:()=(); u=() }; return comptime answer() }\n";
static const char source_comptime_string_text[]=
    "fn join(a:string,b:string)->string { return a+b }\n"
    "fn answer()->i64 { return join(\"ab\",\"cd\")==\"abcd\" ? 42 : 0 }\n"
    "export fn run()->i64 { comptime join(\"ab\",\"cd\"); return comptime answer() }\n";
static const char source_comptime_string_read_text[]=
    "fn answer()->i64 { const t=\"abcabc\"; return t.contains(\"bc\") && t.startsWith(\"ab\") && t.endsWith(\"bc\") && \"a\"<\"b\" && t.indexOf(\"bc\",2)==4 && t.lastIndexOf(\"ab\")==3 ? 42 : 0 }\n"
    "export fn run()->i64 { return comptime answer() }\n";
static const char source_comptime_tuple_text[]=
    "fn make()->(i8,string,()) { var p:(i8,string,())=(1,\"a\",()); p=(7,\"ab\",()); return p }\n"
    "fn answer()->i64 { const t=make(); return t.0==7 && t.1==\"ab\" ? 42 : 0 }\n"
    "export fn run()->i64 { comptime ((),()); comptime make(); return comptime answer() }\n";
static const char source_comptime_float_text[]=
    "fn compute(x:f32)->f32 { return x/2.0 }\n"
    "fn answer()->i64 { var x:f32=compute(3.5); return x==1.75 && (0.0/0.0)!=(0.0/0.0) ? 42 : 0 }\n"
    "export fn run()->i64 { comptime compute(3.5); return comptime answer() }\n";
static const char source_comptime_nullable_text[]=
    "fn wrap(x:i8)->i8? { return x }\n"
    "fn make()->(i8,string)? { var x:(i8,string)?=null; x=(7,\"ab\"); return x }\n"
    "fn same(a:string?,b:string?)->bool { return a==b }\n"
    "fn answer()->i64 { const a:string?=\"ab\"; return (wrap(7)??0)==7 && make()!=null && same(a,a) ? 42 : 0 }\n"
    "export fn run()->i64 { comptime make(); return comptime answer() }\n";
static const char source_comptime_array_text[]=
    "fn make()->Array<i8> { var xs:Array<i8>=[1,2]; const old=xs; xs[0]=7; xs.push(3); return xs }\n"
    "fn answer()->i64 { const xs=make(); return len(xs)==3 && xs[0]==7 && xs[2]==3 ? 42 : 0 }\n"
    "export fn run()->i64 { comptime make(); return comptime answer() }\n";
static const char source_comptime_array_equal_text[]=
    "fn same(a:Array<string?>,b:Array<string?>)->bool { return a==b }\n"
    "fn answer()->i64 { const a:Array<string?>=[\"ab\",null]; const nan:Array<Array<f64>>=[[0.0/0.0]]; return same(a,a) && nan!=nan ? 42 : 0 }\n"
    "export fn run()->i64 { return comptime answer() }\n";
static const char source_comptime_array_path_text[]=
    "fn make()->Array<string> { var xs:Array<Array<string>>=[[\"x\"]]; const old=xs; xs[0][0]=\"a\\0b\"; xs[0].push(\"z\"); return xs[0] }\n"
    "fn answer()->i64 { var xs:Array<Array<i8>>=[[1,2]]; xs[0].fill(7,0,1); xs[0].reserve(6); return xs[0][0]==7 && xs[0][1]==2 && xs[0].capacity==6 ? 42 : 0 }\n"
    "export fn run()->i64 { comptime make(); return comptime answer() }\n";
static const char source_comptime_text_text[]=
    "fn make()->string { const a:Array<f64>=[3.5,-0.0]; return a.join(\",\") }\n"
    "fn answer()->i64 { const n:Array<u64>=[18446744073709551615]; const b:Array<bool>=[true]; const r:Array<rune>=['\\0','A']; return n.join(\"\")==\"18446744073709551615\" && b.join(\"\")==\"true\" && len(r.join(\"\"))==2 ? 42 : 0 }\n"
    "export fn run()->i64 { comptime make(); return comptime answer() }\n";
static const char source_comptime_nominal_text[]=
    "struct Pair { x:i8; text:string }\n"
    "enum Choice { Empty, Some { pair:Pair } }\n"
    "fn make()->Choice { var p=Pair{x:1,text:\"ab\"}; const old=p; p.x=7; return Choice.Some{pair:p} }\n"
    "fn answer()->i64 { var p=Pair{x:1,text:\"ab\"}; const old=p; p.x=7; return old.x==1 && p.x==7 ? 42 : 0 }\n"
    "export fn run()->i64 { comptime make(); return comptime answer() }\n";
static const char source_comptime_lexical_text[]=
    "struct Box {private value:i8;text:string;get()->i8{return this.value};static make()->Box{return comptime Box{value:7,text:\"ab\"}}}\n"
    "class Math {private static twice(n:i64)->i64{return n*2};static value()->i64{return comptime Math.twice(21)}}\n"
    "fn answer()->i64{const b=Box.make();return b.get()==7 && b.text==\"ab\" ? Math.value() : 0}\n"
    "export fn run()->i64{return comptime answer()}\n";
static const char source_comptime_function_text[]=
    "fn make(n:i8)->fn()->i8{var x=n;return fn()->i8{x=x+1;return x}}\n"
    "fn answer()->i64{const f=make(40);f();const g=fn()->i64{return f() as i64};return g()}\n"
    "export fn run()->i64{return comptime answer()}\n";
static const char *source_comptime_resource_text=source_comptime_test_text;
static XrXirStatus source_comptime_test_resource_status(XrCompileResourceStatus status) {
    return status==XR_COMPILE_RESOURCE_OK?XR_XIR_OK:status==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:
        status==XR_COMPILE_RESOURCE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
}
static XrCompileResourceStats source_comptime_test_stats(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);return stats;
}
static void source_comptime_test_monotonic(XrCompileResourceStats before,XrCompileResourceStats after) {
    CHECK(after.allocation_count>=before.allocation_count && after.allocated_bytes>=before.allocated_bytes);
    CHECK(after.work>=before.work && after.peak_bytes>=before.peak_bytes);
}
/* This is a real public producer call. Caller text, authority and session die
 * before the returned Checked artifact or owned query is consumed. */
static XrXirStatus source_comptime_test_build(const XrXirCompileContext *context,const char *text,
    XrXirSourceResult *output,XrXirSourceDiagnostic *diagnostic) {
    size_t length=strlen(text);CHECK(length<4096);
    char bytes[4096];memcpy(bytes,text,length+1);char name[]="source-comptime";
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,name,NULL};
    XrCompilerSession *session=NULL;
    XrCompilerSessionStatus opened=xr_compile_session_new(context->resources,&session);
    if (opened!=XR_COMPILER_SESSION_OK) {
        CHECK(!session);return opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:
            opened==XR_COMPILER_SESSION_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;
    }
    XrXirSourceRequest request={session,NULL,&authority,context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,bytes,length};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,diagnostic,NULL);
    xr_compile_session_free(session);memset(bytes,0xa5,sizeof(bytes));memset(name,0xa5,sizeof(name));
    authority=(XrModuleIdentityAuthority){0};request=(XrXirSourceRequest){0};input=(XrXirSourceText){0};
    return status;
}
static uint32_t source_comptime_test_function(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;size_t length=strlen(name);
    for (uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];
        if (body->name_length==length && !memcmp(body->name,name,length)) { CHECK(found==UINT32_MAX);found=f; }
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void source_comptime_test_fold(const XrXirModule *module,int64_t expected,XrXirType type) {
    const XrXirFunction *body=&module->functions[source_comptime_test_function(module,"run")];bool present=false;
    for (uint32_t i=0;i<body->instruction_count;++i) {
        const XrXirInstruction *op=&body->instructions[i];
        CHECK(op->op!=XR_XIR_CALL && op->op!=XR_XIR_CALL_INDIRECT && op->op!=XR_XIR_FUNCTION_REF);
        if (op->op==(type==XR_XIR_BOOL?XR_XIR_CONST_BOOL:XR_XIR_CONST_INT) && op->type==type && op->immediate==expected)
            present=true;
    }
    CHECK(present);
}
#include "xir_source_comptime_cases.h"
#include "xir_source_comptime_resources.h"
static bool source_comptime_inactive_case(int argc,char **argv) {
    if (argc!=2 || strcmp(argv[1],"--inactive-loan")) return false;
    source_comptime_test_answer(source_comptime_inactive_loan_text,XR_XIR_I64,42);
    fprintf(stderr,"Source inactive Class sibling/data loan independent recipe complete Checked/query normal physical0\n");return true;
}
static bool source_comptime_array_identity_case(int argc,char **argv) {
    if (argc!=2) return false;
    if (!strcmp(argv[1],"--array-identity-resources")) {
        source_comptime_resource_text=source_comptime_array_identity_text;
        source_comptime_test_resources();return true;
    }
    if (strcmp(argv[1],"--array-identity")) return false;
    source_comptime_test_answer(source_comptime_array_identity_text,XR_XIR_I64,42);
    fprintf(stderr,"Source typed Array identities/ref growth independent recipe Checked/query normal physical0\n");return true;
}
static bool source_comptime_ref_closure_case(int argc,char **argv) {
    if (argc!=2) return false;
    if (!strcmp(argv[1],"--ref-closure-resources")) {
        source_comptime_resource_text=source_comptime_ref_closure_text;
        source_comptime_test_resources();return true;
    }
    if (strcmp(argv[1],"--ref-closure")) return false;
    source_comptime_test_answer(source_comptime_ref_closure_text,XR_XIR_I64,42);
    fprintf(stderr,"Source contextual ref closure/owned Cell prefix/scoped suffix/cleanup independent recipe normal physical0\n");return true;
}
static bool source_comptime_struct_identity_case(int argc,char **argv) {
    if (argc!=2) return false;
    if (!strcmp(argv[1],"--struct-identity-resources")) {
        source_comptime_resource_text=source_comptime_struct_identity_text;
        source_comptime_test_resources();return true;
    }
    if (strcmp(argv[1],"--struct-identity")) return false;
    source_comptime_test_answer(source_comptime_struct_identity_text,XR_XIR_I64,42);
    fprintf(stderr,"Source dynamic struct/Class/ref cleanup/owning String snapshot independent recipe normal physical0\n");return true;
}
int main(int argc,char **argv) {
    if (source_comptime_struct_identity_case(argc,argv)) return 0;
    if (source_comptime_ref_closure_case(argc,argv)) return 0;
    if (source_comptime_array_identity_case(argc,argv)) return 0;
    if (source_comptime_inactive_case(argc,argv)) return 0;
    if (argc==2 && !strcmp(argv[1],"--dynamic-loan")) {
        source_comptime_test_answer(source_comptime_dynamic_loan_text,XR_XIR_I64,42);
        fprintf(stderr,"Source growing Class data loan/independent recipe complete Checked/query normal physical0\n");return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--output")) {
        source_comptime_test_answer(source_comptime_output_text,XR_XIR_I64,42);
        fprintf(stderr,"Source original Fibonacci10/pair16 output policy2 independent recipes complete Checked/query normal physical0\n");return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--class")) {
        source_comptime_test_answer(source_comptime_class_text,XR_XIR_I64,42);
        fprintf(stderr,"Source private Class/Array field/projected-ref cleanup/independent recipe complete Checked/query normal physical0\n");return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--panic")) {
        source_comptime_test_answer(source_comptime_panic_text,XR_XIR_I64,42);
        fprintf(stderr,"Source panic/PanicInfo/assertion message/projected-ref cleanup/independent recipe complete Checked/query normal physical0\n");return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--error")) {
        source_comptime_test_answer(source_comptime_error_text,XR_XIR_I64,42);
        fprintf(stderr,"Source typed Error/invoke/projected-ref cleanup/independent recipe complete Checked/query normal physical0\n");return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--projection")) {
        source_comptime_test_answer(source_comptime_projection_text,XR_XIR_I64,42);
        fprintf(stderr,"Source scoped projections/cleanup/reborrow/independent scalar recipe complete Checked/query normal physical0\n");return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--cleanup")) {
        source_comptime_test_answer(source_comptime_cleanup_text,XR_XIR_I64,42);
        fprintf(stderr,"Source lexical cleanup/nested ref loans/independent scalar recipe complete Checked/query normal physical0\n");return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--function")) { source_comptime_test_answer(source_comptime_function_text,XR_XIR_I64,42);return 0; }
    if (argc==2 && !strcmp(argv[1],"--lexical")) {
        source_comptime_resource_text=source_comptime_lexical_text;SourceComptimeTestReport report={0};
        source_comptime_test_trial(&source_comptime_test_limits,SIZE_MAX,false,&report);CHECK(report.status==XR_XIR_OK);
        fprintf(stderr,"Source lexical owner private struct/static class/independent recipe Checked/query normal physical0\n");return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--nominal")) {
        source_comptime_resource_text=source_comptime_nominal_text;SourceComptimeTestReport report={0};
        source_comptime_test_trial(&source_comptime_test_limits,SIZE_MAX,false,&report);CHECK(report.status==XR_XIR_OK);
        fprintf(stderr,"Source nominal independent recipe complete Checked/query producer normal physical0\n");return 0;
    }
    if (argc==2 && (!strcmp(argv[1],"--resources") || !strcmp(argv[1],"--local-resources") || !strcmp(argv[1],"--unit-resources") || !strcmp(argv[1],"--rune-resources") || !strcmp(argv[1],"--cell-resources") || !strcmp(argv[1],"--string-resources") || !strcmp(argv[1],"--string-read-resources") || !strcmp(argv[1],"--tuple-resources") || !strcmp(argv[1],"--float-resources") || !strcmp(argv[1],"--nullable-resources") || !strcmp(argv[1],"--array-resources") || !strcmp(argv[1],"--array-equal-resources") || !strcmp(argv[1],"--array-path-resources") || !strcmp(argv[1],"--text-resources") || !strcmp(argv[1],"--nominal-resources") || !strcmp(argv[1],"--lexical-resources") || !strcmp(argv[1],"--function-resources") || !strcmp(argv[1],"--cleanup-resources") || !strcmp(argv[1],"--projection-resources") || !strcmp(argv[1],"--error-resources") || !strcmp(argv[1],"--panic-resources") || !strcmp(argv[1],"--class-resources") || !strcmp(argv[1],"--output-resources") || !strcmp(argv[1],"--dynamic-loan-resources") || !strcmp(argv[1],"--inactive-loan-resources"))) {
        if (!strcmp(argv[1],"--local-resources")) {
            source_comptime_resource_text=source_comptime_local_text;
            fprintf(stderr,"Source comptime scalar locals whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--unit-resources")) {
            source_comptime_resource_text=source_comptime_unit_text;
            fprintf(stderr,"Source comptime Unit/ordinary scalar result whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--rune-resources")) {
            source_comptime_resource_text=source_comptime_rune_text;
            fprintf(stderr,"Source comptime Rune literal/conversion/equality whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--cell-resources")) {
            source_comptime_resource_text=source_comptime_cell_text;
            fprintf(stderr,"Source comptime private Cell/Unit assignment/loop whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--string-resources")) {
            source_comptime_resource_text=source_comptime_string_text;
            fprintf(stderr,"Source comptime owned String export/parameters/independent replay whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--string-read-resources")) {
            source_comptime_resource_text=source_comptime_string_read_text;
            fprintf(stderr,"Source comptime String comparison/search whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--tuple-resources")) {
            source_comptime_resource_text=source_comptime_tuple_text;
            fprintf(stderr,"Source comptime Tuple/Unit/nested field/Cell independent recipe whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--float-resources")) {
            source_comptime_resource_text=source_comptime_float_text;
            fprintf(stderr,"Source comptime binary Float/Cell/NaN/independent CONST_FLOAT whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--nullable-resources")) {
            source_comptime_resource_text=source_comptime_nullable_text;
            fprintf(stderr,"Source comptime Nullable/Cell/Tuple/unwrap/independent active recipe whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--array-resources")) {
            source_comptime_resource_text=source_comptime_array_text;
            fprintf(stderr,"Source comptime Array/Cell/COW/index/push/owning recipe whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--array-equal-resources")) {
            source_comptime_resource_text=source_comptime_array_equal_text;
            fprintf(stderr,"Source comptime Array/Nullable equality/String/nested NaN whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--array-path-resources")) {
            source_comptime_resource_text=source_comptime_array_path_text;
            fprintf(stderr,"Source comptime private Array index paths/COW/String/range/reserve whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--text-resources")) {
            source_comptime_resource_text=source_comptime_text_text;
            fprintf(stderr,"Source comptime common scalar text/integer IEEE bool Rune/independent String recipe whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--nominal-resources")) {
            source_comptime_resource_text=source_comptime_nominal_text;
            fprintf(stderr,"Source comptime Struct/Enum/field COW/independent nominal recipe whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--lexical-resources")) {
            source_comptime_resource_text=source_comptime_lexical_text;
            fprintf(stderr,"Source lexical declaration owner/private owned recipe/class static whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--function-resources")) {
            source_comptime_resource_text=source_comptime_function_text;
            fprintf(stderr,"Source internal Function values/owned capture/indirect call/cross-frame/independent scalar recipe whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--cleanup-resources")) {
            source_comptime_resource_text=source_comptime_cleanup_text;
            fprintf(stderr,"Source lexical cleanup/nested ref loans/independent scalar recipe whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--projection-resources")) {
            source_comptime_resource_text=source_comptime_projection_text;
            fprintf(stderr,"Source scoped projections/cleanup/reborrow/independent scalar recipe whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--error-resources")) {
            source_comptime_resource_text=source_comptime_error_text;
            fprintf(stderr,"Source typed Error/invoke/projected-ref cleanup whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--output-resources")) {
            source_comptime_resource_text=source_comptime_output_text;
            fprintf(stderr,"Source original Fibonacci10/pair16 policy2 whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--class-resources")) {
            source_comptime_resource_text=source_comptime_class_text;
            fprintf(stderr,"Source private Class/projected-ref cleanup whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--panic-resources")) {
            source_comptime_resource_text=source_comptime_panic_text;
            fprintf(stderr,"Source panic/PanicInfo/assertion message/projected-ref cleanup whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--dynamic-loan-resources")) {
            source_comptime_resource_text=source_comptime_dynamic_loan_text;
            fprintf(stderr,"Source growing Class data loan whole-producer resources\n");
        }
        if (!strcmp(argv[1],"--inactive-loan-resources")) {
            source_comptime_resource_text=source_comptime_inactive_loan_text;
            fprintf(stderr,"Source inactive Class sibling/data loan whole-producer resources\n");
        }
        source_comptime_test_resources();return 0;
    }
    CHECK(argc==1);source_comptime_test_semantics();source_comptime_test_rejections();
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Source comptime genuine scalar/direct closure, two constructions and producer death passed\n");return 0;
}
