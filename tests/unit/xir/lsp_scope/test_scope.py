from pathlib import Path
import hashlib
import os
import subprocess
import sys

exe, stdlib = map(Path, sys.argv[1:3])
base = exe.resolve().parent / "scope-inputs #😀"
project, other, growth = (base / n for n in ("project", "other", "growth"))
bad, utf8, large, empty = (base / n for n in ("bad", "utf8", "large", "empty"))
for folder in (project / "sub", project / "vendor", project / ".hidden", other, growth, bad, utf8, large, empty):
    folder.mkdir(parents=True, exist_ok=True)
for folder, main in ((project, "root.xr"), (other, "other.xr"), (growth, "part00/unit.xr")):
    (folder / "xray.toml").write_text(
        f'[project]\nname="scope-{folder.name}"\nversion="1.0.0"\nmain="{main}"\n', encoding="utf-8"
    )
(project / "root.xr").write_bytes(b"\x80")
(project / "lib.xr").write_text("export fn target(value:i64)->i64{return value}\n", encoding="utf-8")
(project / "reverse.xr").write_text('import "./lib" as lib\nfn other()->i64{return lib.target(2)}\n', encoding="utf-8")
(project / "sub/extra.xr").write_text("fn extra()->i64{return 3}\n", encoding="utf-8")
for rel in ("skip.xr", "vendor/ignored.xr", ".hidden/ignored.xr"):
    (project / rel).write_bytes(b"\x80")
for name in ("other.xr", "second.xr"):
    (other / name).write_text("fn other()->i64{return 4}\n", encoding="utf-8")
for i in range(20):
    folder = growth / f"part{i:02}"
    folder.mkdir(exist_ok=True)
    (folder / "unit.xr").write_text(f"fn unit()->i64{{return {i}}}\n", encoding="utf-8")
(bad / "xray.toml").write_text("[project\n", encoding="utf-8")
(bad / "invalid.xr").write_text("fn invalid()->i64{return 0}", encoding="utf-8")
(utf8 / "invalid.xr").write_bytes(b"\x80")
(large / "large.xr").write_bytes(b"a" * 8193)
missing, hidden = project / "unsaved/deep/new.xr", project / ".hidden/open.xr"
assert not missing.exists() and not missing.parent.exists() and not hidden.exists()
before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in base.rglob("*") if p.is_file()}
args = [str(project), (project / "root.xr").as_uri(), missing.as_uri(), str(other),
        (other / "other.xr").as_uri(), str(growth), str(bad), str(utf8), str(large), str(empty), hidden.as_uri()]
flags = sys.argv[3:]
result = subprocess.run([str(exe.resolve()), *args, *flags], check=False,
                        env=dict(os.environ, XRAY_STDLIB_PATH=str(stdlib.resolve())),
                        timeout=290 if any(f in flags for f in ("--rename", "--lens", "--binding-lenses")) else 29)
assert not missing.exists() and not missing.parent.exists() and not hidden.exists()
assert before == {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in base.rglob("*") if p.is_file()}
raise SystemExit(result.returncode)
