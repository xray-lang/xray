/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * process_cases.h - Observations around the real Process and Pipe classes
 *
 * KEY CONCEPT:
 *   Production methods are extracted verbatim. Only these exported observers
 *   and controlled child arguments belong to the fixture.
 */
#ifndef PROCESS_CASES_H
#define PROCESS_CASES_H
enum {
    PROCESS_ENTRY, PROCESS_SPAWN, PROCESS_DETACHED, PROCESS_CONFIGURED,
    PROCESS_STREAMS, PROCESS_ID, PROCESS_WAIT, PROCESS_TRY_WAIT, PROCESS_KILL,
    PROCESS_RAW_WAIT, PROCESS_RAW_KILL, PROCESS_PIPE_OPEN, PROCESS_PIPE_READ,
    PROCESS_PIPE_WRITE, PROCESS_PIPE_CLOSE, PROCESS_PIPE_CLOSE_READ,
    PROCESS_PIPE_CLOSE_WRITE, PROCESS_ENTRY_COUNT
};
#if defined(XR_PROCESS_SOURCE)
static const char *const process_names[] = {
    "", "spawnProcess", "spawnDetached", "spawnConfigured", "spawnStreams",
    "processId", "waitProcess", "tryProcess", "killProcess", "waitToken",
    "killToken", "openPipe", "readPipe", "writePipe", "closePipe",
    "closeRead", "closeWrite"
};
static const char process_wrappers[] =
    "\nexport fn spawnProcess(program:string,args:Array<string>)->Process?{return Process.spawn(program,args)}\n"
    "export fn spawnDetached(program:string,args:Array<string>)->Process?{return Process.spawn(program,args,ProcessOptions(detached:true))}\n"
    "export fn spawnConfigured(program:string,args:Array<string>,cwd:string)->Process?{\n"
    " var env:Map<string,string>=#{\"XR_PROCESS_CHILD\":\"exact-env\"}\n"
    " return Process.spawn(program,args,ProcessOptions(cwd:cwd,env:env))}\n"
    "export fn spawnStreams(program:string,args:Array<string>,input:Pipe,output:Pipe,error:Pipe)->Process?{\n"
    " return Process.spawn(program,args,ProcessOptions(stdin:input.readEnd(),stdout:output.writeEnd(),stderr:error.writeEnd()))}\n"
    "export fn processId(p:Process)->i64{return p.id()}\n"
    "export fn waitProcess(p:Process)->i64{return p.wait()}\n"
    "export fn tryProcess(p:Process)->i64?{return p.tryWait()}\n"
    "export fn killProcess(p:Process)->bool{return p.kill()}\n"
    "export fn waitToken(h:i64)->i64?{return __processTryWait(h)}\n"
    "export fn killToken(h:i64)->bool{return __processKill(h,15)}\n"
    "export fn openPipe()->Pipe?{return Pipe.open()}\n"
    "export fn readPipe(p:Pipe,n:i64)->Array<u8>?{return p.read(n)}\n"
    "export fn writePipe(p:Pipe,data:Array<u8>)->i64{return p.write(data)}\n"
    "export fn closePipe(p:Pipe)->bool{return p.close()}\n"
    "export fn closeRead(p:Pipe)->bool{return p.closeRead()}\n"
    "export fn closeWrite(p:Pipe)->bool{return p.closeWrite()}\n"
    "export fn ctfeSpawn()->i64{var args:Array<string>=[];return __processSpawn(\"\",args,null,null,null,null,null,null,false)}\n"
    "export fn ctfeWait()->i64?{return __processTryWait(1)}\n"
    "export fn ctfeKill()->bool{return __processKill(1,15)}\n";
#endif
static const unsigned char process_input[] = {0, 255, 13, 10, 42};
static const unsigned char process_error[] = {255, 0, 69};
#endif // PROCESS_CASES_H
