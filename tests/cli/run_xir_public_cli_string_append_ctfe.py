"""Check complete compiler String updates and library joins."""
from run_xir_public_cli_nominal_ctfe import main
from xir_string_append_ctfe_cases import CASES, NEGATIVE

if __name__ == '__main__':
    raise SystemExit(main(CASES, NEGATIVE, 'STRING_APPEND_CTFE'))
