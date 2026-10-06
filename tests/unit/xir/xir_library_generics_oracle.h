/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_generics_oracle.h - Independent values after Program and Instance destruction
 */
#ifndef XIR_LIBRARY_GENERICS_ORACLE_H
#define XIR_LIBRARY_GENERICS_ORACLE_H
static void library_generics_program_oracle(XrXirProgram *program,const uint32_t entries[4]) {
    XrXirInstance *instances[2]={0};XrXirValue escaped[2][2]={0};
    for(unsigned i=0;i<2;++i){XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_new(program,&config,&instances[i])==XR_XIR_CALL_READY);}
    xr_xir_compile_program_drop(program);
    for(unsigned i=0;i<2;++i){
        for(unsigned f=0;f<4;++f){CHECK(xr_xir_instance_start(instances[i],entries[f],NULL,0)==XR_XIR_CALL_READY);
            CHECK(xr_xir_instance_poll_bounded(instances[i],UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
            XrXirValue result={0};CHECK(xr_xir_instance_take_result(instances[i],&result)==XR_XIR_CALL_RETURNED);
            if(f==0){CHECK(result.type==XR_XIR_I64&&result.payload==42);xr_xir_value_drop(&result);}
            else if(f==2){CHECK(result.type==XR_XIR_BOOL&&result.payload==0);xr_xir_value_drop(&result);}
            else escaped[i][f==1?0:1]=result;
        }
        CHECK(xr_xir_instance_free(instances[i])==XR_XIR_CALL_READY);
    }
    for(unsigned i=0;i<2;++i){
        const char *bytes=NULL;size_t n=0;CHECK(xr_xir_string_view(&escaped[i][0],&bytes,&n)&&n==3&&!memcmp(bytes,"A\0B",3));
        XrXirValue copy={0};CHECK(xr_xir_value_copy(&escaped[i][0],&copy)==XR_XIR_VALUE_OK);xr_xir_value_drop(&escaped[i][0]);
        CHECK(xr_xir_string_view(&copy,&bytes,&n)&&n==3&&!memcmp(bytes,"A\0B",3));xr_xir_value_drop(&copy);
        CHECK(xr_xir_value_copy(&escaped[i][1],&copy)==XR_XIR_VALUE_OK);xr_xir_value_drop(&escaped[i][1]);
        XrXirDomain *domain=NULL;CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
        XrXirValueAdmission admission={.arena=xr_xir_value_arena(&copy),.domain=domain,.work=100000,.scratch_bytes=65536};
        XrXirValue field={0};CHECK(xr_xir_enum_get(&copy,0,0,&admission,&field)==XR_XIR_VALUE_OK);
        CHECK(field.type==XR_XIR_I64&&field.payload==73);xr_xir_value_drop(&field);xr_xir_value_drop(&copy);xr_xir_domain_drop(domain);
    }
    if(runtime_live||runtime_bytes)runtime_report_residuals(stderr);CHECK(!runtime_live&&!runtime_bytes);
}
#endif // XIR_LIBRARY_GENERICS_ORACLE_H
