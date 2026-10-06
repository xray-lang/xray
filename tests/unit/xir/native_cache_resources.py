"""Run a complete immutable Source fixture before bounded cache selection."""
import pathlib
import subprocess
import sys
import tempfile

executable, source = map(pathlib.Path, sys.argv[1:3])
mode = sys.argv[3]
assert mode in ("measure", "faults")
with tempfile.TemporaryDirectory(prefix="xir-cache-resource-") as temporary:
    root = pathlib.Path(temporary).resolve()
    text = (source/"tests/fixtures/xir_stdlib_output/root.xr").read_text(encoding="utf-8")
    (root/"root.xr").write_text("export fn padding() -> i64 { return 7 }\n" + text +
                             "\nexport fn writeStdout() -> bool { return false }\nexport fn writer() -> fn(string)->bool { return output.writeStdout }\n", encoding="utf-8")
    subprocess.run([str(executable.resolve()), str(root), str(root/"root.xr"), mode], check=True)
