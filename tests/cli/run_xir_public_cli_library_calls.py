"""Require authentic native arguments and compiler-owned library callbacks."""
from run_xir_public_cli_nominal_ctfe import main
from xir_library_call_cases import CASES, NEGATIVE, SEMANTICS

if __name__=='__main__':
    raise SystemExit(main(CASES,NEGATIVE,'LIBRARY_CALLS',SEMANTICS))
