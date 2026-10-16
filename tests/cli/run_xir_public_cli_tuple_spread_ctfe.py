"""Retain all required Tuple spread compiler and runtime receiving cases."""
from run_xir_public_cli_nominal_ctfe import main
from xir_tuple_spread_cases import CASES, NEGATIVE, SEMANTICS

if __name__=='__main__':
    raise SystemExit(main(CASES,NEGATIVE,'TUPLE_SPREAD',SEMANTICS))
