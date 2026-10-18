/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#pragma push_macro("main")
#undef main
#define main protocol_existing_main
#include "../lsp_protocol_message/test_protocol_message.c"
#pragma pop_macro("main")
#include "cases.h"
static void envelope_zero(void) {CHECK(!id_live&&!id_bytes&&!lsp_json_live&&!lsp_json_bytes);}
static size_t envelope_cases_run(void) {
    size_t count=sizeof(envelope_cases)/sizeof(envelope_cases[0]);
    for(size_t i=0;i<count;++i) {
        const struct EnvelopeCase *entry=&envelope_cases[i];XrJsonValue *input=parsed(entry->text);XlspProtocolEnvelope out={0};
        size_t before=id_calls,json_before=lsp_json_calls;XlspProtocolStatus status=xlsp_protocol_envelope_decode(input,&out);
        if(status!=entry->status)fprintf(stderr,"ENVELOPE case=%zu status=%u expected=%u %s\n",i,status,entry->status,entry->text);
        CHECK(status==entry->status&&out.kind==entry->kind&&lsp_json_calls==json_before);
        if(status==XLSP_PROTOCOL_OK) {
            if(out.kind==XLSP_ENVELOPE_REQUEST)CHECK(xlsp_request_id_valid(&out.id));
            else CHECK(out.id.kind==XLSP_ID_NONE&&id_calls==before);
            if(out.kind==XLSP_ENVELOPE_RESPONSE)CHECK(!out.method&&out.response_id&&(out.result||out.error));
            else CHECK(out.method==xjson_get_string(input,"method")&&out.params==xjson_get(input,"params"));
        } else CHECK(!out.id.kind&&!out.method&&!out.params&&id_calls==before);
        xlsp_protocol_envelope_clear(&out);xjson_free(input);envelope_zero();
    }
    return count;
}
static void envelope_lifetimes(bool failures) {
    XrJsonValue *input=parsed("{\"jsonrpc\":\"2.0\",\"method\":\"m\",\"id\":\"a\\u0000b\\ud83d\\ude00\",\"params\":{}}");
    XlspProtocolEnvelope occupied={0};occupied.kind=XLSP_ENVELOPE_REQUEST;XlspProtocolEnvelope saved=occupied;size_t before=id_calls;
    CHECK(xlsp_protocol_envelope_decode(input,&occupied)==XLSP_PROTOCOL_BAD_ARGUMENT&&!memcmp(&saved,&occupied,sizeof(saved))&&id_calls==before);
    XlspProtocolEnvelope out={0};
    if(failures){id_fail=id_calls;CHECK(xlsp_protocol_envelope_decode(input,&out)==XLSP_PROTOCOL_OUT_OF_MEMORY&&id_calls==id_fail+1&&!out.kind&&!id_live);id_fail=SIZE_MAX;}
    before=id_calls;CHECK(xlsp_protocol_envelope_decode(input,&out)==XLSP_PROTOCOL_OK&&id_calls==before+1);
    identity_facts(&out.id);xjson_free(input);identity_facts(&out.id);xlsp_protocol_envelope_clear(&out);envelope_zero();
    input=parsed("{\"id\":1,\"id\":2}");XlspRequestId id={0};before=id_calls;
    CHECK(xlsp_request_id_parse(input,&id)==XLSP_PROTOCOL_BAD_ARGUMENT&&!id.kind&&id_calls==before);xjson_free(input);envelope_zero();
}
static size_t envelope_mutations(void) {
    XrJsonValue *input=parsed("{\"jsonrpc\":\"2.0\",\"method\":\"m\",\"id\":1,\"params\":[]}");
    XrJsonValue *params=xjson_get(input,"params");XlspProtocolEnvelope out={0};size_t before=id_calls;
    params->as.array.count=-1;CHECK(xlsp_protocol_envelope_decode(input,&out)==XLSP_PROTOCOL_BAD_ARGUMENT);params->as.array.count=0;
    XrJsonValue nodes[XJSON_MAX_DEPTH]={0};XrJsonValue *children[XJSON_MAX_DEPTH]={0};
    for(size_t i=0;i<XJSON_MAX_DEPTH-1;++i){nodes[i].type=XR_JSON_ARRAY;nodes[i].as.array.count=nodes[i].as.array.capacity=1;
        children[i]=&nodes[i+1];nodes[i].as.array.items=&children[i];}
    nodes[XJSON_MAX_DEPTH-2].type=XR_JSON_NUMBER;nodes[XJSON_MAX_DEPTH-2].is_integer=true;
    XrJsonMember *member=&input->as.object.members[input->as.object.count-1];member->value=nodes;
    CHECK(xlsp_protocol_envelope_decode(input,&out)==XLSP_PROTOCOL_OK);xlsp_protocol_envelope_clear(&out);
    nodes[XJSON_MAX_DEPTH-2].type=XR_JSON_ARRAY;nodes[XJSON_MAX_DEPTH-1].type=XR_JSON_NUMBER;nodes[XJSON_MAX_DEPTH-1].is_integer=true;
    CHECK(xlsp_protocol_envelope_decode(input,&out)==XLSP_PROTOCOL_BAD_ARGUMENT&&!out.kind);
    children[0]=nodes;CHECK(xlsp_protocol_envelope_decode(input,&out)==XLSP_PROTOCOL_BAD_ARGUMENT&&!out.kind);
    member->value=params;CHECK(id_calls==before);xjson_free(input);envelope_zero();
    input=parsed("{\"jsonrpc\":\"2.0\",\"method\":\"m\",\"params\":{\"s\":\"x\"}}");
    XrJsonValue *string=xjson_get(xjson_get(input,"params"),"s");unsigned char invalid=255;memcpy(string->as.string,&invalid,1);
    CHECK(xlsp_protocol_envelope_decode(input,&out)==XLSP_PROTOCOL_INVALID_NOTIFICATION&&!out.kind);xjson_free(input);envelope_zero();
    return 5;
}
int main(int argc,char **argv) {
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--resources")));bool failures=argc==2;
    size_t count=envelope_cases_run();envelope_lifetimes(failures);size_t mutations=envelope_mutations();
    if(failures)CHECK(!protocol_existing_main());envelope_zero();
    printf("Envelope cases=%zu mutations=%zu id_sites=1 full_FI=%u id_owner_dead=1 borrowed_DOM_contract=1 JSON0/0 id0/0\n",count,mutations,failures?1u:0u);return 0;
}
