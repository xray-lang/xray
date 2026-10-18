"""Execute independent entry expectations without replacing missing product capabilities."""
from __future__ import annotations
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def fact(path):
    path=Path(path);data=path.read_bytes()
    return {"path":str(path),"bytes":len(data),"sha256":hashlib.sha256(data).hexdigest()}


def job_processes(job):
    if job is None:return 1
    class Accounting(ctypes.Structure):
        _fields_=[("times",ctypes.c_longlong*4),("faults",ctypes.c_uint32),
                  ("total",ctypes.c_uint32),("active",ctypes.c_uint32),("terminated",ctypes.c_uint32)]
    state=Accounting();kernel=ctypes.WinDLL("kernel32",use_last_error=True)
    kernel.QueryInformationJobObject.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_void_p,ctypes.c_uint32,ctypes.c_void_p]
    kernel.QueryInformationJobObject.restype=ctypes.c_int
    if not kernel.QueryInformationJobObject(job,1,ctypes.byref(state),ctypes.sizeof(state),None):raise ctypes.WinError(ctypes.get_last_error())
    return state.active


class Runner:
    def __init__(self,args):
        self.args=args;self.source=Path(__file__).with_name("expectations.json")
        self.original=json.loads(self.source.read_text(encoding="utf8"))
        assert [x["id"] for x in self.original["cases"]]==[f"C{i:02}" for i in range(1,26)]
        args.output.mkdir(parents=True,exist_ok=True)
        self.folder=Path(tempfile.mkdtemp(prefix="run-",dir=args.output))
        self.env=dict(os.environ,XRAY_STDLIB_PATH=str(args.root/"stdlib"),NO_COLOR="1")
        self.record={"schema":1,"group":args.group,"expectations":fact(self.source),"cli_before":fact(args.cli),
                     "source_root":str(args.root),"commands":[],"cases":[],"all25_qualified":False}
        self.input_paths=[Path(__file__),self.source,args.api,args.physical,args.injected,args.resources,
            args.root/"tests/cli/run_program_test_entry_tests.py",args.root/"tests/unit/xir/run_cli_dependencies.py",
            args.root/"tests/unit/xir/run_source_dependencies.py",args.root/"tests/lib/xraytest/windows_owned_job.py",
            *sorted((args.root/"stdlib").rglob("*.xr"))]
        for name in ["xir_cli_product","public_source_program","xir_compile_owner"]:
            self.input_paths += [p for p in sorted((args.root/"tests/fixtures"/name).rglob("*")) if p.is_file()]
        self.record["inputs_before"]=[fact(p) for p in self.input_paths]
        self.job=None
        if os.name=="nt":
            sys.path.insert(0,str(args.root/"tests/lib"))
            from xraytest.windows_owned_job import establish_job
            self.job=establish_job()

    def save(self):
        path=self.folder/"record.json"
        path.write_text(json.dumps(self.record,ensure_ascii=False,indent=2)+"\n",encoding="utf8",newline="\n")
        (self.args.output/"latest.json").write_text(json.dumps({"record":fact(path)},indent=2)+"\n",encoding="utf8",newline="\n")

    def run(self,label,command,*,expected=0,stdout=None,stderr=None,stdin=b"",cwd=None):
        command=list(map(str,command));row={"case":self.current,"label":label,"command":command,"cwd":str(cwd or self.args.root),
            "expected_exit":expected,"expected_stdout":None if stdout is None else stdout.hex(),
            "expected_stderr":None if stderr is None else stderr.hex(),"stdin_hex":stdin.hex()}
        self.record["commands"].append(row);self.save();start=time.monotonic()
        sources=[fact(p) for p in sorted((self.folder/self.current).rglob("*.xr"))]
        row["owned_sources_before"]=sources
        process=subprocess.Popen(command,cwd=cwd or self.args.root,env=self.env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:out,err=process.communicate(stdin,timeout=120)
        except subprocess.TimeoutExpired:
            process.kill();out,err=process.communicate();row.update({"timed_out":True,"exit":process.returncode})
            self.save();raise SystemExit("Entry child deadline expired; parent Job owns all descendants")
        index=len(self.record["commands"])
        for name,data in [("stdout",out),("stderr",err)]:
            path=self.folder/f"{index:03}-{label}.{name}";path.write_bytes(data);row[name]=fact(path)
        row.update({"pid":process.pid,"exit":process.returncode,"seconds":time.monotonic()-start})
        row["owned_sources_after"]=[fact(Path(p["path"])) for p in sources]
        assert row["owned_sources_after"]==sources,"Entry source bytes changed while a consumer ran"
        row["passed"]=(process.returncode==expected and (stdout is None or out==stdout) and (stderr is None or err==stderr))
        self.save()
        assert row["passed"],(label,process.returncode,out,err)
        assert b"AddressSanitizer" not in err and b"runtime error:" not in err,(label,err)
        return out,err

    def sources(self,case):
        folder=self.folder/case["id"];folder.mkdir(exist_ok=True)
        for name,text in case.get("sources",{}).items():
            path=folder/name;path.write_text(text,encoding="utf8",newline="\n")
        return folder,folder/"root.xr"

    def exact(self,case,command,label="literal",cwd=None):
        expected=case["expected"]
        return self.run(label,command,expected=expected["exit"],stdout=bytes.fromhex(expected["stdout_hex"]),
                        stderr=bytes.fromhex(expected["stderr_hex"]),stdin=bytes.fromhex(case.get("stdin_hex","")),cwd=cwd)

    def unit(self,name):
        return self.run(name,[sys.executable,"-X","utf8",self.args.root/"tests/cli/run_program_test_entry_tests.py",
                             "--xray",self.args.cli,"ProgramTestCommand."+name])

    def native(self,case,entry,folder):
        generated=folder/"program.c";binary=folder/"program.exe"
        self.run("emit",[self.args.cli,"build","--c-only",entry,"-o",generated],stderr=b"")
        text=generated.read_bytes()
        assert b"XrXirProgramSpec" in text and b"XrProto" not in text and b"xrt_vm_run" not in text
        import re
        assert not re.search(rb"\(\s*\{",text),"Generated C must be portable C11"
        self.run("native-build",[self.args.cli,"build","--native",entry,"-o",binary],stderr=b"")
        identity=fact(binary);self.record.setdefault("native_products",[]).append(identity)
        for repeat in range(2):self.exact(case,[binary],f"native-{repeat}")
        assert fact(binary)==identity

    def case(self,case):
        ident=case["id"];folder,entry=self.sources(case);cli=self.args.cli;root=self.args.root
        if self.args.group=="api":
            if ident=="C18":
                out,_=self.run("original-policy-FI",[self.args.injected,root/"tests/fixtures/xir_compile_owner/root.xr"])
                assert out=='owner中-ok 42 true\nRUN_OWNER: real default policy, five phase OOM boundaries, one ledger, budget and physical zero PASS\n'.encode()
                out,_=self.run("same-owner-resources",[self.args.resources,"--repl-budget"])
                assert b"exact/minus-one/plus-one" in out and b"physical=0/0" in out
            else:
                self.run("public-only",[self.args.api,ident],stderr=b"")
                out,_=self.run("physical",[self.args.physical,ident],stderr=b"")
                assert b"physical=0/0" in out
                if ident=="C15":self.unit("test_timer_and_cpu_timeouts_release_the_invocation")
            return
        if ident in {"C01","C02","C05","C06","C07","C10","C11"}:
            if ident=="C11":entry=root/case["fixture"]
            if self.args.group=="native":
                if ident=="C01":
                    unicode=folder/"路径 空格";unicode.mkdir()
                    for name,text in case["sources"].items():(unicode/name).write_text(text,encoding="utf8",newline="\n")
                    folder=unicode;entry=folder/"root.xr"
                self.native(case,entry,folder);return
            for repeat in range(2):self.exact(case,[cli,"run",entry],f"run-{repeat}")
            if ident=="C01":
                self.exact(case,[cli,entry],"implicit")
                unicode=folder/"路径 空格";unicode.mkdir()
                for name,text in case["sources"].items():(unicode/name).write_text(text,encoding="utf8",newline="\n")
                self.exact(case,[cli,"run",unicode/"root.xr"],"unicode")
            return
        if ident=="C03":
            for command in ["run","check","build"]:
                args=[cli,command,entry]
                if command=="build":args += ["--c-only","-o",folder/"refused.c"]
                _,error=self.run(command,args,expected=1,stdout=b"")
                assert all(x.encode() in error for x in case["expected"]["stderr_contains"]) and b"\x1b[" not in error
            return
        if ident=="C04":
            self.run("parse-only",[cli,"check","--syntax-only",entry],expected=case["expected"]["syntax_only_exit"])
            self.run("semantic",[cli,"check",entry],expected=case["expected"]["semantic_exit"],stdout=b"")
            self.run("run",[cli,"run",entry],expected=case["expected"]["run_exit"],stdout=b"");return
        if ident=="C08":self.exact(case,[cli,*case["argv"]]);return
        if ident=="C09":self.exact(case,[cli,"run",entry,"--","argument"]);return
        if ident=="C12":
            sentinel=bytes.fromhex(case["expected"]["output_canary_hex"])
            for command,options,suffix in [("build",["--c-only"],".c"),("build",["--native"],".exe"),("deps",["--json"],".json")]:
                output=folder/(command+suffix);output.write_bytes(sentinel);before=set(folder.iterdir())
                self.run(command+suffix,[cli,command,*options,entry,"-o",output],expected=1,stdout=b"")
                assert output.read_bytes()==sentinel and set(folder.iterdir())==before
            return
        if ident=="C13":self.unit(case["test_function"]);return
        if ident=="C14":
            fixture=root/"tests/fixtures/public_source_program";expected=case["expected"]
            report=folder/"filtered.json"
            self.run("filtered",[cli,"test",fixture/"test_root.xr","--filter","absent","--report",report],expected=1,stderr=expected["filtered_stderr"].encode())
            row=json.loads(report.read_text(encoding="utf8"))["files"][0]
            assert [row[k] for k in ["tests","passed","failed","skipped"]]==[1,0,0,1] and not row["errors"] and not row["timeouts"]
            report=folder/"parallel.json"
            out,_=self.run("parallel",[cli,"test",fixture/"test_fail.xr",fixture/"test_root.xr","--jobs","2","--report",report],expected=1,stderr=b"")
            rows=json.loads(report.read_text(encoding="utf8"))["files"]
            assert [Path(x["path"]).name for x in rows]==expected["parallel_paths"] and b"\x1b[" not in out
            assert [[x[k] for k in ["tests","passed","failed","skipped"]] for x in rows]==expected["parallel_counts"]
            assert all(x["errors"]==x["timeouts"]==0 and len(x["cases"])==1 for x in rows)
            self.unit("test_fail_fast_still_tears_down");self.unit("test_after_hooks_cannot_silently_pass");return
        if ident=="C16":
            sys.path.insert(0,str(root/"tests/unit/xir"));from run_source_dependencies import expected_reports
            rows=[(1,1,str((folder/"library.xr").resolve()),[]),(0,1,str(entry.resolve()),[1])]
            reports=expected_reports(str(entry.resolve()),rows)
            for flag,suffix in [("--json",".json"),("--list",".list"),("--shell",".sh")]:
                out,_=self.run(flag,[cli,"deps",entry,flag],stderr=b"");assert out.replace(b"\r\n",b"\n")==reports[suffix]
            self.run("shared-graph-and-output",[sys.executable,"-X","utf8",root/"tests/unit/xir/run_cli_dependencies.py",cli,root/"stdlib"])
            return
        if ident=="C23":
            data,_=self.run("identity",[cli,"--version","--json"],stderr=b"");identity=json.loads(data)
            assert identity["product"]=="xray-lang" and identity["target"]=="windows-x86_64"
            commit=subprocess.check_output(["git","rev-parse","HEAD"],cwd=root,text=True).strip()
            assert identity["commit"]==commit
            dirty=bool(subprocess.check_output(["git","status","--porcelain"],cwd=root,text=True).strip())
            assert identity["dirty"]==dirty
            self.record["CLI_identity"]=identity
            self.run("help",[cli,"--help"],stderr=b"")
            data,_=self.run("info",[cli,"info","--json"],stderr=b"")
            info=json.loads(data);assert info["build"]==identity and Path(info["installation"]["executable"]).resolve()==cli
            self.record["production_release"]=False;return
        if ident=="C24":
            for name in case["expected"]["names"]:self.run("withheld-"+name,[cli,name],expected=case["expected"]["unknown_command_exit"],stdout=b"")
            out,_=self.run("commands",[cli,"--help"],stderr=b"")
            original=["run","test","check","fmt","build","deps","info","doctor","self","help"]
            assert all(name.encode() in out for name in original)
            assert b"repl" in out and b"toolchain" in out
            self.record["original10_commands"]=original;self.record["retained_additional_commands"]=["repl","toolchain"];return
        if ident=="C25":
            public=folder/"public.xr";private=folder/"private.xr"
            private.write_text('fn secret()->i64{return 99}\nexport fn read()->i64{return 1}\n',encoding="utf8",newline="\n")
            public.write_text('import "./private" as P\nprint(P.read())\n',encoding="utf8",newline="\n")
            self.run("permission-control",[cli,"run",public],stdout=b"1\n",stderr=b"")
            public.write_text('import "./private" as P\nprint(P.secret())\n',encoding="utf8",newline="\n")
            _,error=self.run("private-refusal",[cli,"run",public],expected=1,stdout=b"")
            assert b"export" in error or b"private" in error,error
            (folder/"library.xr").write_text('var serial:i64=40\nexport fn read()->i64{return serial}\n',encoding="utf8",newline="\n")
            entry.write_text('import "./library" as Library\nprint(Library.read())\n',encoding="utf8",newline="\n")
            if self.args.group=="native":
                self.native({"expected":{"exit":0,"stdout_hex":"34300a","stderr_hex":""}},entry,folder)
            else:self.run("qualified-library-state",[cli,"run",entry],stdout=b"40\n",stderr=b"")
            return
        raise AssertionError("An original expectation has no real consumer: "+ident)

    def execute(self):
        api={"C15","C17","C18","C19","C20","C21","C22"}
        native={"C01","C02","C05","C06","C07","C10","C11","C25"}
        ids=api if self.args.group=="api" else native if self.args.group=="native" else {x["id"] for x in self.original["cases"]}-api
        for case in self.original["cases"]:
            if case["id"] not in ids:continue
            self.current=case["id"];item={"id":self.current,"expected":case["expected"],"status":"RUNNING"}
            item["scope"]="Actual named normal/rejection assertions, not complete FI/platform/SDK qualification"
            self.record["cases"].append(item);self.save()
            try:self.case(case);item["status"]="PASS"
            except AssertionError as error:item.update({"status":"FAIL","error":str(error)})
            print(self.current,item["status"],flush=True);self.save()
        assert fact(self.args.cli)==self.record["cli_before"],"CLI binary changed during entry qualification"
        self.record["inputs_after"]=[fact(p) for p in self.input_paths]
        assert self.record["inputs_after"]==self.record["inputs_before"],"Static entry inputs changed"
        self.record["job_active_processes"]=job_processes(self.job)
        assert self.record["job_active_processes"]==1,"Entry descendants did not drain"
        self.record["passed"]=sum(x["status"]=="PASS" for x in self.record["cases"])
        self.record["failed"]=sum(x["status"]=="FAIL" for x in self.record["cases"])
        self.record["status"]="FAIL" if self.record["failed"] else "PASS";self.save()
        print("Original entry group",self.args.group,self.record["passed"],"PASS",self.record["failed"],"FAIL; record",self.folder/"record.json")
        return bool(self.record["failed"])


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ["root","cli","output","api","physical","injected","resources"]:p.add_argument("--"+name,type=lambda x:Path(x).resolve(),required=True)
    p.add_argument("--group",choices=["vm","native","api"],required=True)
    return Runner(p.parse_args()).execute()


if __name__=="__main__":raise SystemExit(main())
