"""Execute one independent native root oracle and audit the actual link map."""
from pathlib import Path
import argparse
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(root / 'scripts'))
from check_xr_program_aot_native import forbidden_symbol_family, load_symbol_inventory

EXPECTED = {
    'root_identity': 'native root_identity two-instance/gate/unbound-call physical0 PASS',
    'resume': 'native resume same-root/stale-token/owned-result physical0 PASS',
    'closing': 'native closing root-cleanup-once/admission/twin physical0 PASS',
    'init_failure': 'native init_failure partial-publication/sticky24/owned-panic physical0 PASS',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--case', choices=EXPECTED, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    assert executable.is_file(), 'native root executable missing'
    run = subprocess.run([str(executable), args.case], capture_output=True, timeout=110)
    assert run.returncode == 0, (run.returncode, run.stdout, run.stderr)
    lines = run.stdout.decode('utf-8').splitlines()
    assert len(lines) == 2 and lines[1] == EXPECTED[args.case], lines
    assert re.fullmatch(r'compiler physical blocks/bytes=0/0; peak=\d+', lines[0]), lines
    assert re.fullmatch(r'source fixture compiler ledger: work=\d+ allocated=\d+ peak=\d+ live=\d+\r?\n',
                        run.stderr.decode('utf-8')), run.stderr
    symbols, error = load_symbol_inventory(executable)
    assert symbols is not None, error
    assert forbidden_symbol_family(symbols) is None, 'retired compiler/VM family linked'
    assert re.search(r'(?<![A-Za-z0-9_])(?:__imp_)?_*xr_xir_(?:vm_|compile_(?:source_|specialize|emit_c|vm_))',
                     symbols, re.IGNORECASE) is None, 'Source/emitter/VM linked into native consumer'
    print(EXPECTED[args.case])
    print('NATIVE_ROOT_SYMBOLS current Source/emitter/VM and retired compiler/VM absent')


if __name__ == '__main__':
    main()
