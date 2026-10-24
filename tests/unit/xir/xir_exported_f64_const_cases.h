/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_exported_f64_const_cases.h - Public constants and actual Library hops
 */
#ifndef XIR_EXPORTED_F64_CONST_CASES_H
#define XIR_EXPORTED_F64_CONST_CASES_H
static const char ci_other_source[]="export const PI:f64=16.0\n";
static const char ci_flags_source[]=
    "export const PI:f64=3.14159265358979323846\n"
    "export const E:f64=2.71828182845904523536\n"
    "export const TAU:f64=6.28318530717958647692\n"
    "export const SQRT2:f64=1.41421356237309504880\n"
    "export const LN2:f64=0.69314718055994530942\n"
    "export const LN10:f64=2.30258509299404568402\n"
    "export const LOG2E:f64=1.44269504088896340736\n"
    "export const LOG10E:f64=0.43429448190325182765\n"
    "export const EPSILON:f64=2.22044604925031308085e-16\n"
    "export const MAX_I64:i64=9223372036854775807\n"
    "export const MIN_I64:i64=-9223372036854775807-1\n"
    "export const MAX_F64:f64=1.79769313486231570815e+308\n"
    "export const INF:f64=1.0/0.0\nexport const NAN:f64=0.0/0.0\n"
    "export const ZERO:f64=-(0.0)\n"
    "var initMarker:i64=0\nfn initialize()->i64{initMarker+=1;return 0}\n"
    "const initialized:i64=initialize()\nexport fn initCount()->i64{return initMarker}\n";
static const char ci_facade_source[]=
    "import \"./flags\" as flags\nimport {PI as R} from \"./flags\"\n"
    "export fn through()->f64{return flags.PI}\n"
    "export fn count()->i64{return flags.initCount()}\n";
static const char ci_hop_source[]=
    "import {through,count} from \"./facade\"\n"
    "export fn via()->f64{return through()}\n"
    "export fn countAgain()->i64{return count()}\n";
static const char ci_consumer_source[]=
    "import \"./flags\" as flags\nimport \"./other\" as other\n"
    "import {PI as R,ZERO} from \"./flags\"\n"
    "import {via,countAgain} from \"./hop\"\n"
    "export fn bits()->f64{"
    "if(flags.NAN==flags.NAN || flags.INF<=flags.MAX_F64 || flags.MIN_I64>=0 || flags.MAX_I64<=0){return 0.0};"
    "if(flags.E<=2.0 || flags.TAU<=6.0 || flags.SQRT2<=1.0 || flags.LN2<=0.0 || flags.LN10<=2.0 || "
    "flags.LOG2E<=1.0 || flags.LOG10E<=0.0 || flags.EPSILON<=0.0 || flags.EPSILON>=1.0 || R!=flags.PI){return 0.0};"
    "return via()}\n"
    "export fn shadow()->f64{const R:f64=8.0;return R}\n"
    "export fn initializations()->i64{return countAgain()}\n"
    "export fn all()->f64{return ZERO}\n"
    "export fn otherValue()->f64{return other.PI}\n"
    "export fn callable()->fn()->f64{return bits}\n";
static const char *const ci_entry_names[]={"bits","shadow","initializations","all","otherValue","callable"};
static const char *const ci_logical_names[]={"other.xr","flags.xr","facade.xr","hop.xr"};
static const char *const ci_rejections[]={
    "export var VALUE:f64=1.0",
    "export const VALUE=1.0",
    "export const VALUE:f32=1.0",
    "export const VALUE:string=\"text\"",
    "fn init()->f64{return 1.0} export const VALUE:f64=init()",
    "export const VALUE:f64=\"wrong\"",
    "import {initialized} from \"./flags\";export fn f()->i64{return initialized}",
    "import {PI} from \"./flags\";export fn f(){PI=2.0}",
    "import \"./flags\" as flags;export fn f(){flags.PI=2.0}",
    "import {PI} from \"./flags\";fn alter(ref x:f64){x=2.0} export fn f(){alter(ref PI)}"
};
#endif // XIR_EXPORTED_F64_CONST_CASES_H
