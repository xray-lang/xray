"""Run all required Unit consumers through real public VM and native paths."""
from run_xir_public_cli_nominal_ctfe import main
from xir_unit_argument_cases import CASES, NEGATIVE

if __name__ == '__main__':
    semantics={name:('Source comptime Unit forwarding' if name=='unit-comptime' else
        'ordinary logical Unit READ arguments through VM, generated C and native execution')
        for name,_,_,_ in CASES}
    raise SystemExit(main(CASES, NEGATIVE, 'UNIT_ARGUMENTS', semantics))
