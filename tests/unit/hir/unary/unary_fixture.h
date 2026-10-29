/* Numeric unary syntax and its independent byte oracle. */
#ifndef HIR_UNARY_FIXTURE_H
#define HIR_UNARY_FIXTURE_H
static const char unary_source[] =
    "fn neg8(x:i8)->i8{return -x}\n"
    "fn neg16(x:i16)->i16{return -x}\n"
    "fn neg32(x:i32)->i32{return -x}\n"
    "fn neg64(x:i64)->i64{return -x}\n"
    "fn negu8(x:u8)->u8{return -x}\n"
    "fn negu16(x:u16)->u16{return -x}\n"
    "fn negu32(x:u32)->u32{return -x}\n"
    "fn negu64(x:u64)->u64{return -x}\n"
    "fn inv8(x:i8)->i8{return ~x}\n"
    "fn inv16(x:i16)->i16{return ~x}\n"
    "fn inv32(x:i32)->i32{return ~x}\n"
    "fn inv64(x:i64)->i64{return ~x}\n"
    "fn invu8(x:u8)->u8{return ~x}\n"
    "fn invu16(x:u16)->u16{return ~x}\n"
    "fn invu32(x:u32)->u32{return ~x}\n"
    "fn invu64(x:u64)->u64{return ~x}\n"
    "fn negf32(x:f32)->f32{return -x}\n"
    "fn negf64(x:f64)->f64{return -x}\n"
    "fn widened(x:i8)->i16{return -x}\n"
    "var calls=0\n"
    "fn once()->i64{calls=calls+1;return 21}\n"
    "print(neg8(-128),neg16(-32768),neg32(-2147483648),neg64(-9223372036854775808))\n"
    "print(negu8(1),negu16(1),negu32(1),negu64(1))\n"
    "print(inv8(0),inv16(0),inv32(0),inv64(0))\n"
    "print(invu8(0),invu16(0),invu32(0),invu64(0))\n"
    "var narrow:u8=~1\n"
    "print(narrow)\n"
    "print(widened(-128),-once(),calls)\n"
    "print(negf32(1.5),negf64(2.5),1.0/negf64(0.0)<0.0)\n"
    ;
static const char unary_expected[] =
    "-128 -32768 -2147483648 -9223372036854775808\n"
    "255 65535 4294967295 18446744073709551615\n"
    "-1 -1 -1 -1\n"
    "255 65535 4294967295 18446744073709551615\n"
    "254\n"
    "-128 -21 1\n"
    "-1.5 -2.5 true\n"
    ;
#endif // HIR_UNARY_FIXTURE_H
