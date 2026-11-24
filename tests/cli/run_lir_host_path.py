"""Check shared host path results and filesystem effects on both product backends."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile


@contextmanager
def file_descriptor_limit(enabled):
    if os.name == "nt" or not enabled:
        yield
        return
    import resource

    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    limit = 64 if soft == resource.RLIM_INFINITY else min(soft, 64)
    resource.setrlimit(resource.RLIMIT_NOFILE, (limit, hard))
    try:
        yield
    finally:
        resource.setrlimit(resource.RLIMIT_NOFILE, (soft, hard))


def stat_observations(path, is_link):
    metadata = path.stat()
    is_file = stat.S_ISREG(metadata.st_mode)
    is_dir = stat.S_ISDIR(metadata.st_mode)
    if os.name == "nt":
        mode = stat.S_IRUSR | (metadata.st_mode & stat.S_IWUSR)
        if is_dir:
            mode |= stat.S_IXUSR
        ctime_ns = getattr(metadata, "st_birthtime_ns", metadata.st_ctime_ns)
        ids = [False, False]
    else:
        mode = metadata.st_mode & 0o777
        ctime_ns = metadata.st_ctime_ns
        ids = [True, metadata.st_uid, True, metadata.st_gid]
    return [True, metadata.st_size, mode, metadata.st_mtime_ns // 10**9,
            metadata.st_atime_ns // 10**9, ctime_ns // 10**9, *ids, is_file, is_dir, is_link]


def check_stat_observer(work, root, binary, appendix, run):
    # The copied canonical module is the only fixture with access to private metadata.
    stdlib = work / "stdlib"
    shutil.copytree(root / "stdlib", stdlib)
    with (stdlib / "io/io.xr").open("ab") as module:
        module.write(b"\n" + appendix.read_bytes())

    directory = work / "stat-observer"
    directory.mkdir()
    data_file = directory / "元数据.txt"
    data_file.write_bytes(b"measure\0size")
    (directory / "target-dir").mkdir()
    link = directory / "link-dir"
    if os.name == "nt":
        subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", "link-dir", "target-dir"],
                       cwd=directory, capture_output=True, check=True, timeout=30)
    else:
        link.symlink_to("target-dir", target_is_directory=True)
    fixture = directory / "observe.xr"
    fixture.write_text('import { observeFileStat } from io\nimport path\n'
                       'observeFileStat(path.from("元数据.txt"))\n'
                       'observeFileStat(path.from("link-dir"))\n'
                       'observeFileStat(path.from("absent"))\n', encoding="utf-8")
    values = [*stat_observations(data_file, False), *stat_observations(link, True), False]
    expected = "".join(str(value).lower() + "\n" for value in values).encode("ascii")
    vm = run([binary, "run", fixture], directory, "observe-vm", stdlib=stdlib)
    output = directory / ("observer.exe" if os.name == "nt" else "observer")
    built = run([binary, "build", fixture, "-o", output], directory, "observe-build", stdlib=stdlib)
    assert built[0] == 0 and not built[2], built
    native = run([output], directory, "observe-native")
    assert vm == native == (0, expected, b""), (vm, native, expected)
    print("file_stat_observer: both backends match all metadata fields and the followed link")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root, binary = args.root.resolve(), args.binary.resolve()
    sys.path.insert(0, str(root / "scripts"))
    from xray_process import run_child

    fixtures = root / "tests/fixtures/lir_cli/host_path"
    environment = dict(os.environ, XRAY_STDLIB_PATH=str(root / "stdlib"), NO_COLOR="1")
    args.output.mkdir(parents=True, exist_ok=True)

    def run(command, cwd, stem, limit=False, stdlib=None):
        out, err = cwd / (stem + ".stdout"), cwd / (stem + ".stderr")
        env = dict(environment, XRAY_STDLIB_PATH=str(stdlib)) if stdlib is not None else environment
        with out.open("xb") as stdout, err.open("xb") as stderr:
            with file_descriptor_limit(limit):
                status = run_child(list(map(str, command)), cwd=cwd, env=env,
                                   stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, timeout=30)
        return status, out.read_bytes(), err.read_bytes()

    with tempfile.TemporaryDirectory(prefix="host-path-", dir=args.output.resolve()) as directory:
        work = Path(directory)
        private_types = ("file_storage_private", "file_stat_private")
        for name in ("mkdir", "nul_path", "rename", "rename_nul_source", "rename_nul_destination",
                     "unregistered", "file_open_write", "file_open_refused", "file_stat", "file_stat_nul", *private_types):
            source = fixtures / (name + ".xr")
            vm_dir, native_dir = work / (name + "-vm"), work / (name + "-native")
            for cwd in (vm_dir, native_dir):
                cwd.mkdir()
                (cwd / "file").write_bytes(b"keep")
                if name == "rename":
                    (cwd / "起点.txt").write_bytes(b"old\x00bytes")
                    (cwd / "source").write_bytes(b"new")
                    (cwd / "occupied").write_bytes(b"existing")
                elif name in ("rename_nul_source", "rename_nul_destination"):
                    (cwd / "must-stay").write_bytes(b"untouched")
                if name == "file_open_write":
                    (cwd / "preserved").write_bytes(b"preserve")
                    (cwd / "truncated").write_bytes(b"truncate")
                if name == "file_stat":
                    (cwd / "数据.txt").write_bytes(b"snapshot")
                    (cwd / "目录").mkdir()
            limited = name == "file_open_write"
            vm = run([binary, "run", source], vm_dir, "run", limit=limited)
            output = native_dir / ("program.exe" if os.name == "nt" else "program")
            built = run([binary, "build", source, "-o", output], native_dir, "build")
            if name == "unregistered" or name in private_types:
                assert vm == built, (name, vm, built)
                assert vm[0] == 1 and not vm[1] and not output.exists(), (name, vm)
                expected = (b"import requires an exported declaration" if name in private_types else
                            b"function without a scalar body is not supported by LIR lowering yet")
                assert expected in vm[2], vm
            else:
                assert built[0] == 0 and not built[2], (name, built)
                native = run([output], native_dir, "run", limit=limited)
                assert vm == native, (name, vm, native)
                if name == "mkdir":
                    assert vm == (0, b"true\ntrue\nfalse\nfalse\n", b""), vm
                    assert all((cwd / "目录").is_dir() and not (cwd / "missing").exists()
                               for cwd in (vm_dir, native_dir)), name
                elif name in ("nul_path", "file_stat_nul"):
                    assert vm[0] == 1 and not vm[1] and b"XR_RUN_6002: value operation rejected (status 1)" in vm[2], vm
                    assert all(not (cwd / "must-not-exist").exists() for cwd in (vm_dir, native_dir)), name
                elif name == "rename":
                    collision = b"false\n" if os.name == "nt" else b"true\n"
                    assert vm == (0, b"true\nfalse\n" + collision, b""), vm
                    for cwd in (vm_dir, native_dir):
                        assert not (cwd / "起点.txt").exists() and not (cwd / "missing").exists(), name
                        assert (cwd / "到达.txt").read_bytes() == b"old\x00bytes"
                        if os.name == "nt":
                            assert (cwd / "source").read_bytes() == b"new"
                            assert (cwd / "occupied").read_bytes() == b"existing"
                        else:
                            assert not (cwd / "source").exists()
                            assert (cwd / "occupied").read_bytes() == b"new"
                elif name in ("rename_nul_source", "rename_nul_destination"):
                    assert vm[0] == 1 and not vm[1] and b"XR_RUN_6002: value operation rejected (status 1)" in vm[2], vm
                    for cwd in (vm_dir, native_dir):
                        assert (cwd / "must-stay").read_bytes() == b"untouched"
                        assert not (cwd / "must-not-exist").exists()
                elif name == "file_open_write":
                    assert vm == (0, b"true\n" * 5, b""), vm
                    for cwd in (vm_dir, native_dir):
                        assert (cwd / "preserved").read_bytes() == b"preserve"
                        assert (cwd / "truncated").read_bytes() == b""
                        assert (cwd / "implicit").is_file()
                elif name == "file_stat":
                    expected = b"true\n8\ntrue\nfalse\ntrue\ntrue\nfalse\nfalse\n-1\nfalse\n"
                    assert vm == (0, expected, b""), vm
                    for cwd in (vm_dir, native_dir):
                        assert (cwd / "数据.txt").read_bytes() == b"snapshot"
                        assert (cwd / "目录").is_dir() and not (cwd / "不存在").exists()
                else:
                    assert vm == (0, b"true\n", b""), vm
                    assert all(not (cwd / "missing").exists() for cwd in (vm_dir, native_dir)), name
            assert all((cwd / "file").read_bytes() == b"keep" for cwd in (vm_dir, native_dir)), name
            print(f"{name}: VM and native agree on output, exit and filesystem effects")
        check_stat_observer(work, root, binary, fixtures / "file_stat_observer.xr", run)


if __name__ == "__main__":
    main()
