/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fixed_bytes_product.h - Independent complete quoted literal output
 *
 * KEY CONCEPT:
 *   The golden predates execution and both backends observe the same typed output.
 */
#ifndef FIXED_BYTES_PRODUCT_H
#define FIXED_BYTES_PRODUCT_H
#include "xir/xxir_output.h"
#include "xir/xxir_fixed_array.h"
#include "xir_runtime_allocations.h"
#include "base/xplatform.h"
#ifdef XR_OS_WINDOWS
#include <io.h>
#include <fcntl.h>
#endif
static const char fixed_bytes_expected[]=
    "<section class=\"card\">\n"
    "  <h1>Xray</h1>\n"
    "  <p>Backslashes stay raw: C:\\assets\\page.html</p>\n"
    "</section>\n16\n137\n5\n";
_Static_assert(sizeof(fixed_bytes_expected)-1==110,"independent full literal golden");
typedef struct FixedBytesOutput {char bytes[110];size_t count;uint32_t groups;} FixedBytesOutput;
static XrXirOutputStatus fixed_bytes_write(void *owner,XrXirOutputStream stream,const char *bytes,size_t count) {
    FixedBytesOutput *output=owner;
    CHECK(stream==XR_XIR_STDOUT && count<=sizeof(output->bytes)-output->count);
    memcpy(output->bytes+output->count,bytes,count);output->count+=count;return XR_XIR_OUTPUT_OK;
}
static XrXirOutputStatus fixed_bytes_group(void *owner,const XrXirOutputGroup *group) {
    FixedBytesOutput *output=owner;
    CHECK(group && group->stream==XR_XIR_STDOUT && group->line && group->count==1 && output->groups<4);
    XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,fixed_bytes_write,output,sizeof(output->bytes)};
    ++output->groups;return xr_xir_output_render(&sink,group);
}
static void fixed_bytes_run(XrXirProgram *program,uint32_t entry) {
#ifdef XR_OS_WINDOWS
    CHECK(_setmode(_fileno(stdout),_O_BINARY)!=-1);
#endif
    FixedBytesOutput output={0};XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,fixed_bytes_group,&output};
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_start(instance,entry,NULL,0)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
        CHECK(step<100000);status=xr_xir_instance_poll_bounded(instance,1).outcome.status;
    }
    if(status!=XR_XIR_CALL_RETURNED)fprintf(stderr,"fixed complete program status%u\n",status);
    CHECK(status==XR_XIR_CALL_RETURNED && output.groups==4 && output.count==110 &&
        !memcmp(output.bytes,fixed_bytes_expected,110));
    XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED &&
        value.type==XR_XIR_I64 && !value.payload);xr_xir_value_drop(&value);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(fwrite(output.bytes,1,output.count,stdout)==output.count);
}

static XrXirValue fixed_transfer_call(XrXirInstance *instance,uint32_t entry,
    const XrXirValue *arguments,uint32_t count) {
    CHECK(xr_xir_instance_start(instance,entry,arguments,count)==XR_XIR_CALL_READY);
    XrXirCallStatus status=XR_XIR_CALL_READY;
    for(uint32_t step=0;status==XR_XIR_CALL_READY;++step) {
        CHECK(step<100000);status=xr_xir_instance_poll_bounded(instance,1024).outcome.status;
    }
    CHECK(status==XR_XIR_CALL_RETURNED);XrXirValue value={0};
    CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED);return value;
}
static void fixed_transfer_bytes(const XrXirValue *value) {
    static const uint8_t golden[]={0,255,65,128};int64_t count=-1;
    CHECK(xr_xir_value_valid(value) && xr_xir_fixed_array_count(value,&count)==XR_XIR_VALUE_OK && count==4);
    for(uint32_t i=0;i<sizeof(golden);++i) {
        XrXirValue item={0};XrXirFaultDetail fault={0};
        CHECK(xr_xir_fixed_array_get(value,i,&item,&fault)==XR_XIR_VALUE_OK &&
            item.type==XR_XIR_U8 && item.payload==golden[i]);xr_xir_value_drop(&item);
    }
}
static void fixed_transfer_refusals(const XrXirCompileContext *context,
    XrXirInstance *receiver,const XrXirValue *value,uint32_t read_entry) {
    const XrXirTypeNode node={.kind=XR_XIR_TYPE_FIXED_ARRAY,.element=XR_XIR_U8,.parameter_count=5};
    const XrXirTypes types={&node,1,NULL,NULL};XrXirTypeArena *wrong=NULL;
    CHECK(xr_xir_compile_type_arena_new(context,&types,&wrong)==XR_XIR_VALUE_OK);
    CHECK(!xr_xir_value_argument(value,wrong,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE));
    XrXirValueAdmission admission={wrong,receiver->domain,NULL,NULL,1000000,1048576};
    const XrXirValue saved=*value;
    CHECK(xr_xir_value_admit(value,(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,&admission)==
        XR_XIR_VALUE_BAD_ARGUMENT && !memcmp(value,&saved,sizeof(saved)));
    xr_xir_compile_type_arena_drop(wrong);
    XrXirValue scalar={XR_XIR_I64,0,65};
    CHECK(xr_xir_instance_start(receiver,read_entry,&scalar,1)==XR_XIR_CALL_BAD_ARGUMENT);
}
/* This consumes the Program reference. Both ordinary constructors create their
 * own Domain; retained immutable results must outlive both execution owners. */
static void fixed_transfer_run(const XrXirCompileContext *context,
    XrXirProgram *program,const uint32_t ids[4]) {
    XrXirInstanceConfig config;XrXirInstance *a=NULL,*b=NULL;
    CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&a)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_new(program,&config,&b)==XR_XIR_CALL_READY);
    CHECK(a->domain!=b->domain && !a->owner && !b->owner && a->program==b->program);
    xr_xir_compile_program_drop(program);program=NULL;
    XrXirValue entry=fixed_transfer_call(a,ids[0],NULL,0);xr_xir_value_drop(&entry);
    XrXirValue value=fixed_transfer_call(a,ids[1],NULL,0);fixed_transfer_bytes(&value);
    XrXirValue local=fixed_transfer_call(b,ids[1],NULL,0);fixed_transfer_bytes(&local);
    CHECK(object_pointer(&value)->domain==a->domain && object_pointer(&local)->domain==b->domain);
    xr_xir_value_drop(&local);fixed_transfer_refusals(context,b,&value,ids[2]);
    CHECK(xr_xir_instance_free(a)==XR_XIR_CALL_READY);a=NULL;
    fixed_transfer_bytes(&value);
    XrXirValue read=fixed_transfer_call(b,ids[2],&value,1);
    CHECK(read.type==XR_XIR_I64 && read.payload==65);xr_xir_value_drop(&read);
    XrXirValue copied=fixed_transfer_call(b,ids[3],&value,1);fixed_transfer_bytes(&copied);
    CHECK(xr_xir_instance_free(b)==XR_XIR_CALL_READY);b=NULL;
    fixed_transfer_bytes(&value);fixed_transfer_bytes(&copied);
    XrXirValue retained={0};CHECK(xr_xir_value_copy(&copied,&retained)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&value);xr_xir_value_drop(&copied);fixed_transfer_bytes(&retained);
    xr_xir_value_drop(&retained);
    fprintf(stderr,"fixed transfer: distinct Domains, retired instances and retained byte copies\n");
}
#endif // FIXED_BYTES_PRODUCT_H
