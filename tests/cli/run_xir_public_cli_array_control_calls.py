"""Check the original Array recipes with actual compiler control and object owners."""
from run_xir_public_cli_nominal_ctfe import main
from xir_array_control_call_cases import CASES, NEGATIVE

if __name__=='__main__':
    raise SystemExit(main(CASES,NEGATIVE,'ARRAY_CONTROL_CALLS'))
