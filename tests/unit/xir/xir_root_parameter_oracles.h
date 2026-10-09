/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_root_parameter_oracles.h - Literal Source programs and execution oracles
 */
#ifndef XIR_ROOT_PARAMETER_ORACLES_H
#define XIR_ROOT_PARAMETER_ORACLES_H
typedef struct RootParameterSourceOracle {
    const char *file, *text;
    bool root, unresolved;
    int64_t result;
    const char *output;
    size_t output_bytes;
} RootParameterSourceOracle;
static const RootParameterSourceOracle rps_oracles[] = {
    {"apply.xr",
     "fn apply(callback:fn()->i64)->i64{try{return callback()}catch(error){return 0}}\n"
     "fn pure()->i64{return 7}\n"
     "export fn run()->i64{return apply(pure)}\n"
     "print(run())\n", false,false,7,"7\n",2},
    {"fixed.xr",
     "fn apply(callback:fn()->i64)->i64{try{return callback()}catch(error){return 0}}\n"
     "fn pure()->i64{return 7}\n"
     "export fn run()->i64{const fixed:fn()->i64=pure;return apply(fixed)}\n"
     "print(run())\n", false,true,7,"7\n",2},
    {"root.xr",
     "var state:i64=9\n"
     "fn apply(callback:fn()->i64)->i64{try{return callback()}catch(error){return 0}}\n"
     "fn rooted()->i64{return state}\n"
     "export fn run()->i64{return apply(rooted)}\n"
     "print(run())\n", true,false,9,"9\n",2},
    {"forward.xr",
     "fn apply(callback:fn()->i64)->i64{try{return callback()}catch(error){return 0}}\n"
     "fn forward(callback:fn()->i64)->i64{return apply(callback)}\n"
     "fn pure()->i64{return 7}\n"
     "export fn run()->i64{return forward(pure)}\n"
     "print(run())\n", false,false,7,"7\n",2}
};
#endif
