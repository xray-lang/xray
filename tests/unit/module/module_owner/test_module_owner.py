"""Build a real source tree and remove packaged source before invoking the compiler."""
import pathlib
import subprocess
import sys
import tempfile
with tempfile.TemporaryDirectory(prefix="xray-module-owner-") as directory:
    root = pathlib.Path(directory)
    for name, source in {
        "main.xr": 'import "./dep"\nimport "./leaf"\nvar main = 42\n',
        "dep.xr": 'import "./leaf"\nvar dep = 2\n',
        "leaf.xr": 'var leaf = 3\n',
        "cycle_a.xr": 'import "./cycle_b"\nvar a = 1\n',
        "cycle_b.xr": 'import "./cycle_a"\nvar b = 2\n',
        "missing.xr": 'import "./absent"\n',
        "io/output.xr": 'export fn answer() -> Int { return 42 }\n',
    }.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding="utf-8")
    for i in range(20):
        (root / f"many_{i}.xr").write_text(f"var value_{i} = {i}\n", encoding="utf-8")
    (root / "many.xr").write_text("".join(f'import "./many_{i}"\n' for i in range(20)), encoding="utf-8")
    (root / "io/output.xr").unlink()
    assert not (root / "io/output.xr").exists()
    subprocess.run([sys.argv[1], str(root)], check=True)
