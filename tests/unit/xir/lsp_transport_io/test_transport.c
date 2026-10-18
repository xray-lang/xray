/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_transport.h"
#include "base/xmalloc.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}} while(0)
static void *native_malloc(size_t n){return xr_malloc(n);}
static void *native_realloc(void *p,size_t n){return xr_realloc(p,n);}
static void native_free(void *p){xr_free(p);}
typedef struct Block {void *p;size_t n;} Block;
static Block blocks[16];
static size_t attempts,fail_at=SIZE_MAX,live,heap_bytes,max_bytes;
static bool injected;
static bool fault(void){if(attempts++!=fail_at)return false;injected=true;return true;}
static size_t slot(void *p){size_t i=0;while(i<16&&blocks[i].p!=p)++i;CHECK(i<16);return i;}
static void record(void *p,size_t n){size_t i=slot(NULL);blocks[i]=(Block){p,n};++live;heap_bytes+=n;if(heap_bytes>max_bytes)max_bytes=heap_bytes;}
static void *heap_malloc(size_t n){if(fault())return NULL;void *p=native_malloc(n);if(p)record(p,n);return p;}
static void *heap_calloc(size_t count,size_t n){CHECK(!count||n<=SIZE_MAX/count);void *p=heap_malloc(count*n);if(p)memset(p,0,count*n);return p;}
static void *heap_realloc(void *p,size_t n){
    if(fault())return NULL;bool had=p!=NULL;size_t i=had?slot(p):0;void *next=native_realloc(p,n);if(!next)return NULL;
    if(!had)record(next,n);else{heap_bytes-=blocks[i].n;blocks[i]=(Block){next,n};heap_bytes+=n;if(heap_bytes>max_bytes)max_bytes=heap_bytes;}return next;
}
static void heap_free(void *p){if(!p)return;size_t i=slot(p);heap_bytes-=blocks[i].n;--live;blocks[i]=(Block){0};native_free(p);}
#undef xr_malloc
#undef xr_calloc
#undef xr_realloc
#undef xr_free
#define xr_malloc heap_malloc
#define xr_calloc heap_calloc
#define xr_realloc heap_realloc
#define xr_free heap_free
/* Compile the real transport so every receiving allocation is faultable. */
#include "app/lsp/xlsp_transport.c"
static void finish(XrLspTransport *t,size_t frames){
    bool connected=xlsp_transport_is_connected(t);xlsp_transport_free(t);CHECK(!live&&!heap_bytes);
    fprintf(stderr,"STATS attempts=%zu injected=%u live=%zu bytes=%zu peak=%zu frames=%zu connected=%u\n",attempts,injected?1u:0u,live,heap_bytes,max_bytes,frames,connected?1u:0u);
}
int main(int argc,char **argv){
    CHECK(argc>=2);if(argc>2)fail_at=(size_t)strtoull(argv[2],NULL,10);
    XrLspTransport *t=xlsp_transport_stdio();if(!t){CHECK(injected);finish(NULL,0);return 0;}
    CHECK(xlsp_transport_get_fd(t)>=0);size_t frames=0;
    if(!strcmp(argv[1],"emit")||!strcmp(argv[1],"broken")){
        bool broken=!strcmp(argv[1],"broken");
        if(broken){fprintf(stderr,"READY\n");fflush(stderr);char ack=0;ssize_t count=0;
            for(unsigned wait=0;!count;++wait){CHECK(wait<1000);count=read_nonblock(t->read_fd,&ack,1);if(!count)xr_time_sleep_ms(1);}CHECK(count==1&&ack=='x');}
        const char body[]="{\"text\":\"λ😀\"}\r\n\032";xlsp_transport_write(t,body,sizeof(body)-1);CHECK(t->connected!=broken);
    }else if(!strcmp(argv[1],"idle")){
        size_t n=99;bool blocked=false;char *body=xlsp_transport_try_read(t,&n,&blocked);CHECK(!body&&blocked&&t->connected&&!n);
    }else if(!strcmp(argv[1],"invalid")){
        xlsp_transport_write(t,NULL,1);CHECK(!t->connected);size_t n=99;bool blocked=true;CHECK(!xlsp_transport_try_read(t,&n,&blocked)&&!n&&!blocked);
    }else if(!strcmp(argv[1],"oversize")){
        xlsp_transport_write(t,"{}",(size_t)XLSP_FRAME_MAX_BODY+1);CHECK(!t->connected);
    }else if(!strcmp(argv[1],"buffered")){
        const char wire[]="Content-Length: 2\r\n\r\n{}Content-Length: 2\r\n\r\n[]";
        memcpy(t->read_buf,wire,sizeof(wire)-1);t->read_len=sizeof(wire)-1;
        for(unsigned i=0;i<2;++i){size_t n=0;bool blocked=true;char *body=xlsp_transport_try_read(t,&n,&blocked);
            if(!body){CHECK(injected&&!t->connected&&!blocked);break;}
            CHECK(n==2&&!blocked&&t->connected&&!memcmp(body,i?"[]":"{}",2));++frames;heap_free(body);}
    }else if(!strcmp(argv[1],"probe")){
        for(unsigned i=0;i<1000;++i){size_t n=0;bool blocked=false;char *body=xlsp_transport_try_read(t,&n,&blocked);CHECK(!body&&!n);
            if(!t->connected)break;
            if(t->pending_content_length>=0){CHECK(blocked&&t->pending_content_length>=XLSP_FRAME_MAX_BODY-1);break;}
            CHECK(blocked);xr_time_sleep_ms(1);}
        if(!injected&&t->connected)CHECK(t->pending_content_length>=XLSP_FRAME_MAX_BODY-1);
    }else if(!strcmp(argv[1],"fragment")){
        CHECK(argc==4);size_t length=(size_t)strtoull(argv[3],NULL,10);
        for(size_t i=0;i<length&&t->connected;++i){fprintf(stderr,"READY\n");fflush(stderr);
            for(unsigned wait=0;;++wait){size_t before=t->read_len,n=0;bool blocked=false;char *body=xlsp_transport_try_read(t,&n,&blocked);
                if(body){CHECK(!blocked);xlsp_transport_write(t,body,n);heap_free(body);++frames;break;}
                if(!t->connected)break;
                CHECK(blocked&&wait<1000);if(t->read_len!=before)break;xr_time_sleep_ms(1);}
            if(t->connected){fprintf(stderr,"STEP\n");fflush(stderr);}}
    }else if(!strcmp(argv[1],"relay")){
        for(unsigned wait=0;;++wait){size_t n=0;bool blocked=false;char *body=xlsp_transport_try_read(t,&n,&blocked);
            if(body){CHECK(!blocked&&t->connected);xlsp_transport_write(t,body,n);heap_free(body);++frames;wait=0;}
            else if(!t->connected){CHECK(!blocked&&!n);break;}
            else{CHECK(blocked&&!n&&wait<25000);xr_time_sleep_ms(1);}}
    }else CHECK(false);
    finish(t,frames);return 0;
}
