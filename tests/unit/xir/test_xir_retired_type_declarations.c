/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_retired_type_declarations.c - Complete retired declaration and hash inputs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while (0)
#include "xir/xxir_compile_context.h"
#include "xir/xxir_checked.h"
#include "base/xsha256.h"
#include "retired_type_packets.h"
static void rejected(const XrXirCompileContext *context,const uint8_t *bytes,size_t length) {
    CHECK(length>=64);
    XrSHA256Context digest;uint8_t actual[32];
    xr_sha256_init(&digest);xr_sha256_update(&digest,bytes,32);
    xr_sha256_update(&digest,bytes+64,length-64);xr_sha256_final(&digest,actual);
    CHECK(!memcmp(actual,bytes+32,32));
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    XrXirArtifact *output=NULL;
    CHECK(xr_xir_compile_checked_read(context,bytes,length,&output,NULL)==XR_XIR_BAD_STRUCTURE && !output);
    output=(XrXirArtifact *)(uintptr_t)1;
    CHECK(xr_xir_compile_checked_read(context,bytes,length,&output,NULL)==XR_XIR_BAD_STRUCTURE && output==(XrXirArtifact *)(uintptr_t)1);
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK);
    CHECK(after.work==before.work+32 && before.live_bytes==after.live_bytes && before.allocation_count==after.allocation_count);
}
int main(void) {
    const XrCompileResourceLimits limits={1048576,1048576,1048576};
    XrXirCompileContext context={0};context.limits=xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    rejected(&context,retired_0_sizing78_golden,sizeof(retired_0_sizing78_golden));
    rejected(&context,retired_0_retired_sizing74_golden,sizeof(retired_0_retired_sizing74_golden));
    rejected(&context,retired_0_sizing78_embedded_golden,sizeof(retired_0_sizing78_embedded_golden));
    rejected(&context,retired_0_retired_sizing74_embedded_golden,sizeof(retired_0_retired_sizing74_embedded_golden));
    rejected(&context,retired_0_sizing78_empty_golden,sizeof(retired_0_sizing78_empty_golden));
    rejected(&context,retired_0_retired_sizing74_empty_golden,sizeof(retired_0_retired_sizing74_empty_golden));
    rejected(&context,retired_1_root_construction72_checked,sizeof(retired_1_root_construction72_checked));
    rejected(&context,retired_1_root_construction72_instance,sizeof(retired_1_root_construction72_instance));
    rejected(&context,retired_1_root_construction74_previous,sizeof(retired_1_root_construction74_previous));
    rejected(&context,retired_1_root_construction71_previous,sizeof(retired_1_root_construction71_previous));
    rejected(&context,retired_2_unit78_packet,sizeof(retired_2_unit78_packet));
    rejected(&context,retired_2_previous75_packet,sizeof(retired_2_previous75_packet));
    rejected(&context,retired_2_previous76_packet,sizeof(retired_2_previous76_packet));
    rejected(&context,retired_3_assert_panics78_golden,sizeof(retired_3_assert_panics78_golden));
    rejected(&context,retired_4_checked_range78_golden,sizeof(retired_4_checked_range78_golden));
    rejected(&context,retired_5_checked_scalar78_golden,sizeof(retired_5_checked_scalar78_golden));
    rejected(&context,retired_6_error_marker78_golden,sizeof(retired_6_error_marker78_golden));
    rejected(&context,retired_6_error_marker73_previous,sizeof(retired_6_error_marker73_previous));
    rejected(&context,retired_7_generic_method78_golden,sizeof(retired_7_generic_method78_golden));
    rejected(&context,retired_7_generic_method78_invalid_flags0,sizeof(retired_7_generic_method78_invalid_flags0));
    rejected(&context,retired_7_generic_method78_invalid_flags1,sizeof(retired_7_generic_method78_invalid_flags1));
    rejected(&context,retired_8_rune78_golden,sizeof(retired_8_rune78_golden));
    rejected(&context,retired_9_constant_wire,sizeof(retired_9_constant_wire));
    rejected(&context,retired_10_rest_wire,sizeof(retired_10_rest_wire));
    rejected(&context,retired_11_sizing74_retired_golden,sizeof(retired_11_sizing74_retired_golden));
    rejected(&context,retired_11_sizing75_golden,sizeof(retired_11_sizing75_golden));
    rejected(&context,retired_11_sizing74_retired_embedded_golden,sizeof(retired_11_sizing74_retired_embedded_golden));
    rejected(&context,retired_11_sizing75_embedded_golden,sizeof(retired_11_sizing75_embedded_golden));
    rejected(&context,retired_11_sizing74_retired_empty_golden,sizeof(retired_11_sizing74_retired_empty_golden));
    rejected(&context,retired_11_sizing75_empty_golden,sizeof(retired_11_sizing75_empty_golden));
    rejected(&context,retired_12_assert_panics75_golden,sizeof(retired_12_assert_panics75_golden));
    rejected(&context,retired_13_checked_range75_golden,sizeof(retired_13_checked_range75_golden));
    rejected(&context,retired_14_checked_scalar75_golden,sizeof(retired_14_checked_scalar75_golden));
    rejected(&context,retired_15_error_marker75_golden,sizeof(retired_15_error_marker75_golden));
    rejected(&context,retired_16_generic_method75_golden,sizeof(retired_16_generic_method75_golden));
    rejected(&context,retired_16_generic_method75_invalid_flags0,sizeof(retired_16_generic_method75_invalid_flags0));
    rejected(&context,retired_16_generic_method75_invalid_flags1,sizeof(retired_16_generic_method75_invalid_flags1));
    rejected(&context,retired_17_rune75_golden,sizeof(retired_17_rune75_golden));
    rejected(&context,retired_18_root_construction75_checked,sizeof(retired_18_root_construction75_checked));
    rejected(&context,retired_18_root_construction75_instance,sizeof(retired_18_root_construction75_instance));
    rejected(&context,retired_18_root_construction71_previous_current75,sizeof(retired_18_root_construction71_previous_current75));
    rejected(&context,retired_19_hash75_golden,sizeof(retired_19_hash75_golden));
    rejected(&context,retired_20_nullable_none75_golden,sizeof(retired_20_nullable_none75_golden));
    rejected(&context,retired_20_nullable_some75_golden,sizeof(retired_20_nullable_some75_golden));
    rejected(&context,retired_21_nested_nullable75_golden,sizeof(retired_21_nested_nullable75_golden));
    rejected(&context,retired_22_sizing79_golden,sizeof(retired_22_sizing79_golden));
    rejected(&context,retired_22_sizing79_embedded_golden,sizeof(retired_22_sizing79_embedded_golden));
    rejected(&context,retired_22_sizing79_empty_golden,sizeof(retired_22_sizing79_empty_golden));
    rejected(&context,retired_23_root_construction72_checked,sizeof(retired_23_root_construction72_checked));
    rejected(&context,retired_23_root_construction72_instance,sizeof(retired_23_root_construction72_instance));
    rejected(&context,retired_24_unit79_packet,sizeof(retired_24_unit79_packet));
    rejected(&context,retired_25_assert_panics79_golden,sizeof(retired_25_assert_panics79_golden));
    rejected(&context,retired_26_checked_range79_golden,sizeof(retired_26_checked_range79_golden));
    rejected(&context,retired_27_checked_scalar79_golden,sizeof(retired_27_checked_scalar79_golden));
    rejected(&context,retired_28_error_marker79_golden,sizeof(retired_28_error_marker79_golden));
    rejected(&context,retired_29_generic_method79_golden,sizeof(retired_29_generic_method79_golden));
    rejected(&context,retired_29_generic_method79_invalid_flags0,sizeof(retired_29_generic_method79_invalid_flags0));
    rejected(&context,retired_29_generic_method79_invalid_flags1,sizeof(retired_29_generic_method79_invalid_flags1));
    rejected(&context,retired_30_rune79_golden,sizeof(retired_30_rune79_golden));
    rejected(&context,retired_31_hash79_golden,sizeof(retired_31_hash79_golden));
    rejected(&context,retired_32_nullable_none79_golden,sizeof(retired_32_nullable_none79_golden));
    rejected(&context,retired_32_nullable_some79_golden,sizeof(retired_32_nullable_some79_golden));
    rejected(&context,retired_33_nested_nullable79_golden,sizeof(retired_33_nested_nullable79_golden));
    rejected(&context,retired_34_constant_wire,sizeof(retired_34_constant_wire));
    rejected(&context,retired_35_rest_wire,sizeof(retired_35_rest_wire));
    xr_compile_resources_release(context.resources);
    puts("Complete retired declaration/hash packets rejected with fixed header work and no allocation: 65");return 0;
}
