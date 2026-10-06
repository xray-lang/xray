/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_array_fill_cases.h - Literal range and owning publication oracles
 *
 * KEY CONCEPT:
 *   Invalid endpoints reject before any candidate; old aliases survive publication.
 */
#ifndef XIR_ARRAY_FILL_CASES_H
#define XIR_ARRAY_FILL_CASES_H
static XrXirInstance *fill_open(FillCompile *run) {
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.value_limit=1048576;XrXirInstance *instance=NULL;
    CHECK(xr_xir_instance_new(run->program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue entry=fill_run(instance,run->program->declarations->entry_function);
    CHECK(entry.type==XR_XIR_I64&&!entry.payload);xr_xir_value_drop(&entry);return instance;
}
static void fill_array(const XrXirValue *value,const int64_t *expected,int64_t count) {
    XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(1048576,&reader)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(value),reader,NULL,NULL,100000,1048576};
    int64_t length=-1;CHECK(xr_xir_array_len(value,&admission,&length)==XR_XIR_VALUE_OK&&length==count);
    for(int64_t i=0;i<count;++i){XrXirValue element={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_array_get(value,i,&admission,&element,&fault)==XR_XIR_VALUE_OK);
        CHECK(element.type==XR_XIR_I64&&element.payload==expected[i]);xr_xir_value_drop(&element);
    }
    xr_xir_domain_drop(reader);
}
static void fill_goldens(FillCompile *run) {
    static const char *const names[]={"numbers","independent","vacant","noChange","rebound","readHandle"};
    static const int64_t numbers[]={1,2,3,9,9,9,9,7,7,9,8,7,9,8,7,3};
    static const int64_t independent[]={99,7,1,42,3},vacant[]={0,0,0,0},old[]={1,2,3};
    static const int64_t rebound[]={91,9,9,97},handle[]={1,7,3,7};
    const int64_t *const values[]={numbers,independent,vacant,old,rebound,handle};
    static const int64_t counts[]={16,5,4,3,4,4};
    XrXirInstance *first=fill_open(run),*second=fill_open(run);
    for(unsigned n=0;n<sizeof(names)/sizeof(names[0]);++n){
        XrXirValue value=fill_run(first,fill_find(run->module,names[n]));fill_array(&value,values[n],counts[n]);xr_xir_value_drop(&value);
        if(n==4){value=fill_run(first,fill_find(run->module,"currentTrace"));CHECK(value.type==XR_XIR_I64&&value.payload==1234);xr_xir_value_drop(&value);}
    }
    XrXirValue alias=fill_run(first,fill_find(run->module,"saved"));
    XrXirValue result=fill_run(first,fill_find(run->module,"committed"));
    static const int64_t committed[]={1,7,3};fill_array(&result,committed,3);xr_xir_value_drop(&result);fill_array(&alias,old,3);
    result=fill_run(second,fill_find(run->module,"saved"));fill_array(&result,old,3);xr_xir_value_drop(&result);
    result=fill_run(first,fill_find(run->module,"defaultRebound"));static const int64_t current[]={6,6,6,6};
    fill_array(&result,current,4);xr_xir_value_drop(&result);
    result=fill_run(first,fill_find(run->module,"currentTrace"));CHECK(result.payload==1);xr_xir_value_drop(&result);fill_array(&alias,old,3);
    result=fill_run(first,fill_find(run->module,"nullable"));
    XrXirDomain *reader=NULL;CHECK(xr_xir_domain_new(1048576,&reader)==XR_XIR_VALUE_OK);
    XrXirValueAdmission admission={xr_xir_value_arena(&result),reader,NULL,NULL,100000,1048576};
    int64_t count=-1;CHECK(xr_xir_array_len(&result,&admission,&count)==XR_XIR_VALUE_OK&&count==3);
    for(int64_t i=0;i<3;++i){XrXirValue element={0};XrXirFaultDetail detail={0};bool some=true;const XrXirValue *payload=NULL;
        CHECK(xr_xir_array_get(&result,i,&admission,&element,&detail)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_nullable_view(&element,&some,&payload)&&!some&&!payload);xr_xir_value_drop(&element);
    }
    xr_xir_domain_drop(reader);xr_xir_value_drop(&result);
    xr_xir_value_drop(&alias);CHECK(xr_xir_instance_free(first)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(second)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    puts("fill fixed1-2-3/default/rebound1234/COW99-42/twoInstances physical0");
}
static void fill_ranges(FillCompile *run) {
    static const char *const names[]={"negative","negativeEnd","reversed","excessive","excessiveEmpty","minimum","maximum"};
    static const int64_t old[]={1,2,3};
    for(unsigned n=0;n<sizeof(names)/sizeof(names[0]);++n){
        XrXirInstance *instance=fill_open(run);XrXirValue alias=fill_run(instance,fill_find(run->module,"saved"));
        CHECK(xr_xir_instance_start(instance,fill_find(run->module,names[n]),NULL,0)==XR_XIR_CALL_READY);
        XrXirCallResult failure=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome;
        CHECK(failure.status==XR_XIR_CALL_NUMERIC_RANGE&&failure.panic.detail.code==422);
        CHECK(!failure.panic.detail.reserved&&!failure.panic.detail.index&&!failure.panic.detail.length);
        char message[96];const char *literal="numeric conversion is out of range";
        CHECK(xr_xir_panic_message_format(failure.panic.detail,message,sizeof(message))==strlen(literal)&&!strcmp(message,literal));
        XrXirCallResult copy={0};CHECK(xr_xir_instance_copy_failure(instance,&copy)==XR_XIR_CALL_BAD_STATE);
        CHECK(xr_xir_call_result_empty(&copy));
        XrXirCallResult repeated=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome;
        CHECK(repeated.status==failure.status&&!memcmp(&repeated.panic.detail,&failure.panic.detail,sizeof(failure.panic.detail)));
        XrXirValue absent={0};CHECK(xr_xir_instance_take_result(instance,&absent)==XR_XIR_CALL_BAD_STATE&&!absent.type&&!absent.payload);
        XrXirValue current=fill_run(instance,fill_find(run->module,"saved"));fill_array(&current,old,3);xr_xir_value_drop(&current);fill_array(&alias,old,3);
        if(!n){current=fill_run(instance,fill_find(run->module,"currentTrace"));CHECK(current.payload==123);xr_xir_value_drop(&current);}
        xr_xir_value_drop(&alias);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY&&!runtime_live&&!runtime_bytes);
    }
    puts("fill strict negative/reversed/excessive/INTMIN/INTMAX exactE0422 args123 root+alias preserved physical0");
}
static void fill_escape(FillCompile *run) {
    XrXirInstance *instance=fill_open(run);XrXirValue text=fill_run(instance,fill_find(run->module,"text"));
    XrXirValue alias=fill_run(instance,fill_find(run->module,"saved"));
    xr_xir_compile_artifact_free(run->lowered);run->lowered=NULL;
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_xir_compile_program_drop(run->program);run->program=NULL;run->module=NULL;
    XrXirDomain *domain=object_pointer(&text)->domain;CHECK(domain);
    XrXirValueAdmission admission={xr_xir_value_arena(&text),domain,NULL,NULL,100000,1048576};
    static const char *const expected[]={"kept","a\0中","tail"};static const size_t sizes[]={4,5,4};
    for(int64_t i=0;i<3;++i){XrXirValue item={0};XrXirFaultDetail fault={0};const char *bytes=NULL;size_t length=0;
        CHECK(xr_xir_array_get(&text,i,&admission,&item,&fault)==XR_XIR_VALUE_OK);
        CHECK(xr_xir_string_view(&item,&bytes,&length)&&length==sizes[i]&&!memcmp(bytes,expected[i],length));xr_xir_value_drop(&item);
    }
    static const int64_t old[]={1,2,3};fill_array(&alias,old,3);
    xr_xir_value_drop(&alias);xr_xir_value_drop(&text);
    CHECK(!runtime_live&&!runtime_bytes&&fill_stats(&run->context).live_bytes==run->baseline.live_bytes);
    puts("fill escaped NULString5+alias after Program/Instance death final dualphysical0");
}
#endif
