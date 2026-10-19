"""Require complete declared Array mutation arguments and ordered root access."""
from run_xir_public_cli_nominal_ctfe import main
from xir_array_mutation_call_cases import CASES, NEGATIVE, SEMANTICS

if __name__=='__main__':
    raise SystemExit(main(CASES,NEGATIVE,'ARRAY_MUTATION_CALLS',SEMANTICS))
