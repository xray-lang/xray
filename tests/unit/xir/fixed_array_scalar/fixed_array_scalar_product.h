/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fixed_array_scalar_product.h - Shared exact scalar array execution oracle
 *
 * KEY CONCEPT:
 *   Both products execute list, repeat, inline and boxed transactions.
 */
#ifndef FIXED_ARRAY_SCALAR_PRODUCT_H
#define FIXED_ARRAY_SCALAR_PRODUCT_H
#include "xir/xxir_output.h"
static const char fixed_array_scalar_text[]=
    "var hits=0\n"
    "fn once()->u32{hits+=1;return 4294967295}\n"
    "fn pass(x:[u64;3])->[u64;3]{return x}\n"
    "struct Words { lanes:[u32;4] }\n"
    "var a:[u32;4]=[once();4]\n"
    "assert(hits==1 && len(a)==4 && a[3]==4294967295)\n"
    "var b=copy a\nb[1]=16909060\n"
    "assert(a[1]==4294967295 && b[1]==16909060)\n"
    "var r=Words{lanes:a}\nvar s=copy r\ns.lanes[2]=7\n"
    "assert(r.lanes[2]==4294967295 && s.lanes[2]==7)\n"
    "var wide:[u64;3]=[0,9223372036854775808,18446744073709551615]\n"
    "var moved=pass(wide)\nmoved[0]=42\n"
    "assert(wide[0]==0 && moved[0]==42 && moved[1]==9223372036854775808 && moved[2]==18446744073709551615)\n"
    "var packed:Array<[u64;3]>=[wide]\nvar restored=packed[0]\nrestored[2]=1\n"
    "assert(packed[0][2]==18446744073709551615 && restored[2]==1)\n"
    "var bytes:[u8;5]=[255;5]\nbytes[4]=42\nassert(bytes[0]==255 && bytes[4]==42)\n"
    "println(42)\n";
typedef struct FixedScalarOutput {char bytes[3];size_t count;} FixedScalarOutput;
static XrXirOutputStatus fixed_scalar_write(void *owner,XrXirOutputStream stream,const char *bytes,size_t count) {
    FixedScalarOutput *output=owner;
    CHECK(stream==XR_XIR_STDOUT && count<=sizeof(output->bytes)-output->count);
    memcpy(output->bytes+output->count,bytes,count);output->count+=count;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus fixed_scalar_group(void *owner,const XrXirOutputGroup *group) {
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,fixed_scalar_write,owner,3};
    return xr_xir_output_render(&sink,group);
}
static void fixed_array_scalar_run(XrXirProgram *program,uint32_t entry) {
    FixedScalarOutput output={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,fixed_scalar_group,&output};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
        CHECK(step<100000);status=xr_xir_instance_poll_bounded(instance,100).outcome.status;
    }
    if(status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"fixed scalar status%u\n",status);
    CHECK(status==XR_XIR_CALL_RETURNED && output.count==3 && !memcmp(output.bytes,"42\n",3));
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);
    xr_xir_value_drop(&value);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(fwrite(output.bytes,1,output.count,stdout)==output.count);
}
#endif
