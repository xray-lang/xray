/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_channel_source_types.c - Governed Source identity and independent descriptor packets
 */
#include "xir_construction_fixture.h"
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_value_internal.h"
#include "shared/xnative_declaration.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_channel_types74_golden.h"
#include "xir_channel_types76_golden.h"
#include "xir_task_unit73_golden.h"
static void channel_wire(void) {
    const XrXirCompileContext *context=effects_source_owner(UINT64_C(67108864),UINT64_C(128000000));
    _Static_assert(XR_XIR_CHECKED_SCHEMA==28 && XR_XIR_CHECKED_CONTRACT==76 && XR_XIR_VALUE_ABI_VERSION==29,
        "Channel descriptors require the current semantic and runtime generation");
    const uint8_t *expected[]={channel_types76_golden,channel_unit76_golden};
    const size_t lengths[]={sizeof(channel_types76_golden),sizeof(channel_unit76_golden)};
    for (unsigned variant=0;variant<2;++variant) {
        XrXirTypeNode nodes[2]={{.kind=XR_XIR_TYPE_CHANNEL,.element=variant ? XR_XIR_UNIT : XR_XIR_I64},
            {.kind=XR_XIR_TYPE_CHANNEL,.element=XR_XIR_STRING}};
        XrXirTypes types={nodes,2,NULL,NULL}; XrXirType parameters[2]={(XrXirType)256,(XrXirType)257};
        XrXirInstruction instruction={.op=XR_XIR_RETURN}; XrXirBlock block={.count=1};
        XrXirFunction function={.name="channelTypes",.name_length=12,.parameters=parameters,.parameter_count=2,
            .blocks=&block,.block_count=1,.instructions=&instruction,.instruction_count=1};
        XrXirModule module={.stage=XR_XIR_BUILT,.functions=&function,.function_count=1,.types=&types};
        XrXirArtifact *checked=NULL,*read=NULL; XrXirCheckedPacket packet={0};
        CHECK(xir_fixture_check(context,&module,&checked,NULL)==XR_XIR_OK);
        CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
        CHECK(packet.length==lengths[variant] && !memcmp(packet.bytes,expected[variant],packet.length));
        CHECK(xr_xir_compile_checked_read(context,expected[variant],lengths[variant],&read,NULL)==XR_XIR_OK);
        memset(nodes,0xcc,sizeof(nodes)); memset(parameters,0xcc,sizeof(parameters));
        CHECK(xr_xir_compile_artifact_verify(read,NULL)==XR_XIR_OK && xr_xir_compile_artifact_verify(checked,NULL)==XR_XIR_OK);
        const XrXirTypes *owned=xr_xir_compile_artifact_module(read)->types;
        CHECK(owned->nodes[0].kind==XR_XIR_TYPE_CHANNEL && owned->nodes[0].element==(variant ? XR_XIR_UNIT : XR_XIR_I64));
        xr_xir_compile_artifact_free(read); xr_xir_compile_artifact_free(checked); xr_xir_compile_checked_packet_free(&packet);
    }
    size_t before=effects_compile_attempts; XrXirArtifact *out=NULL;
    const uint8_t *retired[]={channel_types74_golden,channel_unit74_golden};
    const size_t retired_lengths[]={sizeof(channel_types74_golden),sizeof(channel_unit74_golden)};
    for (unsigned i=0;i<2;++i) {
        CHECK(xr_xir_compile_checked_read(context,retired[i],retired_lengths[i],&out,NULL)==XR_XIR_BAD_STRUCTURE && !out);
        out=(XrXirArtifact *)(uintptr_t)1;
        CHECK(xr_xir_compile_checked_read(context,retired[i],retired_lengths[i],&out,NULL)==XR_XIR_BAD_STRUCTURE &&
            out==(XrXirArtifact *)(uintptr_t)1 && effects_compile_attempts==before);
        out=NULL;
    }
    CHECK(xr_xir_compile_checked_read(context,task_unit73_golden,sizeof(task_unit73_golden),&out,NULL)==XR_XIR_BAD_STRUCTURE && !out);
    out=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(context,task_unit73_golden,sizeof(task_unit73_golden),&out,NULL)==XR_XIR_BAD_STRUCTURE &&
        out==(XrXirArtifact *)(uintptr_t)1 && effects_compile_attempts==before);
}
static void channel_source_case_channels(const char *text, XrXirStatus expected,unsigned channels) {
    const XrXirCompileContext *context=effects_source_owner(UINT64_C(67108864),UINT64_C(128000000));
    XrCompilerSession *session=NULL; CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_MEMORY,"channel-types",NULL};
    XrXirSourceRequest request={session,NULL,&authority,context,XR_CHANNEL_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceText input={NULL,text,strlen(text)}; XrXirSourceResult result={0};
    XrXirSourceDiagnostic diagnostic={0}; char *failure=NULL;
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,&result,&diagnostic,&failure);
    xr_compile_session_free(session);
    bool valid=expected==XR_XIR_OK;
    if (status!=expected) fprintf(stderr,"Channel Source status=%u message=%s input=%s\n",status,diagnostic.message,text);
    CHECK(status==expected && (valid ? result.checked && result.snapshot : !result.checked && !result.snapshot));
    if (valid) {
        const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot); unsigned native=0;
        CHECK(view && view->complete);
        for (uint32_t i=0;i<view->declaration_count;++i) if (view->declarations[i].kind==XR_XIR_SOURCE_TYPE &&
            view->declarations[i].native_identity==XR_NATIVE_DECLARATION_CHANNEL) {
            const XrXirSourceDeclaration *declaration=&view->declarations[i];
            CHECK(!strcmp(declaration->name,"Channel") && declaration->generic_parameter_count==1 &&
                declaration->generic_constraints && declaration->generic_constraints[0].markers==XR_XIR_CONSTRAINT_SENDABLE);
            ++native;
        }
        CHECK(native==channels);
        XrXirCheckedPacket packet={0}; XrXirArtifact *read=NULL;
        CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
        xr_xir_compile_source_result_free(&result);
        CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&read,NULL)==XR_XIR_OK);
        xr_xir_compile_checked_packet_free(&packet); CHECK(xr_xir_compile_artifact_verify(read,NULL)==XR_XIR_OK);
        xr_xir_compile_artifact_free(read);
    }
    xr_xir_compile_source_result_free(&result); xr_compile_resources_free(failure);
}
static void channel_source_case(const char *text,XrXirStatus expected) {
    channel_source_case_channels(text,expected,1);
}
int main(void) {
    const XrNativeTypeDeclaration *channel=xr_native_declaration_by_name("Channel");
    CHECK(channel && channel->id==XR_NATIVE_DECLARATION_CHANNEL && channel->member_count==10 && xr_native_declaration_validate(channel));
    const XrNativeOperation operations[10]={XR_NATIVE_OPERATION_CHANNEL_CAPACITY,XR_NATIVE_OPERATION_CHANNEL_IS_CLOSED,
        XR_NATIVE_OPERATION_CHANNEL_SEND,XR_NATIVE_OPERATION_CHANNEL_RECV,XR_NATIVE_OPERATION_CHANNEL_RECV_OR,XR_NATIVE_OPERATION_CHANNEL_TRY_SEND,
        XR_NATIVE_OPERATION_CHANNEL_TRY_RECV,XR_NATIVE_OPERATION_CHANNEL_SEND_TIMEOUT,XR_NATIVE_OPERATION_CHANNEL_RECV_TIMEOUT,XR_NATIVE_OPERATION_CHANNEL_CLOSE};
    for (uint32_t i=0;i<channel->member_count;++i) CHECK(channel->members[i].lowered==(operations[i]!=XR_NATIVE_OPERATION_NONE) &&
        channel->members[i].operation==operations[i]);
    for (uint32_t i=7;i<=8;++i) CHECK(!strcmp(channel->members[i].failures,
        "allocation,retain,limit,numeric_range,host"));
    _Alignas(8) const unsigned char retired_domain24[8]={0x58,0x52,0x44,0x4d,0x18,0x00,0x00,0x01};
    CHECK(!xr_xir_domain_current((const XrXirDomain *)retired_domain24));
    channel_wire();
    channel_source_case("fn identity(ch: Channel<i64>) -> Channel<i64> { return ch }",XR_XIR_OK);
    channel_source_case("fn identity(ch: Channel<string>) -> Channel<string> { return ch }",XR_XIR_OK);
    channel_source_case("fn name(ch: Channel<i64>) -> string { return typeName(ch) }",XR_XIR_OK);
    channel_source_case("fn identity(ch: Channel<Array<i64>>) -> Channel<Array<i64>> { return ch }",XR_XIR_OK);
    channel_source_case("fn identity(ch: Channel<()>) -> Channel<()> { return ch }",XR_XIR_OK);
    channel_source_case("fn identity<T: Sendable>(ch: Channel<T>) -> Channel<T> { return ch }",XR_XIR_OK);
    channel_source_case("fn result(ch: Channel<i64>, value: Recv<i64>) -> Recv<i64> { return value }",XR_XIR_OK);
    channel_source_case("fn result(ch: Channel<()>, value: Recv<()>) -> Recv<()> { return value }",XR_XIR_OK);
    channel_source_case("fn result(ch: Channel<i64>, value: Recv<string>) -> Recv<string> { return value }",XR_XIR_OK);
    channel_source_case("fn result(ch: Channel<i64>, value: Recv<Array<i64>>) -> Recv<Array<i64>> { return value }",XR_XIR_OK);
    channel_source_case("fn result(ch: Channel<i64>, value: Recv<Channel<i64>>) -> Recv<Channel<i64>> { return value }",XR_XIR_OK);
    channel_source_case("fn result<T: Sendable>(ch: Channel<T>, value: Recv<T>) -> Recv<T> { return value }",XR_XIR_OK);
    channel_source_case("fn result(ch: Channel<i64>, value: SendResult) -> SendResult { return value }",XR_XIR_OK);
    channel_source_case_channels("fn result(value: Recv<i64>) -> Recv<i64> { return value }",XR_XIR_OK,0);
    channel_source_case_channels("fn result(value: Recv<()>) -> Recv<()> { return value }",XR_XIR_OK,0);
    channel_source_case_channels("fn result(value: SendResult) -> SendResult { return value }",XR_XIR_OK,0);
    channel_source_case("fn wrong<T>(ch: Channel<T>) { }",XR_XIR_BAD_TYPE);
    channel_source_case("fn wrong(ch: Channel<i64, string>) { }",XR_XIR_BAD_TYPE);
    channel_source_case("const channel = Channel<i64>(1)",XR_XIR_OK);
    channel_source_case("const channel = Channel<()>(0)",XR_XIR_OK);
    channel_source_case("const channel: Channel<i64> = Channel(1)",XR_XIR_OK);
    channel_source_case("const channel = Channel<i64>()",XR_XIR_OK);
    channel_source_case("const channel: Channel<i64> = Channel()",XR_XIR_OK);
    channel_source_case("const channel = Channel<i64>(1, 2)",XR_XIR_BAD_TYPE);
    channel_source_case("const channel = Channel<i64>(true)",XR_XIR_BAD_TYPE);
    channel_source_case("const channel = Channel(1)",XR_XIR_BAD_TYPE);
    channel_source_case("const channel = Channel<i64>(1)\nconst closed=channel.isClosed\nchannel.close()\nconst capacity=channel.capacity",XR_XIR_OK);
    channel_source_case("const channel = Channel<i64>(1)\nchannel.capacity()",XR_XIR_BAD_TYPE);
    channel_source_case("const channel = Channel<i64>(1)\nchannel.close(1)",XR_XIR_BAD_TYPE);
    channel_source_case("const channel = Channel<i64>(1)\nchannel.send(42)",XR_XIR_OK);
    channel_source_case("fn attempt(channel: Channel<i64>) -> SendResult { return channel.trySend(42) }",XR_XIR_OK);
    channel_source_case("fn attempt(channel: Channel<i64>) -> Recv<i64> { return channel.tryRecv() }",XR_XIR_OK);
    channel_source_case("fn attempt<T: Sendable>(channel: Channel<T>) -> Recv<T> { return channel.tryRecv() }",XR_XIR_OK);
    channel_source_case("fn attempt(channel: Channel<()>) -> Recv<()> { return channel.tryRecv() }",XR_XIR_OK);
    channel_source_case("const channel=Channel<i64>(1)\nchannel.trySend()",XR_XIR_BAD_TYPE);
    channel_source_case("const channel=Channel<i64>(1)\nchannel.trySend(true)",XR_XIR_BAD_TYPE);
    channel_source_case("const channel=Channel<i64>(1)\nchannel.tryRecv(1)",XR_XIR_BAD_TYPE);
    channel_source_case("fn send(channel:Channel<i64>){channel.send(42)}",XR_XIR_OK);
    channel_source_case("fn recv(channel:Channel<i64>)->Recv<i64>{return channel.recv()}",XR_XIR_OK);
    channel_source_case("fn recv<T:Sendable>(channel:Channel<T>)->Recv<T>{return channel.recv()}",XR_XIR_OK);
    channel_source_case("fn recv(channel:Channel<()>)->Recv<()>{return channel.recv()}",XR_XIR_OK);
    channel_source_case("fn recv(channel:Channel<()>){channel.recvOr(Coro.yield())}",XR_XIR_OK);
    channel_source_case("fn recv(channel:Channel<i64>)->i64{return channel.recvOr(7)}",XR_XIR_OK);
    channel_source_case("fn send(channel:Channel<i64>){channel.send()}",XR_XIR_BAD_TYPE);
    channel_source_case("fn send(channel:Channel<i64>){channel.send(true)}",XR_XIR_BAD_TYPE);
    channel_source_case("fn recv(channel:Channel<i64>){channel.recv(7)}",XR_XIR_BAD_TYPE);
    channel_source_case("fn recv(channel:Channel<i64>){channel.recvOr(true)}",XR_XIR_BAD_TYPE);
    channel_source_case("fn recv<T:Sendable>(channel:Channel<T>)->Recv<T>{return channel.recv()}\nfn wrong(channel:Channel<()>){recv<()>(channel)}",XR_XIR_BAD_TYPE);
    channel_source_case("fn send(channel:Channel<i64>)->SendResult{return channel.sendTimeout(42,0)}",XR_XIR_OK);
    channel_source_case("fn recv(channel:Channel<i64>)->Recv<i64>{return channel.recvTimeout(0)}",XR_XIR_OK);
    channel_source_case("fn recv<T:Sendable>(channel:Channel<T>)->Recv<T>{return channel.recvTimeout(0)}",XR_XIR_OK);
    channel_source_case("fn send(channel:Channel<()>)->SendResult{return channel.sendTimeout(Coro.yield(),0)}",XR_XIR_OK);
    channel_source_case("fn recv(channel:Channel<()>)->Recv<()>{return channel.recvTimeout(0)}",XR_XIR_OK);
    channel_source_case("fn wrong(channel:Channel<i64>){channel.sendTimeout(42)}",XR_XIR_BAD_TYPE);
    channel_source_case("fn wrong(channel:Channel<i64>){channel.sendTimeout(true,0)}",XR_XIR_BAD_TYPE);
    channel_source_case("fn wrong(channel:Channel<i64>){channel.sendTimeout(42,true)}",XR_XIR_BAD_TYPE);
    channel_source_case("fn wrong(channel:Channel<i64>){channel.recvTimeout()}",XR_XIR_BAD_TYPE);
    channel_source_case("fn wrong(channel:Channel<i64>){channel.recvTimeout(true)}",XR_XIR_BAD_TYPE);
    effects_source_owners_free(); puts("Channel Source types, identity recipes and independent Checked76 descriptors PASS; blocking type checks and retired74 early rejection");
    return 0;
}
