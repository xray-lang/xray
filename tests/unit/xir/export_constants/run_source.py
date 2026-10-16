"""Give each Source and source-free library execution its own disposable inputs."""
import argparse,os,shutil,sys,tempfile
from pathlib import Path
code=Path(__file__).resolve().parents[4];sys.path.insert(0,str(code/'tests/cli'))
from xir_no_child_process import NoChildProcess
parser=argparse.ArgumentParser();parser.add_argument('binary');parser.add_argument('--emit');parser.add_argument('--direct',action='store_true');args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix='xir-exported-constants-') as temporary:
    root=Path(temporary)
    if not args.direct:
        for source in Path(__file__).parent.glob('*.xr'):shutil.copyfile(source,root/source.name)
    command=[args.binary]+([] if args.direct else [str(root)])
    if args.emit:command.append(args.emit)
    policy=NoChildProcess(code,root)
    result=policy.run('exported-constant-consumer',command,dict(os.environ))
    sys.stdout.buffer.write(result.stdout);sys.stderr.buffer.write(result.stderr)
    if policy.supported:print('NoChild absolute-path denial control and inherited Job process limit2 PASS',flush=True)
    raise SystemExit(result.returncode)
