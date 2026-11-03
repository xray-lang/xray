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
static const char host_pin_text[]="export fn answer(cpu:i64)->bool{return __pinToCpu(cpu)}\n";
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
