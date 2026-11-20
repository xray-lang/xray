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
#include "lir/xlir_ownership.h"

static void array_call_last_use_source(void) {
    static const char text[] =
        "fn bump(input:Array<i64>, delta:i64)->Array<i64> {\n"
        "  var edited=input\n  edited[0]=edited[0]+delta\n  return edited\n}\n"
        "fn arg()->i64 {print(30)\nreturn 2}\n"
        "fn last_use() {var a=[1,2]\na=bump(a,2+1)\nprint(a[0])}\n"
        "fn used_later() {var a=[1,2]\nconst b=bump(a,2)\nprint(a[0])\nprint(b[0])}\n"
        "fn later_argument() {var a=[1,2]\na=bump(a,arg())\nprint(a[0])}\n"
        "last_use()\nused_later()\nlater_argument()\n";
    XrLirLowered output = lower_text(text);
    XrLirOwnershipPlan *plan = NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=output.module}, &plan, NULL));
    uint32_t receivers = 0, calls = 0;
    XrLirArrayCallMove move = {0};
    for (uint32_t f = 1; f <= xr_lir_ownership_function_count(plan); ++f) {
        XrLirOwnershipFunction row;
        OK(xr_lir_ownership_function_at(plan, f, &row));
        receivers += row.array_entry_retain.instruction != 0;
        calls += row.array_call_count;
        if (!row.array_call_count) continue;
        OK(xr_lir_ownership_array_call_move_at(plan, row.array_call_offset, &move));
        XrLirFunction caller = {output.module,f};
        XrLirBlock block;
        XrLirInstructionView call, retain, owner_drop, temporary_drop;
        OK(xr_lir_block_at(output.module, caller, move.block - 1, &block));
        OK(xr_lir_instruction_view(output.module, block, move.instruction - 1, &call));
        OK(xr_lir_instruction_view(output.module, block, move.retain - 1, &retain));
        OK(xr_lir_instruction_view(output.module, block, move.owner_drop - 1, &owner_drop));
        OK(xr_lir_instruction_view(output.module, block, move.temporary_drop - 1, &temporary_drop));
        CHECK(call.op == XR_LIR_CALL && call.callee == move.callee && call.operands[0] == move.temporary &&
            retain.op == XR_LIR_ARRAY_RETAIN && retain.result == move.temporary && retain.operands[0] == move.owner &&
            owner_drop.op == XR_LIR_ARRAY_DROP && owner_drop.operands[0] == move.owner &&
            temporary_drop.op == XR_LIR_ARRAY_DROP && temporary_drop.operands[0] == move.temporary);
        CHECK(move.retain < move.instruction && move.owner_drop > move.instruction && move.temporary_drop > move.instruction);
    }
    CHECK(receivers == 1 && calls == 1);
    xr_lir_lowered_free(&output);
    XrLirArrayCallMove saved;
    OK(xr_lir_ownership_array_call_move_at(plan, 0, &saved));
    CHECK(saved.owner == move.owner && saved.temporary == move.temporary && saved.callee == move.callee);
    CHECK(xr_lir_ownership_array_call_move_at(plan, 1, &saved) == XR_LIR_INVALID_ARGUMENT && saved.owner == move.owner);
    xr_lir_ownership_plan_destroy(plan);
}

static void array_readonly_original_source(void) {
    /* Original complete push-plus-sum workload; only compilation is observed.
     * No loop bound or index is rewritten to manufacture a candidate. */
    static const char text[] = "fn main() {\n"
        "    var a: Array<i64> = []\n    var i = 0\n    while (i < 5000000) {\n"
        "        a.push(i)\n        i = i + 1\n    }\n    var total = 0\n    i = 0\n"
        "    while (i < len(a)) {\n        total = total + a[i]\n        i = i + 1\n    }\n"
        "    print(total)\n}\nmain()\n";
    XrLirLowered output=lower_text(text);
    XrLirOwnershipPlan *plan=NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=output.module},&plan,NULL));
    CHECK(xr_lir_ownership_array_region_count(plan)==1);
    XrLirArraySetRegion region={0}; OK(xr_lir_ownership_array_region_at(plan,0,&region));
    CHECK(region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET &&
        region.range.kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_SAME_LEN &&
        region.owner_home.kind==XR_LIR_ARRAY_SET_OWNER_HOME_READ_ONLY_WINDOW);
    XrLirFunction function={output.module,region.function}; XrLirBlock block={0}; XrLirInstructionView get={0};
    OK(xr_lir_block_at(output.module,function,region.primary_block-1,&block));
    OK(xr_lir_instruction_view(output.module,block,region.primary_instruction-1,&get));
    XrLirArrayIntrinsic shape={0}; OK(xr_lir_array_intrinsic_view(output.module,get.intrinsic,&shape));
    CHECK(get.op==XR_LIR_CALL_INTRINSIC && shape.operation==XR_LIR_ARRAY_OP_GET && get.result && get.operand_count==2);
    XrLirOwnershipFunction owner;
    OK(xr_lir_ownership_function_at(plan,region.function,&owner));
    CHECK(owner.fixed_update_count==1);
    XrLirArraySetRegion push={0}; OK(xr_lir_ownership_fixed_array_update_at(plan,owner.fixed_update_offset,&push));
    CHECK(push.function==region.function && push.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_PUSH &&
        push.push_counted && push.range.kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT && !push.owner_home.kind);
    function=(XrLirFunction){output.module,push.function};
    OK(xr_lir_block_at(output.module,function,push.primary_block-1,&block));
    XrLirInstructionView append={0}; OK(xr_lir_instruction_view(output.module,block,push.primary_instruction-1,&append));
    OK(xr_lir_array_intrinsic_view(output.module,append.intrinsic,&shape));
    CHECK(append.op==XR_LIR_CALL_INTRINSIC && shape.operation==XR_LIR_ARRAY_OP_PUSH && append.operand_count==2 &&
        append.operands[1]);
    OK(xr_lir_ownership_array_regions_verify(output.module,plan));
    xr_lir_ownership_plan_destroy(plan); xr_lir_lowered_free(&output);
}

static void array_readonly_invariant_bound_source(void) {
    /* The ten-million-element workload compares against a formal bound. */
    static const char text[] = "fn push_sum(n: i64) -> i64 {\n"
        "    var values: Array<i64> = []\n"
        "    for (var i: i64 = 0; i < n; i++) {\n"
        "        values.push(i)\n"
        "    }\n"
        "    var sum: i64 = 0\n"
        "    for (var i: i64 = 0; i < n; i++) {\n"
        "        sum += values[i]\n"
        "    }\n"
        "    return sum\n"
        "}\n\n"
        "const n = 10000000\n"
        "print(push_sum(n))\n";
    XrLirLowered output=lower_text(text);
    XrLirOwnershipPlan *plan=NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=output.module},&plan,NULL));
    CHECK(xr_lir_ownership_array_region_count(plan)==1);
    XrLirArraySetRegion region={0}; OK(xr_lir_ownership_array_region_at(plan,0,&region));
    XrLirFunction function={output.module,region.function}; XrLirFunctionView view={0};
    OK(xr_lir_function_view(output.module,function,&view));
    CHECK(region.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_GET &&
        region.range.kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT &&
        view.parameter_count==1 && region.range.length==view.parameters[0] && !region.range.length_instruction &&
        region.owner_home.kind==XR_LIR_ARRAY_SET_OWNER_HOME_READ_ONLY_WINDOW);
    XrLirOwnershipFunction owner;
    OK(xr_lir_ownership_function_at(plan,region.function,&owner));
    CHECK(owner.fixed_update_count==1);
    XrLirArraySetRegion push={0}; OK(xr_lir_ownership_fixed_array_update_at(plan,owner.fixed_update_offset,&push));
    CHECK(push.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_PUSH && push.push_counted && push.range.length==view.parameters[0]);
    OK(xr_lir_ownership_array_regions_verify(output.module,plan));
    xr_lir_ownership_plan_destroy(plan); xr_lir_lowered_free(&output);
}

static void array_push_exit_retained_source(void) {
    static const char text[] =
        "fn show_window(end: i64, start: i64) {\n"
        "    var values: Array<i64> = [9]\n    var i = start\n    while (i < end) {\n"
        "        values.push(i)\n        i = i + 1\n    }\n    print(values)\n}\n\n"
        "fn show_snapshot(end: i64) {\n    var values: Array<i64> = [40, 50]\n"
        "    const snapshot = values\n    var i = 0\n    while (i < end) {\n"
        "        values.push(i)\n        i = i + 1\n    }\n    print(snapshot)\n    print(values)\n}\n\n"
        "show_window(0, 0)\nshow_window(3, 0)\nshow_window(9, -2)\nshow_window(7, 9)\n"
        "show_window(9223372036854775807, 9223372036854775802)\nshow_snapshot(7)\n";
    XrLirLowered output=lower_text(text); XrLirOwnershipPlan *plan=NULL;
    OK(xr_lir_ownership_plan_create(&(XrLirOwnershipRequest){.module=output.module},&plan,NULL));
    XrLirArraySetRegion window={0};
    for (uint32_t f=1; f<=xr_lir_function_count(output.module); ++f) {
        XrLirOwnershipFunction owner;
        OK(xr_lir_ownership_function_at(plan,f,&owner));
        for (uint32_t r=0; r<owner.fixed_update_count; ++r) {
            XrLirArraySetRegion push;
            OK(xr_lir_ownership_fixed_array_update_at(plan,owner.fixed_update_offset+r,&push));
            if (push.primary_kind!=XR_LIR_ARRAY_REGION_PRIMARY_PUSH) continue;
            CHECK(!window.function);
            window=push;
        }
    }
    CHECK(window.function);
    XrLirFunction function={output.module,window.function}; XrLirFunctionView view={0};
    OK(xr_lir_function_view(output.module,function,&view));
    CHECK(view.parameter_count==2 && window.primary_kind==XR_LIR_ARRAY_REGION_PRIMARY_PUSH &&
        window.push_counted && window.range.kind==XR_LIR_ARRAY_SET_RANGE_UNIT_FORWARD_LT_GUARDED_LIMIT &&
        window.range.length==view.parameters[0] && window.range.entry_iv==view.parameters[1] &&
        !window.range.length_instruction);
    XrLirBlock current={0}; XrLirBlockView body={0}; bool retained=false,displayed=false;
    OK(xr_lir_block_at(output.module,function,window.exit_block-1,&current));
    OK(xr_lir_block_view(output.module,current,&body));
    uint32_t bridges=0;
    while (body.instruction_count==1) {
        XrLirInstructionView branch={0}; OK(xr_lir_instruction_view(output.module,current,0,&branch));
        CHECK(branch.op==XR_LIR_BR && branch.edge_count==1 && ++bridges<view.block_count);
        OK(xr_lir_block_at(output.module,function,branch.edges[0].target-1,&current));
        OK(xr_lir_block_view(output.module,current,&body));
    }
    CHECK(bridges && current.id!=window.exit_block);
    for (uint32_t n=0;n<body.instruction_count;++n) {
        XrLirInstructionView op={0}; OK(xr_lir_instruction_view(output.module,current,n,&op));
        retained|=op.op==XR_LIR_ARRAY_RETAIN;
        if (op.op==XR_LIR_CALL_INTRINSIC) {
            XrLirArrayIntrinsic shape={0}; OK(xr_lir_array_intrinsic_view(output.module,op.intrinsic,&shape));
            displayed|=shape.operation==XR_LIR_ARRAY_OP_DISPLAY;
        }
    }
    CHECK(retained && displayed);
    OK(xr_lir_ownership_array_regions_verify(output.module,plan));
    xr_lir_ownership_plan_destroy(plan); xr_lir_lowered_free(&output);
}

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
    array_call_last_use_source();
    array_readonly_original_source();
    array_readonly_invariant_bound_source();
    array_push_exit_retained_source();
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
        if (i + 1 == sizeof(cases) / sizeof(cases[0])) {
            source_fixture_rejected(cases[i],
                "E0382: ref of module binding `values` conflicts with access by `update` via `inspect`");
            continue;
        }
        XrLirLowered output = lower_text(cases[i]);
        CHECK(xr_lir_intrinsic_count(output.module) == 4 * (XR_LIR_ARRAY_OP_DISPLAY + 2));
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
