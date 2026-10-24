/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_library_integer_bits_cases.h - Ordinary parameterized functions across complete cold Libraries
 */
#ifndef XIR_LIBRARY_INTEGER_BITS_CASES_H
#define XIR_LIBRARY_INTEGER_BITS_CASES_H
static const char bits_base_source[]=
"export fn identity(value:i64)->i64{return value}\n";
static const char bits_library_source[]=
"import \"./base\" as base\nexport fn bitAnd(a:i64,b:i64)->i64{return base.identity(a)&base.identity(b)}\nexport fn bitOr(a:i64,b:i64)->i64{return base.identity(a)|base.identity(b)}\nexport fn bitXor(a:i64,b:i64)->i64{return base.identity(a)^base.identity(b)}\nexport fn bitNot(a:i64)->i64{return ~base.identity(a)}\nexport fn shiftLeft(a:i64,b:i64)->i64{return base.identity(a)<<base.identity(b)}\nexport fn shiftRight(a:i64,b:i64)->i64{return base.identity(a)>>base.identity(b)}\n";
static const char bits_root_source[]=
"import \"./bits\" as bits\nexport fn andEntry(a:i64,b:i64)->i64{return bits.bitAnd(a,b)}\nexport fn orEntry(a:i64,b:i64)->i64{return bits.bitOr(a,b)}\nexport fn xorEntry(a:i64,b:i64)->i64{return bits.bitXor(a,b)}\nexport fn notEntry(a:i64)->i64{return bits.bitNot(a)}\nexport fn leftEntry(a:i64,b:i64)->i64{return bits.shiftLeft(a,b)}\nexport fn rightEntry(a:i64,b:i64)->i64{return bits.shiftRight(a,b)}\n";
#endif // XIR_LIBRARY_INTEGER_BITS_CASES_H
