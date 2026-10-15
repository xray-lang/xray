"""Check declared Array callback arguments without source or host fallback."""
from run_xir_public_cli_nominal_ctfe import main
from xir_array_callback_call_cases import CASES, NEGATIVE, SEMANTICS

if __name__=='__main__':
    raise SystemExit(main(CASES,NEGATIVE,'ARRAY_CALLBACK_CALLS',SEMANTICS))
