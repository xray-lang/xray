/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * slice_u8_default_class_cases.h - Scoped default Class body and Cell storage cases
 */
#ifndef SLICE_U8_DEFAULT_CLASS_CASES_H
#define SLICE_U8_DEFAULT_CLASS_CASES_H
#include "xir/xxir_view_access_internal.h"
#define DEFAULT_CLASS_OPTIONS "class Options{value:i64;init(){this.value=7}}\n"
#define DEFAULT_CLASS_USE "fn use(v:Slice<u8>,options:Options=Options())->i64{return len(v)+options.value}\n"
static const char default_class_positive[]=DEFAULT_CLASS_OPTIONS DEFAULT_CLASS_USE
 "export fn storage_receiver()->i64{var text=\"abc\";var view:Slice<u8>=text.bytes();return use(view)-9}";
static const char default_class_borrowed_input[]=DEFAULT_CLASS_OPTIONS DEFAULT_CLASS_USE
 "export fn incoming(v:Slice<u8>)->i64{return use(v)}";
static const char default_class_existing_alias[]=
 "class Box{items:Array<u8>;init(){this.items=[1]}}\nconst shared=Box()\nfn same()->Box{return shared}\n"
 "fn mutate(v:Slice<u8>,box:Box=same())->i64{box.items[0]=9;return len(v)}\n"
 "export fn same_object_borrow(v:Slice<u8>)->i64{return mutate(v)}";
/* The incoming view may refer to shared.items; the default returns that existing object. */
static const char default_class_existing_alias_read[]=
 "class Box{items:Array<u8>;init(){this.items=[1]}}\nconst shared=Box()\nfn same()->Box{return shared}\n"
 "fn read(v:Slice<u8>,box:Box=same())->i64{return len(v)+len(box.items)}\n"
 "export fn same_object_borrow(v:Slice<u8>)->i64{return read(v)}";
static const char default_class_unknown[]=DEFAULT_CLASS_OPTIONS
 "fn make()->Options{return Options()}\nfn factory()->fn()->Options{return make}\n"
 "fn use(v:Slice<u8>,options:Options)->i64{return len(v)+options.value}\n"
 "export fn unknown()->i64{var text=\"abc\";var view:Slice<u8>=text.bytes();const f=factory();return use(view,f())}";
static const char default_class_opaque[]=DEFAULT_CLASS_OPTIONS
 "fn make()->Options{return Options()}\nfn factory()->fn()->Options{return make}\n"
 "fn hidden()->Options{const f=factory();return f()}\n"
 "fn use(v:Slice<u8>,options:Options)->i64{return len(v)+options.value}\n"
 "export fn opaque()->i64{var text=\"abc\";var view:Slice<u8>=text.bytes();return use(view,hidden())}";
static const char default_class_native[]=DEFAULT_CLASS_OPTIONS
 "fn hidden()->Options{print(\"opaque\");return Options()}\n"
 "fn use(v:Slice<u8>,options:Options)->i64{return len(v)+options.value}\n"
 "export fn native()->i64{var text=\"abc\";var view:Slice<u8>=text.bytes();return use(view,hidden())}";
static const char default_class_wrong_type[]=DEFAULT_CLASS_OPTIONS DEFAULT_CLASS_USE
 "export fn wrong()->i64{var text=\"abc\";var view:Slice<u8>=text.bytes();return use(view,123)}";

static const char imported_default_library[]=
 "export class Options{value:i64;init(){this.value=7}}\n"
 "export fn use(v:Slice<u8>,options:Options=Options())->i64{return len(v)+options.value}\n"
 "export fn make()->Options{return Options()}\n";
static const char imported_default_root[]=
 "import {use} from \"./lib\"\n"
 "export fn storage_receiver()->i64{var text=\"abc\";var view:Slice<u8>=text.bytes();return use(view)-9}\n";
static const char imported_default_generic_library[]=
 "export class Options{value:i64;init(){this.value=7}}\n"
 "export fn use<T>(v:Slice<u8>,item:T,options:Options=Options())->i64{return len(v)-2}\n";
static const char imported_default_generic_root[]=
 "import {use} from \"./lib\"\n"
 "export fn storage_receiver()->i64{var text=\"abc\";var view:Slice<u8>=text.bytes();return use<i64>(view,7)}\n";
static const char imported_default_ordinary_root[]=
 "import {use,make} from \"./lib\"\n"
 "export fn storage_receiver()->i64{var text=\"abc\";var view:Slice<u8>=text.bytes();return use(view,make())-9}\n";
static const char imported_default_slot_library[]=
 "var counter:i64=7\n"
 "export class Options{value:i64;init(){this.value=counter}}\n"
 "export fn use(v:Slice<u8>,options:Options=Options())->i64{return len(v)+options.value}\n";

static const char imported_default_callable_library[]=
 "export fn use<T>(value:T,cb:fn(T)->T=fn(x:T)->T{return x})->T{return cb(value)}\n";
static const char imported_default_callable_root[]=
 "import {use} from \"./lib\"\nexport fn storage_receiver()->i64{return use<i64>(1)}\n";

#endif
