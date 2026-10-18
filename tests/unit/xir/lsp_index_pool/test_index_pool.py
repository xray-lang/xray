from pathlib import Path
import subprocess
import sys

root = Path.cwd() / "fixture-inputs #"
root.mkdir(exist_ok=True)
(root / "nested").mkdir(exist_ok=True)
(root / ".ignored").mkdir(exist_ok=True)
(root / "alpha.xr").write_text("/* 😀 */ export fn alpha()->i64 { return 1 }\n", encoding="utf-8")
(root / "recovered.xr").write_text("export fn kept()->i64 { return 1 }\nvar *** = ;\n", encoding="utf-8")
(root / "nested" / "emoji-😀.xr").write_text("fn third()->i64 { return 3 }\n", encoding="utf-8")
(root / ".ignored" / "hidden.xr").write_text("fn hidden()->i64 { return 4 }\n", encoding="utf-8")
(root / "readme.txt").write_text("not a source", encoding="utf-8")
result = subprocess.run([sys.argv[1], str(root.resolve()), *sys.argv[2:]], check=False)
raise SystemExit(result.returncode)
