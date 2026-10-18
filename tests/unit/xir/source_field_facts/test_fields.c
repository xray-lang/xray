/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_source_bindings.h"
#include "api/xsource_compile.h"
#include "base/xwindows_utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(1);}}while(0)
#include "xir_sdk_resource_test.h"
static const char *const texts[]={
    "enum E { A { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value}->value }\n",
    "enum E { A { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value:alias}->alias }\n",
    "enum E { A { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value:value}->value }\n",
    "enum E { A { value:i64 }, B { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value}, E.B {value}->value }\n",
    "enum E { A { value:i64 }, B { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value:value}, E.B {value}->value }\n",
    "enum E { A { value:i64 }, B { value:i64 } }\nvar x=E.A {value:1}\nvar y=match(x){ E.A {value}, E.B {value:value}->value }\n"
};
static int run(int argc,char **argv){
    CHECK(argc==3);unsigned mode=(unsigned)strtoul(argv[2],NULL,10);CHECK(mode<6);
    (void)sdk_unlimited;
    XrCompileResourceLimits limits={UINT64_C(64)*1024*1024,UINT64_C(8)*1024*1024,UINT64_C(128000000)};
    XrCompileResources *resources=sdk_ledger(&limits);XrXirCompileContext context={resources,xr_xir_compile_default_limits()};
    size_t length=strlen(texts[mode]),uri_length=strlen(argv[1]);
    char *text=sdk_fixture_malloc(length+1),*uri=sdk_fixture_malloc(uri_length+1);CHECK(text&&uri);
    memcpy(text,texts[mode],length+1);memcpy(uri,argv[1],uri_length+1);
    XlspSourceDocument input={uri,text,uri_length,length,1};XlspSourceBindings *owner=NULL;
    CHECK(xlsp_source_bindings_build(&context,&input,1,&owner)==XR_XIR_OK&&owner);
    XrCompileResourceStats stats=sdk_stats(resources);
    memset(text,'?',length);memset(uri,'?',uri_length);sdk_fixture_free(text);sdk_fixture_free(uri);xr_compile_resources_release(resources);
    const XrXirSourceView *view=xlsp_source_bindings_view(owner,0);const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(owner,0);
    CHECK(view&&view->complete&&syntax&&syntax->declaration_count==view->declaration_count);
    const char *name=mode==1?"alias":"value";unsigned found=0;
    for(uint32_t i=0;i<view->declaration_count;++i){
        const XrXirSourceDeclaration *d=&view->declarations[i];if(d->kind!=XR_XIR_SOURCE_BINDING||strcmp(d->name,name))continue;
        ++found;const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[i];
        CHECK(fact->role==XR_XIR_SOURCE_SYNTAX_NAME&&fact->name.line==3&&fact->name.column>0);
        CHECK(fact->name.end_line==3&&fact->name.end_column-fact->name.column==(int)strlen(name));
        if(mode==0){CHECK(d->target&&d->target<=view->declaration_count);const XrXirSourceDeclaration *field=&view->declarations[d->target-1];
            CHECK(field->kind==XR_XIR_SOURCE_MEMBER&&!strcmp(field->name,"value"));}
        else CHECK(!d->target);
    }
    CHECK(found==1);unsigned markers=0;
    for(uint32_t i=0;i<syntax->marker_count;++i){
        const XrXirSourceMarker *marker=&syntax->markers[i];CHECK(marker->role==XR_XIR_SOURCE_MARKER_PATTERN_FIELD_BINDING);
        CHECK(marker->declaration&&marker->declaration<=view->declaration_count);
        const XrXirSourceDeclaration *binding=&view->declarations[marker->declaration-1];
        CHECK(binding->kind==XR_XIR_SOURCE_BINDING&&!strcmp(binding->name,"value"));
        CHECK(marker->range.line==3&&marker->range.column>0&&marker->range.end_column-marker->range.column==5);
        unsigned fields=0;for(uint32_t j=0;j<view->reference_count;++j){
            XrXirSourceRange span=syntax->references[j];const XrXirSourceReference *ref=&view->references[j];
            if(span.module!=marker->range.module||span.line!=3||span.column!=marker->range.column||span.end_column!=marker->range.end_column)continue;
            const XrXirSourceDeclaration *field=&view->declarations[ref->declaration-1];
            if(field->kind==XR_XIR_SOURCE_MEMBER){CHECK(ref->declaration==ref->target&&!strcmp(field->name,"value"));++fields;}
        }CHECK(fields==1);++markers;
    }
    CHECK(markers==(mode==0||mode==4||mode==5?1u:mode==3?2u:0u));
    xlsp_source_bindings_free(owner);CHECK(!runtime_live&&!runtime_bytes);
    printf("Owned Source field mode=%u implicit_target=%u declaration=%u work=%llu physical0/0\n",mode,mode==0?1u:0u,found,(unsigned long long)stats.work);return 0;
}
int wmain(int argc,wchar_t **wide){XrWinPathStatus status;char **argv=xr_win_utf16_arguments(argc,wide,&status);CHECK(argv);int result=run(argc,argv);xr_win_utf8_arguments_free(argc,argv);return result;}
