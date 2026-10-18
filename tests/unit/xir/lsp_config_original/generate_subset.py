"""Build a checked subset from original config bodies without server substitutes."""
from pathlib import Path
import hashlib
import re
import sys


def function_span(text, start):
    opening = text.index('{', start)
    depth, at, state = 1, opening + 1, 'code'
    while at < len(text):
        char, pair = text[at], text[at:at + 2]
        if state in ('string', 'character'):
            if char == '\\':
                at += 2
                continue
            if char == ('"' if state == 'string' else "'"):
                state = 'code'
        elif state == 'line':
            if char == '\n':
                state = 'code'
        elif state == 'block':
            if pair == '*/':
                state = 'code'
                at += 2
                continue
        elif pair in ('//', '/*'):
            state = 'line' if pair == '//' else 'block'
            at += 2
            continue
        elif char in ('"', "'"):
            state = 'string' if char == '"' else 'character'
        elif char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if not depth:
                return start, at + 1
        at += 1
    raise ValueError('Unterminated original function')


if __name__ == '__main__':
    original, entry, output = map(Path, sys.argv[1:])
    raw = original.read_bytes()
    text = raw.decode('utf-8')
    matches = list(re.finditer(r'^TEST\((\w+)\)', text, re.MULTILINE))
    names = [m[1] for m in matches]
    calls = re.findall(r'^\s+RUN_TEST\((\w+)\);', entry.read_text(encoding='utf-8'), re.MULTILINE)
    if len(names) != 16 or calls != [x for x in names if x != 'config_defaults']:
        raise SystemExit('Original helper inventory changed; reconcile every test.')
    removed = [function_span(text, m.start()) for m in matches if m[1] == 'config_defaults']
    mains = list(re.finditer(r'^int main\(', text, re.MULTILINE))
    if len(mains) != 1:
        raise SystemExit('Expected one original full entry point.')
    removed.append(function_span(text, mains[0].start()))
    subset = text
    for start, end in sorted(removed, reverse=True):
        subset = subset[:start] + '\n' * subset[start:end].count('\n') + subset[end:]
    for match in matches:
        if match[1] == 'config_defaults':
            continue
        start, end = function_span(text, match.start())
        if text[start:end] not in subset:
            raise SystemExit('Original test body changed: ' + match[1])
    def include_path(match):
        return '#include "' + (original.parent / match[1]).resolve().as_posix() + '"'
    subset = re.sub(r'^#include "([^"]+)"', include_path, subset, flags=re.MULTILINE)
    header = '/* Original source SHA256: ' + hashlib.sha256(raw).hexdigest() + ' */\n'
    output.write_text(header + subset + '\n' + entry.read_text(encoding='utf-8'),
                      encoding='utf-8', newline='\n')
    print('Generated 15 original bodies unchanged; real server defaults remains required separately.')
