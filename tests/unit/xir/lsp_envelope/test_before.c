/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "app/lsp/xlsp_protocol_message.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(2);}}while(0)
#include "../lsp_open_json_owner.h"
#include "before_message.c"
int main(void) {
    const char text[]="{\"id\":1,\"id\":2}";XrJsonValue *input=xjson_parse(text,sizeof(text)-1);CHECK(input);
    XlspRequestId id={0};CHECK(xlsp_request_id_parse(input,&id)==XLSP_PROTOCOL_OK&&id.kind==XLSP_ID_NUMBER&&id.as.number==1);
    xlsp_request_id_free(&id);xjson_free(input);CHECK(!lsp_json_live&&!lsp_json_bytes);
    fputs("ENVELOPE_BEFORE duplicate_id_selected=1 JSON0/0\n",stderr);return 1;
}
