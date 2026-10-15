"""Check complete owning Array, text and tuple library constants."""
from run_xir_public_cli_nominal_ctfe import main
from xir_library_owned_result_cases import CASES, NEGATIVE

if __name__ == '__main__':
    raise SystemExit(main(CASES, NEGATIVE, 'LIBRARY_OWNED_RESULTS'))
