"""Run the independent native capacity oracle and inspect actual linked symbols."""
from pathlib import Path
import argparse
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / 'scripts'))
from check_xr_program_aot_native import forbidden_symbol_family, load_symbol_inventory

EXPECTED = ('NATIVE_CAPACITY_COMPLETE runtimeOnly=1 instances=2 typed17_NUL10=4 '
            'lowerBounds=9/11 aliases=3 trace12/0 negative422=4 overflowLIMIT=1 '
            'occupiedPreserved=1 escapedNullDomainBAD_ARGUMENT=3 BuiltPathInstances=2 lower13=2 BOUNDS430_1/1=2 physical=0/0')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    args = parser.parse_args()
    executable = args.executable.resolve()
    assert executable.is_file(), 'native capacity executable missing'
    run = subprocess.run([str(executable)], capture_output=True, timeout=110)
    assert run.returncode == 0, (run.returncode, run.stdout, run.stderr)
    lines = run.stdout.decode('utf-8').splitlines()
    assert len(lines) == 2 and lines[1] == EXPECTED, lines
    assert re.fullmatch(r'compiler physical blocks/bytes=0/0; peak=\d+', lines[0]), lines
    assert re.fullmatch(r'source fixture compiler ledger: work=\d+ allocated=\d+ peak=\d+ live=\d+\r?\n',
                        run.stderr.decode('utf-8')), run.stderr
    symbols, error = load_symbol_inventory(executable)
    assert symbols is not None, error
    assert forbidden_symbol_family(symbols) is None, 'legacy compiler/VM family linked into consumer'
    assert re.search(r'(?<![A-Za-z0-9_])(?:__imp_)?_*xr_xir_(?:vm_|compile_(?:source_|specialize|emit_c|vm_))',
                     symbols, re.IGNORECASE) is None, 'XIR Source/emitter/VM linked into native consumer'
    print(EXPECTED)
    print('NATIVE_CAPACITY_SYMBOLS current Source/emitter/VM and legacy compiler/VM absent')

if __name__ == '__main__':
    main()
