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

static void array_reference_mutator_transport(void) {
    XrLirLowered output = lower_text("fn update(values:ref Array<u64>,value:u64){values[0]=value\n"
        "values.push(value)}\nvar values:Array<u64>=[0]\nupdate(ref values,18446744073709551615)\n");
    uint32_t mutators = 0;
    for (uint32_t f = 0; f < xr_lir_function_count(output.module); ++f) {
        XrLirFunction function = {0};
        XrLirFunctionView view = {0};
        OK(xr_lir_function_at(output.module, f, &function));
        OK(xr_lir_function_view(output.module, function, &view));
        if (!view.parameter_count || view.parameter_types[0] != XR_LIR_ARRAY_REF) continue;
        for (uint32_t b = 0; b < view.block_count; ++b) {
            XrLirBlock block = {0};
            XrLirBlockView body = {0};
            OK(xr_lir_block_at(output.module, function, b, &block));
            OK(xr_lir_block_view(output.module, block, &body));
            for (uint32_t i = 0; i < body.instruction_count; ++i) {
                XrLirInstructionView op = {0};
                OK(xr_lir_instruction_view(output.module, block, i, &op));
                CHECK(op.op != XR_LIR_ARRAY_REF_READ && op.op != XR_LIR_ARRAY_REF_WRITE && op.op != XR_LIR_CELL_NEW);
                if (op.op != XR_LIR_CALL_INTRINSIC) continue;
                XrLirArrayIntrinsic shape = {0};
                OK(xr_lir_array_intrinsic_view(output.module, op.intrinsic, &shape));
                CHECK(shape.id && (shape.operation == XR_LIR_ARRAY_OP_SET || shape.operation == XR_LIR_ARRAY_OP_PUSH));
                XrLirType receiver = XR_LIR_TYPE_INVALID;
                OK(xr_lir_value_type(output.module, (XrLirValue){output.module, function.id, op.operands[0]}, &receiver));
                CHECK(receiver == XR_LIR_ARRAY_REF);
                ++mutators;
            }
        }
    }
    CHECK(mutators == 2);
    xr_lir_lowered_free(&output);
}

static void array_sources(void) {
    array_reference_mutator_transport();
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
        "fn fill(n:i64,value:i64)->Array<i64>{return Array<i64>(n,value)}\nprint(len(fill(16,7)))\n",
        "fn fill(n:i64,value:string)->Array<string>{return Array<string>(n,value)}\nprint(len(fill(3,\"seed\")))\n",
        "fn narrow(n:i64)->u8{return n as u8}\nconst bytes:Array<u8>=[narrow(257),255]\n"
        "print(bytes[0] as i64)\n",
        "fn words(value:u64)->Array<u64>{return [value,value]}\n"
        "const values=words(18446744073709551615)\nprint(values[1] as i64)\n",
        "fn append(values:ref Array<u8>,value:u8){values.push(value)}\n"
        "var values:Array<u8>=[1]\nappend(ref values,2)\n"
        "fn local(){var values:Array<u8>=[3]\nappend(ref values,4)}\nlocal()\n",
        "var values:Array<u64>=[18446744073709551615]\n"
        "fn inspect(){values=[9223372036854775808]}\n"
        "fn update(other:ref Array<u64>){other.push(7)\ninspect()\nother.push(9)}\n"
        "update(ref values)\nprint(values[1] as i64)\n",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        XrLirLowered output = lower_text(cases[i]);
        CHECK(xr_lir_intrinsic_count(output.module) == 4 * (XR_LIR_ARRAY_OP_REPEAT + 2));
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
