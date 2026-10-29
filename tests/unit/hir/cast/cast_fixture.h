/* Complete explicit numeric and scalar text conversions with independent byte oracles. */
#ifndef HIR_CAST_FIXTURE_H
#define HIR_CAST_FIXTURE_H
#include "xray_source.h"
static const char cast_source[] =
    "fn casts_i8(x:i8){print(\"i8\",x as i8,x as i16,x as i32,x as i64,x as u8,x as u16,x as u32,x as u64,x as f32,x as f64)}\n"
    "fn casts_i16(x:i16){print(\"i16\",x as i8,x as i16,x as i32,x as i64,x as u8,x as u16,x as u32,x as u64,x as f32,x as f64)}\n"
    "fn casts_i32(x:i32){print(\"i32\",x as i8,x as i16,x as i32,x as i64,x as u8,x as u16,x as u32,x as u64,x as f32,x as f64)}\n"
    "fn casts_i64(x:i64){print(\"i64\",x as i8,x as i16,x as i32,x as i64,x as u8,x as u16,x as u32,x as u64,x as f32,x as f64)}\n"
    "fn casts_u8(x:u8){print(\"u8\",x as i8,x as i16,x as i32,x as i64,x as u8,x as u16,x as u32,x as u64,x as f32,x as f64)}\n"
    "fn casts_u16(x:u16){print(\"u16\",x as i8,x as i16,x as i32,x as i64,x as u8,x as u16,x as u32,x as u64,x as f32,x as f64)}\n"
    "fn casts_u32(x:u32){print(\"u32\",x as i8,x as i16,x as i32,x as i64,x as u8,x as u16,x as u32,x as u64,x as f32,x as f64)}\n"
    "fn casts_u64(x:u64){print(\"u64\",x as i8,x as i16,x as i32,x as i64,x as u8,x as u16,x as u32,x as u64,x as f32,x as f64)}\n"
    "fn casts_f32(x:f32){print(\"f32\",x as i8,x as i16,x as i32,x as i64,x as u8,x as u16,x as u32,x as u64,x as f32,x as f64)}\n"
    "fn casts_f64(x:f64){print(\"f64\",x as i8,x as i16,x as i32,x as i64,x as u8,x as u16,x as u32,x as u64,x as f32,x as f64)}\n"
    "casts_i8(-1)\n"
    "casts_i16(-1)\n"
    "casts_i32(-1)\n"
    "casts_i64(-1)\n"
    "casts_u8(255)\n"
    "casts_u16(255)\n"
    "casts_u32(255)\n"
    "casts_u64(255)\n"
    "casts_f32(5.75)\n"
    "casts_f64(5.75)\n"
    "print(18446744073709551615 as i8,(18446744073709551615) as i64,18446744073709551615 as u64,9223372036854775808 as i64)\n"
    "fn rounded(x:f64)->f64{return (x as f32) as f64}\n"
    "print(rounded(16777217.0),rounded(16777219.0))\n"
    "fn narrowedZero(x:f64)->f32{return x as f32}\n"
    "print(1.0/(narrowedZero(-0.0) as f64)<0.0)\n"
    "var text=\"a\\0\344\270\255\"\n"
    "print(text as string,string(text),\"${text}\")\n"
    "print(true as string,string(false),\"${true}\",'\344\270\255' as string,string('\344\270\255'),\"${'\344\270\255'}\")\n"
    "print(-42 as string,string(5.75),\"${(-42 as i8)} ${5.75 as f32}\")\n"
    "var calls=0\n"
    "fn once()->i64{calls=calls+1;return 7}\n"
    "print(once() as i8,once() as string,string(once()),\"${once()}\",calls)\n"
    "fn textOnce()->string{calls=calls+1;return text}\n"
    "print(textOnce() as string,string(textOnce()),\"${textOnce()}\",calls)\n"
    ;
static const char cast_expected[] =
    "i8 -1 -1 -1 -1 255 65535 4294967295 18446744073709551615 -1.0 -1.0\n"
    "i16 -1 -1 -1 -1 255 65535 4294967295 18446744073709551615 -1.0 -1.0\n"
    "i32 -1 -1 -1 -1 255 65535 4294967295 18446744073709551615 -1.0 -1.0\n"
    "i64 -1 -1 -1 -1 255 65535 4294967295 18446744073709551615 -1.0 -1.0\n"
    "u8 -1 255 255 255 255 255 255 255 255.0 255.0\n"
    "u16 -1 255 255 255 255 255 255 255 255.0 255.0\n"
    "u32 -1 255 255 255 255 255 255 255 255.0 255.0\n"
    "u64 -1 255 255 255 255 255 255 255 255.0 255.0\n"
    "f32 5 5 5 5 5 5 5 5 5.75 5.75\n"
    "f64 5 5 5 5 5 5 5 5 5.75 5.75\n"
    "-1 -1 18446744073709551615 -9223372036854775808\n"
    "16777216.0 16777220.0\n"
    "true\n"
    "a\000\344\270\255 a\000\344\270\255 a\000\344\270\255\n"
    "true false true \344\270\255 \344\270\255 \344\270\255\n"
    "-42 5.75 -42 5.75\n"
    "7 7 7 7 4\n"
    "a\000\344\270\255 a\000\344\270\255 a\000\344\270\255 7\n"
    ;
typedef struct CastRefusal {const char *name,*source,*message;XrSourceStatus status;int line;} CastRefusal;
static const CastRefusal cast_refusals[] = {
    {"bool_operand","fn f(x:bool)->i64{return x as i64}\n","numeric cast requires a concrete numeric operand",XR_SOURCE_REJECTED,1},
    {"rune_operand","fn f(x:rune)->u32{return x as u32}\n","numeric cast requires a concrete numeric operand",XR_SOURCE_REJECTED,1},
    {"string_operand","fn f(x:string)->i64{return x as i64}\n","numeric cast requires a concrete numeric operand",XR_SOURCE_REJECTED,1},
    {"nullable_operand","fn f(x:i64?)->i64{return x as i64}\n","numeric cast requires a concrete numeric operand",XR_SOURCE_REJECTED,1},
    {"generic_operand","fn f<T>(x:T)->i64{return x as i64}\n","numeric cast requires a concrete numeric operand",XR_SOURCE_REJECTED,1},
    {"bool_target","var answer=unknown as bool\n","numeric cast requires a concrete nonnullable numeric target",XR_SOURCE_REJECTED,1},
    {"rune_target","var answer=unknown as rune\n","numeric cast requires a concrete nonnullable numeric target",XR_SOURCE_REJECTED,1},
    {"safe_target","var answer=unknown as i64?\n","numeric cast requires a concrete nonnullable numeric target",XR_SOURCE_REJECTED,1},
    {"safe_string","var answer=unknown as string?\n","numeric cast requires a concrete nonnullable numeric target",XR_SOURCE_REJECTED,1},
    {"unknown_target","var answer=unknown as MissingType\n","name does not resolve to an admitted nominal type",XR_SOURCE_REJECTED,1},
    {"text_nullable","fn f(x:i64?)->string{return x as string}\n","value has no admitted string conversion",XR_SOURCE_REJECTED,1},
    {"text_array","var answer=string([1])\n","value has no admitted string conversion",XR_SOURCE_REJECTED,1},
    {"text_generic","fn f<T>(x:T)->string{return string(x)}\n","value has no admitted string conversion",XR_SOURCE_REJECTED,1},
    {"interpolated_nullable","fn f(x:i64?)->string{return \"${x}\"}\n","interpolated value has no admitted string conversion",XR_SOURCE_REJECTED,1},
    {"interpolated_array","var answer=\"${[1]}\"\n","interpolated value has no admitted string conversion",XR_SOURCE_REJECTED,1},
    {"string_empty","var answer=string()\n","expected expression",XR_SOURCE_INVALID,2},
    {"string_two","var answer=string(unknown,unknown)\n","expected ')' after type cast argument",XR_SOURCE_INVALID,1},
    {"string_type_argument","var answer=string<i64>(unknown)\n","expected '(' after type cast",XR_SOURCE_INVALID,1},
};
typedef struct CastPanic {const char *name,*source;} CastPanic;
static const CastPanic cast_panics[] = {
    {"nan","print((0.0/0.0) as i64)\n"},
    {"infinity","print((1.0/0.0) as i64)\n"},
    {"out_of_range","print(9223372036854775808.0 as i64)\n"},
};
#endif // HIR_CAST_FIXTURE_H
