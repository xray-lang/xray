"""Check the complete variadic Array concat READ argument recipe."""
from run_xir_public_cli_nominal_ctfe import main
from xir_array_concat_call_cases import CASES, NEGATIVE, SEMANTICS

if __name__=='__main__':
    raise SystemExit(main(CASES,NEGATIVE,'ARRAY_CONCAT_CALLS',SEMANTICS))
