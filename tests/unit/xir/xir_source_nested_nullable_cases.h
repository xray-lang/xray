/* Independent Source execution expectations; one optional layer per operation. */
#ifndef XIR_SOURCE_NESTED_NULLABLE_CASES_H
#define XIR_SOURCE_NESTED_NULLABLE_CASES_H
#include "xir/xxir_nullable.h"
#include "xir/xxir_array.h"
#include "xir/xxir_class.h"
enum { SN_MAIN,SN_CURRENT,SN_FACTORY,SN_COALESCED,SN_FORCED,SN_DOUBLED,SN_NARROWED,
    SN_NARROW_COALESCE,SN_NARROW_DOUBLE,SN_TWO_FACTS,SN_SHORT_FACTS,SN_DEEP_CLAIMS,
    SN_DEEP_UNWRAP,SN_RETURNED,SN_REBOUND,SN_NULL_DEFAULT,SN_NULL_OUTER,SN_THROWING,
    SN_STRING,SN_ARRAY,SN_CLASS,SN_CLASS_PLACE,SN_CAPTURED,SN_CALLABLE,SN_RECORD,SN_ENUM,SN_BOOL,SN_FLOAT,SN_COUNT };
static const char *const sn_names[SN_COUNT]={"main","current","factory","coalesced","forced",
    "doubled","narrowed","narrowCoalesce","narrowDouble","twoFacts","shortFacts","deepClaims",
    "deepUnwrap","returned","rebound","nullDefault","nullOuter","throwing","stringOwned",
    "arrayOwned","classOwned","classPlace","capturedDefault","callableProbe","recordProbe","enumProbe","boolProbe","floatProbe"};
static void sn_find(const XrXirModule *module,uint32_t output[SN_COUNT]) {
    for (uint32_t n=0;n<SN_COUNT;++n) {
        output[n]=UINT32_MAX;size_t length=strlen(sn_names[n]);
        for (uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *function=&module->functions[f];
            if (function->name_length==length && !memcmp(function->name,sn_names[n],length)) {
                CHECK(output[n]==UINT32_MAX);output[n]=f;
            }
        }
        CHECK(output[n]!=UINT32_MAX);
    }
}
static XrXirCallResult sn_poll(XrXirInstance *instance) {
    XrXirCallResult result={0};
    for (uint32_t tick=0;tick<4096;++tick) {
        result=xr_xir_instance_poll_bounded(instance,1).outcome;
        if (result.status!=XR_XIR_CALL_READY) return result;
    }
    CHECK(false);return result;
}
static void sn_config(XrXirInstanceConfig *config) {
    CHECK(xr_xir_instance_config_init(config,sizeof(*config))==XR_XIR_CALL_READY);
    config->value_limit=1048576;
}
static XrXirCallStatus sn_run(XrXirInstance *instance,uint32_t function,int mode,XrXirValue *value) {
    XrXirValue argument={XR_XIR_I64,0,mode};
    XrXirCallStatus status=xr_xir_instance_start(instance,function,mode<0?NULL:&argument,mode<0?0:1);
    if (status!=XR_XIR_CALL_READY) return status;
    XrXirCallResult result=sn_poll(instance);
    if (result.status==XR_XIR_CALL_RETURNED) CHECK(xr_xir_instance_take_result(instance,value)==XR_XIR_CALL_RETURNED);
    else if (result.status==XR_XIR_CALL_RUNTIME_PANIC) {
        CHECK(result.panic.detail.code==XR_XIR_PANIC_NULL_UNWRAP && !result.panic.detail.reserved &&
            !result.panic.detail.index && !result.panic.detail.length);
        CHECK(xr_xir_instance_take_result(instance,value)==XR_XIR_CALL_BAD_STATE && !value->type && !value->payload);
    }
    else CHECK(xr_xir_instance_take_result(instance,value)==XR_XIR_CALL_BAD_STATE && !value->type && !value->payload);
    return result.status;
}
static const XrXirValue *sn_payload(const XrXirValue *value,bool expected) {
    bool some=false;const XrXirValue *element=NULL;
    CHECK(xr_xir_value_valid(value) && xr_xir_nullable_view(value,&some,&element) && some==expected);
    CHECK((element!=NULL)==expected);return element;
}
static void sn_number(const XrXirValue *value,int64_t expected) {
    CHECK(value->type==XR_XIR_I64 && !value->reserved && value->payload==expected);
}
static void sn_outer(const XrXirValue *value,uint32_t state) {
    const XrXirValue *inner=sn_payload(value,state!=0);
    if (inner) {const XrXirValue *number=sn_payload(inner,state==2);if (number) sn_number(number,7);}
}
static void sn_inner(const XrXirValue *value,bool present,int64_t expected) {
    const XrXirValue *number=sn_payload(value,present);if (number) sn_number(number,expected);
}
static void sn_expect(XrXirInstance *instance,uint32_t entry,int mode,int64_t expected,bool panic) {
    XrXirValue value={0};CHECK(sn_run(instance,entry,mode,&value)==
        (panic?XR_XIR_CALL_RUNTIME_PANIC:XR_XIR_CALL_RETURNED));
    if (!panic) sn_number(&value,expected);
    xr_xir_value_drop(&value);
}
static void sn_vector(XrXirInstance *instance,const uint32_t functions[SN_COUNT],XrXirValue held[6]) {
    sn_expect(instance,functions[SN_MAIN],-1,41,false);
    for (int mode=0;mode<3;++mode) {
        XrXirValue value={0};
        CHECK(sn_run(instance,functions[SN_FACTORY],mode,&held[mode])==XR_XIR_CALL_RETURNED);sn_outer(&held[mode],(uint32_t)mode);
        CHECK(sn_run(instance,functions[SN_COALESCED],mode,&value)==XR_XIR_CALL_RETURNED);
        sn_inner(&value,mode!=1,mode==0?9:7);xr_xir_value_drop(&value);
        sn_expect(instance,functions[SN_CURRENT],-1,mode==0?101:1,false);
        CHECK(sn_run(instance,functions[SN_FORCED],mode,&value)==(mode==0?XR_XIR_CALL_RUNTIME_PANIC:XR_XIR_CALL_RETURNED));
        if (mode) sn_inner(&value,mode==2,7);
        xr_xir_value_drop(&value);
        sn_expect(instance,functions[SN_DOUBLED],mode,7,mode!=2);
        CHECK(sn_run(instance,functions[SN_NARROWED],mode,&value)==XR_XIR_CALL_RETURNED);
        sn_inner(&value,mode==2,7);xr_xir_value_drop(&value);
        CHECK(sn_run(instance,functions[SN_NARROW_COALESCE],mode,&value)==XR_XIR_CALL_RETURNED);
        sn_inner(&value,mode!=1,mode==0?9:7);xr_xir_value_drop(&value);
        sn_expect(instance,functions[SN_CURRENT],-1,mode==0?101:1,false);
        sn_expect(instance,functions[SN_NARROW_DOUBLE],mode,mode==0?0:7,mode==1);
        sn_expect(instance,functions[SN_TWO_FACTS],mode,mode==0?0:mode==1?41:14,false);
        sn_expect(instance,functions[SN_SHORT_FACTS],mode,mode==2?14:0,false);
        sn_expect(instance,functions[SN_DEEP_CLAIMS],mode,mode==0?0:mode==1?41:42,false);
        sn_expect(instance,functions[SN_DEEP_UNWRAP],mode,mode==0?0:7,mode==1);
        CHECK(sn_run(instance,functions[SN_NULL_DEFAULT],mode,&value)==XR_XIR_CALL_RETURNED);
        sn_inner(&value,mode==2,7);xr_xir_value_drop(&value);
        CHECK(sn_run(instance,functions[SN_NULL_OUTER],mode,&value)==XR_XIR_CALL_RETURNED);
        sn_outer(&value,(uint32_t)mode);xr_xir_value_drop(&value);
        CHECK(sn_run(instance,functions[SN_THROWING],mode,&value)==(mode==0?XR_XIR_CALL_RUNTIME_PANIC:XR_XIR_CALL_RETURNED));
        if (mode) sn_inner(&value,mode==2,7);
        xr_xir_value_drop(&value);
        CHECK(sn_run(instance,functions[SN_CAPTURED],mode,&value)==XR_XIR_CALL_RETURNED);
        const XrXirValue *text=sn_payload(&value,mode!=1);
        if (text) {const char *bytes=NULL;size_t length=0;
            CHECK(xr_xir_string_view(text,&bytes,&length));
            static const char expected[]="a\0\xe4\xb8\xad";
            CHECK(length==(mode==0?4u:5u) && !memcmp(bytes,mode==0?"cap\0":expected,length));}
        xr_xir_value_drop(&value);sn_expect(instance,functions[SN_CURRENT],-1,mode==0?101:1,false);
    }
    sn_expect(instance,functions[SN_RETURNED],-1,0,true);sn_expect(instance,functions[SN_REBOUND],-1,0,true);
    for (uint32_t n=0;n<3;++n) CHECK(sn_run(instance,functions[SN_STRING+n],-1,&held[3+n])==XR_XIR_CALL_RETURNED);
    sn_expect(instance,functions[SN_CLASS_PLACE],-1,9,false);
    for (uint32_t n=SN_CALLABLE;n<=SN_FLOAT;++n) sn_expect(instance,functions[n],-1,7,false);
}
static void sn_retained(XrXirValue held[6]) {
    for (uint32_t state=0;state<3;++state) sn_outer(&held[state],state);
    const XrXirValue *string=sn_payload(sn_payload(&held[3],true),true);
    const char *bytes=NULL;size_t length=0;CHECK(xr_xir_string_view(string,&bytes,&length));
    static const char expected[]="a\0\xe4\xb8\xad";CHECK(length==5 && !memcmp(bytes,expected,5));
    for (uint32_t n=4;n<6;++n) {
        const XrXirValue *leaf=sn_payload(sn_payload(&held[n],true),true);
        XrXirValueAdmission admission={xr_xir_value_arena(leaf),NULL,NULL,NULL,1000000,1048576};
        XrXirValue result={0};XrXirFaultDetail fault={0};
        CHECK((n==4?xr_xir_array_get(leaf,0,&admission,&result,&fault):xr_xir_class_get(leaf,0,&admission,&result))==XR_XIR_VALUE_OK);
        sn_number(&result,7);xr_xir_value_drop(&result);
    }
    XrXirValue copy={0};XirObject *object=object_pointer(&held[1]);
    uint32_t references=atomic_load(&object->references);atomic_store(&object->references,UINT32_MAX);
    CHECK(xr_xir_value_copy(&held[1],&copy)==XR_XIR_VALUE_REFCOUNT_LIMIT && !copy.type && !copy.payload);
    atomic_store(&object->references,references);
    for (uint32_t n=0;n<6;++n) xr_xir_value_drop(&held[n]);
}
static void sn_faults(XrXirProgram *program,const uint32_t functions[SN_COUNT]) {
    static const struct {unsigned entry;int mode;} selected[]={
        {SN_FACTORY,2},{SN_CAPTURED,2},{SN_STRING,-1},{SN_ARRAY,-1},{SN_CLASS,-1},
        {SN_CALLABLE,-1},{SN_RECORD,-1},{SN_ENUM,-1},
        {SN_FACTORY,0},{SN_FACTORY,1},{SN_CAPTURED,0},{SN_CAPTURED,1}};
    const size_t live=runtime_live,bytes=runtime_bytes;
    for (unsigned n=0;n<sizeof(selected)/sizeof(selected[0]);++n) {
        size_t sites=0;
        for (size_t pass=0;pass<=sites;++pass) {
            runtime_attempts=0;runtime_fail_at=pass?pass-1:SIZE_MAX;
            XrXirInstance *instance=NULL;XrXirInstanceConfig config;sn_config(&config);XrXirValue result={0};
            XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
            if (status==XR_XIR_CALL_READY) status=sn_run(instance,functions[selected[n].entry],selected[n].mode,&result);
            size_t attempts=runtime_attempts;runtime_fail_at=SIZE_MAX;
            if (!pass) {
                CHECK(status==XR_XIR_CALL_RETURNED);sites=attempts;CHECK(sites && sites<20000);
                if (selected[n].entry==SN_FACTORY) sn_outer(&result,(uint32_t)selected[n].mode);
                if (selected[n].entry==SN_CAPTURED) {
                    const XrXirValue *text=sn_payload(&result,selected[n].mode!=1);
                    if (text) {
                        const char *data=NULL;size_t length=0;static const char expected[]="a\0\xe4\xb8\xad";
                        CHECK(xr_xir_string_view(text,&data,&length));
                        CHECK(length==(selected[n].mode==0?4u:5u) &&
                            !memcmp(data,selected[n].mode==0?"cap\0":expected,length));
                    }
                }
            }
            else CHECK(attempts>pass-1 && status==XR_XIR_CALL_OOM && !result.type && !result.payload);
            xr_xir_value_drop(&result);if (instance) CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
            CHECK(runtime_live==live && runtime_bytes==bytes);
        }
        printf("Source nested runtime %s mode=%d OOM sites=%zu physical baseline restored\n",sn_names[selected[n].entry],selected[n].mode,sites);
    }
    for (int mode=0;mode<3;++mode) {
        bool finished=false;
        for (uint32_t prefix=0;prefix<4096;++prefix) {
            XrXirInstance *instance=NULL;XrXirInstanceConfig config;sn_config(&config);XrXirValue result={0};
            CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
            sn_expect(instance,functions[SN_MAIN],-1,41,false);
            XrXirValue argument={XR_XIR_I64,0,mode};
            CHECK(xr_xir_instance_start(instance,functions[SN_CAPTURED],&argument,1)==XR_XIR_CALL_READY);
            XrXirCallStatus status=XR_XIR_CALL_READY;
            for (uint32_t tick=0;tick<prefix && status==XR_XIR_CALL_READY;++tick)
                status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
            if (status==XR_XIR_CALL_RETURNED) {CHECK(xr_xir_instance_take_result(instance,&result)==status);xr_xir_value_drop(&result);}
            else {CHECK(status==XR_XIR_CALL_READY && xr_xir_instance_take_result(instance,&result)==XR_XIR_CALL_BAD_STATE);
                CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
                CHECK(sn_poll(instance).status==XR_XIR_CALL_CANCELLED);sn_expect(instance,functions[SN_MAIN],-1,41,false);}
            CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY && runtime_live==live && runtime_bytes==bytes);
            if (status==XR_XIR_CALL_RETURNED) {finished=true;printf("Source captured mode=%d cancel prefixes=%u physical baseline restored\n",mode,prefix);break;}
        }
        CHECK(finished);
    }
}
static uint64_t sn_domain_case(XrXirProgram *program,uint32_t function,uint64_t limit,XrXirCallStatus expected) {
    size_t live=runtime_live,bytes=runtime_bytes;XrXirInstance *instance=NULL;XrXirInstanceConfig config;sn_config(&config);
    config.value_limit=limit;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue result={0};XrXirCallStatus status=sn_run(instance,function,2,&result);
    CHECK(status==expected);XrXirDomainStats stats=xr_xir_domain_stats(instance->domain);CHECK(stats.peak_bytes<=limit);
    if (status==XR_XIR_CALL_RETURNED) sn_outer(&result,2);
    else CHECK(!result.type && !result.payload);
    xr_xir_value_drop(&result);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(runtime_live==live && runtime_bytes==bytes);return stats.peak_bytes;
}
static void sn_domain_bounds(XrXirProgram *program,uint32_t function) {
    uint64_t peak=sn_domain_case(program,function,1048576,XR_XIR_CALL_RETURNED);CHECK(peak>1 && peak<=1048576);
    CHECK(sn_domain_case(program,function,peak,XR_XIR_CALL_RETURNED)==peak);
    (void)sn_domain_case(program,function,peak-1,XR_XIR_CALL_LIMIT);
}
static void sn_program_cases(XrXirProgram *program,const uint32_t functions[SN_COUNT],XrXirValue held[6]) {
    XrXirInstance *instance=NULL;XrXirInstanceConfig config;sn_config(&config);
    CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);sn_vector(instance,functions,held);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);sn_faults(program,functions);
    sn_domain_bounds(program,functions[SN_FACTORY]);
}
#endif
