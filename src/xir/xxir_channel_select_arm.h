/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_channel_select_arm.h - Typed receive handles and owned send pairs
 */
#ifndef XXIR_CHANNEL_SELECT_ARM_H
#define XXIR_CHANNEL_SELECT_ARM_H
#include "xxir_channel.h"

static inline XrXirType xir_channel_select_operand_type(const XrXirTypes *types,
    XrXirType type,bool *sending) {
    *sending=false;
    if(xr_xir_type_is_channel(types,type))return type;
    const XrXirTypeNode *pair=xr_xir_type_node(types,type);
    if(!pair||pair->kind!=XR_XIR_TYPE_TUPLE||pair->parameter_count!=2||!pair->parameters||
        pair->parameters[0].mode!=XR_PARAM_READ||pair->parameters[1].mode!=XR_PARAM_READ)return XR_XIR_UNIT;
    XrXirType channel=pair->parameters[0].type;
    if(!xr_xir_type_is_channel(types,channel)||
        pair->parameters[1].type!=xr_xir_channel_element(types,channel))return XR_XIR_UNIT;
    *sending=true;return channel;
}
#endif // XXIR_CHANNEL_SELECT_ARM_H
