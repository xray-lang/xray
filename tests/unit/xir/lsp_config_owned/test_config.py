"""Exercise actual owned TOML and configuration adapters with exact budgets."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

cases = [
    ('[lsp]\ndiagnostics_enabled = false\ndiagnostic_debounce_ms = 500\n'
     'completion_max_items = 50\nformat_tab_size = 2\nformat_insert_spaces = false\n', 0),
    ('[lsp]\ndiagnostics_enabled = true\nignore = ["node_modules", "build", "*.log"]\n', 0),
    ('[lsp]\ninlay_hints_type_annotations = false\ninlay_hints_parameter_names = false\n', 0),
    ('[lsp]\nformat_tab_size = 8\n', 0),
    ('[lsp]\nlog_path = "日志/λ.log"\nlog_to_stderr = false\n'
     'format_align_branch_arrows = true\nformat_align_enum_values = true\n'
     'format_align_struct_fields = true\nformat_align_trailing_comments = true\n'
     'format_wrap_long_lines = true\nformat_multiline_trailing_comma = true\n'
     'format_max_line_length = 120\nignore = ["", "cache/", "?.tmp", "/a/b/", "λ"]\n'
     'unknown_setting = { value = "ignored" }\n', 0),
    ('[build]\ntarget = "native"\n', 10),
    ('[lsp]\nformat_tab_size = "bad"\n', 1),
    ('[lsp]\nignore = ["valid", 3]\n', 1),
    ('[lsp]\nlog_path = "a\\u0000b"\n', 1),
    ('[lsp]\ncompletion_max_items = 2147483648\n', 1),
    ('[lsp]\nformat_tab_size = 0\n', 1),
    (None, 10),
    ('[lsp\nformat_tab_size = 2\n', 1),
    ('[lsp]\nignore = ["a\\u0000b"]\n', 1),
    ('[lsp]\ndiagnostic_debounce_ms = -1\n', 1),
    ('[lsp]\nlog_to_stderr = 1\n', 1),
]
with tempfile.TemporaryDirectory(prefix='xray-config-', dir=os.getcwd()) as folder:
    for mode, (text, wanted) in enumerate(cases):
        root = Path(folder) / str(mode)
        root.mkdir()
        if text is not None:
            (root / 'xray.toml').write_text(text, encoding='utf-8', newline='\n')
        result = subprocess.run([sys.argv[1], os.path.relpath(root), str(mode), str(wanted),
                                 'normal' if os.environ.get('XR_LSP_CONFIG_NORMAL_ONLY') else 'full'],
                                capture_output=True, text=True, encoding='utf-8', timeout=29)
        print(result.stdout, end='')
        if result.returncode:
            print(result.stderr, file=sys.stderr)
            raise SystemExit(result.returncode)
