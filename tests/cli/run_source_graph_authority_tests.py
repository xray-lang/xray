#!/usr/bin/env python3
"""Exercise exact project, package, script, and lock authorities through the CLI."""

from __future__ import annotations

import argparse
import hashlib
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile


def write(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def invoke(
    binary: Path,
    home: Path,
    command: str,
    entry: Path,
    *options: str,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["HOME"] = str(home)
    environment["USERPROFILE"] = str(home)
    return subprocess.run(
        [str(binary), command, *options, str(entry)],
        cwd=str(entry.parent),
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def require_success(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        raise AssertionError(
            f"{label} failed with {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def require_failure(
    result: subprocess.CompletedProcess[str], label: str, diagnostic: str
) -> None:
    output = result.stdout + result.stderr
    if result.returncode == 0 or diagnostic not in output:
        raise AssertionError(
            f"{label} did not reject with {diagnostic!r}\n"
            f"returncode: {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def create_cached_package(home: Path) -> str:
    package_source = "export fn answer() -> i64 { return 42 }\n"
    archive = home / ".xray" / "cache" / "acme-dep-1.2.3.tar.gz"
    archive.parent.mkdir(parents=True, exist_ok=True)
    payload = package_source.encode("utf-8")
    with tarfile.open(archive, "w:gz") as package:
        member = tarfile.TarInfo("src/main.xr")
        member.size = len(payload)
        member.mode = 0o644
        member.mtime = 0
        package.addfile(member, io.BytesIO(payload))
    write(
        home / ".xray" / "packages" / "acme" / "dep" / "1.2.3" / "src" / "main.xr",
        package_source,
    )
    return "sha256:" + hashlib.sha256(archive.read_bytes()).hexdigest()


def write_consumer(root: Path, checksum: str | None) -> Path:
    write(root / "xray.toml", '[project]\nname = "authority-consumer"\nmain = "main.xr"\n')
    if checksum is not None:
        write(
            root / "xray.lock",
            "[package.acme/dep]\n"
            'version = "1.2.3"\n'
            f'checksum = "{checksum}"\n'
            "dependencies = []\n",
        )
    entry = root / "main.xr"
    write(entry, 'import { answer } from "acme/dep"\nprint(answer())\n')
    return entry


def check_positive(binary: Path, home: Path, label: str, entry: Path, output: str) -> None:
    require_success(invoke(binary, home, "check", entry), f"{label} check")
    run = invoke(binary, home, "run", entry)
    require_success(run, f"{label} run")
    if output not in run.stdout.splitlines():
        raise AssertionError(f"{label} run did not publish {output!r}:\n{run.stdout}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()

    with tempfile.TemporaryDirectory(prefix="xray-source-authority-") as temporary:
        root = Path(temporary)
        home = root / "home"
        checksum = create_cached_package(home)

        script = root / "script"
        write(script / "dep.xr", "export fn value() -> i64 { return 5 }\n")
        write(script / "main.xr", 'import { value } from "./dep"\nprint(value())\n')
        check_positive(binary, home, "script authority", script / "main.xr", "5")

        package = root / "package"
        write(
            package / "xray.toml",
            '[package]\nname = "acme/app"\nversion = "1.0.0"\nmain = "main.xr"\n',
        )
        write(package / "dep.xr", "export fn value() -> i64 { return 7 }\n")
        write(package / "main.xr", 'import { value } from "./dep"\nprint(value())\n')
        check_positive(binary, home, "package authority", package / "main.xr", "7")

        consumer = write_consumer(root / "consumer", checksum)
        check_positive(binary, home, "project lock authority", consumer, "42")
        dump = invoke(binary, home, "run", consumer, "--dump-bytecode")
        require_success(dump, "project lock bytecode route")
        if "LOAD_MODULE_SLOT" not in dump.stdout + dump.stderr:
            raise AssertionError(
                "project package import did not consume the graph module slot\n"
                f"stdout:\n{dump.stdout}\nstderr:\n{dump.stderr}"
            )

        missing_lock = write_consumer(root / "missing-lock", None)
        for command in ("check", "run"):
            require_failure(
                invoke(binary, home, command, missing_lock),
                f"missing lock {command}",
                "requires an exact checksummed xray.lock entry",
            )

        wrong_lock = write_consumer(root / "wrong-lock", "sha256:" + "0" * 64)
        for command in ("check", "run"):
            require_failure(
                invoke(binary, home, command, wrong_lock),
                f"wrong checksum {command}",
                "checksum does not match its cached archive",
            )

        invalid_project = root / "invalid-project"
        write(
            invalid_project / "xray.toml",
            '[project]\nname = "mutated/authority"\nmain = "main.xr"\n',
        )
        write(invalid_project / "main.xr", "print(1)\n")
        for command in ("check", "run"):
            require_failure(
                invoke(binary, home, command, invalid_project / "main.xr"),
                f"authority mutation {command}",
                "cannot establish exact project/package module authority",
            )

    print("source graph authority CLI tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
