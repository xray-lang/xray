"""Real Source/VM/native Set<i64> owner and binding regression fixtures."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser=argparse.ArgumentParser()
parser.add_argument("--root",type=Path,required=True)
parser.add_argument("--xray",type=Path,required=True)
parser.add_argument("--check-only",action="store_true")
args=parser.parse_args()
fixtures=args.root/"tests/fixtures/set_i64_physical"
with tempfile.TemporaryDirectory(prefix="xray-set-i64-") as temporary:
    for source in sorted(fixtures.glob("*.xr")):
        expected=source.with_suffix(".stdout").read_text(encoding="utf-8")
        def execute(command):
            return subprocess.run([str(x) for x in command],cwd=args.root,capture_output=True,text=True,encoding="utf-8",timeout=30)
        checked=execute([args.xray,"check",source])
        assert checked.returncode==0,(source,checked.stderr)
        if args.check_only:
            continue
        vm=execute([args.xray,"run",source])
        assert vm.returncode==0 and vm.stdout==expected and not vm.stderr,(source,"VM",vm.returncode,vm.stdout,vm.stderr)
        target=Path(temporary)/(source.stem+".exe")
        built=execute([args.xray,"build",source,"-o",target])
        assert built.returncode==0 and target.is_file(),(source,"build",built.stderr)
        native=execute([target])
        assert native.returncode==0 and native.stdout==expected and not native.stderr,(source,"native",native.returncode,native.stdout,native.stderr)
    rejected={
        "f64.xr":"SET_NEW is not supported by LIR lowering yet",
        "string.xr":"module slot payload type is not supported by LIR lowering yet",
        "captured.xr":"FUNCTION_REF captured physical binding transport is not supported by LIR lowering yet",
    }
    for filename,diagnostic in rejected.items():
        source=fixtures/"unsupported"/filename
        checked=execute([args.xray,"check",source])
        assert checked.returncode==0,(source,checked.stderr)
        if args.check_only:
            continue
        for command in ([args.xray,"run",source],
                        [args.xray,"build","--c-only","-o",Path(temporary)/"unsupported.c",source]):
            result=execute(command)
            assert result.returncode==1 and not result.stdout and diagnostic in result.stderr,(source,command,result.returncode,result.stdout,result.stderr)
print("Set<i64> physical fixtures: "+("Source check only" if args.check_only else "VM and native")+" PASS")
