"""Compile a historical consumer object; only the retired symbols may block it."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess

p = argparse.ArgumentParser()
p.add_argument("--compiler", required=True)
p.add_argument("--archive", required=True)
p.add_argument("--root", required=True)
p.add_argument("--work", required=True)
p.add_argument("--runtime", required=True)
p.add_argument("--link-flags", default="")
a = p.parse_args()
work = Path(a.work)
work.mkdir(parents=True, exist_ok=True)
here = Path(__file__).resolve().parent
runtime = "/MD" if "DLL" in a.runtime else "/MT"
records = []


def run(command, success=True):
    proc = subprocess.run(command, capture_output=True, text=True, errors="replace", cwd=work)
    records.append(dict(command=command, status=proc.returncode, stdout=proc.stdout, stderr=proc.stderr))
    (work / "commands.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
    if success and proc.returncode:
        raise AssertionError(records[-1])
    return proc


def compile_c(name, source):
    path = work / (name + ".c")
    path.write_text(source, encoding="utf-8")
    obj = work / (name + ".obj")
    run([a.compiler, "/nologo", "/c", "/std:c11", "/utf-8", "/W4", "/WX", runtime,
         "/DWIN32_LEAN_AND_MEAN", "/DNOMINMAX", "/D_CRT_SECURE_NO_WARNINGS",
         "/I" + str(here), "/I" + str(Path(a.root) / "src"), str(path), "/Fo" + str(obj)])
    return obj


def link(name, obj, success):
    exe = work / (name + ".exe")
    flags = [x.strip('"') for x in shlex.split(a.link_flags, posix=False)]
    result = run([a.compiler, "/nologo", runtime, str(obj), a.archive,
                  "/Fe" + str(exe), "/link", *flags], success)
    return exe, result


positive = compile_c("current_consumer", r'''
#include "aot/program/xr_xir_native_artifact.h"
int main(void) {
    XrCompileResourceLimits limits={UINT64_MAX,UINT64_MAX,UINT64_MAX};
    XrCompileResources *r=0;
    if (xr_compile_resources_new(&limits,&r)!=XR_COMPILE_RESOURCE_OK) return 1;
    XrToolchainInput request={0};
    request.schema_version=2; request.provider=3;
    request.provider_version="test"; request.target_triple="test"; request.codegen_options="";
    request.sysroot_id.bytes[0]=1; request.runtime_sdk_id.bytes[0]=2; request.target_profile_id.bytes[0]=3;
    XrToolchainBinding binding;
    if (xr_compile_toolchain_binding_build(r,&request,&binding)!=XR_TOOLCHAIN_BINDING_OK) return 2;
    XrXirNativeInput in={0}, sealed;
    in.schema_version=2; in.checked_schema=XR_XIR_CHECKED_SCHEMA; in.checked_contract=XR_XIR_CHECKED_CONTRACT;
    in.value_abi=XR_XIR_VALUE_ABI_VERSION; in.call_abi=XR_XIR_CALL_ABI_VERSION; in.program_abi=XR_XIR_PROGRAM_ABI_VERSION;
    in.architecture=XR_XIR_ARCH_X86_64; in.function_count=1; in.module_count=1;
    in.source_checked_id.bytes[0]=1; in.closed_checked_id.bytes[0]=2; in.lowered_layout_id.bytes[0]=3;
    in.codegen_policy_id.bytes[0]=4; in.generated_digest.bytes[0]=5; in.toolchain=binding;
    XrXirCompileContext ctx={r,{0}};
    if (xr_compile_native_input_seal(&ctx,&in,&sealed)!=XR_XIR_OK) return 3;
    XrXirNativeArtifact *artifact=0;
    if (xr_compile_native_artifact_seal(&ctx,&sealed,"MZ",2,2,&artifact)!=XR_XIR_OK) return 4;
    xr_compile_resources_release(r);
    if (xr_compile_native_artifact_verify(artifact,&sealed,2)!=XR_XIR_OK) return 5;
    xr_compile_native_artifact_free(artifact);
    return 0;
}
''')
exe, _ = link("current_consumer", positive, True)
run([str(exe)])
legacy = compile_c("schema1_consumer", r'''
#include "schema1_declarations.h"
int main(void) {
    XrAotToolchainInput in={0}; XrAotToolchainBinding binding={0};
    XrNativeArtifact artifact={0}; XrGeneratedC generated={0}; XrFingerprint id={{0}};
    int result=xr_aot_toolchain_binding_build(&in,&binding);
    result+=xr_aot_toolchain_binding_equal(&binding,&binding);
    result+=xr_native_artifact_seal(&generated,&binding,(const uint8_t *)"MZ",2,&artifact);
    result+=xr_native_artifact_verify(&artifact,id,id,id,&binding);
    xr_native_artifact_free(&artifact);
    return result;
}
''')
_, rejected = link("schema1_consumer", legacy, False)
assert rejected.returncode != 0
log = rejected.stdout + rejected.stderr
symbols = ("xr_aot_toolchain_binding_build", "xr_aot_toolchain_binding_equal",
           "xr_native_artifact_seal", "xr_native_artifact_verify", "xr_native_artifact_free")
for symbol in symbols:
    assert symbol in log, (symbol, log)
assert "LNK1120: 5" in log or "undefined symbol" in log, log
print("current production archive executed; historical schema1 object rejected at five retired symbols")
