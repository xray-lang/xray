"""Capture the original macOS carrier and its debugger output without rebuilding it."""

import argparse
import atexit
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tarfile


REFERENCE = "4db048c39b99e14c09d300b9111d5a9874701d86"
ARCHIVE_SHA256 = "5d17d1a5b1323f131555e35e0d3390c0ddc7cb16e66335cd574fb54a4477351e"
BINARY_SHA256 = "e2409ff3bcb3642a5a90bf9547da10240421d0912069fed55d745090fe6e843e"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--inspect-only", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {"state": "PREPARING", "reference": REFERENCE, "commands": []}

    def save():
        (output / "result.json").write_text(
            json.dumps(report, indent=2) + "\n", encoding="utf-8"
        )

    def retain_incomplete_observation():
        if report["state"] in ("PREPARING", "RUNNING"):
            report.update(state="INCOMPLETE", boundary="Capture did not reach a complete observation")
            save()

    atexit.register(retain_incomplete_observation)
    save()
    archive = args.archive.read_bytes()
    if digest(archive) != ARCHIVE_SHA256:
        raise ValueError("The reference archive does not match the supplied original")
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as members:
        candidates = [
            member for member in members.getmembers()
            if member.isfile() and PurePosixPath(member.name).name == "xray"
        ]
        if len(candidates) != 1:
            raise ValueError("The archive must contain one regular xray executable")
        with members.extractfile(candidates[0]) as stream:
            binary_bytes = stream.read()
    if digest(binary_bytes) != BINARY_SHA256:
        raise ValueError("The executable does not match the supplied original")
    binary = output / "xray-reference"
    binary.write_bytes(binary_bytes)
    binary.chmod(0o755)
    report.update(archiveSha256=ARCHIVE_SHA256, binarySha256=BINARY_SHA256)

    names = subprocess.check_output(
        ["git", "-C", str(root), "ls-tree", "-r", "--name-only", REFERENCE, "demos"],
        text=True,
    ).splitlines()
    inputs = {}
    for name in ("control_flow", "atomic"):
        matches = [path for path in names if PurePosixPath(path).name == name + ".xr"]
        if len(matches) != 1:
            raise ValueError("The reference tree must contain one " + name + " input")
        inputs[name] = matches[0]
    sources = subprocess.check_output(
        ["git", "-C", str(root), "archive", "--format=tar", REFERENCE,
         "stdlib", *inputs.values()]
    )
    source_root = output / "source"
    source_root.mkdir()
    with tarfile.open(fileobj=io.BytesIO(sources), mode="r:") as members:
        for member in members:
            relative = PurePosixPath(member.name)
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError("Reference source archive contains an unsafe path")
            target = source_root.joinpath(*relative.parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                with members.extractfile(member) as stream:
                    target.write_bytes(stream.read())
            else:
                raise ValueError("Reference source archive contains a non-file entry")
    report["inputs"] = {
        name: {"path": path, "sha256": digest((source_root / path).read_bytes())}
        for name, path in inputs.items()
    }
    if args.inspect_only:
        report.update(state="NOT_RUN", boundary="Archive and input identity inspection only")
        save()
        return
    if sys.platform != "darwin":
        raise RuntimeError("Execution requires the original supported macOS host")
    debugger = shutil.which("lldb")
    if debugger is None:
        raise RuntimeError("lldb is required to capture the original failure")
    sys.path.insert(0, str(root / "scripts"))
    from xray_process import run_child

    report["state"] = "RUNNING"
    environment = dict(os.environ, NO_COLOR="1", XRAY_STDLIB_PATH=str(source_root / "stdlib"))

    def run(name, argv):
        stdout = output / (name + ".stdout")
        stderr = output / (name + ".stderr")
        with stdout.open("xb") as out, stderr.open("xb") as err:
            code = run_child(
                list(map(str, argv)), env=environment, cwd=str(source_root),
                stdin=subprocess.DEVNULL, stdout=out, stderr=err, timeout=60,
            )
        report["commands"].append({
            "name": name, "argv": list(map(str, argv)), "exit": code,
            "ownedChildrenDrained": True,
            "stdoutSha256": digest(stdout.read_bytes()),
            "stderrSha256": digest(stderr.read_bytes()),
        })
        save()
        return code

    if run("identity", [binary, "--version", "--json"]) != 0:
        raise RuntimeError("The original executable could not report its identity")
    for name, path in inputs.items():
        program = [binary, "run", source_root / path]
        run(name + "-direct", program)
        run(name + "-lldb", [
            debugger, "--batch", "--no-lldbinit",
            "--one-line", "settings set target.disable-aslr false",
            "--one-line", "image list -o -f", "--one-line", "run",
            "--one-line", "image list -o -f", "--one-line", "thread backtrace all",
            "--one-line", "register read",
            "--one-line", "disassemble --frame --bytes --count 32", "--", *program,
        ])
    report.update(
        state="REFERENCE_OBSERVATION_COMPLETE",
        boundary="Raw exits and debugger evidence only. Runtime adoption remains unproven.",
    )
    save()


if __name__ == "__main__":
    main()
