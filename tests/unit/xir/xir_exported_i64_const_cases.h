/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_exported_i64_const_cases.h - Public constants and actual Library hops
 */
#ifndef XIR_EXPORTED_I64_CONST_CASES_H
#define XIR_EXPORTED_I64_CONST_CASES_H
static const char ci_other_source[]="export const PROT_READ:i64=16\n";
static const char ci_flags_source[]=
    "import \"./other\" as other\n"
    "export const PROT_NONE:i64=0\nexport const PROT_READ:i64=1\n"
    "export const PROT_WRITE:i64=2\nexport const PROT_EXEC:i64=4\n"
    "var initMarker:i64=0\nfn initialize()->i64{initMarker+=1;return 0}\n"
    "const initialized:i64=initialize()\n"
    "export fn initCount()->i64{return initMarker}\n"
    "export fn plain()->i64{return PROT_READ}\n"
    "export fn otherValue()->i64{return other.PROT_READ}\n"
    "export class Namesake {}\n";
static const char ci_facade_source[]=
    "import \"./flags\" as flags\nimport {PROT_WRITE as W} from \"./flags\"\n"
    "export fn through()->i64{return flags.PROT_READ | W}\n"
    "export fn count()->i64{return flags.initCount()}\n";
static const char ci_hop_source[]=
    "import {through,count} from \"./facade\"\n"
    "export fn via()->i64{return through()}\n"
    "export fn countAgain()->i64{return count()}\n";
static const char ci_consumer_source[]=
    "import \"./flags\" as flags\nimport \"./other\" as other\n"
    "import {PROT_WRITE as W,PROT_READ as R} from \"./flags\"\n"
    "import {via,countAgain} from \"./hop\"\n"
    "export fn bits()->i64{return via()}\n"
    "export fn shadow()->i64{const R:i64=8;return R+W}\n"
    "export fn initializations()->i64{return countAgain()}\n"
    "export fn all()->i64{return flags.PROT_NONE+R+flags.PROT_WRITE+flags.PROT_EXEC}\n"
    "export fn otherValue()->i64{return other.PROT_READ}\n"
    "export fn callable()->fn()->i64{return bits}\n";
static const char *const ci_entry_names[]={"bits","shadow","initializations","all","otherValue","callable"};
static const char *const ci_logical_names[]={"other.xr","flags.xr","facade.xr","hop.xr"};
static const char *const ci_rejections[]={
    "export var VALUE:i64=1",
    "export const VALUE=1",
    "export const VALUE:f32=1.0",
    "fn initializeValue()->i64{return 3} export const VALUE:i64=initializeValue()",
    "export const {value}={value:1}",
    "import {initialized} from \"./flags\";export fn f()->i64{return initialized}",
    "import {PROT_READ} from \"./flags\";export fn f(){PROT_READ=2}",
    "import \"./flags\" as flags;export fn f(){flags.PROT_READ=2}",
    "import {PROT_READ} from \"./flags\";fn alter(ref x:i64){x=2} export fn f(){alter(ref PROT_READ)}"
};
#endif // XIR_EXPORTED_I64_CONST_CASES_H
