/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * pipe_cases.h - Entrypoints around the unchanged public Pipe implementation
 *
 * KEY CONCEPT:
 *   The producer extracts the actual complete Pipe class from sys.xr. These
 *   functions observe that implementation without replacing its methods.
 */
#ifndef PIPE_CASES_H
#define PIPE_CASES_H
enum { PIPE_ENTRY,PIPE_OPEN,PIPE_READ,PIPE_WRITE,PIPE_CLOSE_READ,PIPE_CLOSE_WRITE,
 PIPE_CLOSE_ALL,PIPE_ENDS,PIPE_RAW_READ,PIPE_RAW_WRITE,PIPE_RAW_CLOSE,PIPE_HANDOFF,
 PIPE_DEFER_ENDS,PIPE_ENTRY_COUNT };
#if defined(XR_PIPE_SOURCE)
static const char *const pipe_names[]={"","openPipe","readPipe","writePipe","closeRead",
 "closeWrite","closeAll","ends","readToken","writeToken","closeToken","handoff","deferEnds"};
static const char pipe_wrappers[]=
 "\nexport fn openPipe()->Pipe?{return Pipe.open()}\n"
 "export fn readPipe(p:Pipe,n:i64)->Array<u8>?{return p.read(n)}\n"
 "export fn writePipe(p:Pipe,data:Array<u8>)->i64{return p.write(data)}\n"
 "export fn closeRead(p:Pipe)->bool{return p.closeRead()}\n"
 "export fn closeWrite(p:Pipe)->bool{return p.closeWrite()}\n"
 "export fn closeAll(p:Pipe)->bool{return p.close()}\n"
 "export fn ends(p:Pipe)->(i64,i64){return(p.readEnd(),p.writeEnd())}\n"
 "export fn readToken(h:i64,n:i64)->Array<u8>?{return __pipeRead(h,n)}\n"
 "export fn writeToken(h:i64,data:Array<u8>)->i64{return __pipeWrite(h,data)}\n"
 "export fn closeToken(h:i64)->bool{return __pipeClose(h)}\n"
 "fn sendPipe(h:i64)->i64{var data:Array<u8>=[0,255,13,10,42];return __pipeWrite(h,data)}\n"
 "export fn handoff()->i64{var opened=Pipe.open();assert(opened!=null);var p=opened!\n"
 " var task=go sendPipe(p.writeEnd());var data=p.read(5);var written=await task\n"
 " assert(written==5&&data!=null);var bytes=data!\n"
 " assert(len(bytes)==5&&bytes[0]==0&&bytes[1]==255&&bytes[2]==13&&bytes[3]==10&&bytes[4]==42)\n"
 " assert(p.close());return 42}\n"
 "export fn deferEnds()->(i64,i64){var opened=Pipe.open();assert(opened!=null);var p=opened!\n"
 " var result=(p.readEnd(),p.writeEnd());defer{__pipeClose(result.0);__pipeClose(result.1)};return result}\n"
 "export fn ctfeOpen()->(i64,i64)?{return __pipeOpen()}\n"
 "export fn ctfeRead()->Array<u8>?{return __pipeRead(1,0)}\n"
 "export fn ctfeWrite()->i64{var data:Array<u8>=[];return __pipeWrite(1,data)}\n"
 "export fn ctfeClose()->bool{return __pipeClose(1)}\n";
#endif
static const unsigned char pipe_bytes[]={0,255,13,10,42};
#endif // PIPE_CASES_H
