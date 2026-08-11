#!/usr/bin/env python3
"""What the installed product exposes, and what it must not.

`xray_core` is the compiler's internal development aggregate. If it reached an
install prefix, an installed AOT build could link against it and pick up
whatever the compiler happens to contain -- so this asserts its absence
directly, and also that no emitted link command mentions it or the source tree.
Installed AOT builds must use the precise runtime archives the link manifest
chooses; bytecode embedders use the dedicated VM runtime archive.

Two prefixes are installed: the XrayCore component alone (what a plain user
gets -- no SDK headers, no AOT archive) and the full install. Checking only the
full one would not catch the component leaking SDK content.

The native build at the end runs with XRAY_INCLUDE/XRAY_LIB/XRAY_STDLIB_PATH
cleared and the config/cache dirs redirected: anything pointing back at the
source tree would let a broken install pass by borrowing headers and archives
it does not actually ship.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


class Gate:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def record(self, condition: bool, name: str, detail: str = "") -> None:
        if condition:
            self.passed += 1
            print(f"  PASS: {name}")
        else:
            self.failed += 1
            suffix = f": {detail}" if detail else ""
            print(f"  FAIL: {name}{suffix}")


def run(command: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        env=env,
        text=True,
        encoding="utf-8",
        errors="strict",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=120,
        check=False,
    )


def is_executable(path: Path) -> bool:
    """Installed, and actually runnable.

    A plain is_file() would pass an xray whose execute bit did not survive the
    install, which is the one thing every later smoke check depends on. On
    Windows os.access(X_OK) is effectively an existence test, so this is no
    weaker there than the file check it replaces.
    """
    return path.is_file() and os.access(path, os.X_OK)


def install(build: Path, prefix: Path, component: str | None = None) -> subprocess.CompletedProcess[str]:
    command = ["cmake", "--install", str(build), "--prefix", str(prefix)]
    if component:
        command.extend(["--component", component])
    return run(command)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    root = args.project_root.resolve(strict=True)
    build = args.build_dir.resolve(strict=True)
    binary = args.binary.resolve(strict=True)
    gate = Gate()

    target_process = run([str(binary), "toolchain", "list", "--target", "native", "--json"])
    try:
        host_target = json.loads(target_process.stdout)["normalizedTarget"]
    except (json.JSONDecodeError, KeyError) as exc:
        raise SystemExit(f"cannot determine host target: {target_process.stdout}") from exc

    executable_name = "xray.exe" if os.name == "nt" else "xray"
    static_prefix = "" if os.name == "nt" else "lib"
    static_suffix = ".lib" if os.name == "nt" else ".a"

    with tempfile.TemporaryDirectory(prefix="xray-install-surface-") as temporary:
        work = Path(temporary)
        core = work / "core-prefix"
        full = work / "prefix"
        print("=== Install Public Surface Tests ===")
        print(f"Build:  {build}")
        print(f"Prefix: {full}\n")

        core_install = install(build, core, "XrayCore")
        gate.record(core_install.returncode == 0, "Core component install", core_install.stdout[-4000:])
        core_xray = core / "bin" / executable_name
        gate.record(is_executable(core_xray), "Core installed xray executable")
        gate.record(
            (core / f"lib/xray/vm/{host_target}/{static_prefix}xray_vm_runtime{static_suffix}").is_file(),
            "Core installed VM runtime",
        )
        gate.record((core / "lib/xray/stdlib/path/path.xr").is_file(), "Core installed stdlib")
        gate.record((core / "share/xray/install/install-marker.json").is_file(), "Core installed marker")
        gate.record((core / "share/xray/install/payload-manifest.json").is_file(), "Core payload manifest")
        gate.record((core / "include/xray/xray_target_plan_load.h").is_file(),
                    "Core installs opaque TargetPlan load header")
        gate.record((core / "include/xray/xray_runtime_generation.h").is_file(),
                    "Core installs module generation authority header")
        gate.record(not (core / "include/xray/xray.h").exists(), "Core excludes SDK headers")
        gate.record(
            not (core / f"lib/xray/aot/{host_target}/{static_prefix}xray_aot_core{static_suffix}").exists(),
            "Core excludes AOT SDK archive",
        )

        verify = run(
            [
                sys.executable,
                str(root / "scripts/verify_payload_manifest.py"),
                "--root",
                str(core),
                "--binary",
                str(core_xray),
            ]
        )
        gate.record(verify.returncode == 0, "Core payload manifest and binary identity", verify.stdout)
        evaluation = run([str(core_xray), "-e", 'print("ok")'])
        gate.record(evaluation.returncode == 0 and evaluation.stdout.strip() == "ok", "Core eval smoke", evaluation.stdout)

        smoke = work / "core_smoke.xr"
        shutil.copyfile(root / "tests/test_harness/single_pass.xr", smoke)
        gate.record(run([str(core_xray), "check", str(smoke)]).returncode == 0, "Core check smoke")
        fmt = run([str(core_xray), "fmt", str(smoke)])
        fmt_check = run([str(core_xray), "fmt", "--check", str(smoke)])
        gate.record(fmt.returncode == 0 and fmt_check.returncode == 0, "Core fmt smoke")
        gate.record(run([str(core_xray), "test", "--quiet", str(smoke)]).returncode == 0, "Core test smoke")
        bytecode = work / "core_smoke.xrc"
        compile_result = run([str(core_xray), "compile", str(smoke), "-o", str(bytecode)])
        gate.record(compile_result.returncode == 0 and bytecode.is_file(), "Core compile smoke", compile_result.stdout)

        full_install = install(build, full)
        gate.record(full_install.returncode == 0, "cmake install", full_install.stdout[-4000:])
        full_xray = full / "bin" / executable_name
        gate.record(is_executable(full_xray), "installed xray executable")
        expected = [
            (full / f"lib/xray/aot/{host_target}/{static_prefix}xray_aot_core{static_suffix}", "installed xray_aot_core archive"),
            (full / f"lib/xray/aot/{host_target}/{static_prefix}xray_rt_coro{static_suffix}", "installed xray_rt_coro archive"),
            (full / f"lib/xray/aot/{host_target}/manifest.json", "installed runtime manifest"),
            (full / f"lib/xray/vm/{host_target}/{static_prefix}xray_vm_runtime{static_suffix}", "installed xray_vm_runtime archive"),
            (full / "lib/xray/sdk/src/aot/xrt.h", "installed private AOT SDK"),
            (full / "include/xray/xray_target_plan_load.h", "installed TargetPlan load header"),
            (full / "include/xray/xray_runtime_generation.h", "installed generation authority header"),
            (full / "lib/xray/stdlib/path/path.xr", "installed stdlib source"),
            (full / "share/xray/install/install-marker.json", "installed payload marker"),
            (full / "share/xray/install/payload-manifest.json", "installed payload manifest"),
        ]
        for path, name in expected:
            gate.record(path.is_file(), name, f"missing {path}")

        verify = run(
            [
                sys.executable,
                str(root / "scripts/verify_payload_manifest.py"),
                "--root",
                str(full),
                "--binary",
                str(full_xray),
            ]
        )
        gate.record(verify.returncode == 0, "full payload manifest and binary identity", verify.stdout)
        forbidden = list((full / "lib").rglob(f"{static_prefix}xray_core{static_suffix}"))
        gate.record(not forbidden, f"does not install {static_prefix}xray_core{static_suffix}", str(forbidden))

        runtime_source = work / "runtime_time.xr"
        runtime_binary = work / ("runtime_time.exe" if os.name == "nt" else "runtime_time")
        shutil.copyfile(root / "tests/aot/filetests/link/runtime_time.xr", runtime_source)
        isolated_env = os.environ.copy()
        for name in ("XRAY_INCLUDE", "XRAY_LIB", "XRAY_STDLIB_PATH", "XRAY_ZIG"):
            isolated_env.pop(name, None)
        isolated_env["LOCALAPPDATA"] = str(work / "localappdata")
        isolated_env["XDG_CONFIG_HOME"] = str(work / "config")
        isolated_env["XDG_CACHE_HOME"] = str(work / "cache")
        native = run(
            [
                str(full_xray),
                "build",
                "--native",
                "--dump-link-command",
                str(runtime_source),
                "-o",
                str(runtime_binary),
            ],
            cwd=work,
            env=isolated_env,
        )
        gate.record(native.returncode == 0, "installed xray builds runtime_time.xr", native.stdout[-8000:])
        source_aot = str(root / "src/aot")
        gate.record(source_aot.lower() not in native.stdout.lower(), "installed AOT command is independent of the source tree")
        gate.record("-lxray_core" not in native.stdout and "xray_core.lib" not in native.stdout.lower(), "installed runtime_time link command does not use xray_core")
        runtime = run([str(runtime_binary)]) if runtime_binary.is_file() else None
        gate.record(
            runtime is not None and runtime.returncode == 0 and runtime.stdout.strip() == "7",
            "installed runtime_time binary output",
            runtime.stdout if runtime else "missing executable",
        )

        print(f"\n=== Results: {gate.passed} passed, {gate.failed} failed ===")
        return 0 if gate.failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
