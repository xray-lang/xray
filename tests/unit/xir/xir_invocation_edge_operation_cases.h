/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_invocation_edge_operation_cases.h - Complete local edge rows and publication
 */
#ifndef XIR_INVOCATION_EDGE_OPERATION_CASES_H
#define XIR_INVOCATION_EDGE_OPERATION_CASES_H

/* The literal Checked fixture supplies all ten physical words. No second
 * reader, saved admission result or construction field produces the oracle. */
static void conditional_compact_edge_operations(const XrXirCompileContext *context,
    EffectInvocationCertificate *certificate) {
    EffectInvocationFinal *equations=certificate->final;
    EffectBodyRequest body=effect_body_owned_request(context,certificate->body);uint32_t seen=0,refusals=0;
    for (uint32_t e=0;e<equations->edge_count;++e) {
        const EffectInvocationEdge *edge=&equations->edges[e];uint32_t caller=equations->nodes[edge->caller].body;
        uint32_t bit=0;XrXirInstruction expected={0};
        if (!caller && edge->instruction==3) {
            bit=1;expected=(XrXirInstruction){.op=XR_XIR_CALL,.type=XR_XIR_I64,.args={0,2},.immediate=1};
        } else if (!caller && edge->instruction==2) {
            bit=2;expected=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=(XrXirType)257,.immediate=2};
        } else if (caller==1 && !edge->instruction) {
            bit=4;expected=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64,.args={0,1}};
        }
        if (!bit || (seen&bit)) continue;seen|=bit;
        EffectInvocationEdgeOperation decoded;decoded.operation=NULL;
        XrXirOp opcode=XR_XIR_RETURN;EffectInvocationEdgeRead selection={edge->caller,e,&opcode};
        ConditionalCompactRead probe={context,0,UINT64_MAX};EffectBodyRequest observed=body;
        observed.read=(EffectBodyRead){&probe,conditional_compact_read_work};
        uint64_t live=rp_stats(context).live_bytes;
        CHECK(effect_invocation_edge_read_selected(context,equations,&observed,&certificate->terms.types,&selection,&decoded)==
            XR_XIR_OK && decoded.operation==&decoded.storage && conditional_compact_same(decoded.operation,&expected) &&
            opcode==expected.op && rp_stats(context).live_bytes==live);
        uint64_t calls=probe.calls;CHECK(calls);
        for (uint64_t reject=0;reject<calls;++reject) {
            decoded.operation=NULL;opcode=XR_XIR_RETURN;probe=(ConditionalCompactRead){context,0,reject};
            CHECK(effect_invocation_edge_read_selected(context,equations,&observed,&certificate->terms.types,&selection,&decoded)==
                XR_XIR_BUDGET && !decoded.operation && opcode==XR_XIR_RETURN && probe.calls==reject+1 &&
                rp_stats(context).live_bytes==live);++refusals;
        }
        decoded.operation=(const XrXirInstruction *)(uintptr_t)1;opcode=XR_XIR_RETURN;
        XrCompileResourceStats before=rp_stats(context);size_t attempts=rp_attempts;
        CHECK(effect_invocation_edge_read_selected(context,equations,&body,&certificate->terms.types,&selection,&decoded)==
            XR_XIR_BAD_STRUCTURE && decoded.operation==(const XrXirInstruction *)(uintptr_t)1 && opcode==XR_XIR_RETURN &&
            rp_stats(context).work==before.work && rp_attempts==attempts);
        decoded.operation=NULL;uint32_t target=equations->edges[e].target;equations->edges[e].target=equations->count;
        CHECK(effect_invocation_edge_read_selected(context,equations,&body,&certificate->terms.types,&selection,&decoded)==
            XR_XIR_BAD_STRUCTURE && !decoded.operation && opcode==XR_XIR_RETURN);equations->edges[e].target=target;
        CHECK(effect_invocation_edge_read_selected(context,equations,&body,&certificate->terms.types,&selection,&decoded)==
            XR_XIR_OK && decoded.operation==&decoded.storage && conditional_compact_same(decoded.operation,&expected) &&
            opcode==expected.op && rp_stats(context).live_bytes==live);
    }
    CHECK(seen==7);printf("Edge operation3 literal complete rows, %u read refusals, occupied/bad edge refusals, atomic outputs, restored positives PASS\n",refusals);
}
#endif // XIR_INVOCATION_EDGE_OPERATION_CASES_H
