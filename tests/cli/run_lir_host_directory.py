"""Compare checked directory leaves on VM and native using a private io overlay."""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading

from owned_case_workers import SpawnGate, run_ordered


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, help="Absolute path to the host C compiler")
    args = parser.parse_args()
    if args.compiler and (not args.compiler.is_absolute() or not args.compiler.is_file()):
        parser.error("--compiler must name an existing absolute compiler path")
    binary, root, output = args.binary.resolve(), args.root.resolve(), args.output.resolve()
    sys.path.insert(0, str(root / "scripts"))
    from xray_process import RecoveryRequired, run_child

    fixtures = root / "tests/fixtures/lir_cli/host_directory"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="host-directory-", dir=output) as directory:
        work = Path(directory)
        stdlib = work / "stdlib"
        shutil.copytree(root / "stdlib", stdlib)
        with (stdlib / "io/io.xr").open("ab") as source:
            source.write(b"\n" + (fixtures / "wrappers.xr").read_bytes())
        io_lines = (stdlib / "io/io.xr").read_text(encoding="utf-8").splitlines()
        inner_close = next(number for number, line in enumerate(io_lines, 1) if "// innermost close" in line)
        cross_frame = next(number for number, line in enumerate(io_lines, 1) if "// cross-frame cleanup call" in line)
        read_source = next(number for number, line in enumerate(io_lines, 1) if "// checked read" in line)
        close_source = next(number for number, line in enumerate(io_lines, 1) if "// checked innermost close" in line)
        bound_read = next((number, line.index("__dirNext") + 1) for number, line in enumerate(io_lines, 1)
                          if "// checked bound read" in line)
        live_read = next((number, line.index("__dirNext") + 1) for number, line in enumerate(io_lines, 1)
                         if "// live Array read" in line)
        live_close = next((number, line.index("__dirClose") + 1) for number, line in enumerate(io_lines, 1)
                          if line.endswith("// live Array close"))
        live_close_only = next((number, line.index("__dirClose") + 1) for number, line in enumerate(io_lines, 1)
                               if "// live Array close-only failure" in line)
        env = dict(os.environ, XRAY_STDLIB_PATH=str(stdlib), NO_COLOR="1")
        gate = SpawnGate()
        progress = threading.Lock()
        finished = {}
        published = 0

        def run(command, cwd, label):
            out, err = cwd / (label + ".stdout"), cwd / (label + ".stderr")
            with out.open("xb") as stdout, err.open("xb") as stderr:
                status = run_child(list(map(str, command)), cwd=cwd, env=env,
                                   stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, timeout=30,
                                   spawn_guard=gate.launch)
            return status, out.read_bytes(), err.read_bytes()

        name = "目录项.txt"
        successes = {
            "roundtrip": f"{name}\ntrue\n".encode(), "same_spelling": b"true\n",
            "cleanup_late": f"{name}\n".encode(), "cleanup_pair": b"17\n",
            "cleanup_next": f"{name}\ntrue\n".encode(), "cleanup_next_non_tail": f"{name}\ntrue\n".encode(),
            "cleanup_loop": b"3\n0\n", "cleanup_loop_late_control": b"7\n3\n",
            "cleanup_loop_retired": b"2\n0\n",
            "cleanup_loop_live_owner": b"2\n1\n1\n0\n1\n0\n",
            "cleanup_loop_present_name": b"7\n0\n",
            "cleanup_loop_path_formal": b"1\n1\n0\n-1\n",
            "cleanup_loop_checked_array": b"1\n1\n0\n-1\n",
        }
        faults = ("invalid_close", "invalid_next", "stale_next", "cleanup_lifo_failure",
                  "cleanup_next_read_failure", "cleanup_next_dual_failure", "cleanup_next_close_failure",
                  "cleanup_next_bound_read_failure", "cleanup_loop_live_owner_failure",
                  "cleanup_loop_live_owner_close_failure")
        rejections = {
            "private_leaf": b"import requires an exported declaration",
            "cleanup_partial": b"directory cleanup requires a complete normal exit",
            "cleanup_managed_exit": b"directory cleanup requires a scalar frame",
            "cleanup_cross_frame": b"operation needs a panic landing with active cleanup",
            "cleanup_next_reassigned": b"operation needs a panic landing with active cleanup",
            "cleanup_next_cross_frame": b"operation needs a panic landing with active cleanup",
            "cleanup_loop_write": b"operation needs a panic landing with active cleanup",
            "cleanup_loop_escape": b"operation needs a panic landing with active cleanup",
            "cleanup_loop_unguarded_name": b"operation needs a panic landing with active cleanup",
            "cleanup_loop_path_reused": b"operation needs a panic landing with active cleanup",
            "cleanup_loop_checked_array_prior_owner": b"operation needs a panic landing with active cleanup",
        }
        def execute(index, case):
            nonlocal published
            vm_dir, native_dir = work / (case + "-vm"), work / (case + "-native")
            for cwd in (vm_dir, native_dir):
                (cwd / "one").mkdir(parents=True)
                (cwd / "one" / name).write_bytes(b"entry")
                (cwd / "empty").mkdir()
                if case in ("cleanup_loop", "cleanup_loop_late_control"):
                    (cwd / "other").mkdir()
                    (cwd / "directory").mkdir()
                    for entry in ("Zulu", name, "alpha"):
                        (cwd / "directory" / entry).write_bytes(b"entry")
            source = fixtures / (case + ".xr")
            executable = native_dir / ("program.exe" if os.name == "nt" else "program")
            vm = run([binary, "run", source], vm_dir, "run")
            build = [binary, "build", "--native", "-o", executable, source]
            if args.compiler:
                build += ["--cc", args.compiler]
            built = run(build, native_dir, "build")
            if case in rejections:
                assert vm == built and vm[0] == 1 and not vm[1] and not executable.exists(), (case, vm, built)
                assert rejections[case] in vm[2], (case, vm)
                if case == "cleanup_cross_frame":
                    assert f"io.xr:{cross_frame}:".encode() in vm[2], (case, vm)
            else:
                assert built[0] == 0 and not built[2] and executable.is_file(), (case, built)
                native = run([executable], native_dir, "run")
                assert vm == native, (case, vm, native)
                if case in successes:
                    assert vm == (0, successes[case], b""), (case, vm)
                else:
                    assert vm[0] == 1 and not vm[1], (case, vm)
                    assert b"XR_RUN_6002: value operation rejected (status 1)" in vm[2], (case, vm)
                    if case == "cleanup_lifo_failure":
                        assert f"io.xr:{inner_close}:9".encode() in vm[2], (case, vm)
                    if case == "cleanup_loop_live_owner_failure":
                        primary, secondary = vm[2].splitlines()
                        assert f"io.xr:{live_read[0]}:{live_read[1]}".encode() in primary, (case, vm)
                        assert secondary.startswith(b"note: directory cleanup rejected its token at "), (case, vm)
                        assert f"io.xr:{live_close[0]}:{live_close[1]}".encode() in secondary, (case, vm)
                    if case == "cleanup_loop_live_owner_close_failure":
                        primary, = vm[2].splitlines()
                        assert f"io.xr:{live_close_only[0]}:{live_close_only[1]}".encode() in primary, (case, vm)
                    if case.startswith("cleanup_next_"):
                        lines = vm[2].splitlines()
                        primary = close_source if case == "cleanup_next_close_failure" else read_source
                        column = 9 if case == "cleanup_next_close_failure" else 12
                        if case == "cleanup_next_bound_read_failure":
                            primary, column = bound_read
                        assert f"io.xr:{primary}:{column}".encode() in lines[0], (case, vm)
                        if case == "cleanup_next_dual_failure":
                            assert len(lines) == 2 and lines[1].startswith(b"note: directory cleanup rejected its token at "), (case, vm)
                            assert f"io.xr:{close_source}:9".encode() in lines[1], (case, vm)
                        else:
                            assert len(lines) == 1, (case, vm)
            with progress:
                finished[index] = case
                while published in finished:
                    completed = finished.pop(published)
                    print(f"{completed}: VM and native agree on the directory contract", flush=True)
                    published += 1

        run_ordered((*successes, *faults, *rejections), execute, 2, gate, RecoveryRequired)


if __name__ == "__main__":
    main()
