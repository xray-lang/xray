"""Run the complete closed owned text family through real public consumers."""
from run_xir_public_cli_nominal_ctfe import main
from xir_owned_text_cases import CASES, NEGATIVE

if __name__ == '__main__':
    raise SystemExit(main(CASES,NEGATIVE,'OWNED_TEXT',
        {name:'closed owned display through the shared formatter; no I/O in CTFE' for name,_,_,_ in CASES}))
