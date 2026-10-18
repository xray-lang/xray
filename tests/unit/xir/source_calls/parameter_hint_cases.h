/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef PARAMETER_HINT_CASES_H
#define PARAMETER_HINT_CASES_H
typedef struct ParameterHintExpected {const char *label;uint32_t mode,native_identity;XrXirSourceDeclarationKind kind;} ParameterHintExpected;
typedef struct ParameterHintCase {const char *source;ParameterHintExpected hints[3];size_t count;bool unavailable;} ParameterHintCase;
static const ParameterHintCase parameter_cases[]={
    {"fn pair(left:i64,right:i64)->i64{return left+right}\nfn main()->i64{\n const left=1\n return pair(left,2)\n}\n",
        {{"right:",XR_PARAM_READ,0,XR_XIR_SOURCE_FUNCTION}},1,false},
    {"fn increment(slot:ref i64,delta:i64){slot+=delta}\nfn main(){\n var value=1\n increment(ref value,2)\n}\n",
        {{"slot:",XR_PARAM_REF,0,XR_XIR_SOURCE_FUNCTION},{"delta:",XR_PARAM_READ,0,XR_XIR_SOURCE_FUNCTION}},2,false},
    {"fn preserve<Thing>(value:Thing)->Thing{return value}\nfn wrapper<Actual>(item:Actual)->Actual{\n return preserve(item)\n}\nfn main()->i64{return wrapper(1)}\n",
        {{"value:",XR_PARAM_READ,0,XR_XIR_SOURCE_FUNCTION},{"item:",XR_PARAM_READ,0,XR_XIR_SOURCE_FUNCTION}},2,false},
    {"fn main(){\n const values=[1]\n const joined=values.concat([2],[3])\n}\n",
        {{"arrays:",XR_PARAM_READ,14,XR_XIR_SOURCE_MEMBER},{"arrays:",XR_PARAM_READ,14,XR_XIR_SOURCE_MEMBER}},2,false},
    {"fn main(){\n var values=[1]\n const filled=values.fill(2)\n}\n",
        {{"value:",XR_PARAM_READ,28,XR_XIR_SOURCE_MEMBER}},1,false},
    {"fn main(){assertEqual(1,2)}\n",
        {{"a:",XR_PARAM_READ,2,XR_XIR_SOURCE_INTRINSIC},{"b:",XR_PARAM_READ,2,XR_XIR_SOURCE_INTRINSIC}},2,false},
    {"fn main(){assertPanics(fn(){assert(false)})}\n",
        {{"action:",XR_PARAM_READ,4,XR_XIR_SOURCE_INTRINSIC},{"cond:",XR_PARAM_READ,1,XR_XIR_SOURCE_INTRINSIC}},2,false},
    {"struct Box<Element>{const value:Element pick(other:Element)->Element{return other}}\nfn main()->i64{const box=Box<i64>{value:1}\n return box.pick(2)\n}\n",
        {{"other:",XR_PARAM_READ,0,XR_XIR_SOURCE_FUNCTION}},1,false},
    {"fn pick(value:i64,fallback:i64=3)->i64{return value}\nfn main()->i64{return pick(1)}\n",
        {{"value:",XR_PARAM_READ,0,XR_XIR_SOURCE_FUNCTION}},1,false},
    {"fn noop(){}\nfn main(){noop()}\n",{{0}},0,false},
    {"fn main()->i64{const saved=fn(value:i64)->i64{return value}\n return saved(1)\n}\n",{{0}},0,true},
    {"fn take(text:string,count:i64){}\nfn main(){take(\"\xf0\x9f\x98\x80\",2)}\n",
        {{"text:",XR_PARAM_READ,0,XR_XIR_SOURCE_FUNCTION},{"count:",XR_PARAM_READ,0,XR_XIR_SOURCE_FUNCTION}},2,false},
    {"fn main()->i64{const values=[1]\n return values.get(0)\n}\n",
        {{"index:",XR_PARAM_READ,4,XR_XIR_SOURCE_MEMBER}},1,false},
    {"struct Array<T>{const value:T get(index:i64)->T{return this.value}}\nfn main()->i64{const values=Array<i64>{value:1}\n return values.get(0)\n}\n",
        {{"index:",XR_PARAM_READ,0,XR_XIR_SOURCE_FUNCTION}},1,false}
};
#endif
