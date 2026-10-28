/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * Owned diagnostic roots and source projections, without executable authority. */
#include "xhir_diagnostics.h"
#include "../shared/xr_utf8_core.h"
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>

typedef union ReportMemory {
    struct {union ReportMemory *next;} link;
    union {long double floating;uint64_t integer;void *pointer;} alignment;
} ReportMemory;
typedef struct ReportSource {char *path,*text;size_t length;uint8_t revision[32];bool known;} ReportSource;
typedef struct ReportRoot {XrFrontendRoot view;XrFrontendLabel *labels;} ReportRoot;
struct XrSourceDiagnostic {
    XrCompileResources *resources;
    ReportMemory *memory;
    ReportSource *sources;uint32_t source_count,source_capacity;
    ReportRoot *roots;uint32_t count,capacity;
    enum {REPORT_BUILDING,REPORT_INCOMPLETE,REPORT_COMPLETE} state;
};
static void *report_alloc(XrFrontendReport *r,size_t count,size_t width,XrCompileResourceStatus *status) {
    if(*status!=XR_COMPILE_RESOURCE_OK || !count)return NULL;
    if(!width || count>(SIZE_MAX-sizeof(ReportMemory))/width){*status=XR_COMPILE_RESOURCE_OVERFLOW;return NULL;}
    ReportMemory *m=NULL;*status=xr_compile_resources_calloc(r->resources,1,sizeof(*m)+count*width,(void **)&m);
    if(*status!=XR_COMPILE_RESOURCE_OK)return NULL;
    m->link.next=r->memory;r->memory=m;return m+1;
}
static char *report_text(XrFrontendReport *r,const char *s,size_t n,XrCompileResourceStatus *status) {
    if(!s){*status=XR_COMPILE_RESOURCE_BAD_ARGUMENT;return NULL;}
    if(n==SIZE_MAX){*status=XR_COMPILE_RESOURCE_OVERFLOW;return NULL;}
    char *copy=report_alloc(r,n+1,1,status);if(copy)memcpy(copy,s,n);return copy;
}
static bool report_grow(XrFrontendReport *r,void **data,uint32_t *capacity,uint32_t count,
    size_t width,XrCompileResourceStatus *status) {
    if(count<*capacity)return true;
    if(count==UINT32_MAX){*status=XR_COMPILE_RESOURCE_OVERFLOW;return false;}
    uint32_t next=*capacity?(*capacity>UINT32_MAX/2?UINT32_MAX:*capacity*2):8;
    void *fresh=report_alloc(r,next,width,status);if(!fresh)return false;
    if(count)memcpy(fresh,*data,(size_t)count*width);
    *data=fresh;*capacity=next;return true;
}
XrCompileResourceStatus xr_frontend_report_new(XrCompileResources *resources,XrFrontendReport **out) {
    if(!resources || !out || *out)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrFrontendReport *r=NULL;XrCompileResourceStatus s=xr_compile_resources_calloc(resources,1,sizeof(*r),(void **)&r);
    if(s==XR_COMPILE_RESOURCE_OK){r->resources=resources;*out=r;}return s;
}
void xr_frontend_report_free(XrFrontendReport *r) {
    if(!r)return;
    while(r->memory){ReportMemory *m=r->memory;r->memory=m->link.next;xr_compile_resources_free(m);}
    xr_compile_resources_free(r);
}
XrCompileResourceStatus xr_frontend_report_source(XrFrontendReport *r,const char *path,
    const char *text,size_t length,const uint8_t *revision,uint32_t *out) {
    if(!r || r->state!=REPORT_BUILDING || !out || (!text&&length))return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrCompileResourceStatus s=XR_COMPILE_RESOURCE_OK;
    if(!report_grow(r,(void **)&r->sources,&r->source_capacity,r->source_count,sizeof(*r->sources),&s))return s;
    ReportSource source={0};if(path)source.path=report_text(r,path,strlen(path),&s);
    if(text){source.text=report_text(r,text,length,&s);source.length=length;}
    if(revision){source.known=true;memcpy(source.revision,revision,32);}
    if(s!=XR_COMPILE_RESOURCE_OK)return s;
    r->sources[r->source_count]=source;*out=r->source_count++;return s;
}
XrFrontendLocation xr_frontend_report_location(const XrFrontendReport *r,uint32_t source,
    int line,int column,const char *name) {
    XrFrontendLocation v={0};if(!r || source>=r->source_count)return v;
    const ReportSource *s=&r->sources[source];v.path=s->path;
    if(s->known){v.flags|=XR_FRONTEND_LOCATION_REVISION;memcpy(v.revision,s->revision,32);}
    if(line<=0 || column<=0)return v;
    v.line=line;v.column=column;v.end_line=line;v.end_column=column;v.flags|=XR_FRONTEND_LOCATION_LINE;
    if(!s->text)return v;
    size_t at=0;int row=1;while(at<s->length&&row<line)if(s->text[at++]=='\n')++row;
    if(row!=line || (size_t)(column-1)>s->length-at)return v;
    size_t line_end=at;while(line_end<s->length&&s->text[line_end]!='\r'&&s->text[line_end]!='\n')++line_end;
    if((size_t)(column-1)>line_end-at)return v;
    at+=(size_t)(column-1);size_t n=name?strlen(name):0;
    if(n>line_end-at || (n&&memcmp(s->text+at,name,n)))return v;
    size_t end=at+n;uint32_t urow=0,units=0,start_row=0,start_units=0;
    for(size_t p=0;p<end;) {
        if(p==at){start_row=urow;start_units=units;}
        XrUtf8Step step=xr_utf8_core_decode_step((const uint8_t *)s->text+p,s->length-p);
        if(step.error!=XR_UTF8_OK || !step.scalar || step.consumed>end-p)return v;
        if(step.scalar=='\r') {if(p+1<s->length&&s->text[p+1]=='\n')++step.consumed;
            if(step.consumed>end-p||urow==UINT32_MAX)return v;++urow;units=0;}
        else if(step.scalar=='\n'){if(urow==UINT32_MAX)return v;++urow;units=0;}
        else {uint32_t width=step.scalar>UINT32_C(0xffff)?2u:1u;if(units>UINT32_MAX-width)return v;units+=width;}
        p+=step.consumed;
    }
    if(at==end){start_row=urow;start_units=units;}
    v.start_byte=at;v.end_byte=end;
    if(name){v.flags|=XR_FRONTEND_LOCATION_BYTES;if(n<=(size_t)(INT_MAX-column))v.end_column=column+(int)n;}
    v.utf16_line=start_row;v.utf16_column=start_units;v.utf16_end_line=urow;v.utf16_end_column=units;
    v.flags|=XR_FRONTEND_LOCATION_UTF16;return v;
}
XrCompileResourceStatus xr_frontend_report_add(XrFrontendReport *r,uint32_t code,const char *message,
    XrFrontendLocation primary,const XrFrontendLabel *labels,uint32_t count) {
    if(!r || r->state!=REPORT_BUILDING || !message || (count&&!labels))return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    XrCompileResourceStatus s=XR_COMPILE_RESOURCE_OK;
    if(!report_grow(r,(void **)&r->roots,&r->capacity,r->count,sizeof(*r->roots),&s))return s;
    ReportRoot root={0};root.view=(XrFrontendRoot){code,1,NULL,primary,count};
    root.view.message=report_text(r,message,strlen(message),&s);
    root.labels=report_alloc(r,count,sizeof(*root.labels),&s);
    for(uint32_t i=0;i<count&&s==XR_COMPILE_RESOURCE_OK;++i){root.labels[i]=labels[i];root.labels[i].message=report_text(r,labels[i].message,strlen(labels[i].message),&s);}
    if(s==XR_COMPILE_RESOURCE_OK)r->roots[r->count++]=root;return s;
}
void xr_frontend_report_seal(XrFrontendReport *r,bool complete) {if(r&&r->state==REPORT_BUILDING)r->state=complete?REPORT_COMPLETE:REPORT_INCOMPLETE;}
bool xr_frontend_report_info(const XrFrontendReport *r,XrFrontendReportInfo *out) {
    if(!r||!out||r->state==REPORT_BUILDING)return false;*out=(XrFrontendReportInfo){r->count,r->state==REPORT_COMPLETE};return true;
}
bool xr_frontend_report_root(const XrFrontendReport *r,uint32_t at,XrFrontendRoot *out) {
    if(!r||!out||r->state==REPORT_BUILDING||at>=r->count)return false;*out=r->roots[at].view;return true;
}
bool xr_frontend_report_label(const XrFrontendReport *r,uint32_t root,uint32_t at,XrFrontendLabel *out) {
    if(!r||!out||r->state==REPORT_BUILDING||root>=r->count||at>=r->roots[root].view.labels)return false;
    *out=r->roots[root].labels[at];return true;
}

typedef struct ReportRender {char *text;size_t used,capacity;bool valid;} ReportRender;
static void report_render_append(ReportRender *r,const char *format,...) XR_PRINTF_FMT(2,3);
static void report_render_append(ReportRender *r,const char *format,...) {
    if(!r->valid)return;
    va_list args;va_start(args,format);
    int n=vsnprintf(r->text?r->text+r->used:NULL,r->text?r->capacity-r->used:0,format,args);
    va_end(args);
    if(n<0 || (size_t)n>SIZE_MAX-r->used-1 ||
        (r->text&&(size_t)n>=r->capacity-r->used)){r->valid=false;return;}
    r->used+=(size_t)n;
}
static void report_render_location(ReportRender *r,XrFrontendLocation at,const char *kind,const char *message) {
    if(at.path&&*at.path&&(at.flags&XR_FRONTEND_LOCATION_LINE))
        report_render_append(r,"%s:%d:%d: %s: %s\n",at.path,at.line,at.column,kind,message);
    else if(at.flags&XR_FRONTEND_LOCATION_LINE)
        report_render_append(r,"line %d:%d: %s: %s\n",at.line,at.column,kind,message);
    else report_render_append(r,"%s: %s\n",kind,message);
}
static void report_render_report(ReportRender *r,const XrFrontendReport *report) {
    XrFrontendReportInfo info={0};
    if(xr_frontend_report_info(report,&info)&&info.roots) {
        for(uint32_t i=0;i<info.roots;++i) {
            XrFrontendRoot root={0};
            if(!xr_frontend_report_root(report,i,&root)){r->valid=false;return;}
            report_render_location(r,root.primary,"error",root.message);
            for(uint32_t j=0;j<root.labels;++j) {
                XrFrontendLabel label={0};
                if(!xr_frontend_report_label(report,i,j,&label)){r->valid=false;return;}
                report_render_location(r,label.location,"note",label.message);
            }
        }
    }
}
XrCompileResourceStatus xr_frontend_report_render(const XrFrontendReport *report,char **output) {
    XrFrontendReportInfo info={0};
    if(!output||*output||!xr_frontend_report_info(report,&info))return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    ReportRender size={.valid=true};report_render_report(&size,report);
    if(!size.valid)return XR_COMPILE_RESOURCE_OVERFLOW;
    char *text=NULL;XrCompileResourceStatus status=xr_compile_resources_alloc(report->resources,size.used+1,(void **)&text);
    if(status!=XR_COMPILE_RESOURCE_OK)return status;
    text[0]=0;
    ReportRender actual={text,0,size.used+1,true};report_render_report(&actual,report);
    if(!actual.valid||actual.used!=size.used){xr_compile_resources_free(text);return XR_COMPILE_RESOURCE_BAD_ARGUMENT;}
    *output=text;return XR_COMPILE_RESOURCE_OK;
}
