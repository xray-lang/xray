/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_semantic_tokens.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1);}}while(0)
#include "lsp_open_json_owner.h"
_Static_assert(XLSP_TOKEN_COUNT==22&&XLSP_TOKEN_FUNCTION==12&&XLSP_TOKEN_PARAMETER==7&&
    XLSP_TOKEN_MODIFIER==16&&XLSP_MOD_DECLARATION==1&&XLSP_MOD_DEFINITION==2&&
    XLSP_MOD_READONLY==4&&XLSP_MOD_MODIFICATION==64,"Semantic protocol indices must remain unchanged");

static void facts(XrJsonValue *provider) {
    static const char *const names[]={"namespace","type","class","enum","interface","struct",
        "typeParameter","parameter","variable","property","enumMember","event","function",
        "method","macro","keyword","modifier","comment","string","number","regexp","operator"};
    static const char *const modifiers[]={"declaration","definition","readonly","static","deprecated",
        "async","modification","documentation","defaultLibrary"};
    CHECK(provider&&provider->type==XR_JSON_OBJECT&&provider->as.object.count==3);
    XrJsonValue *legend=xjson_get_object(provider,"legend"),*full=xjson_get_object(provider,"full");
    CHECK(legend&&legend->as.object.count==2&&full&&full->as.object.count==1);
    XrJsonValue *types=xjson_get_array(legend,"tokenTypes"),*mods=xjson_get_array(legend,"tokenModifiers");
    CHECK(types&&types->as.array.count==22&&mods&&mods->as.array.count==9&&XLSP_TOKEN_COUNT==22);
    for(int i=0;i<22;++i)CHECK(!strcmp(xjson_array_get(types,i)->as.string,names[i]));
    for(int i=0;i<9;++i)CHECK(!strcmp(xjson_array_get(mods,i)->as.string,modifiers[i]));
    XrJsonValue *delta=xjson_get(full,"delta"),*range=xjson_get(provider,"range");
    CHECK(delta&&delta->type==XR_JSON_BOOL&&delta->as.boolean&&range&&range->type==XR_JSON_BOOL&&range->as.boolean);
}
int main(int argc,char **argv) {
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--normal")));bool full=argc==1;
    lsp_json_calls=0;XrJsonValue *provider=xlsp_semantic_tokens_provider();CHECK(provider);facts(provider);
    size_t sites=lsp_json_calls;CHECK(sites);xjson_free(provider);CHECK(!lsp_json_live&&!lsp_json_bytes);
    if(full)for(size_t fault=0;fault<sites;++fault) {
        lsp_json_calls=0;lsp_json_fail=fault;lsp_json_injected=false;
        provider=xlsp_semantic_tokens_provider();CHECK(!provider&&lsp_json_injected&&lsp_json_calls>fault);
        CHECK(!lsp_json_live&&!lsp_json_bytes);
    }
    lsp_json_fail=SIZE_MAX;lsp_json_injected=false;
    provider=xlsp_semantic_tokens_provider();CHECK(provider);facts(provider);xjson_free(provider);
    CHECK(!lsp_json_live&&!lsp_json_bytes);
    printf("Semantic provider exact22/9 legend full/delta/range JSONFI=%zu full=%u physical=0/0\n",sites,full?1u:0u);
    return 0;
}
