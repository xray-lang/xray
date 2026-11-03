/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * slice_u8_byte_bulk_cases.h - Exact Source loan and callback boundaries
 *
 * KEY CONCEPT:
 *   Complete real bodies distinguish constructor values, empty environments
 *   and named Array copies from unproved aliases or hidden captures.
 */
#ifndef SLICE_U8_BYTE_BULK_CASES_H
#define SLICE_U8_BYTE_BULK_CASES_H

typedef struct SliceByteBulkCase {
    const char *name,*text;
    XrXirStatus status;
    const char *message;
} SliceByteBulkCase;
static const SliceByteBulkCase slice_byte_bulk_cases[]={
    {"fresh","class Writer{out:Array<u8>;init(){this.out=[(1 as u8)]} bump(){this.out.push((2 as u8))}}\nfn work(src:Slice<u8>)->i64{var w=Writer();const bound=w.bump;bound();w.out.push((3 as u8));return len(src)+len(w.out)}\nfn main(){var src=[(9 as u8),(8 as u8)];assert(work(src[:])==5);print(42)}\nmain()\n",XR_XIR_OK,""},
    {"take","class Writer{out:Array<u8>;init(){this.out=[1 as u8]} bump(){this.out.push(2 as u8)} finish()->Array<u8>{return this.out}}\nfn work(src:Slice<u8>)->Array<u8>{var w=Writer();w.bump();assert(len(src)==2);return (move w).finish()}\nvar src:Array<u8>=[3 as u8,4 as u8]\nvar result=work(src[:])\nassert(len(result)==2)\nassert(result[1]==(2 as u8))\nprint(42)\n",XR_XIR_OK,""},
    {"field-copy","class Writer{out:Array<u8>;init(){this.out=[1 as u8]} bump(){this.out.push(2 as u8)} finish()->Array<u8>{return this.out}}\nfn exercise(){\nvar owner=Writer()\nvar input=owner.out\nvar loan:Slice<u8>=input[:]\nowner.bump()\nassert(len(owner.out)==2)\nassert(len(loan)==1)\nassert(loan[0]==(1 as u8))\nprint(42)\n}\nexercise()\n",XR_XIR_OK,""},
    {"scalar-default","const DEFAULT:i64=7\nfn use(v:Slice<u8>,n:i64=DEFAULT)->i64{return len(v)+n}\nfn exercise(){var a:Array<u8>=[1,2];assert(use(a[:])==9);print(42)}\nexercise()\n",XR_XIR_OK,""},
    {"uncaptured-return","fn read(v:Slice<u8>)->i64{return len(v)}\nfn readFunction()->fn(Slice<u8>)->i64{return read}\nfn exercise(){var a:Array<u8>=[1,2];const v:Slice<u8>=a[:];const reader=readFunction();assert(reader(v)==2);print(42)}\nexercise()\n",XR_XIR_OK,""},
    {"factory","class Writer{out:Array<u8>;init(){this.out=[(1 as u8)]} bump(){this.out.push((2 as u8))}}\nconst shared=Writer()\nfn factory()->Writer{return shared}\nfn work(src:Slice<u8>)->i64{var w=factory();w.out.push((3 as u8));return len(src)}\n",XR_XIR_UNRESOLVED,"(ARRAY_PUSH)"},
    {"later-rebind","class Writer{out:Array<u8>;init(){this.out=[(1 as u8)]} bump(){this.out.push((2 as u8))}}\nfn work(src:Slice<u8>,other:Writer)->i64{var w=Writer();var i=0;while(i<2){w.out.push((3 as u8));w=other;i++};return len(src)}\n",XR_XIR_UNRESOLVED,"(ARRAY_PUSH)"},
    {"bound-receiver","class Writer{out:Array<u8>;init(){this.out=[(1 as u8)]} bump(){this.out.push((2 as u8))}}\nfn work(src:Slice<u8>,owner:Writer)->i64{const bound=owner.bump;var loan:Slice<u8>=src[0:len(src)];bound();return len(loan)}\n",XR_XIR_UNRESOLVED,"(CALL_INDIRECT)"},
    {"unknown-capture","fn work(src:Slice<u8>,callback:fn())->i64{var loan:Slice<u8>=src[0:len(src)];callback();return len(loan)}\n",XR_XIR_UNRESOLVED,"(CALL_INDIRECT)"},
    {"projected-field","class Writer{out:Array<u8>;init(){this.out=[1 as u8]} bump(){this.out.push(2 as u8)} finish()->Array<u8>{return this.out}}\nvar owner=Writer()\nvar loan:Slice<u8>=owner.out[:]\nowner.bump()\nprint(loan[0])\n",XR_XIR_BAD_TYPE,"projected view root has not completed Source admission"},
    {"take-read","class Writer{out:Array<u8>;init(){this.out=[1 as u8]} bump(){this.out.push(2 as u8)} finish()->Array<u8>{return this.out}}\nfn work(src:Slice<u8>){var w=Writer();var old=move w;w.bump();assert(len(src)==2)}\nvar src:Array<u8>=[1 as u8,2 as u8]\nwork(src[:])\nprint(99)\n",XR_XIR_BAD_VALUE,"(CELL_READ)"},
    {"take-loop","class Writer{out:Array<u8>;init(){this.out=[1 as u8]} bump(){this.out.push(2 as u8)} finish()->Array<u8>{return this.out}}\nfn work(src:Slice<u8>){var w=Writer();var i=0;while(i<2){var old=move w;assert(len(old.out)==1);i++};assert(len(src)==2)}\nvar src:Array<u8>=[1 as u8,2 as u8]\nwork(src[:])\nprint(99)\n",XR_XIR_BAD_VALUE,"(CELL_TAKE)"},
    {"mutable-default","var DEFAULT:i64=7\nfn use(v:Slice<u8>,n:i64=DEFAULT)->i64{return len(v)+n}\nfn exercise(){var a:Array<u8>=[1,2];assert(use(a[:])==9)}\nexercise()\n",XR_XIR_UNRESOLVED,"(CALL_DEFAULT)"},
    {"managed-default","const DEFAULT:Array<i64>=[7]\nfn use(v:Slice<u8>,n:Array<i64>=DEFAULT)->i64{return len(v)+n[0]}\nfn exercise(){var a:Array<u8>=[1,2];assert(use(a[:])==9)}\nexercise()\n",XR_XIR_UNRESOLVED,"(CALL_DEFAULT)"},
    {"returned-capture","class Writer{out:Array<u8>;init(){this.out=[1 as u8]} bump(){this.out.push(2 as u8)}}\nfn factory(owner:Writer)->fn(){return fn(){owner.bump()}}\nfn exercise(){var owner=Writer();const callback=factory(owner);var input:Array<u8>=[1,2];var loan:Slice<u8>=input[:];callback();assert(len(loan)==2)}\nexercise()\n",XR_XIR_UNRESOLVED,"(CALL_INDIRECT)"},
    {"forwarding","fn noop(){}\nfn forwarding(callback:fn())->fn(){return callback}\nfn exercise(){const callback=forwarding(noop);var input:Array<u8>=[1,2];var loan:Slice<u8>=input[:];callback();assert(len(loan)==2)}\nexercise()\n",XR_XIR_UNRESOLVED,"(CALL_INDIRECT)"}
};
static void slice_byte_bulk_source_controls(const XrXirCompileContext *context) {
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    for(uint32_t i=0;i<sizeof(slice_byte_bulk_cases)/sizeof(slice_byte_bulk_cases[0]);++i) {
        const SliceByteBulkCase *test=&slice_byte_bulk_cases[i];
        XrXirSourceResult source={0};XrXirSourceDiagnostic diagnostic={0};
        XrXirStatus status=source_check(context,test->text,&source,&diagnostic,XR_XIR_PROGRAM);
        if(status!=test->status || (status!=XR_XIR_OK && !strstr(diagnostic.message,test->message)))
            fprintf(stderr,"byte view %s status=%u expected=%u: %s\n",test->name,status,test->status,diagnostic.message);
        CHECK(status==test->status && diagnostic.status==status);
        if(status==XR_XIR_OK)CHECK(source.checked && source.snapshot);
        else CHECK(!source.checked && !source.snapshot && strstr(diagnostic.message,test->message));
        xr_xir_compile_source_result_free(&source);
        CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK &&
            after.held_bytes==before.held_bytes);
    }
}
#endif // SLICE_U8_BYTE_BULK_CASES_H
