/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_lower_array_cases.h - Authentic closed Array lowering and owner retirement
 */
#include "lir/xlir_array.h"
#include "lir/xlir_rebuild.h"

static void array_sources(void) {
    static const char *cases[] = {
        "fn count(xs:Array<i64>)->i64{return len(xs)}\nprint(count([]))\n",
        "fn choose(flag:bool,a:Array<i64>,b:Array<i64>)->Array<i64>{return flag?a:b}\n"
        "const xs=choose(true,[11],[22])\nprint(xs[0])\n",
        "fn local()->i64{var xs=[1,2]\nconst saved=xs\nxs.push(3)\nxs[0]=9\n"
        "return saved[0]*100+xs[0]*10+len(xs)}\nprint(local())\n",
        "var xs=[1,2]\nfn update(n:i64){if(n>0){xs.push(n)\nxs[0]=7}}\n"
        "update(3)\nprint(xs[0])\nprint(len(xs))\n",
        "fn text(xs:Array<string>)->Array<string>{var result=xs\nresult.push(\"tail\")\n"
        "result[0]=\"head\"\nreturn result}\nconst original=[\"old\"]\n"
        "const changed=text(original)\nprint(original[0])\nprint(changed[0])\n",
        "var words:Array<string>=[]\nfn add(n:i64){var i=0\nwhile(i<n){words.push(\"x\")\ni=i+1}}\n"
        "add(3)\nprint(len(words))\n",
        "fn loop(n:i64)->i64{const xs=[7,8]\nvar sum=0\nvar i=0\n"
        "while(i<n){sum=sum+xs[i%2]\ni=i+1}\nreturn sum}\nprint(loop(4))\n",
        "fn loop(n:i64)->i64{var sum=0\nvar i=0\nwhile(i<n){var xs=[i]\n"
        "xs.push(i+1)\nsum=sum+xs[0]+xs[1]\ni=i+1}\nreturn sum}\nprint(loop(3))\n",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrLirLowered output = lower_text(cases[i]);
        CHECK(xr_lir_intrinsic_count(output.module) == 2 * XR_LIR_ARRAY_OP_REPEAT);
        XrLirModule *copy = NULL;
        OK(xr_lir_rebuild(output.module, &copy));
        xr_lir_lowered_free(&output);
        OK(xr_lir_verify(copy, NULL));
        xr_lir_module_destroy(copy);
    }
    XrLirLowered flow = lower_text("fn nested(a:bool,b:bool)->i64{if(a){var xs=[7,8]\n"
        "const saved=xs\nif(b){xs.push(9)}else{xs[0]=2}\nreturn saved[0]*10+xs[0]}\nreturn 0}\nprint(nested(true,false))\n");
    xr_lir_lowered_free(&flow);
    char large[8192];
    size_t used = (size_t)snprintf(large, sizeof(large), "const xs=[");
    for (unsigned i = 0; i < 300; ++i) {
        int count = snprintf(large + used, sizeof(large) - used, "%s%u", i ? "," : "", i);
        CHECK(count > 0 && (size_t)count < sizeof(large) - used);
        used += (size_t)count;
    }
    CHECK(snprintf(large + used, sizeof(large) - used, "]\nprint(xs[299])\n") > 0);
    XrLirLowered output = lower_text(large);
    xr_lir_lowered_free(&output);
    rejected("const xs:Array<bool>=[true]\nprint(len(xs))\n", false, "module slot payload");
    rejected("const xs:Array<Array<i64>>=[[1]]\nprint(len(xs))\n", false, "module slot payload");
}
