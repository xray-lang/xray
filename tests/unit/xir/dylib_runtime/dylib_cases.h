/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * dylib_cases.h - Public observations around the unchanged Dylib class
 *
 * KEY CONCEPT:
 *   The complete Dylib and Path classes come from actual standard sources.
 *   These wrappers expose results without granting raw handle authority.
 */
#ifndef DYLIB_CASES_H
#define DYLIB_CASES_H
enum {
    DYLIB_ENTRY,DYLIB_PATH,DYLIB_OPEN,DYLIB_SYMBOL,DYLIB_CLOSE,DYLIB_ERROR,
    DYLIB_TOKEN,DYLIB_RAW_SYMBOL,DYLIB_RAW_CLOSE,DYLIB_PEEK,DYLIB_OFFSET,
    DYLIB_DEFER,DYLIB_ENTRY_COUNT
};
#if defined(XR_DYLIB_SOURCE)
static const char *const dylib_names[]={"","makePath","openLibrary","lookupSymbol","closeLibrary","loaderError",
    "token","symbolToken","closeToken","peek","offset","deferred"};
static const char dylib_wrappers[]=
    "\nexport fn makePath(raw:string)->Path{return Path(raw)}\n"
    "export fn openLibrary(path:Path)->Dylib?{return Dylib.open(path)}\n"
    "export fn lookupSymbol(lib:Dylib,name:string)->Ptr<u8>?{return lib.symbol(name)}\n"
    "export fn closeLibrary(lib:Dylib)->bool{return lib.close()}\n"
    "export fn loaderError()->string{return Dylib.lastError()}\n"
    "export fn token(lib:Dylib)->i64{return lib._handle}\n"
    "export fn symbolToken(handle:i64,name:string)->Ptr<u8>?{return __dylibSymbol(handle,name)}\n"
    "export fn closeToken(handle:i64)->bool{return __dylibClose(handle)}\n"
    "export fn peek(p:Ptr<u8>)->i64{unsafe{return p[0] as i64}}\n"
    "export fn offset(p:Ptr<u8>)->Ptr<u8>{return p.offset(1)}\n"
    "export fn deferred(path:Path)->Ptr<u8>?{var opened=Dylib.open(path);if(opened==null){return null}\n"
    " var lib=opened!;defer{lib.close()};return lib.symbol(\"xray_dylib_bytes\")}\n"
    "export fn ctfeOpen()->i64{return __dylibOpen(Path(\"absent\"))}\n"
    "export fn ctfeSymbol()->Ptr<u8>?{return __dylibSymbol(1,\"absent\")}\n"
    "export fn ctfeClose()->bool{return __dylibClose(1)}\n"
    "export fn ctfeError()->string{return __dylibLastError()}\n";
#endif
#if defined(XR_OS_WINDOWS)
static const unsigned char dylib_expected[]={0,255,42};
#endif
#endif // DYLIB_CASES_H
