/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_typed_native_oracles.h
 */
#ifndef XIR_LIBRARY_TYPED_NATIVE_ORACLES_H
#define XIR_LIBRARY_TYPED_NATIVE_ORACLES_H
typedef struct RootParameterSourceOracle {
    const char *file,*text;bool root,unresolved;int64_t result;
    const char *output;size_t output_bytes;uint32_t modules,index;
} RootParameterSourceOracle;
static const RootParameterSourceOracle rps_oracles[]={
    {"typed_nominal",NULL,false,false,41,"41\n",3,2,0},
    {"typed_interface",NULL,false,false,41,"41\n",3,2,1},
    {"construction",NULL,false,false,41,"41\n",3,2,2},
    {"generic_parent_lr",NULL,false,false,41,"41\n",3,2,3},
    {"generic_parent_rl",NULL,false,false,41,"41\n",3,2,4},
    {"member_witness",NULL,false,false,41,"41\n",3,2,5},
    {"carrier_member",NULL,false,true,41,"41\n",3,2,6},
    {"enum_owned",NULL,false,false,41,"41\n",3,2,7},
    {"class_owned",NULL,false,false,41,"41\n",3,2,8},
};
#define H1_TYPED_CASES 9u
#endif /* XIR_LIBRARY_TYPED_NATIVE_ORACLES_H */
