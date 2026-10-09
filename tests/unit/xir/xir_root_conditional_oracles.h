/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_conditional_oracles.h - Literal conditional witness expectations
 *
 * KEY CONCEPT:
 *   The selected implementation's use of a callback determines whether its
 *   actual bound may remain precise. The callback's address grants no proof.
 */
#ifndef XIR_ROOT_CONDITIONAL_ORACLES_H
#define XIR_ROOT_CONDITIONAL_ORACLES_H
typedef struct RootConditionalOracle {
    const char *name, *text;
    bool root, unresolved;
    int64_t result;
    const char *bytes;
} RootConditionalOracle;
#define ROOT_CONDITIONAL_INTERFACE "interface Apply{apply(callback:fn()->i64)->i64}\n"
#define ROOT_CONDITIONAL_RUNNER \
    "struct Runner implements Apply{apply(callback:fn()->i64)->i64{try{return callback()}catch(error){return 0}}}\n"
#define ROOT_CONDITIONAL_FORWARD \
    "fn forward<T:Apply>(receiver:T,callback:fn()->i64)->i64{try{return receiver.apply(callback)}catch(error){return 0}}\n"
#define ROOT_CONDITIONAL_PURE "fn pure()->i64{return 7}\n"
static const RootConditionalOracle root_conditional_oracles[] = {
    {"witness-pure", ROOT_CONDITIONAL_INTERFACE ROOT_CONDITIONAL_RUNNER
        ROOT_CONDITIONAL_FORWARD ROOT_CONDITIONAL_PURE
        "export fn run()->i64{return forward<Runner>(Runner{},pure)}\nprint(run())\n",
        false,false,7,"7\n"},
    {"witness-root", ROOT_CONDITIONAL_INTERFACE ROOT_CONDITIONAL_RUNNER
        ROOT_CONDITIONAL_FORWARD
        "var state:i64=9\nfn rooted()->i64{return state}\n"
        "export fn run()->i64{return forward<Runner>(Runner{},rooted)}\nprint(run())\n",
        true,false,9,"9\n"},
    {"witness-fixed", ROOT_CONDITIONAL_INTERFACE ROOT_CONDITIONAL_RUNNER
        ROOT_CONDITIONAL_FORWARD ROOT_CONDITIONAL_PURE
        "export fn run()->i64{const fixed:fn()->i64=pure;return forward<Runner>(Runner{},fixed)}\nprint(run())\n",
        false,true,7,"7\n"},
    {"witness-fixed-implementation", ROOT_CONDITIONAL_INTERFACE
        "struct Runner implements Apply{apply(callback:fn()->i64)->i64{const fixed:fn()->i64=callback;try{return fixed()}catch(error){return 0}}}\n"
        ROOT_CONDITIONAL_FORWARD ROOT_CONDITIONAL_PURE
        "export fn run()->i64{return forward<Runner>(Runner{},pure)}\nprint(run())\n",
        false,true,7,"7\n"},
    {"witness-capture", ROOT_CONDITIONAL_INTERFACE
        "struct Runner implements Apply{apply(callback:fn()->i64)->i64{const thunk=fn()->i64{try{return callback()}catch(error){return 0}};try{return thunk()}catch(error){return 0}}}\n"
        ROOT_CONDITIONAL_FORWARD ROOT_CONDITIONAL_PURE
        "export fn run()->i64{return forward<Runner>(Runner{},pure)}\nprint(run())\n",
        false,false,7,"7\n"},
    {"witness-composition", ROOT_CONDITIONAL_INTERFACE ROOT_CONDITIONAL_RUNNER
        ROOT_CONDITIONAL_FORWARD ROOT_CONDITIONAL_PURE
        "fn relay<T:Apply>(receiver:T,callback:fn()->i64)->i64{return forward<T>(receiver,callback)}\n"
        "export fn run()->i64{return relay<Runner>(Runner{},pure)}\nprint(run())\n",
        false,false,7,"7\n"},
    {"witness-recursive", ROOT_CONDITIONAL_INTERFACE ROOT_CONDITIONAL_RUNNER
        ROOT_CONDITIONAL_FORWARD ROOT_CONDITIONAL_PURE
        "fn recur<T:Apply>(receiver:T,callback:fn()->i64,n:i64)->i64{if(n==0){return forward<T>(receiver,callback)};return recur<T>(receiver,callback,n-1)}\n"
        "export fn run()->i64{return recur<Runner>(Runner{},pure,3)}\nprint(run())\n",
        false,false,7,"7\n"},
    {"witness-declaration-order", ROOT_CONDITIONAL_INTERFACE
        "export fn run()->i64{return forward<Runner>(Runner{},pure)}\n"
        ROOT_CONDITIONAL_PURE ROOT_CONDITIONAL_FORWARD ROOT_CONDITIONAL_RUNNER
        "print(run())\n", false,false,7,"7\n"},
    {"cleanup-scalar", ROOT_CONDITIONAL_INTERFACE ROOT_CONDITIONAL_RUNNER
        ROOT_CONDITIONAL_FORWARD ROOT_CONDITIONAL_PURE
        "export fn run()->i64{const number=11;defer{print(number)};return forward<Runner>(Runner{},pure)}\nprint(run())\n",
        false,false,7,"11\n7\n"},
    {"cleanup-callable-carrier", ROOT_CONDITIONAL_INTERFACE ROOT_CONDITIONAL_RUNNER
        ROOT_CONDITIONAL_FORWARD ROOT_CONDITIONAL_PURE
        "export fn run()->i64{const callback=pure;defer{const held=callback};return forward<Runner>(Runner{},pure)}\nprint(run())\n",
        false,false,7,"7\n"},
    {"cleanup-root-slot", ROOT_CONDITIONAL_INTERFACE ROOT_CONDITIONAL_RUNNER
        ROOT_CONDITIONAL_FORWARD ROOT_CONDITIONAL_PURE
        "var state:i64=9\nexport fn run()->i64{defer{print(state)};return forward<Runner>(Runner{},pure)}\nprint(run())\n",
        true,false,7,"9\n7\n"},
    {"cleanup-nested", ROOT_CONDITIONAL_INTERFACE ROOT_CONDITIONAL_RUNNER
        ROOT_CONDITIONAL_FORWARD ROOT_CONDITIONAL_PURE
        "export fn run()->i64{const outer=11;defer{const inner=13;defer{print(inner)};print(outer)};return forward<Runner>(Runner{},pure)}\nprint(run())\n",
        false,false,7,"11\n13\n7\n"}
};
#undef ROOT_CONDITIONAL_INTERFACE
#undef ROOT_CONDITIONAL_RUNNER
#undef ROOT_CONDITIONAL_FORWARD
#undef ROOT_CONDITIONAL_PURE
#endif // XIR_ROOT_CONDITIONAL_ORACLES_H
