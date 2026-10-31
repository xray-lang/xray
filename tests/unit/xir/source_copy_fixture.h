/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_copy_fixture.h - Complete ordinary copy, nested COW, identity and lexical-call sources.
 */
#ifndef SOURCE_COPY_FIXTURE_H
#define SOURCE_COPY_FIXTURE_H
static const char source_copy_fixture[] =
    "class Box{value:i64;init(value:i64){this.value=value}}\n"
    "fn copy(x:i64)->i64{return x+1}\n"
    "export fn answer()->i64{\n"
    "var values=[1,2];var saved=copy values;values[0]=9\n"
    "var nested=[[3]];var other=copy nested;nested[0][0]=8\n"
    "var item=Box(4);var alias=copy item;alias.value=5\n"
    "var record={value:[7]};var part=copy record.value;record.value[0]=10\n"
    "var tuple=([11],[12]);var field=copy tuple.0;field[0]=13\n"
    "var scalar=41;var scalarCopy=copy scalar;scalar=0\n"
    "return saved[0]==1 && other[0][0]==3 && item.value==5 && part[0]==7 && "
    "tuple.0[0]==11 && field[0]==13 && scalarCopy==41 && copy(41)==42 ?42:0}\n"
    "export fn kept()->string{var a=[\"A\\0B\",\"other\"];var b=copy a;"
    "a[0]=\"changed\";return b[0]}\n";
#endif
