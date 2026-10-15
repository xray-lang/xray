/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_types.c - Canonical type identities and declaration bounds survive producers
 */
#include "xir/xxir_source_type_facts.h"
#include "api/xsource_authority.h"
#include "api/xsource_paths.h"
#include "xir/xxir_stdlib_internal.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_interface.h"
#include "xir/xxir_implementation.h"
#include "xir/xxir_nominal.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#define xr_compile_resources_new type_resource_new_impl
#include "xir_sdk_resource_test.h"
#undef xr_compile_resources_new
static size_t type_ledgers;
XR_FUNC XrCompileResourceStatus xr_compile_resources_new(const XrCompileResourceLimits *bounds,XrCompileResources **out) {
    CHECK(bounds&&bounds->allocated_bytes<=UINT64_C(64)*1024*1024&&bounds->live_bytes<=UINT64_C(8)*1024*1024&&bounds->work<=UINT64_C(128000000));
    ++type_ledgers;return type_resource_new_impl(bounds,out);
}
static const char *const texts[]={
    "final class C {value:i64 constructor(v:i64){this.value=v}}\nstruct S {value:i64}\nenum E {A,B}\n",
    "interface Base {measure()->i64}\ninterface Left extends Base {}\ninterface Right extends Base {}\ninterface Both extends Left,Right {}\nstruct Meter implements Both {value:i64;measure()->i64{return this.value}}\n",
    "interface Reader<T:Sendable> {read()->T}\nstruct Box<T:Sendable> implements Reader<T> {value:T;read()->T{return this.value}}\n",
    "import {Shape as Parent} from \"./lib\"\ninterface Local extends Parent {}\nstruct Item implements Local {value:i64;size()->i64{return this.value}}\n",
    "class Base {}\nclass Child extends Base {}\n"
};
#include "type_program.h"
static uint32_t declaration(const XrXirSourceView *view,const char *name) {
    for(uint32_t i=0;i<view->declaration_count;++i)if(view->declarations[i].kind==XR_XIR_SOURCE_TYPE&&!strcmp(view->declarations[i].name,name))return i;
    CHECK(false);return UINT32_MAX;
}
static void inspect(const XrXirSourceView *view,const XrXirSourceTypeFacts *facts,unsigned mode) {
    CHECK(view&&view->complete&&facts&&facts->declaration_count==view->declaration_count);
    unsigned count=0;for(uint32_t i=0;i<facts->declaration_count;++i){const XrXirSourceTypeFact *f=&facts->declarations[i];
        if(f->kind==XR_XIR_SOURCE_TYPE_NONE)continue;CHECK(view->declarations[i].kind==XR_XIR_SOURCE_TYPE);
        if(f->kind==XR_XIR_SOURCE_TYPE_INTERFACE)CHECK(view->types->interfaces&&f->table_index<view->types->interfaces->count);
        else CHECK(view->types->nominals&&f->table_index<view->types->nominals->count);
        if(!f->range_known){CHECK(!f->range.module&&!f->range.line&&!f->range.column&&!f->range.end_line&&!f->range.end_column);continue;}++count;
        CHECK(f->range.module==view->declarations[i].range.module&&f->range.line>0&&f->range.column>0&&f->range.end_line>=f->range.line&&f->range.end_column>0);
    }
    if(mode==0){CHECK(count==3);const XrXirSourceTypeFact *c=&facts->declarations[declaration(view,"C")];
        CHECK(c->kind==XR_XIR_SOURCE_TYPE_CLASS&&c->range_known&&c->range.line==1&&c->range.end_line==1&&c->range.end_column==59);
        CHECK(facts->declarations[declaration(view,"S")].kind==XR_XIR_SOURCE_TYPE_STRUCT&&facts->declarations[declaration(view,"E")].kind==XR_XIR_SOURCE_TYPE_ENUM);}
    else if(mode==1){CHECK(count==5);uint32_t base=facts->declarations[declaration(view,"Base")].table_index;
        uint32_t both=facts->declarations[declaration(view,"Both")].table_index,meter=facts->declarations[declaration(view,"Meter")].table_index;
        const XrXirInterfaceDeclaration *row=&view->types->interfaces->declarations[both];CHECK(row->parent_count==2);
        CHECK(view->types->interfaces->declarations[row->parents[0].declaration].parents[0].declaration==base);
        CHECK(view->types->interfaces->declarations[row->parents[1].declaration].parents[0].declaration==base);
        CHECK(view->implementations&&view->implementations->count==1&&view->implementations->records[0].nominal_declaration==meter&&view->implementations->records[0].interface.declaration==both);}
    else if(mode==2){CHECK(count==2);uint32_t reader=facts->declarations[declaration(view,"Reader")].table_index;
        CHECK(view->types->interfaces->declarations[reader].parameter_count==1&&view->implementations&&view->implementations->records[0].interface.argument_count==1);}
    else if(mode==3){CHECK(count==3&&view->module_count==2);uint32_t shape=facts->declarations[declaration(view,"Shape")].table_index;
        uint32_t local=facts->declarations[declaration(view,"Local")].table_index;CHECK(view->types->interfaces->declarations[local].parents[0].declaration==shape);}
    printf("Source type facts mode=%u actual_types=%u canonical table identities, original parents/implements and real bounds\n",mode,count);
}
#include "type_resources.h"
static int run(int argc,char **argv) {
    CHECK(argc==3||argc==7||argc==8);(void)sdk_unlimited;(void)sdk_ledger;
    if(argc!=3)return type_resource_command(argc,argv);
    unsigned mode=(unsigned)strtoul(argv[2],NULL,10);CHECK(mode<5);type_ledgers=0;
    XrCompileResourceStats stats={0};XrXirSourceSnapshot *snapshot=NULL;XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=type_program(argv[1],mode,type_limits,&stats,&snapshot,&diagnostic);
    if(status!=XR_XIR_OK)fprintf(stderr,"Required Source type program mode=%u status=%u\n",mode,(unsigned)status);
    if(status!=XR_XIR_OK)fprintf(stderr,"Canonical Source diagnostic: %s\n",diagnostic.message);
    CHECK(type_ledgers==1);
    if(status==XR_XIR_OK)inspect(xr_xir_compile_source_snapshot_view(snapshot),xr_xir_compile_source_snapshot_type_facts(snapshot),mode);
    else CHECK(!snapshot);
    xr_xir_compile_source_snapshot_free(snapshot);CHECK(!runtime_live&&!runtime_bytes);
    printf("Source type program mode=%u status=%u allocated=%llu peak=%llu work=%llu physical=0/0\n",mode,(unsigned)status,
        (unsigned long long)stats.allocated_bytes,(unsigned long long)stats.peak_bytes,(unsigned long long)stats.work);return status==XR_XIR_OK?0:1;
}
int wmain(int argc,wchar_t **wide){XrWinPathStatus s;char **argv=xr_win_utf16_arguments(argc,wide,&s);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
