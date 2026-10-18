/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_framing.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}} while(0)
typedef struct Case {const char *wire;bool valid;int length;} Case;
static const Case cases[]={
    {"Content-Length: 2\r\n\r\n{}",true,2},
    {"content-length:2\r\n\r\n{}",true,2},
    {"CONTENT-LENGTH:\t0002 \t\r\n\r\n{}tail",true,2},
    {"Content-Length: 0\r\n\r\n",true,0},
    {"Content-Type: application/vscode-jsonrpc; charset=utf-8\r\nContent-Length: 2\r\n\r\n{}",true,2},
    {"Content-Length:2\r\nContent-Type:application/json; CHARSET=\"UTF-8\"; x=\"a;\\\"b\"\r\n\r\n{}",true,2},
    {"Content-Length:2\r\nContent-Type:application/json; charset=\"u\\tf-8\"\r\n\r\n{}",true,2},
    {"X-Unknown: a:b; x\tvalue\r\nContent-Length:2\r\n\r\n{}",true,2},
    {"Content-Length:2\r\nContent-Type:application/json\r\n\r\n{}",true,2},
    {"X-Content-Length:2\r\n\r\n{}",false,0},
    {"Content-Length:2junk\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nContent-Length:3\r\n\r\n{}",false,0},
    {"Content-Length:2\r\ncontent-length:2\r\n\r\n{}",false,0},
    {"Content-Length:+2\r\n\r\n{}",false,0},
    {"Content-Length:-1\r\n\r\n",false,0},
    {"Content-Length:\r\n\r\n",false,0},
    {"Content-Length:67108865\r\n\r\n",false,0},
    {"Content-Length:184467440737095516160000\r\n\r\n",false,0},
    {"Content-Length:1.0\r\n\r\nx",false,0},
    {" Content-Length:2\r\n\r\n{}",false,0},
    {"Content-Length :2\r\n\r\n{}",false,0},
    {"Content-Length:2\n\n{}",false,0},
    {"Content-Length:2\rX\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nX:bad\001value\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nX:λ\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nContent-Type:application/json; charset=utf-16\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nContent-Type:application/json; charset=utf8\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nContent-Type:application/json; charset=utf-8; CHARSET=utf-8\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nContent-Type:application/json; charset=\"utf-8\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nContent-Type:application/json; charset=utf-8junk\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nContent-Type:application/json; x\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nContent-Type:application/json; x=\"a\"junk\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nContent-Type:/json\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nContent-Type:application/\r\n\r\n{}",false,0},
    {"Content-Length:2\r\nContent-Type:application/json\r\nContent-Type:application/json\r\n\r\n{}",false,0},
    {"Unknown:2\r\n\r\n{}",false,0}
};
int main(void){
    size_t prefixes=0;
    for(size_t i=0;i<sizeof(cases)/sizeof(*cases);++i){const Case *c=&cases[i];size_t header=77;int length=-77;size_t n=strlen(c->wire);
        CHECK(xlsp_frame_read(c->wire,n,&header,&length)==(c->valid?XLSP_FRAME_OK:XLSP_FRAME_ERROR));
        if(!c->valid){CHECK(header==77&&length==-77);continue;}
        CHECK(length==c->length&&!memcmp(c->wire+header-4,"\r\n\r\n",4));
        for(size_t k=0;k<header+(size_t)length;++k){size_t h=77;int l=-77;CHECK(xlsp_frame_read(c->wire,k,&h,&l)==XLSP_FRAME_PARTIAL);
            if(k<header)CHECK(h==77&&l==-77);else CHECK(h==header&&l==length);++prefixes;}
    }
    char head[64];for(int delta=-1;delta<=1;++delta){size_t n=(size_t)XLSP_FRAME_MAX_BODY+(size_t)(delta+1)-1;int written=xlsp_frame_header(head,sizeof(head),n);
        if(delta>0)CHECK(written==-1);else{CHECK(written>0);size_t h=77;int l=-77;CHECK(xlsp_frame_read(head,(size_t)written,&h,&l)==XLSP_FRAME_PARTIAL&&h==(size_t)written&&l==(int)n);}}
    CHECK(xlsp_frame_header(head,3,2)==-1&&xlsp_frame_header(NULL,0,2)==-1);size_t h=77;int l=-77;
    CHECK(xlsp_frame_read(NULL,0,&h,&l)==XLSP_FRAME_ERROR&&h==77&&l==-77);
    CHECK(xlsp_frame_read("",0,NULL,&l)==XLSP_FRAME_ERROR&&l==-77);
    const char nul[]="Content-Length:2\r\nX:bad\0value\r\n\r\n{}";CHECK(xlsp_frame_read(nul,sizeof(nul)-1,&h,&l)==XLSP_FRAME_ERROR&&h==77&&l==-77);
    printf("Framing cases=%zu exact partial prefixes=%zu known fields/ASCII/charset/decimal limits allocation0\n",sizeof(cases)/sizeof(*cases),prefixes);return 0;
}
