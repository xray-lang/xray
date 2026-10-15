"""Require a closed owning compiler constant in an open generic field default."""
from pathlib import Path
import argparse,json,os,subprocess,tempfile
from xir_no_child_process import NoChildProcess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--output',type=Path);args=parser.parse_args()
    root,binary=args.root.resolve(),args.binary.resolve();binary.relative_to(root);temporary=None
    if args.output:output=args.output.resolve();output.relative_to(root);output.mkdir(parents=True,exist_ok=False)
    else:temporary=tempfile.TemporaryDirectory(prefix='open-generic-default-',dir=binary.parent);output=Path(temporary.name)
    try:
        policy=NoChildProcess(root,output);stdlib=output/'stdlib';stdlib.mkdir()
        (output/'future-packet.xr').write_text(
            'fn seed()->Array<i8>{var xs:Array<i8>=[7,11,13];return xs.reserve(8)}\n'
            'export struct TextPacket<T>{value:T;items:Array<i8>=comptime seed();init(value){this.value=value}}\n',encoding='utf-8')
        entry=output/'root.xr';entry.write_text('import {TextPacket} from "./future-packet"\nconst packet=TextPacket<i64>(1)\nprint(packet.items[0],packet.items.capacity)\n',encoding='utf-8')
        env=dict(os.environ,XRAY_STDLIB_PATH=str(stdlib),NO_COLOR='1',PATH=str(output/'no-C-provider'))
        result=policy.run('open-generic-default-vm',[str(binary),'run',str(entry)],env)
        (output/'vm.stdout').write_bytes(result.stdout);(output/'vm.stderr').write_bytes(result.stderr)
        (output/'result.json').write_text(json.dumps(dict(exit_code=result.returncode,expected_stdout_hex=b'7 8\n'.hex(),
            actual_stdout_hex=result.stdout.hex(),stderr=result.stderr.decode('utf-8',errors='replace'),
            positive_requirement=True,expected_success=True,Windows_absolute_child_denial=policy.control),indent=2)+'\n',encoding='utf-8')
        assert (result.returncode,result.stdout,result.stderr)==(0,b'7 8\n',b''),result
        print('Closed owning constant in generic field default PASS');return 0
    finally:
        if temporary:temporary.cleanup()
if __name__=='__main__':raise SystemExit(main())
