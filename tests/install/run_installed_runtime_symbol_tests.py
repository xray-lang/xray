#!/usr/bin/env python3
"""Inspect the installed runtime TargetPlan load boundary fail closed."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


def bootstrap() -> None:
    lib = Path(__file__).resolve().parents[1] / "lib"
    if str(lib) not in sys.path:
        sys.path.insert(0, str(lib))


bootstrap()
from xraytest import binary as binlib  # noqa: E402


REQUIRED = {
    "xr_artifact_probe",
    "xr_runtime_artifact_authority_load_available",
    "xr_runtime_artifact_authority_free",
    "xr_runtime_artifact_authority_identity",
    "xr_runtime_artifact_authority_verify",
    "xr_runtime_target_authority_native_hosted",
    "xr_runtime_target_plan_load",
    "xr_target_plan_free",
    "xr_target_plan_verify",
    "xr_target_profile_verify",
    "xr_xtp_decode_candidate",
    "xr_xtp_materialize_target_plan",
}
FORBIDDEN = {
    "xr_ownership_certificate_build",
    "xr_semantic_plan_build",
    "xr_target_plan_build",
    "xr_xtp_encode_plan",
}
FORBIDDEN_PREFIXES = ("xa_analyzer", "xanalyzer_", "xi_", "xr_parse", "xtc_")


def run(command: list[str], *, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        text=True,
        encoding="utf-8",
        errors="strict",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=120,
        check=False,
    )


def install(build: Path, prefix: Path, component: str | None = None) -> subprocess.CompletedProcess[str]:
    command = ["cmake", "--install", str(build), "--prefix", str(prefix)]
    if component:
        command.extend(["--component", component])
    return run(command)


def legacy_symbols(symbols: set[str]) -> set[str]:
    exact = {
        "xr_eval_bytecode",
        "xr_run_bytecode_file",
        "xr_detect_output_format",
        "xr_output_c_source",
    }
    return {
        symbol for symbol in symbols
        if symbol.startswith(("xray_vm_", "xr_vm_", "xvm_", "xr_bytecode_",
                              "xr_bundle_", "xr_load_module_", "xr_proto_"))
        or symbol in exact
    }


def inspect(archive: Path, allowed_legacy: set[str]) -> list[str]:
    names = binlib.defined_symbol_names(archive)
    if names is None:
        raise AssertionError("no supported symbol inspector is available")
    symbols = set(names)
    missing = sorted(REQUIRED - symbols)
    forbidden = sorted(FORBIDDEN & symbols)
    compiler = sorted(
        symbol for symbol in symbols if symbol.startswith(FORBIDDEN_PREFIXES)
    )
    unexpected_legacy = sorted(legacy_symbols(symbols) - allowed_legacy)
    if missing or forbidden or compiler or unexpected_legacy:
        raise AssertionError(
            json.dumps({
                "missingRequired": missing,
                "forbiddenBuildersOrEncoders": forbidden,
                "compilerSymbols": compiler,
                "uninventoriedLegacySymbols": unexpected_legacy,
            }, indent=2, sort_keys=True)
        )
    return names


def compile_header(cc: Path, prefix: Path, work: Path) -> None:
    work.mkdir(parents=True, exist_ok=True)
    source = work / "target_plan_load_header_probe.c"
    source.write_text(
        "#include <xray_target_plan_load.h>\n"
        "static XrRuntimeArtifactAuthority *authority;\n"
        "static XrRuntimeArtifactAuthorityIdentity identity;\n"
        "static XrTargetPlan *plan;\n"
        "int main(void) { return xr_runtime_artifact_authority_load_available() || "
        "authority != 0 || plan != 0 || "
        "identity.schema_version != 0; }\n",
        encoding="utf-8",
    )
    if os.name == "nt":
        obj = work / "target_plan_load_header_probe.obj"
        command = [
            str(cc), "/nologo", "/W4", "/WX", "/c",
            f"/I{prefix / 'include/xray'}", f"/Fo{obj}", str(source),
        ]
    else:
        obj = work / "target_plan_load_header_probe.o"
        command = [
            str(cc), "-std=c11", "-Wall", "-Wextra", "-Werror", "-c",
            "-I", str(prefix / "include/xray"), "-o", str(obj), str(source),
        ]
    result = run(command, cwd=work)
    if result.returncode != 0 or not obj.is_file():
        raise AssertionError(f"installed header is not standalone:\n{result.stdout}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--project-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--cc", type=Path, required=True)
    args = parser.parse_args()
    root = args.project_root.resolve(strict=True)
    build = args.build_dir.resolve(strict=True)
    binary = args.binary.resolve(strict=True)
    cc = args.cc.resolve(strict=True)
    dumpbin = cc.parent / "dumpbin.exe"
    if os.name == "nt":
        if not dumpbin.is_file():
            raise SystemExit(f"MSVC symbol inspector is missing: {dumpbin}")
        os.environ["DUMPBIN"] = str(dumpbin)
    inventory = json.loads(
        (root / "contracts/target-machine/legacy-product-residue.json").read_text(
            encoding="utf-8", errors="strict"
        )
    )
    allowed_legacy = set(inventory["legacy_symbol_tokens"])

    target = run([str(binary), "toolchain", "list", "--target", "native", "--json"])
    host = json.loads(target.stdout)["normalizedTarget"]
    archive_name = "xray_vm_runtime.lib" if os.name == "nt" else "libxray_vm_runtime.a"

    with tempfile.TemporaryDirectory(prefix="xray-installed-runtime-symbols-") as temporary:
        work = Path(temporary)
        symbol_sets: list[list[str]] = []
        for component, directory in (("XrayCore", "core"), (None, "full")):
            prefix = work / directory
            installed = install(build, prefix, component)
            if installed.returncode != 0:
                raise AssertionError(f"install failed:\n{installed.stdout}")
            header = prefix / "include/xray/xray_target_plan_load.h"
            if not header.is_file():
                raise AssertionError(f"installed load header is missing: {header}")
            archive = prefix / f"lib/xray/vm/{host}/{archive_name}"
            if not archive.is_file():
                raise AssertionError(f"installed runtime archive is missing: {archive}")
            symbol_sets.append(inspect(archive, allowed_legacy))
            compile_header(cc, prefix, work / directory)
        if symbol_sets[0] != symbol_sets[1]:
            raise AssertionError("Core and full installs expose different runtime symbols")

    print("installed runtime symbol inventory: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
