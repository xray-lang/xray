#!/usr/bin/env python3
"""
Add type annotations to .xr test files.
Uses heuristic inference from parameter names and usage patterns.
"""

import re
import sys
import glob
import os

# Heuristic type inference from parameter names
NAME_TYPE_MAP = {
    # Integer names
    'n': 'i64', 'i': 'i64', 'j': 'i64', 'k': 'i64', 'x': 'i64', 'y': 'i64',
    'z': 'i64', 'a': 'i64', 'b': 'i64', 'c': 'i64', 'count': 'i64', 'num': 'i64',
    'size': 'i64', 'len': 'i64', 'length': 'i64', 'index': 'i64', 'idx': 'i64',
    'start': 'i64', 'end': 'i64', 'step': 'i64', 'depth': 'i64', 'level': 'i64',
    'width': 'i64', 'height': 'i64', 'age': 'i64', 'port': 'i64', 'id': 'i64',
    'limit': 'i64', 'max': 'i64', 'min': 'i64', 'total': 'i64', 'sum': 'i64',
    'left': 'i64', 'right': 'i64', 'low': 'i64', 'high': 'i64', 'mid': 'i64',
    'row': 'i64', 'col': 'i64', 'r': 'i64', 'amount': 'i64', 'base': 'i64',
    'exp': 'i64', 'power': 'i64', 'target': 'i64', 'capacity': 'i64',
    'expected': 'i64', 'actual': 'i64', 'result': 'i64', 'val': 'i64',
    'first': 'i64', 'second': 'i64', 'third': 'i64',
    'rows': 'i64', 'cols': 'i64', 'times': 'i64', 'iterations': 'i64',
    'threshold': 'i64', 'repeat_count': 'i64',
    
    # Float names
    'rate': 'f64', 'price': 'f64', 'balance': 'f64', 'salary': 'f64',
    'pi': 'f64', 'radius': 'f64', 'area': 'f64', 'weight': 'f64',
    'temperature': 'f64', 'temp': 'f64', 'ratio': 'f64',
    'dx': 'f64', 'dy': 'f64',
    
    # String names
    'name': 'string', 'msg': 'string', 'message': 'string', 's': 'string',
    'str': 'string', 'text': 'string', 'label': 'string', 'title': 'string',
    'key': 'string', 'prefix': 'string', 'suffix': 'string', 'sep': 'string',
    'separator': 'string', 'delimiter': 'string', 'pattern': 'string',
    'path': 'string', 'url': 'string', 'host': 'string', 'method': 'string',
    'filename': 'string', 'type_name': 'string', 'class_name': 'string',
    'char': 'string', 'unique_char': 'string', 'word': 'string',
    'input': 'string', 'output': 'string', 'fmt': 'string', 'format': 'string',
    'tag': 'string', 'category': 'string', 'description': 'string',
    'op': 'string', 'operator': 'string', 'color': 'string',
    'species': 'string', 'breed': 'string', 'sound': 'string',
    
    # Bool names
    'flag': 'bool', 'is_valid': 'bool', 'enabled': 'bool', 'verbose': 'bool',
    'debug': 'bool', 'done': 'bool', 'found': 'bool', 'ok': 'bool',
    'condition': 'bool', 'ascending': 'bool', 'reverse': 'bool',
    
    # Array names
    'arr': 'Array<any>', 'list': 'Array<any>', 'items': 'Array<any>',
    'data': 'Array<any>', 'numbers': 'Array<i64>', 'strings': 'Array<string>',
    'elements': 'Array<any>', 'values': 'Array<any>', 'args': 'Array<any>',
    'tasks': 'Array<any>', 'results': 'Array<any>', 'parts': 'Array<any>',
    'lines': 'Array<string>', 'words': 'Array<string>',
    
    # Function names
    'fn_arg': 'fn(): any', 'callback': 'fn(): void', 'handler': 'fn(): void',
    'f': 'fn(any): any', 'func': 'fn(any): any', 'predicate': 'fn(any): bool',
    'transform': 'fn(any): any', 'mapper': 'fn(any): any',
    'compareFn': 'fn(any, any): i64',
    
    # Map names
    'map': 'Map<string, any>', 'dict': 'Map<string, any>',
    'config': 'Map<string, any>', 'options': 'Map<string, any>',
    'headers': 'Map<string, string>',
}

def infer_param_type(param_name, fn_name, fn_body_lines):
    """Infer parameter type from name and usage context."""
    # Clean param name
    clean = param_name.strip()
    if clean.startswith('_'):
        clean = clean[1:]
    
    # Direct name match
    if clean in NAME_TYPE_MAP:
        return NAME_TYPE_MAP[clean]
    
    # Pattern matching on name
    if clean.startswith('is_') or clean.startswith('has_') or clean.startswith('can_') or clean.startswith('should_'):
        return 'bool'
    if clean.endswith('_count') or clean.endswith('_size') or clean.endswith('_index') or clean.endswith('_num'):
        return 'i64'
    if clean.endswith('_name') or clean.endswith('_str') or clean.endswith('_text') or clean.endswith('_msg'):
        return 'string'
    if clean.endswith('_list') or clean.endswith('_arr') or clean.endswith('_array'):
        return 'Array<any>'
    if clean.endswith('_map') or clean.endswith('_dict'):
        return 'Map<string, any>'
    if clean.endswith('_fn') or clean.endswith('_func') or clean.endswith('_callback'):
        return 'fn(): any'
    
    # Context: check usage in body
    body_text = '\n'.join(fn_body_lines) if fn_body_lines else ''
    
    # If used in string interpolation or concat with string
    if f'"{clean}"' in body_text or f"${{{clean}}}" in body_text:
        pass  # could be anything
    
    # If compared with a number
    if re.search(rf'\b{re.escape(clean)}\s*[<>=!]+\s*\d', body_text):
        return 'i64'
    if re.search(rf'\d\s*[<>=!]+\s*{re.escape(clean)}\b', body_text):
        return 'i64'
    
    # If used in arithmetic
    if re.search(rf'\b{re.escape(clean)}\s*[\+\-\*/%]', body_text):
        return 'i64'
    
    # If used as array index
    if re.search(rf'\[{re.escape(clean)}\]', body_text):
        return 'i64'
    
    # If .length or .push called on it
    if re.search(rf'\b{re.escape(clean)}\.length\b', body_text):
        return 'Array<any>'
    if re.search(rf'\b{re.escape(clean)}\.push\b', body_text):
        return 'Array<any>'
    
    # If string methods called on it
    if re.search(rf'\b{re.escape(clean)}\.(toUpperCase|toLowerCase|trim|split|contains|startsWith|endsWith|replace|repeat)\b', body_text):
        return 'string'
    
    # Default to any
    return 'any'

def infer_return_type(fn_name, fn_body_lines):
    """Infer return type from function body."""
    if not fn_body_lines:
        return 'void'
    
    body_text = '\n'.join(fn_body_lines)
    
    # Check if function has return statements with values
    returns = re.findall(r'\breturn\s+(.+)', body_text)
    if not returns:
        return 'void'
    
    # Check return value patterns
    for ret in returns:
        ret = ret.strip().rstrip(';').strip()
        if ret == '' or ret == 'null':
            continue
        if ret == 'true' or ret == 'false':
            return 'bool'
        if re.match(r'^-?\d+$', ret):
            return 'i64'
        if re.match(r'^-?\d+\.\d+$', ret):
            return 'f64'
        if ret.startswith('"') or ret.startswith("'"):
            return 'string'
        if ret.startswith('['):
            return 'Array<any>'
        if ret.startswith('{') or ret.startswith('#{'):
            return 'Map<string, any>'
    
    # If all returns are void/null, it's void
    non_empty = [r for r in returns if r.strip() and r.strip() != 'null']
    if not non_empty:
        return 'void'
    
    return 'any'

def process_file(filepath, dry_run=False):
    """Process a single .xr file to add type annotations."""
    with open(filepath, 'r') as f:
        content = f.read()
    
    lines = content.split('\n')
    changes = 0
    
    # Track function bodies for context
    new_lines = []
    i = 0
    while i < len(lines):
        line = lines[i]
        
        # Match function declaration: fn name(params)
        # But NOT arrow functions or @test decorated functions (which have no params)
        m = re.match(r'^(\s*)(fn\s+\w+\s*)\(([^)]*)\)(.*)$', line)
        if m:
            indent = m.group(1)
            fn_prefix = m.group(2)
            params_str = m.group(3)
            rest = m.group(4)
            
            fn_name_m = re.match(r'fn\s+(\w+)', fn_prefix)
            fn_name = fn_name_m.group(1) if fn_name_m else ''
            
            # Skip test functions (they have no params)
            if not params_str.strip():
                new_lines.append(line)
                i += 1
                continue
            
            # Collect function body for context
            body_lines = []
            brace_depth = line.count('{') - line.count('}')
            j = i + 1
            while j < len(lines) and brace_depth > 0:
                body_lines.append(lines[j])
                brace_depth += lines[j].count('{') - lines[j].count('}')
                j += 1
            
            # Process parameters
            params = params_str.split(',')
            new_params = []
            for p in params:
                p_stripped = p.strip()
                if not p_stripped:
                    new_params.append(p)
                    continue
                
                # Skip if already has type annotation
                if ':' in p_stripped:
                    new_params.append(p)
                    continue
                
                # Skip rest params
                if p_stripped.startswith('...'):
                    new_params.append(p)
                    continue
                
                # Handle default values: param = value
                default_match = re.match(r'^(\s*)(\w+)\s*=\s*(.+)$', p_stripped)
                if default_match:
                    param_name = default_match.group(2)
                    default_val = default_match.group(3).strip()
                    # Infer from default value
                    if default_val == 'true' or default_val == 'false':
                        ptype = 'bool'
                    elif re.match(r'^-?\d+$', default_val):
                        ptype = 'i64'
                    elif re.match(r'^-?\d+\.\d+$', default_val):
                        ptype = 'f64'
                    elif default_val.startswith('"') or default_val.startswith("'"):
                        ptype = 'string'
                    elif default_val == 'null':
                        ptype = 'any'
                    else:
                        ptype = infer_param_type(param_name, fn_name, body_lines)
                    
                    leading_space = p[:len(p) - len(p.lstrip())]
                    new_params.append(f"{leading_space}{param_name}: {ptype} = {default_val}")
                    changes += 1
                else:
                    # Simple param name
                    param_name = p_stripped
                    if not re.match(r'^[a-zA-Z_]\w*$', param_name):
                        new_params.append(p)
                        continue
                    
                    ptype = infer_param_type(param_name, fn_name, body_lines)
                    leading_space = p[:len(p) - len(p.lstrip())]
                    new_params.append(f"{leading_space}{param_name}: {ptype}")
                    changes += 1
            
            new_params_str = ','.join(new_params)
            
            # Check return type
            has_return_type = rest.strip().startswith(':')
            if not has_return_type:
                ret_type = infer_return_type(fn_name, body_lines)
                if ret_type != 'void':
                    # Add return type before the opening brace
                    brace_idx = rest.find('{')
                    if brace_idx >= 0:
                        rest = f": {ret_type} " + rest[brace_idx:]
                        changes += 1
                    else:
                        # Return type with { on next line
                        rest = f": {ret_type}" + rest
                        changes += 1
                else:
                    # void return - add ': void' 
                    brace_idx = rest.find('{')
                    if brace_idx >= 0:
                        rest = ": void " + rest[brace_idx:]
                        changes += 1
            
            new_line = f"{indent}{fn_prefix}({new_params_str}){rest}"
            new_lines.append(new_line)
        else:
            new_lines.append(line)
        
        i += 1
    
    if changes > 0 and not dry_run:
        with open(filepath, 'w') as f:
            f.write('\n'.join(new_lines))
    
    return changes

def main():
    dry_run = '--dry-run' in sys.argv
    target_dir = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else 'tests/regression'
    
    files = sorted(glob.glob(f'{target_dir}/**/*.xr', recursive=True))
    total_changes = 0
    files_changed = 0
    
    for f in files:
        changes = process_file(f, dry_run)
        if changes > 0:
            files_changed += 1
            total_changes += changes
            print(f"  {f}: {changes} changes")
    
    print(f"\nTotal: {total_changes} changes in {files_changed} files")
    if dry_run:
        print("(dry run, no files modified)")

if __name__ == '__main__':
    main()
