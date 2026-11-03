/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * host_os_source_cases.h - Complete controlled canonical declaration inputs
 */
#ifndef HOST_OS_SOURCE_CASES_H
#define HOST_OS_SOURCE_CASES_H
/* These are controlled modules, not claims about complete stdlib import. The
 * extra field forces the Path read to select its actual named coordinate. */
static const char host_path_text[]=
 "export class Path{marker:i64\nraw:string\ninit(raw:string){this.marker=42;this.raw=raw}}\n";
static const char host_io_text[]=
 "import {Path} from path\n"
 "class Child extends Path{extra:string\ninit(raw:string){super(raw);this.extra=\"derived-owned-tail\"}}\n"
 "export fn make(raw:string)->Path{return Child(raw)}\n"
 "export fn chmodPath(path:Path,mode:i64)->bool{return __chmod(path,mode)}\n"
 "export fn chdirPath(path:Path)->bool{return __chdir(path)}\n"
 "fn borrowed(path:ref Path)->bool{return __chdir(path)}\n"
 "export fn viaRef(raw:string)->bool{var path:Path=Child(raw);return borrowed(ref path)}\n"
 "export fn ctfeDir()->bool{return __chdir(Child(\".\"))}\n"
 "export fn ctfeMode()->bool{return __chmod(Child(\".\"),384)}\n";
static const char host_stat_text[]=
 "import {Path} from path\n"
 "class Child extends Path{extra:string\ninit(raw:string){super(raw);this.extra=\"stat-owned-tail\"}}\n"
 "export fn make(raw:string)->Path{return Child(raw)}\n"
 "export fn query(path:Path)->bool{var r=__stat(path);if(r==null){return false;}\n"
 "return r!.size==3&&r!.mode>=0&&r!.mtime>0&&r!.atime>0&&r!.ctime>0&&"
 "(r!.uid==null||r!.uid!>=0)&&(r!.gid==null||r!.gid!>=0)&&r!.isFile&&!r!.isDir&&!r!.isSymlink}\n"
 "export fn snapshot(path:Path)->__FileStat?{return __stat(path)}\n"
 "export fn ctfeStat()->bool{return __stat(Child(\".\"))!=null}\n";
static const char host_io_bool_text[]=
 "import {Path} from path\n"
 "class Child extends Path{tail:string\ninit(raw:string){super(raw);this.tail=\"io-owned-tail\"}}\n"
 "export fn make(raw:string)->Path{return Child(raw)}\n"
 "export fn mkdirPath(path:Path)->bool{return __mkdir(path)}\n"
 "export fn rmdirPath(path:Path)->bool{return __rmdir(path)}\n"
 "export fn removePath(path:Path)->bool{return __remove(path)}\n"
 "export fn renamePath(old:Path,next:Path)->bool{return __rename(old,next)}\n"
 "export fn utimePath(path:Path)->bool{return __utimeNow(path)}\n"
 "fn refs(a:ref Path,b:ref Path)->bool{return __rename(a,b)}\n"
 "export fn byRef(a:string,b:string)->bool{var old:Path=Child(a);var next:Path=Child(b);return refs(ref old,ref next)}\n"
 "export fn ctfeMutation()->bool{return __mkdir(Child(\".\"))}\n";
static const char host_count_text[]="export fn answer()->i64{return __cpuCount()}\n";
static const char host_io_links_text[]=
 "import {Path} from path\n"
 "class Child extends Path{tail:string\ninit(raw:string){super(raw);this.tail=\"links-owned-tail\"}}\n"
 "export fn make(raw:string)->Path{return Child(raw)}\n"
 "export fn symlinkPath(source:Path,link:Path)->bool{return __symlink(source,link)}\n"
 "export fn readlinkPath(path:Path)->string?{return __readlink(path)}\n"
 "export fn realpathPath(path:Path)->string?{return __realpath(path)}\n"
 "fn fromRef(path:ref Path)->string?{return __realpath(path)}\n"
 "export fn byRef(raw:string)->string?{var path:Path=Child(raw);return fromRef(ref path)}\n"
 "export fn ctfeSymlink()->bool{return __symlink(Child(\".\"),Child(\".\"))}\n"
 "export fn ctfeReadlink()->string?{return __readlink(Child(\".\"))}\n"
 "export fn ctfeRealpath()->string?{return __realpath(Child(\".\"))}\n";
static const char host_pin_text[]="export fn answer(cpu:i64)->bool{return __pinToCpu(cpu)}\n";
static const char host_io_temp_text[]=
 "import {Path} from path\n"
 "export fn directory(root:string)->string?{return __makeTempDir(root)}\n"
 "export fn file(root:string)->string?{return __makeTempFile(root)}\n"
 "export fn nested(v:Array<Array<string?>>)->Array<Array<string?>>{return v}\n"
 "export fn ctfeDirectory()->string?{return __makeTempDir(\".\")}\n"
 "export fn ctfeFile()->string?{return __makeTempFile(\".\")}\n";
static const char host_signal_frontier_text[]=
 "fn plain(n:i64)->i64{return n}\n"
 "fn read(v:Slice<u8>)->i64{return (v[0] as i64)+(v[1] as i64)}\n"
 "fn countRef(v:ref Array<u8>)->i64{return len(v)}\n"
 "export fn parent()->i64{defer{assert(plain(42)==42)};assert(__cpuCount()>0);"
 "var bytes:Array<u8>=[20,22];const loan:Slice<u8>=bytes[:];const total=read(loan);"
 "const count=countRef(ref bytes);bytes=[42];return plain(total+count-2)}\n"
 "export fn failed()->i64{assert(false);return 0}\n"
 "enum FrontierFailure{Raised}\n"
 "fn raise() throws(FrontierFailure)->i64{throw FrontierFailure.Raised}\n"
 "export fn heldParent()->i64{assert(__cpuCount()>0);const task=go raise();"
 "try{return try await task}catch(FrontierFailure.Raised){return 42}}\n"
 "const initialValue:i64=parent()\n"
 "export fn initialized()->i64{return initialValue}\n";
static const char host_system_text[]=
 "export fn uid()->i64?{return __uid()}\n"
 "export fn gid()->i64?{return __gid()}\n"
 "export fn parent()->i64{return __ppid()}\n"
 "export fn signal(pid:i64,signal:i64)->bool{return __kill(pid,signal)}\n"
 "export fn total()->i64{return __totalMemory()}\n"
 "export fn available()->i64{return __freeMemory()}\n"
 "export fn elapsed()->f64{return __uptime()}\n"
 "export fn load()->Array<f64>{return __loadavg()}\n";
static const char *const host_io_controls[]={
 "import {Path} from path\nclass Wrong{raw:string\ninit(raw:string){this.raw=raw}}\nexport fn wrong(p:Wrong)->bool{return __chdir(p)}\n",
 "import {Path} from path\nexport fn conflict(v:Slice<u8>,p:Path)->i64{const changed=__chdir(p);return len(v)}\n",
 "export fn noRead(p:Path)->bool{return __chdir(p)}\n",
 "import {Path} from path\nexport fn borrowed(p:ref Path)->bool{return __chdir(p)}\n",
 ("import {Path} from path\nclass Child extends Path{items:Array<u8>\ninit(){super(\".\");this.items=[1 as u8]}}\n"
 "export fn fieldClosed()->i64{var p=Child();var loan:Slice<u8>=p.items[:];__chdir(p);return len(loan)}\n")
};
static const XrXirStatus host_io_statuses[]={XR_XIR_BAD_TYPE,XR_XIR_UNRESOLVED,XR_XIR_BAD_TYPE,XR_XIR_OK,XR_XIR_BAD_TYPE};
#endif // HOST_OS_SOURCE_CASES_H
