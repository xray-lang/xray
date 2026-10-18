"""Compile a real native Program using only the relocated installed runtime SDK."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile


EXPECTED = b"Native compiler owner: 42 and owned UTF-8 with embedded NUL\n"


def main():
    parser = argparse.ArgumentParser()
    for name in ["cmake", "ninja", "build", "producer", "consumer-source", "project",
                 "compiler", "dumpbin", "provider", "output"]:
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    build = Path(args.build).resolve()
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    run_root = Path(tempfile.mkdtemp(prefix="客户 native-", dir=output)).resolve()
    env = dict(os.environ, TEMP=str(run_root), TMP=str(run_root),
               PYTHONDONTWRITEBYTECODE="1", PYTHONIOENCODING="utf-8")
    commands = []

    def save():
        (run_root / "commands.json").write_text(
            json.dumps(commands, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    def run(name, argv, cwd=run_root):
        row = dict(name=name, argv=list(map(str, argv)), state="RUNNING")
        commands.append(row)
        save()
        result = subprocess.run(row["argv"], cwd=cwd, env=env, capture_output=True, timeout=120)
        (run_root / (name + ".stdout")).write_bytes(result.stdout)
        (run_root / (name + ".stderr")).write_bytes(result.stderr)
        row.update(state="TERMINAL", exit_code=result.returncode)
        save()
        assert result.returncode == 0, (name, result.returncode, str(run_root))
        return result.stdout

    def move_owned(source, destination):
        source = source.resolve()
        destination = destination.resolve()
        assert source.is_relative_to(run_root) and destination.is_relative_to(run_root)
        assert source != run_root and not destination.exists()
        source.rename(destination)

    original = run_root / "original-prefix"
    moved = run_root / "relocated-prefix"
    run("install-sdk", [args.cmake, "--install", build, "--prefix", original,
                        "--component", "XraySDK"])
    move_owned(original, moved)
    sdk = moved / "lib/xray/xir-runtime-sdk"
    manifest_bytes = (sdk / "sdk_manifest.json").read_bytes()
    assert manifest_bytes == (build / "xir-runtime-sdk/sdk_manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    expected_files = {row["path"] for row in manifest["files"]} | {"sdk_manifest.json"}
    assert {p.relative_to(sdk).as_posix() for p in sdk.rglob("*") if p.is_file()} == expected_files
    for row in manifest["files"]:
        data = (sdk / row["path"]).read_bytes()
        assert len(data) == row["length"] and hashlib.sha256(data).hexdigest() == row["sha256"]
    assert not list(moved.rglob("*.xr")) and not list(moved.rglob("*.chk"))
    assert not list(moved.rglob("xray.exe")) and not list(moved.rglob("CMakeCache.txt"))
    customer = run_root / "customer-source"
    customer.mkdir()
    shutil.copyfile(Path(args.project) / "CMakeLists.txt", customer / "CMakeLists.txt")
    shutil.copyfile(args.consumer_source, customer / "consumer.c")
    run("emit-real-program", [args.producer, customer / "native-program.c"])
    customer_build = run_root / "customer-build"
    run("configure-customer", [args.cmake, "-S", customer, "-B", customer_build,
                               "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                               "-DCMAKE_C_COMPILER=" + args.compiler,
                               "-DXIR_SDK_BUNDLE_DIR=" + str(sdk)])
    detected = (customer_build / "consumer-provider.txt").read_text(encoding="utf-8").strip()
    assert detected == {"msvc": "MSVC", "clang": "Clang"}[args.provider]
    run("build-customer", [args.cmake, "--build", customer_build, "--parallel", "2"])
    rows = json.loads((customer_build / "compile_commands.json").read_text(encoding="utf-8"))
    assert len(rows) == 2
    root = Path(args.consumer_source).resolve().parents[3]
    for row in rows:
        command = row["command"].replace("\\", "/").lower()
        assert str(root).replace("\\", "/").lower() not in command
        assert str(build / "xir-runtime-sdk").replace("\\", "/").lower() not in command
        assert str(build / "generated").replace("\\", "/").lower() not in command
        assert Path(row["file"]).resolve().is_relative_to(customer)
    dependencies = run("customer-dependencies", [args.ninja, "-C", customer_build, "-t", "deps"])
    normalized = dependencies.replace(b"\\", b"/").lower()
    assert str(root).replace("\\", "/").lower().encode("utf-8") not in normalized
    assert str(build / "xir-runtime-sdk").replace("\\", "/").lower().encode("utf-8") not in normalized
    counts = re.findall(rb"#deps (\d+)", dependencies)
    assert len(counts) == 2 and all(int(count) > 0 for count in counts)
    executable = customer_build / "installed_native_owner.exe"
    imports = run("imports", [args.dumpbin, "/dependents", executable])
    dlls = re.findall(rb"^\s*([^\s]+\.dll)\s*$", imports, re.M | re.I)
    assert dlls and not any(name.lower().startswith(b"xray") for name in dlls)
    assert run("run-installed", [executable], customer_build).replace(b"\r\n", b"\n") == EXPECTED
    unavailable = run_root / "unavailable-sdk-prefix"
    move_owned(moved, unavailable)
    assert not sdk.exists() and not original.exists()
    assert run("run-without-sdk", [executable], customer_build).replace(b"\r\n", b"\n") == EXPECTED
    summary = dict(provider=args.provider, detected_compiler=detected,
                   manifest_files=len(manifest["files"]),
                   original_install_prefix_absent=True, relocated_sdk_hidden_before_second_run=True,
                   customer_compile_uses_only_customer_and_installed_sdk=True,
                   actual_emitted_packet_and_ledger_lifetime_checks=True,
                   xray_dll_imports=[], commands=len(commands), run_root=str(run_root),
                   full_source_sdk_installer_and_safety_qualified=False)
    (run_root / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
