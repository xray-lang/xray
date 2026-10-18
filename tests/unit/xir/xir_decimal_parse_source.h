/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_decimal_parse_source.h - Exact shared ordinary Source test input
 */
#ifndef XIR_DECIMAL_PARSE_SOURCE_H
#define XIR_DECIMAL_PARSE_SOURCE_H
static const char decimal_parse_source[]=
"export fn inspect(text:string)->f64? {return f64.tryParse(text)}\n"
        "export fn main()->i64 {const folded=comptime inspect(\"1.7976931348623157e308\");"
        "const ordinary=inspect(\"1.7976931348623157e308\");"
        "return (folded!=null && ordinary!=null && folded! == 1.7976931348623157e308 && ordinary! == folded! && inspect(\"bad\")==null)?42:0}\n"
        "export fn observed()->f64? {return inspect(\"1.7976931348623157e308\")}\n";
#endif // XIR_DECIMAL_PARSE_SOURCE_H
