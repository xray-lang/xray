"""Check independent Source results and actual native/mixed link boundaries."""
from pathlib import Path
import argparse
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / 'scripts'))
from check_xr_program_aot_native import forbidden_symbol_family, load_symbol_inventory

EXPECTED = {'apply.xr': (7, '370a', 0), 'fixed.xr': (7, '370a', 0),
            'root.xr': (9, '390a', 1), 'forward.xr': (7, '370a', 0)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--case', choices=EXPECTED, required=True)
    parser.add_argument('--mode', choices=['native', 'native-vm', 'vm-native'], required=True)
    args = parser.parse_args()
    result, output, slots = EXPECTED[args.case]
    executable = args.executable.resolve()
    run = subprocess.run([str(executable), args.case, args.mode], capture_output=True, timeout=110)
    assert run.returncode == 0, (run.returncode, run.stdout, run.stderr)
    assert not run.stderr, run.stderr
    lines = run.stdout.decode('utf-8').splitlines()
    for instance in range(2):
        expected = (f'H1_EXEC_OUTPUT file={args.case} instance={instance} typed_groups=1 typed_type=I64 '
                    f'value={result} writes=1 bytes={output} begins=1 ready=1 published={slots} released={slots}')
        assert lines.count(expected) == 1, (expected, lines)
        pattern = (rf'H1_PROVIDER file={re.escape(args.case)} mode={args.mode} instance={instance} '
                   r'nativeSteps=(\d+) vmSteps=(\d+) nativeReleases=(\d+) vmReleases=(\d+) crossings=(\d+)')
        witness = [re.fullmatch(pattern, line) for line in lines]
        witness = [item for item in witness if item]
        assert len(witness) == 1, lines
        native_steps, vm_steps, native_releases, vm_releases, crossings = map(int, witness[0].groups())
        assert native_steps and native_releases
        if args.mode == 'native':
            assert (vm_steps, vm_releases, crossings) == (0, 0, 0)
        else:
            assert vm_steps and vm_releases and crossings
    final = (f'H1_NATIVE_DONE file={args.case} mode={args.mode} independentI64={result} outputBytes=2 '
             'twoInstances=1 codeReleases=1 physical=0/0')
    assert lines[-1] == final, lines
    symbols, error = load_symbol_inventory(executable)
    assert symbols is not None, error
    assert forbidden_symbol_family(symbols) is None, 'retired compiler/VM family linked'
    assert not re.search(r'(?<![A-Za-z0-9_])(?:__imp_)?_*xr_xir_compile_(?:source_|emit_c)', symbols, re.I)
    if args.mode == 'native':
        assert not re.search(r'(?<![A-Za-z0-9_])(?:__imp_)?_*xr_xir_(?:vm_|compile_(?:specialize|vm_))', symbols, re.I)
    else:
        assert re.search(r'(?<![A-Za-z0-9_])_*xr_xir_compile_vm_bind(?![A-Za-z0-9_])', symbols)
    assert re.search(r'h1native_[0-3]_f\d+', symbols), 'actual generated native bodies absent'
    print(final)
    print('H1_NATIVE_SYMBOLS canonical native bodies, owned mixed VM bindings when selected, Source/emitter absent')


if __name__ == '__main__':
    main()
