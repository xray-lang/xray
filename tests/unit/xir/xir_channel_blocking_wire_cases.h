/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_channel_blocking_wire_cases.h - Independently framed blocking operand roles
 */
#ifndef XIR_CHANNEL_BLOCKING_WIRE_CASES_H
#define XIR_CHANNEL_BLOCKING_WIRE_CASES_H
#include "xir_construction_fixture.h"
#include "xir_channel_blocking76_golden.h"
static void blocking_wire_cases(void) {
    LibraryCompileOwner owner={0};CHECK(library_compile_owner_new(&owner,&library_compile_limits)==XR_XIR_OK);
    const uint8_t *expected[]={blocking_send_i6476_golden,blocking_send_unit76_golden,
        blocking_recv_or_i6476_golden,blocking_recv_or_unit76_golden};
    const size_t lengths[]={sizeof(blocking_send_i6476_golden),sizeof(blocking_send_unit76_golden),
        sizeof(blocking_recv_or_i6476_golden),sizeof(blocking_recv_or_unit76_golden)};
    _Static_assert(XR_XIR_CHANNEL_SEND==159 && XR_XIR_CHANNEL_RECV==160 && XR_XIR_CHANNEL_RECV_OR==161,
        "blocking operations use their independently specified wire ordinals");
    for (unsigned variant=0;variant<4;++variant) {
        bool unit=(variant&1)!=0,send=variant<2;
        XrXirType element=unit ? XR_XIR_UNIT : XR_XIR_I64,result=send ? XR_XIR_I64 : element;
        XrXirTypeNode node={.kind=XR_XIR_TYPE_CHANNEL,.element=element};XrXirTypes types={&node,1,NULL,NULL};
        XrXirType parameters[]={(XrXirType)256,XR_XIR_I64};uint32_t count=unit ? 1u : 2u,operands[]={0,1};
        XrXirInstruction instructions[]={{.op=send ? XR_XIR_CHANNEL_SEND : XR_XIR_CHANNEL_RECV_OR,.type=result,.args={0,count}},
            {.op=XR_XIR_RETURN,.args={result==XR_XIR_UNIT ? 0u : count,0}}};
        XrXirBlock block={.count=2};XrXirFunction function={.name="blockingWire",.name_length=12,.parameters=parameters,
            .parameter_count=count,.result=result,.blocks=&block,.block_count=1,.instructions=instructions,.instruction_count=2,
            .operands=operands,.operand_count=count};
        XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.types=&types};
        XrXirArtifact *checked=NULL,*read=NULL;XrXirCheckedPacket packet={0};
        CHECK(xir_fixture_check(&owner.context,&module,&checked,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
        CHECK(packet.length==lengths[variant] && !memcmp(packet.bytes,expected[variant],packet.length));
        CHECK(xr_xir_compile_checked_read(&owner.context,expected[variant],lengths[variant],&read,NULL)==XR_XIR_OK);
        memset(&node,0xcc,sizeof(node));memset(parameters,0xcc,sizeof(parameters));memset(operands,0xcc,sizeof(operands));
        CHECK(xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK && xr_xir_compile_artifact_verify(read,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(read);xr_xir_compile_artifact_free(checked);xr_xir_compile_checked_packet_free(&packet);
    }
    library_compile_owner_drop(&owner);puts("Blocking Checked76 complete literal send/recvOr i64/Unit roles: independent writer/read and deep ownership PASS");
}
#endif // XIR_CHANNEL_BLOCKING_WIRE_CASES_H
