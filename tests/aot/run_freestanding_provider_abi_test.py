#!/usr/bin/env python3
"""A freestanding provider object must stay inside its declared ABI.

The generated object for a freestanding coroutine target is compiled with the
provider ABI defines and then checked three ways: nothing outside the declared
allow-list may be left undefined, the case must still reach a core set of
provider hooks, and it must not carry the hosted value-ops bridge or reference
a hosted runtime symbol.

The first two are separate because a hook is referenced only when the lowered
program needs it, so equality against the whole allow-list would reject a legal
lowering. What equality did cover and the core set does not is noted on
REQUIRED_UNDEFINED.

Explicit providers compile the same already-emitted C file. Their rows prove
raw generated-C object and ABI conformance; they do not prove Xray's provider
discovery or native link-and-run path.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import os
import re
import shutil
import sys
from pathlib import Path


def _bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


_bootstrap()
from xraytest import binary as binlib  # noqa: E402
from xraytest import platform, proc, toolchain, workspace  # noqa: E402

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPT_DIR.parent.parent
CASE = SCRIPT_DIR / "provider" / "freestanding_coro" / "main.xr"

SKIP_EXIT = 77

# Exactly the symbols a freestanding provider object may leave undefined.
ALLOWED_UNDEFINED = (
    "memcpy",
    "memset",
    "xr_aot_await_task",
    "xr_aot_await_task_resume",
    "xr_aot_frame_alloc",
    "xr_aot_frame_free",
    "xr_aot_run_main",
    "xr_aot_runtime_config_init",
    "xr_aot_runtime_delete",
    "xr_aot_runtime_new",
    "xr_aot_spawn",
    "xr_aot_trace_frame_value",
    "xr_runtime_core_enable_object_destroy_ops",
    "xr_runtime_core_enable_task_destroy_ops",
    "xrt_closure_new",
)

# The provider hooks this case must still reach. Membership in ALLOWED_UNDEFINED
# alone cannot carry that: several hooks are referenced only when the lowered
# program needs them -- a frame with no traced root never mentions the trace
# hook -- so requiring the whole list would fail on a legal lowering. Checking a
# core set instead keeps a case that stops reaching the provider from passing
# the escape test by referencing almost nothing.
#
# What the core set does not watch, and why, for each name in the difference:
#   memcpy, memset                     not provider hooks
#   xr_aot_runtime_config_init/new/    emitted together with xr_aot_run_main,
#     delete                           which the core set does require
#   xr_aot_await_task_resume           the other resume spellings are outside
#                                      ALLOWED, so a switch still escapes
#   xr_aot_trace_frame_value           UNWATCHED: this case traces no frame
#                                      root, so nothing pins frame-root tracing
#   xr_runtime_core_enable_object_/    UNWATCHED: driven by the OBJECTS and TASK
#     task_destroy_ops                 capability bits, and no gate ties the
#                                      declared capabilities to the emitted ones
REQUIRED_UNDEFINED = (
    "xr_aot_await_task",
    "xr_aot_frame_alloc",
    "xr_aot_frame_free",
    "xr_aot_run_main",
    "xr_aot_spawn",
    "xrt_closure_new",
)

if not set(REQUIRED_UNDEFINED) <= set(ALLOWED_UNDEFINED):
    raise AssertionError("required provider hooks must be declared as allowed")

HOSTED_SYMBOL_RE = re.compile(
    r"^(pthread|malloc$|free$|epoll|kqueue|netpoll|xray_rt_coro|xray_core)",
    re.IGNORECASE,
)

PROVIDER_DEFINES = (
    "-DXRAY_TARGET_RUNTIME_PROVIDER=1",
    "-DXRAY_PROVIDER_ABI=1",
    "-DXRAY_PROVIDER_REQUIRED_CAPS=0x1021",
    "-DXRAY_PROVIDER_REQUIRED_HOOKS=0x17",
    "-DXRAY_PROVIDER_TARGET_METADATA_HASH=1",
)


SUPPORTED_PROVIDERS = ("clang", "gcc", "zig", "msvc")
STATUS_PASS = "PASS"
STATUS_FAIL = "FAIL"
STATUS_NOT_RUN = "NOT_RUN"


@dataclass(frozen=True)
class ProviderSpec:
    name: str
    path: str


def _parse_provider(raw: str) -> ProviderSpec:
    name, separator, path = raw.partition("=")
    if separator != "=" or name not in SUPPORTED_PROVIDERS or not path:
        choices = ", ".join(f"{item}=PATH" for item in SUPPORTED_PROVIDERS)
        raise argparse.ArgumentTypeError(f"provider must be one of: {choices}")
    return ProviderSpec(name=name, path=path)


def _probe_argv(compiler: toolchain.CCompiler) -> list[str]:
    if compiler.driver == toolchain.CC_DRIVER_ZIG:
        return [compiler.path, "env"]
    if compiler.driver == toolchain.CC_DRIVER_MSVC:
        return [compiler.path, "/nologo", "/?"]
    return [compiler.path, "--version"]


def _provider_name(compiler: toolchain.CCompiler, banner: str) -> str | None:
    lowered = banner.lower()
    if (compiler.driver == toolchain.CC_DRIVER_ZIG and
            ".zig_exe" in lowered and ".lib_dir" in lowered and
            re.search(r"\.version\s*=\s*\"\d+\.\d+\.\d+\"", lowered)):
        return "zig"
    clang_marker = "clang" in lowered
    gcc_marker = (
        "free software foundation" in lowered or
        re.search(r"\bgcc(?:[- ])", lowered) is not None
    )
    if clang_marker and gcc_marker:
        return None
    if gcc_marker:
        return "gcc"
    if clang_marker:
        return "clang"
    if compiler.driver == toolchain.CC_DRIVER_MSVC and "microsoft" in lowered:
        return "msvc"
    return None


def _driver_for(spec: ProviderSpec, resolved: str) -> str:
    basename = Path(resolved).name.lower()
    if spec.name == "zig":
        return toolchain.CC_DRIVER_ZIG
    if spec.name == "msvc" or basename in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
        return toolchain.CC_DRIVER_MSVC
    return toolchain.CC_DRIVER_GNU


def _resolve_provider(spec: ProviderSpec, timeout: int
                      ) -> tuple[toolchain.CCompiler | None, str, str]:
    resolved = shutil.which(spec.path)
    if resolved is None:
        return None, STATUS_NOT_RUN, f"compiler is not executable: {spec.path}"
    compiler = toolchain.CCompiler(path=resolved, driver=_driver_for(spec, resolved))
    probe = proc.run(_probe_argv(compiler), timeout=timeout)
    if not probe.ok:
        return None, STATUS_FAIL, f"compiler probe failed: {resolved}"
    actual = _provider_name(compiler, probe.combined_text())
    if actual != spec.name:
        return None, STATUS_FAIL, (
            f"provider identity mismatch: requested={spec.name} "
            f"actual={actual or 'unknown'}"
        )
    return compiler, STATUS_PASS, ""


def _auto_provider(timeout: int
                   ) -> tuple[ProviderSpec | None, toolchain.CCompiler | None, str, str]:
    compiler = toolchain.find_c_compiler()
    if compiler is None:
        return None, None, STATUS_NOT_RUN, "no C compiler found"
    probe = proc.run(_probe_argv(compiler), timeout=timeout)
    if not probe.ok:
        return None, None, STATUS_FAIL, f"compiler probe failed: {compiler.path}"
    name = _provider_name(compiler, probe.combined_text())
    if name is None:
        return None, None, STATUS_FAIL, f"unsupported C provider: {compiler.path}"
    return ProviderSpec(name=name, path=compiler.path), compiler, STATUS_PASS, ""


def _compile_object_argv(compiler: toolchain.CCompiler, source: Path,
                         output: Path) -> list[str]:
    include_dirs = (PROJECT_DIR / "src" / "aot", PROJECT_DIR / "include")
    if compiler.driver == toolchain.CC_DRIVER_MSVC:
        argv = [
            compiler.path,
            "/nologo",
            "/TC",
            "/std:c11",
            "/experimental:c11atomics",
            "/utf-8",
            "/GS-",
            "/Zl",
            "/c",
            *(f"/D{definition[2:]}" for definition in PROVIDER_DEFINES),
            *(f"/I{include_dir}" for include_dir in include_dirs),
            str(source),
            f"/Fo{output}",
        ]
        return argv

    argv = [compiler.path]
    if compiler.driver == toolchain.CC_DRIVER_ZIG:
        argv.extend(["cc", "-O2"])
    argv.extend([
        "-std=c11",
        "-ffreestanding",
        "-fno-stack-protector",
        *PROVIDER_DEFINES,
        *(f"-I{include_dir}" for include_dir in include_dirs),
        "-c",
        str(source),
        "-o",
        str(output),
    ])
    return argv


_C11_TRIGRAPHS = {
    "=": "#",
    "(": "[",
    "/": "\\",
    ")": "]",
    "'": "^",
    "<": "{",
    "!": "|",
    ">": "}",
    "-": "~",
}


def _phase1_replace_trigraphs(source: str) -> str:
    """Canonicalize every C11 trigraph before line splicing or tokenization."""
    output: list[str] = []
    index = 0
    length = len(source)
    while index < length:
        if source[index:index + 2] == "??" and index + 2 < length:
            replacement = _C11_TRIGRAPHS.get(source[index + 2])
            if replacement is not None:
                output.append(replacement)
                index += 3
                continue
        output.append(source[index])
        index += 1
    return "".join(output)


def _phase2_splice_lines(source: str) -> str:
    """Remove C phase-2 line splices before comments or tokens are recognized."""
    output: list[str] = []
    index = 0
    length = len(source)
    while index < length:
        if source[index] == "\\" and index + 1 < length:
            following = source[index + 1]
            if following == "\n":
                index += 2
                continue
            if following == "\r":
                index += 2
                if index < length and source[index] == "\n":
                    index += 1
                continue
        output.append(source[index])
        index += 1
    return "".join(output)


def _c_tokens(source: str):
    """Yield C tokens needed by the shape gate while ignoring comments and literals."""
    source = _phase2_splice_lines(_phase1_replace_trigraphs(source))
    index = 0
    length = len(source)
    while index < length:
        current = source[index]
        following = source[index + 1] if index + 1 < length else ""
        if current.isspace():
            index += 1
            continue
        if current == "/" and following == "/":
            index += 2
            while index < length and source[index] not in "\r\n":
                index += 1
            continue
        if current == "/" and following == "*":
            index += 2
            while index + 1 < length and source[index:index + 2] != "*/":
                index += 1
            index = min(length, index + 2)
            continue
        if current in ('"', "'"):
            quote = current
            index += 1
            while index < length:
                if source[index] == "\\":
                    index += 2
                    continue
                if source[index] == quote:
                    index += 1
                    break
                index += 1
            continue
        if current.isalpha() or current == "_":
            end = index + 1
            while end < length and (source[end].isalnum() or source[end] == "_"):
                end += 1
            yield source[index:end]
            index = end
            continue
        if current == "<" and following == "%":
            yield "{"
            index += 2
            continue
        yield current
        index += 1


def _generated_c_violation(generated: str) -> str | None:
    previous_previous = None
    previous = None
    for token in _c_tokens(generated):
        if previous == "(" and token == "{":
            return "freestanding provider object emitted a GNU statement expression"
        if token == "__builtin_alloca":
            return "freestanding provider object emitted non-portable builtin alloca"
        if token == "xrt_runtime_value_ops":
            return "freestanding provider object emitted the hosted value-ops bridge"
        if previous_previous == "runtime_cfg" and previous == "." and token == "value_ops":
            return "freestanding provider object emitted the hosted value-ops bridge"
        previous_previous, previous = previous, token
    return None


def _check_provider(spec: ProviderSpec, compiler: toolchain.CCompiler, provider_c: Path,
                    workspace_dir: Path, timeout: int) -> tuple[str, str]:
    suffix = ".obj" if compiler.driver == toolchain.CC_DRIVER_MSVC else ".o"
    provider_o = workspace_dir / f"provider-{spec.name}{suffix}"
    compile_result = proc.run(
        _compile_object_argv(compiler, provider_c, provider_o), timeout=timeout
    )
    if not compile_result.ok:
        return STATUS_FAIL, (
            "provider object failed to compile\n" + compile_result.combined_text()[:8000]
        )

    try:
        undefined = binlib.undefined_symbol_names(provider_o, timeout=timeout)
    except binlib.SymbolInspectionError as error:
        return STATUS_FAIL, f"object symbol inspector failed: {error}"
    if undefined is None:
        return STATUS_NOT_RUN, "no object symbol inspector is installed"

    escaped = sorted(set(undefined) - set(ALLOWED_UNDEFINED))
    if escaped:
        detail = "\n".join(f"  + {name}" for name in escaped)
        return STATUS_FAIL, "freestanding provider object escaped its declared ABI\n" + detail

    unreached = sorted(set(REQUIRED_UNDEFINED) - set(undefined))
    if unreached:
        detail = "\n".join(f"  - {name}" for name in unreached)
        return STATUS_FAIL, (
            "freestanding provider object no longer reaches its provider ABI\n" + detail
        )

    hosted = [name for name in undefined if HOSTED_SYMBOL_RE.search(name)]
    if hosted:
        detail = "\n".join(f"  {name}" for name in hosted)
        return STATUS_FAIL, (
            "freestanding provider object references a hosted runtime symbol\n" + detail
        )
    return STATUS_PASS, ""


def _self_test() -> int:
    source = Path("provider.c")
    output = Path("provider.o")
    gnu = toolchain.CCompiler("/tool/clang", toolchain.CC_DRIVER_GNU)
    gcc = toolchain.CCompiler("/tool/gcc-16", toolchain.CC_DRIVER_GNU)
    zig = toolchain.CCompiler("/tool/zig", toolchain.CC_DRIVER_ZIG)
    msvc = toolchain.CCompiler(r"C:\VC\cl.exe", toolchain.CC_DRIVER_MSVC)

    gnu_argv = _compile_object_argv(gnu, source, output)
    if "-std=c11" not in gnu_argv or "cc" in gnu_argv or "/TC" in gnu_argv:
        raise AssertionError("GNU-family object command is malformed")
    zig_argv = _compile_object_argv(zig, source, output)
    if zig_argv[1] != "cc" or "-O2" not in zig_argv or "-std=c11" not in zig_argv:
        raise AssertionError("Zig object command is malformed")
    msvc_argv = _compile_object_argv(msvc, source, Path("provider.obj"))
    if ("/TC" not in msvc_argv or "/std:c11" not in msvc_argv or
            "/GS-" not in msvc_argv or "/Zl" not in msvc_argv or
            any(arg.startswith("-D") or arg.startswith("-I") for arg in msvc_argv)):
        raise AssertionError("MSVC object command is malformed")

    if _provider_name(gnu, "Homebrew clang version 22.1.4") != "clang":
        raise AssertionError("Clang provider identity was not recognized")
    if _provider_name(gcc, "gcc-16 (Homebrew GCC 16.2.0)") != "gcc":
        raise AssertionError("GCC provider identity was not recognized")
    contradictory = (
        "clang (Homebrew GCC 16.2.0) 16.2.0\n"
        "Copyright (C) 2026 Free Software Foundation, Inc."
    )
    if _provider_name(gnu, contradictory) is not None:
        raise AssertionError("contradictory provider identity was accepted")
    if _provider_name(zig, '.{ .zig_exe = "/tool/zig", .lib_dir = "/lib", '
                           '.version = "0.16.0" }') != "zig":
        raise AssertionError("Zig provider identity was not recognized")
    if _provider_name(msvc, "Microsoft (R) C/C++ Optimizing Compiler") != "msvc":
        raise AssertionError("MSVC provider identity was not recognized")

    if _generated_c_violation("int clean(void) { return 0; }") is not None:
        raise AssertionError("portable generated C was rejected")
    if _phase1_replace_trigraphs("??=??(??/??)??'??<??!??>??-") != "#[\\]^{|}~":
        raise AssertionError("C11 trigraph canonicalization is incomplete")
    if _phase1_replace_trigraphs("??x ????/") != "??x ??\\":
        raise AssertionError("unknown or overlapping trigraphs were rewritten recursively")
    for mutation in (
        "int f(void) { return ({ 1; }); }",
        "int f(void) { return ( /* hidden */ { 1; }); }",
        "int f(void) { return (\\\n{ 1; }); }",
        "int f(void) { return (<% 1; %>); }",
        "void f(void) { __builtin_alloca(8); }",
        "void f(void) { __builtin_al\\\nloca(8); }",
        "void f(void) { __builtin_al\\\r\nloca(8); }",
        "void f(void) { __builtin_al??/\nloca(8); }",
        "void f(void) { __builtin_al??/\r\nloca(8); }",
        "void f(void) { __builtin_al??/\rloca(8); }",
        "void f(void) { xrt_runtime_value_ops(); }",
        "void f(void) { xrt_runtime_\\\nvalue_ops(); }",
        "void f(void) { xrt_runtime_??/\nvalue_ops(); }",
        "void f(void) { runtime_cfg /* hidden */ . value_ops(); }",
        "void f(void) { runtime_\\\ncfg.value_ops(); }",
        "void f(void) { runtime_??/\ncfg.value_ops(); }",
        "int f(void) { return (??< 1; ??>); }",
        "int f(void) { return (??/\n??< 1; ??>); }",
        "/* hidden *??/\n/ int f(void) { return ({ 1; }); }",
    ):
        if _generated_c_violation(mutation) is None:
            raise AssertionError("generated-C mutation was not rejected")
    for clean in (
        'const char *text = "({ __builtin_alloca xrt_runtime_value_ops";',
        "/* ({ __builtin_alloca xrt_runtime_value_ops */ int clean(void);",
        "// hidden \\\n({ __builtin_alloca(8); })\nint clean(void);",
        "/\\\n* ({ __builtin_alloca(8); }) */ int clean(void);",
        "// hidden ??/\n({ __builtin_alloca(8); })\nint clean(void);",
        "/??/\n* ({ __builtin_alloca(8); }) */ int clean(void);",
        "/??/\n/ ({ __builtin_alloca(8); })\nint clean(void);",
        'const char *text = "__builtin_al??/\nloca xrt_runtime_??/\nvalue_ops";',
        'const char *text = "hidden??/" ({ __builtin_alloca(8); })";',
        'const char *text = "??< __builtin_alloca xrt_runtime_value_ops ??>";',
    ):
        if _generated_c_violation(clean) is not None:
            raise AssertionError("comment or string token was treated as generated code")
    print("freestanding provider ABI self-test passed")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Freestanding provider ABI gate")
    ap.add_argument("xray", nargs="?", default=None)
    ap.add_argument(
        "--provider",
        action="append",
        type=_parse_provider,
        default=[],
        help="explicit provider as clang=PATH, gcc=PATH, zig=PATH, or msvc=PATH",
    )
    ap.add_argument("--self-test", action="store_true")
    ns = ap.parse_args(argv[1:])

    if ns.self_test:
        return _self_test()
    if len({spec.name for spec in ns.provider}) != len(ns.provider):
        ap.error("each explicit provider may be requested only once")

    xray = Path(ns.xray or os.environ.get("XRAY_BIN") or (PROJECT_DIR / "build" / "xray"))
    timeout = platform.env_timeout("XRAY_TEST_CASE_TIMEOUT", 300)

    providers: list[tuple[ProviderSpec, toolchain.CCompiler]] = []
    not_run: list[tuple[ProviderSpec, str]] = []
    resolution_failed = False
    if ns.provider:
        for spec in ns.provider:
            compiler, status, reason = _resolve_provider(spec, timeout)
            if compiler is None:
                print(
                    f"provider={spec.name} compiler={spec.path} "
                    f"scope=generated-c-object status={status} reason={reason}"
                )
                if status == STATUS_NOT_RUN:
                    not_run.append((spec, reason))
                else:
                    resolution_failed = True
            else:
                providers.append((spec, compiler))
    else:
        spec, compiler, status, reason = _auto_provider(timeout)
        if spec is None or compiler is None:
            print(f"provider=auto scope=generated-c-object status={status} reason={reason}")
            return SKIP_EXIT if status == STATUS_NOT_RUN else 1
        providers.append((spec, compiler))

    if not providers:
        return 1 if resolution_failed else SKIP_EXIT

    with workspace.Workspace("xray-provider-abi") as ws:
        provider_c = ws.path("provider.c")
        result = proc.run(
            [xray, "build", "--native", "--target", "native", "-c", "-o", provider_c, CASE],
            timeout=timeout,
        )
        if not result.ok:
            sys.stderr.write("provider C generation failed\n")
            sys.stderr.write(result.combined_text()[:8000])
            return 1

        generated_bytes = provider_c.read_bytes()
        generated = generated_bytes.decode("utf-8", "strict")
        generated_sha256 = hashlib.sha256(generated_bytes).hexdigest()
        violation = _generated_c_violation(generated)
        if violation is not None:
            sys.stderr.write(violation + "\n")
            return 1

        failed = resolution_failed
        for spec, compiler in providers:
            status, detail = _check_provider(spec, compiler, provider_c, ws.root, timeout)
            print(
                f"provider={spec.name} compiler={compiler.path} scope=generated-c-object "
                f"generated_c_sha256={generated_sha256} status={status}"
            )
            if detail:
                stream = sys.stderr if status == "FAIL" else sys.stdout
                stream.write(detail + "\n")
            failed = failed or status == STATUS_FAIL
            if status == STATUS_NOT_RUN:
                not_run.append((spec, detail))
        if failed:
            return 1

    if not_run:
        print("freestanding provider ABI matrix incomplete")
        return SKIP_EXIT
    print(f"freestanding provider ABI test passed ({len(providers)} provider(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
