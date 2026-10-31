"""Execute the original functional demo and preserve conservative self-binding refusals."""
from pathlib import Path
import argparse, json, os, subprocess, sys, tempfile

EXPECTED = (b"counter: 1, 2, 3\ntwice(addOne, 5) = 7\ndoubleThenAdd(5) = 20\n"
            b"triple(7) = 21\nquadruple(7) = 28\nsum of even squares: 220\n"
            b"fib(0) = 0\nfib(1) = 1\nfib(2) = 1\nfib(3) = 2\nfib(4) = 3\n"
            b"fib(5) = 5\nfib(6) = 8\nfib(7) = 13\nfib(8) = 21\nfib(9) = 34\n")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--native", action="store_true")
    args = parser.parse_args()
    sys.path.insert(0, str(args.root / "scripts"))
    from xray_process import run_child
    output = Path(tempfile.mkdtemp(prefix="xray-local-recursive-"))
    records = []
    def run(name, command):
        stdout, stderr = output / (name + ".stdout"), output / (name + ".stderr")
        with stdout.open("xb") as out, stderr.open("xb") as err:
            code = run_child([str(x) for x in command], cwd=output, env=os.environ.copy(), stdout=out, stderr=err, timeout=30)
        records.append(dict(name=name, args=[str(x) for x in command], exit=code))
        (output / "commands.json").write_text(json.dumps(records, indent=2) + "\n")
        return code, stdout.read_bytes(), stderr.read_bytes()
    demo = args.root / "demos/04-functional/functional.xr"
    code, out, err = run("check", [args.binary, "check", demo])
    assert code == 0 and not out and not err, (code, out, err, output)
    code, out, err = run("vm", [args.binary, "run", demo])
    assert code == 0 and out == EXPECTED and not err, (code, out, err, output)
    generated = output / "functional.c"
    code, out, err = run("emit", [args.binary, "build", "-c", demo, "-o", generated])
    assert code == 0 and not err and generated.is_file(), (code, out, err, output)
    raw = generated.read_bytes()
    assert b'#include "xir/xxir_instance_hash.h"' in raw
    assert b"XrXirHashRequest" in raw and b"xr_xir_instance_hash_apply" in raw
    for name in ("rebind_old", "nested_write", "shadow", "borrow", "defer_write", "other_uninitialized"):
        source = args.root / "tests/fixtures/xir_local_recursive_binding" / (name + ".xr")
        code, out, err = run(name, [args.binary, "check", source])
        assert code == 1 and not out and b"error: name is not an initialized value" in err, (name, code, out, err, output)
    if args.native:
        executable = output / ("functional.exe" if os.name == "nt" else "functional")
        code, out, err = run("build-native", [args.binary, "build", demo, "-o", executable])
        assert code == 0 and not err and executable.is_file(), (code, out, err, output)
        code, out, err = run("native", [executable])
        assert code == 0 and out == EXPECTED and not err, (code, out, err, output)
    print("complete functional Source/VM/C, conservative self-binding negatives" + ("/MSVC native" if args.native else "") + " PASS " + str(output))

if __name__ == "__main__":
    main()
