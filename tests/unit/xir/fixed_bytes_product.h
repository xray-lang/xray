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
#endif // FIXED_BYTES_PRODUCT_H
