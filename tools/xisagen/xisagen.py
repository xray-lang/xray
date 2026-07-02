#!/usr/bin/env python3
"""
xisagen — XISA code generator (Python rewrite)

Reads declarative .def files, generates C headers for Xi IR and AOT metadata:
  - xi-ops:  xisa/xi/ops.def     → xi_ops_gen.h
  - xi-lowering: xisa/xi/lowering.def → Xi target lowering headers and tests
  - xi-verify: xisa/xi/verifier.def → xi_verify_gen.h
  - aot-rep: xisa/aot/rep.def    → xaot_rep_gen.h
  - aot-abi: xisa/aot/abi.def    → xaot_abi_gen.h
  - aot-layout: xisa/aot/layout.def → xaot_layout_gen.h

Usage:
  python3 xisagen.py xi-ops  <ops.def>     <output.h>
  python3 xisagen.py xi-lowering <ops.def> <lowering.def> <output-root>
  python3 xisagen.py xi-verify <ops.def> <verifier.def> <output.h>
  python3 xisagen.py aot-rep <rep.def>     <output.h>
  python3 xisagen.py aot-abi <rep.def> <abi.def> <output.h>
  python3 xisagen.py aot-layout <rep.def> <layout.def> <output.h>
  python3 xisagen.py test
"""

import sys
import re
import os
from dataclasses import dataclass, field
from typing import Optional

# ============================================================
# Common utilities
# ============================================================

def die(msg: str):
    print(f"xisagen: error: {msg}", file=sys.stderr)
    sys.exit(1)

def read_file(path: str) -> str:
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return f.read()
    except OSError as e:
        die(f"cannot read {path}: {e}")

def write_file(path: str, content: str):
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(content)

# ============================================================
# S-expression lexer + parser for .def files
# ============================================================

@dataclass
class SExpr:
    """S-expression node: either an atom or a list."""
    pass

@dataclass
class SAtom(SExpr):
    value: str          # raw text
    line: int = 0
    col: int = 0

    @property
    def is_keyword(self) -> bool:
        return self.value.startswith(':')

    @property
    def is_variable(self) -> bool:
        return self.value.startswith('$')

    @property
    def is_string(self) -> bool:
        return self.value.startswith('"')

    @property
    def is_number(self) -> bool:
        if not self.value:
            return False
        v = self.value
        if v.startswith('-'):
            v = v[1:]
        if not v:
            return False
        if v.startswith('0x') or v.startswith('0X'):
            return len(v) > 2 and all(c in '0123456789abcdefABCDEF' for c in v[2:])
        if v.startswith('0b') or v.startswith('0B'):
            return len(v) > 2 and all(c in '01' for c in v[2:])
        return v.isdigit()

    @property
    def int_value(self) -> int:
        v = self.value
        if v.startswith('0x') or v.startswith('0X'):
            return int(v, 16)
        if v.startswith('0b') or v.startswith('0B'):
            return int(v, 2)
        if v.startswith('-'):
            return -int(v[1:], 0)
        return int(v)

    @property
    def str_value(self) -> str:
        """Strip quotes from string atoms."""
        if self.is_string:
            return self.value[1:-1]
        return self.value

@dataclass
class SList(SExpr):
    children: list       # list of SExpr
    line: int = 0
    col: int = 0

    @property
    def head(self) -> Optional[SExpr]:
        return self.children[0] if self.children else None

    def get_kw(self, keyword: str) -> Optional[SExpr]:
        """Find value after :keyword in this list."""
        for i, child in enumerate(self.children):
            if isinstance(child, SAtom) and child.value == keyword:
                if i + 1 < len(self.children):
                    return self.children[i + 1]
        return None

    def get_kw_int(self, keyword: str, default: int = 0) -> int:
        v = self.get_kw(keyword)
        if v and isinstance(v, SAtom) and v.is_number:
            return v.int_value
        return default

    def get_kw_str(self, keyword: str, default: str = '') -> str:
        v = self.get_kw(keyword)
        if v and isinstance(v, SAtom):
            return v.str_value
        return default

    def get_kw_list(self, keyword: str) -> Optional['SList']:
        v = self.get_kw(keyword)
        if isinstance(v, SList):
            return v
        return None


def tokenize_sexpr(text: str, path: str = '<input>'):
    """Tokenize S-expression text into (token, line, col) tuples."""
    tokens = []
    i = 0
    line = 1
    line_start = 0

    while i < len(text):
        c = text[i]

        # Whitespace
        if c in ' \t\r':
            i += 1
            continue
        if c == '\n':
            i += 1
            line += 1
            line_start = i
            continue

        # Comment
        if c == ';':
            while i < len(text) and text[i] != '\n':
                i += 1
            continue

        col = i - line_start + 1

        # Parens
        if c == '(':
            tokens.append(('(', line, col))
            i += 1
            continue
        if c == ')':
            tokens.append((')', line, col))
            i += 1
            continue

        # String
        if c == '"':
            start = i
            i += 1
            while i < len(text) and text[i] != '"':
                if text[i] == '\\':
                    i += 1
                i += 1
            if i >= len(text):
                die(f"{path}:{line}:{col}: unterminated string")
            i += 1  # closing "
            tokens.append((text[start:i], line, col))
            continue

        # Number (including negative)
        if c.isdigit() or (c == '-' and i + 1 < len(text) and text[i + 1].isdigit()):
            start = i
            if c == '-':
                i += 1
            if i + 1 < len(text) and text[i] == '0' and text[i + 1] in 'xXbB':
                i += 2
                while i < len(text) and (text[i].isalnum()):
                    i += 1
            else:
                while i < len(text) and text[i].isdigit():
                    i += 1
            tokens.append((text[start:i], line, col))
            continue

        # Symbol / keyword / variable
        # Starts with: letter _ : $ + * /
        if c.isalpha() or c in '_:$+*/':
            start = i
            i += 1
            while i < len(text) and (text[i].isalnum() or text[i] in '_-.:'):
                i += 1
            tokens.append((text[start:i], line, col))
            continue

        die(f"{path}:{line}:{col}: unexpected character '{c}'")

    return tokens


def parse_sexpr(tokens: list, path: str = '<input>') -> list[SExpr]:
    """Parse token list into S-expression tree. Returns list of top-level forms."""
    pos = [0]  # mutable index

    def parse_one() -> Optional[SExpr]:
        if pos[0] >= len(tokens):
            return None
        tok, line, col = tokens[pos[0]]

        if tok == ')':
            die(f"{path}:{line}:{col}: unexpected ')'")

        if tok == '(':
            pos[0] += 1
            children = []
            while pos[0] < len(tokens) and tokens[pos[0]][0] != ')':
                child = parse_one()
                if child:
                    children.append(child)
            if pos[0] >= len(tokens):
                die(f"{path}:{line}:{col}: unclosed '('")
            pos[0] += 1  # skip ')'
            return SList(children=children, line=line, col=col)

        # Atom
        pos[0] += 1
        return SAtom(value=tok, line=line, col=col)

    forms = []
    while pos[0] < len(tokens):
        form = parse_one()
        if form:
            forms.append(form)
    return forms


# ============================================================
# L3: Xi semantic op metadata
# ============================================================

VALID_XI_EFFECTS = {
    'allocates',
    'may-suspend',
    'may-throw',
    'memory-read',
    'memory-write',
    'releases',
    'retains',
    'side-effect',
}

VALID_XI_TARGETS = {
    'aot-c',
    'aot-verify',
    'vm-bytecode',
}

VALID_XI_LOWERING_POLICIES = {
    'generated',
    'pass-local',
    'special',
    'verifier-only',
}

VALID_XI_SPECULATION_POLICIES = {
    'never',
    'safe',
}

VALID_XI_VN_KINDS = {
    'memory-read',
    'none',
    'pure',
}

VALID_XI_ALGEBRAIC_TRAITS = {
    'associative',
    'commutative',
}

VALID_XI_TBAA_GROUPS = {
    'array',
    'chan',
    'const',
    'field',
    'global',
    'json',
    'none',
    'shared',
    'struct',
    'tls',
    'top',
    'tuple',
    'upval',
}

VALID_XI_RESULT_NATIVE_TYPES = {
    'i8',
    'u8',
    'i16',
    'u16',
    'i32',
    'u32',
    'i64',
    'u64',
    'f32',
    'f64',
    'bool',
}

VALID_XI_RESULT_KINDS = {
    'value',
    'void',
    'dynamic',
}

VALID_XI_RESULT_OWNERSHIPS = {
    'borrowed',
    'call-result',
    'none',
    'owned',
}

VALID_XI_BACKEND_REWRITES = {
    'none',
    'builtin',
}

VALID_XI_ESCAPE_USES = {
    'arg',
    'global',
    'heap',
    'none',
}

VALID_XI_ESCAPE_ALLOCS = {
    'heap',
    'none',
}

VALID_XI_OWN_USES = {
    'borrow',
    'consume',
    'method-args',
    'stored-value',
}

VALID_XI_IC_SITES = {
    'field',
    'method',
    'none',
}


@dataclass
class XiOperandDef:
    kind: str
    name: str
    attrs: dict = field(default_factory=dict)


@dataclass
class XiResultDef:
    kind: str
    name: str
    attrs: dict = field(default_factory=dict)


@dataclass
class XiOpDef:
    name: str
    ident: str
    cls: str
    arity: int
    operands: list[XiOperandDef]
    results: list[XiResultDef]
    effects: list
    requires: list
    observable: list
    targets: list
    result_kind: str
    result_ownership: str
    result_native_type: str
    lowering_policy: str
    speculation: str
    vn_kind: str
    algebraic: list
    tbaa_group: str
    backend_rewrite: str
    backend_rewrite_name: Optional[str]
    escape_use: str
    escape_alloc: str
    own_use: str
    ic_site: str
    negated_op: Optional[str]


def _xi_c_ident(token: str) -> str:
    ident = re.sub(r'[^0-9A-Za-z_]', '_', token).upper()
    ident = re.sub(r'_+', '_', ident).strip('_')
    if ident and ident[0].isdigit():
        ident = '_' + ident
    return ident


def _xi_op_ident(name: str) -> str:
    if not name.startswith('xi.'):
        die(f"Xi op name must start with 'xi.': {name}")
    return _xi_c_ident(name[3:])


def _sexpr_atom_value(expr: SExpr, context: str) -> str:
    if not isinstance(expr, SAtom):
        die(f"{context}: expected atom")
    return expr.str_value


def _xi_get_kw(form: SList, keyword: str) -> Optional[SExpr]:
    found = []
    for i, child in enumerate(form.children):
        if isinstance(child, SAtom) and child.value == keyword:
            if i + 1 >= len(form.children):
                die(f"{keyword}: missing value")
            found.append(form.children[i + 1])
    if len(found) > 1:
        die(f"duplicate {keyword}")
    return found[0] if found else None


def _xi_get_kw_str(form: SList, keyword: str, default: str = '') -> str:
    value = _xi_get_kw(form, keyword)
    if value is None:
        return default
    return _sexpr_atom_value(value, keyword)


def _xi_get_kw_list(form: SList, keyword: str) -> Optional[SList]:
    value = _xi_get_kw(form, keyword)
    if value is None:
        return None
    if not isinstance(value, SList):
        die(f"{keyword}: expected list")
    return value


def _xi_parse_atom_list(expr: Optional[SList], context: str) -> list:
    if expr is None:
        return []
    values = []
    for child in expr.children:
        values.append(_sexpr_atom_value(child, context))
    return values


def _xi_parse_arity(expr: Optional[SExpr], default: int, context: str) -> int:
    if expr is None:
        return default
    value = _sexpr_atom_value(expr, context)
    if value == 'variadic':
        return 0xFF
    if not isinstance(expr, SAtom) or not expr.is_number:
        die(f"{context}: arity must be a number or variadic")
    arity = expr.int_value
    if arity < 0 or arity > 254:
        die(f"{context}: fixed arity must be in [0, 254]")
    return arity


def _xi_parse_value_defs(expr: Optional[SList], context: str, result: bool) -> list:
    if expr is None:
        return []
    values = []
    for entry in expr.children:
        if not isinstance(entry, SList) or len(entry.children) < 2:
            die(f"{context}: expected entries like (value $name)")
        kind = _sexpr_atom_value(entry.children[0], context)
        name = _sexpr_atom_value(entry.children[1], context)
        if not name.startswith('$'):
            die(f"{context}: value name must start with '$': {name}")
        attrs = {}
        i = 2
        while i < len(entry.children):
            key_expr = entry.children[i]
            if not isinstance(key_expr, SAtom) or not key_expr.is_keyword:
                die(f"{context}: expected attribute keyword")
            if i + 1 >= len(entry.children):
                die(f"{context}: missing value for {key_expr.value}")
            key = key_expr.value[1:]
            value = _sexpr_atom_value(entry.children[i + 1], context)
            attrs[key] = value
            i += 2
        item = XiResultDef(kind=kind, name=name, attrs=attrs)
        if not result:
            item = XiOperandDef(kind=kind, name=name, attrs=attrs)
        values.append(item)
    return values


def _xi_parse_backend_rewrite(expr: Optional[SList],
                              context: str) -> tuple[str, Optional[str]]:
    if expr is None:
        return 'none', None
    if len(expr.children) != 2:
        die(f"{context}: expected (builtin name)")
    kind = _sexpr_atom_value(expr.children[0], context)
    if kind not in VALID_XI_BACKEND_REWRITES or kind == 'none':
        die(f"{context}: unknown backend rewrite '{kind}'")
    name = _sexpr_atom_value(expr.children[1], context)
    if not name:
        die(f"{context}: backend rewrite name cannot be empty")
    return kind, name


def parse_xi_ops_def(text: str, path: str = '<input>') -> list[XiOpDef]:
    forms = parse_sexpr(tokenize_sexpr(text, path), path)
    ops = []
    seen = set()
    for form in forms:
        if not isinstance(form, SList) or not form.children:
            die(f"{path}: top-level form must be a list")
        head = _sexpr_atom_value(form.children[0], path)
        if head != 'define-xi-op':
            die(f"{path}:{form.line}:{form.col}: expected define-xi-op")
        if len(form.children) < 2:
            die(f"{path}:{form.line}:{form.col}: missing Xi op name")
        name = _sexpr_atom_value(form.children[1], 'define-xi-op')
        if name in seen:
            die(f"{path}: duplicate Xi op '{name}'")
        seen.add(name)
        ident = _xi_op_ident(name)
        cls = _xi_get_kw_str(form, ':class')
        if not cls:
            die(f"{path}: Xi op '{name}' is missing :class")
        operands = _xi_parse_value_defs(_xi_get_kw_list(form, ':operands'),
                                        f"{name}:operands", result=False)
        results = _xi_parse_value_defs(_xi_get_kw_list(form, ':results'),
                                       f"{name}:results", result=True)
        arity = _xi_parse_arity(_xi_get_kw(form, ':arity'), len(operands), f"{name}:arity")
        effects = _xi_parse_atom_list(_xi_get_kw_list(form, ':effects'), f"{name}:effects")
        for effect in effects:
            if effect not in VALID_XI_EFFECTS:
                die(f"{path}: Xi op '{name}' uses unknown effect '{effect}'")
        requires = _xi_parse_atom_list(_xi_get_kw_list(form, ':requires'), f"{name}:requires")
        observable = _xi_parse_atom_list(_xi_get_kw_list(form, ':observable'),
                                         f"{name}:observable")
        targets = _xi_parse_atom_list(_xi_get_kw_list(form, ':targets'), f"{name}:targets")
        if not targets:
            die(f"{path}: Xi op '{name}' is missing :targets")
        for target in targets:
            if target not in VALID_XI_TARGETS:
                die(f"{path}: Xi op '{name}' uses unknown target '{target}'")
        result_native_type = _xi_get_kw_str(form, ':result-native-type', 'none')
        if result_native_type != 'none' and result_native_type not in VALID_XI_RESULT_NATIVE_TYPES:
            die(f"{path}: Xi op '{name}' uses unknown result native type "
                f"'{result_native_type}'")
        result_kind = _xi_get_kw_str(form, ':result-kind', 'value')
        if result_kind not in VALID_XI_RESULT_KINDS:
            die(f"{path}: Xi op '{name}' uses unknown result kind '{result_kind}'")
        result_ownership = _xi_get_kw_str(form, ':result-ownership', 'owned')
        if result_ownership not in VALID_XI_RESULT_OWNERSHIPS:
            die(f"{path}: Xi op '{name}' uses unknown result ownership "
                f"'{result_ownership}'")
        if result_kind == 'void' and result_native_type != 'none':
            die(f"{path}: Xi op '{name}' cannot combine void result kind with "
                f"result native type '{result_native_type}'")
        lowering_policy = _xi_get_kw_str(form, ':lowering-policy', 'generated')
        if lowering_policy not in VALID_XI_LOWERING_POLICIES:
            die(f"{path}: Xi op '{name}' uses unknown lowering policy "
                f"'{lowering_policy}'")
        speculation = _xi_get_kw_str(form, ':speculation', 'never')
        if speculation not in VALID_XI_SPECULATION_POLICIES:
            die(f"{path}: Xi op '{name}' uses unknown speculation policy "
                f"'{speculation}'")
        vn_kind = _xi_get_kw_str(form, ':vn-kind', 'none')
        if vn_kind not in VALID_XI_VN_KINDS:
            die(f"{path}: Xi op '{name}' uses unknown value-numbering kind "
                f"'{vn_kind}'")
        algebraic = _xi_parse_atom_list(_xi_get_kw_list(form, ':algebraic'),
                                        f"{name}:algebraic")
        for trait in algebraic:
            if trait not in VALID_XI_ALGEBRAIC_TRAITS:
                die(f"{path}: Xi op '{name}' uses unknown algebraic trait "
                    f"'{trait}'")
        tbaa_group = _xi_get_kw_str(form, ':tbaa-group', 'none')
        if tbaa_group not in VALID_XI_TBAA_GROUPS:
            die(f"{path}: Xi op '{name}' uses unknown TBAA group "
                f"'{tbaa_group}'")
        escape_use = _xi_get_kw_str(form, ':escape-use', 'none')
        if escape_use not in VALID_XI_ESCAPE_USES:
            die(f"{path}: Xi op '{name}' uses unknown escape-use "
                f"'{escape_use}'")
        escape_alloc = _xi_get_kw_str(form, ':escape-alloc', 'none')
        if escape_alloc not in VALID_XI_ESCAPE_ALLOCS:
            die(f"{path}: Xi op '{name}' uses unknown escape-alloc "
                f"'{escape_alloc}'")
        own_use = _xi_get_kw_str(form, ':own-use', 'consume')
        if own_use not in VALID_XI_OWN_USES:
            die(f"{path}: Xi op '{name}' uses unknown own-use "
                f"'{own_use}'")
        ic_site = _xi_get_kw_str(form, ':ic-site', 'none')
        if ic_site not in VALID_XI_IC_SITES:
            die(f"{path}: Xi op '{name}' uses unknown ic-site "
                f"'{ic_site}'")
        negated_op = _xi_get_kw_str(form, ':negates-to')
        if negated_op and not negated_op.startswith('xi.'):
            die(f"{path}: Xi op '{name}' has invalid negates-to target "
                f"'{negated_op}'")
        backend_rewrite, backend_rewrite_name = _xi_parse_backend_rewrite(
            _xi_get_kw_list(form, ':backend-rewrite'),
            f"{name}:backend-rewrite")
        if backend_rewrite != 'none' and lowering_policy == 'verifier-only':
            die(f"{path}: Xi op '{name}' cannot combine backend rewrite with "
                "verifier-only lowering")
        ops.append(XiOpDef(name=name, ident=ident, cls=cls, arity=arity, operands=operands,
                           results=results, effects=effects, requires=requires,
                           observable=observable, targets=targets,
                           result_kind=result_kind,
                           result_ownership=result_ownership,
                           result_native_type=result_native_type,
                           lowering_policy=lowering_policy,
                           speculation=speculation,
                           vn_kind=vn_kind,
                           algebraic=algebraic,
                           tbaa_group=tbaa_group,
                           backend_rewrite=backend_rewrite,
                           backend_rewrite_name=backend_rewrite_name,
                           escape_use=escape_use,
                           escape_alloc=escape_alloc,
                           own_use=own_use,
                           ic_site=ic_site,
                           negated_op=negated_op if negated_op else None))
    op_by_name = {op.name: op for op in ops}
    for op in ops:
        if op.negated_op is None:
            continue
        target = op_by_name.get(op.negated_op)
        if target is None:
            die(f"{path}: Xi op '{op.name}' negates to unknown op "
                f"'{op.negated_op}'")
        if op.cls != 'comparison' or target.cls != 'comparison':
            die(f"{path}: Xi op '{op.name}' negates-to must connect "
                "comparison ops")
        if target.negated_op != op.name:
            die(f"{path}: Xi op '{op.name}' negates-to relation is not "
                f"symmetric with '{target.name}'")
    return ops


def _xi_bit_expr(prefix: str, values: list) -> str:
    if not values:
        return '0'
    return ' | '.join(f'{prefix}_{_xi_c_ident(v)}' for v in values)


def _xi_effect_flag_expr(values: list) -> str:
    flag_map = {
        'side-effect': 'XI_FLAG_SIDE_EFFECT',
        'may-throw': 'XI_FLAG_MAY_THROW',
        'may-suspend': 'XI_FLAG_MAY_SUSPEND',
        'memory-read': 'XI_FLAG_READS_MEM',
        'memory-write': 'XI_FLAG_WRITES_MEM',
    }
    flags = []
    for value in values:
        flag = flag_map.get(value)
        if flag:
            flags.append(flag)
    return ' | '.join(flags) if flags else '0'


def generate_xi_ops_header(ops: list[XiOpDef]) -> str:
    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/ops.def */')
    lines.append('')
    lines.append('#ifndef XI_OPS_GEN_H')
    lines.append('#define XI_OPS_GEN_H')
    lines.append('')
    lines.append('#include <stdbool.h>')
    lines.append('#include <stddef.h>')
    lines.append('#include <stdint.h>')
    lines.append('#include "xi.h"')
    lines.append('')
    lines.append('#define XI_OP_ARITY_VARIADIC 0xFFu')
    lines.append('')
    lines.append('/* ========== Effect Flags ========== */')
    lines.append('')
    lines.append('#define XI_EFFECT_NONE 0')
    for i, effect in enumerate(sorted(VALID_XI_EFFECTS)):
        lines.append(f'#define XI_EFFECT_{_xi_c_ident(effect)} (1u << {i})')
    lines.append('')
    lines.append('/* ========== Target Flags ========== */')
    lines.append('')
    lines.append('#define XI_TARGET_NONE 0')
    for i, target in enumerate(sorted(VALID_XI_TARGETS)):
        lines.append(f'#define XI_TARGET_{_xi_c_ident(target)} (1u << {i})')
    lines.append('')

    seen_classes = []
    for op in ops:
        if op.cls not in seen_classes:
            seen_classes.append(op.cls)
    lines.append('/* ========== Op Classes ========== */')
    lines.append('')
    lines.append('typedef enum {')
    for i, cls in enumerate(seen_classes):
        lines.append(f'    XI_GEN_CLASS_{_xi_c_ident(cls)} = {i},')
    lines.append('    XI_GEN_CLASS__COUNT')
    lines.append('} XiGeneratedOpClass;')
    lines.append('')
    lines.append('/* ========== Result Kinds ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_RESULT_VALUE = 0,')
    lines.append('    XI_GEN_RESULT_VOID = 1,')
    lines.append('    XI_GEN_RESULT_DYNAMIC = 2,')
    lines.append('    XI_GEN_RESULT__COUNT')
    lines.append('} XiGeneratedResultKind;')
    lines.append('')
    lines.append('/* ========== Result Ownership Policies ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_RESULT_OWNERSHIP_OWNED = 0,')
    lines.append('    XI_GEN_RESULT_OWNERSHIP_BORROWED = 1,')
    lines.append('    XI_GEN_RESULT_OWNERSHIP_NONE = 2,')
    lines.append('    XI_GEN_RESULT_OWNERSHIP_CALL_RESULT = 3,')
    lines.append('    XI_GEN_RESULT_OWNERSHIP__COUNT')
    lines.append('} XiGeneratedResultOwnership;')
    lines.append('')
    lines.append('/* ========== Lowering Policies ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_LOWERING_GENERATED = 0,')
    lines.append('    XI_GEN_LOWERING_PASS_LOCAL = 1,')
    lines.append('    XI_GEN_LOWERING_SPECIAL = 2,')
    lines.append('    XI_GEN_LOWERING_VERIFIER_ONLY = 3,')
    lines.append('    XI_GEN_LOWERING__COUNT')
    lines.append('} XiGeneratedLoweringPolicy;')
    lines.append('')
    lines.append('/* ========== Speculation Policies ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_SPECULATION_NEVER = 0,')
    lines.append('    XI_GEN_SPECULATION_SAFE = 1,')
    lines.append('    XI_GEN_SPECULATION__COUNT')
    lines.append('} XiGeneratedSpeculationPolicy;')
    lines.append('')
    lines.append('/* ========== Value Numbering Kinds ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_VN_NONE = 0,')
    lines.append('    XI_GEN_VN_PURE = 1,')
    lines.append('    XI_GEN_VN_MEMORY_READ = 2,')
    lines.append('    XI_GEN_VN__COUNT')
    lines.append('} XiGeneratedValueNumberingKind;')
    lines.append('')
    lines.append('/* ========== TBAA Groups ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_TBAA_NONE = 0,')
    lines.append('    XI_GEN_TBAA_TOP = 1,')
    lines.append('    XI_GEN_TBAA_CONST = 2,')
    lines.append('    XI_GEN_TBAA_FIELD = 3,')
    lines.append('    XI_GEN_TBAA_ARRAY = 4,')
    lines.append('    XI_GEN_TBAA_STRUCT = 5,')
    lines.append('    XI_GEN_TBAA_SHARED = 6,')
    lines.append('    XI_GEN_TBAA_GLOBAL = 7,')
    lines.append('    XI_GEN_TBAA_UPVAL = 8,')
    lines.append('    XI_GEN_TBAA_TLS = 9,')
    lines.append('    XI_GEN_TBAA_JSON = 10,')
    lines.append('    XI_GEN_TBAA_TUPLE = 11,')
    lines.append('    XI_GEN_TBAA_CHAN = 12,')
    lines.append('    XI_GEN_TBAA__COUNT')
    lines.append('} XiGeneratedTbaaGroup;')
    lines.append('')
    lines.append('/* ========== Backend Rewrite Kinds ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_BACKEND_REWRITE_NONE = 0,')
    lines.append('    XI_GEN_BACKEND_REWRITE_BUILTIN = 1,')
    lines.append('    XI_GEN_BACKEND_REWRITE__COUNT')
    lines.append('} XiGeneratedBackendRewrite;')
    lines.append('')
    lines.append('/* ========== Escape Use Policies ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_ESCAPE_USE_NONE = 0,')
    lines.append('    XI_GEN_ESCAPE_USE_ARG = 1,')
    lines.append('    XI_GEN_ESCAPE_USE_HEAP = 2,')
    lines.append('    XI_GEN_ESCAPE_USE_GLOBAL = 3,')
    lines.append('    XI_GEN_ESCAPE_USE__COUNT')
    lines.append('} XiGeneratedEscapeUse;')
    lines.append('')
    lines.append('/* ========== Escape Allocation Policies ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_ESCAPE_ALLOC_NONE = 0,')
    lines.append('    XI_GEN_ESCAPE_ALLOC_HEAP = 1,')
    lines.append('    XI_GEN_ESCAPE_ALLOC__COUNT')
    lines.append('} XiGeneratedEscapeAlloc;')
    lines.append('')
    lines.append('/* ========== Ownership Use Policies ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_OWN_USE_CONSUME = 0,')
    lines.append('    XI_GEN_OWN_USE_BORROW = 1,')
    lines.append('    XI_GEN_OWN_USE_STORED_VALUE = 2,')
    lines.append('    XI_GEN_OWN_USE_METHOD_ARGS = 3,')
    lines.append('    XI_GEN_OWN_USE__COUNT')
    lines.append('} XiGeneratedOwnUse;')
    lines.append('')
    lines.append('/* ========== Inline Cache Site Policies ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_IC_SITE_NONE = 0,')
    lines.append('    XI_GEN_IC_SITE_METHOD = 1,')
    lines.append('    XI_GEN_IC_SITE_FIELD = 2,')
    lines.append('    XI_GEN_IC_SITE__COUNT')
    lines.append('} XiGeneratedIcSite;')
    lines.append('')
    lines.append('/* ========== Algebraic Trait Flags ========== */')
    lines.append('')
    lines.append('#define XI_GEN_ALGEBRAIC_NONE 0')
    for i, trait in enumerate(sorted(VALID_XI_ALGEBRAIC_TRAITS)):
        lines.append(f'#define XI_GEN_ALGEBRAIC_{_xi_c_ident(trait)} (1u << {i})')
    lines.append('')
    lines.append(f'enum {{ XI_GEN_OP_COUNT = {len(ops)} }};')
    lines.append('typedef char xi_generated_op_count_must_match_XiOp[')
    lines.append('    ((int) XI_OP_COUNT == (int) XI_GEN_OP_COUNT) ? 1 : -1];')
    lines.append('')

    lines.append('/* ========== Op Metadata Shape ========== */')
    lines.append('')
    lines.append('typedef struct {')
    lines.append('    const char *name;')
    lines.append('    const char *ident;')
    lines.append('    uint8_t op_class;')
    lines.append('    uint8_t arity;')
    lines.append('    uint8_t operand_count;')
    lines.append('    uint8_t result_count;')
    lines.append('    uint8_t result_kind;')
    lines.append('    uint8_t result_ownership;')
    lines.append('    uint8_t lowering_policy;')
    lines.append('    uint8_t speculation;')
    lines.append('    uint8_t value_numbering;')
    lines.append('    uint8_t tbaa_group;')
    lines.append('    uint8_t backend_rewrite;')
    lines.append('    uint8_t escape_use;')
    lines.append('    uint8_t escape_alloc;')
    lines.append('    uint8_t own_use;')
    lines.append('    uint8_t ic_site;')
    lines.append('    uint16_t negated_op;')
    lines.append('    uint8_t default_flags;')
    lines.append('    uint32_t algebraic_traits;')
    lines.append('    uint32_t effects;')
    lines.append('    uint32_t targets;')
    lines.append('    const char *backend_rewrite_name;')
    lines.append('    const char *result_native_type;')
    lines.append('} XiGeneratedOpInfo;')
    lines.append('')

    lines.append('#define XI_GENERATED_OPS(X) \\')
    for i, op in enumerate(ops):
        arity = 'XI_OP_ARITY_VARIADIC' if op.arity == 0xFF else str(op.arity)
        effects = _xi_bit_expr('XI_EFFECT', op.effects)
        targets = _xi_bit_expr('XI_TARGET', op.targets)
        flags = _xi_effect_flag_expr(op.effects)
        native_type = 'NULL' if op.result_native_type == 'none' else f'"{op.result_native_type}"'
        result_kind = f'XI_GEN_RESULT_{_xi_c_ident(op.result_kind)}'
        result_ownership = f'XI_GEN_RESULT_OWNERSHIP_{_xi_c_ident(op.result_ownership)}'
        lowering_policy = f'XI_GEN_LOWERING_{_xi_c_ident(op.lowering_policy)}'
        speculation = f'XI_GEN_SPECULATION_{_xi_c_ident(op.speculation)}'
        vn_kind = f'XI_GEN_VN_{_xi_c_ident(op.vn_kind)}'
        tbaa_group = f'XI_GEN_TBAA_{_xi_c_ident(op.tbaa_group)}'
        backend_rewrite = f'XI_GEN_BACKEND_REWRITE_{_xi_c_ident(op.backend_rewrite)}'
        escape_use = f'XI_GEN_ESCAPE_USE_{_xi_c_ident(op.escape_use)}'
        escape_alloc = f'XI_GEN_ESCAPE_ALLOC_{_xi_c_ident(op.escape_alloc)}'
        own_use = f'XI_GEN_OWN_USE_{_xi_c_ident(op.own_use)}'
        ic_site = f'XI_GEN_IC_SITE_{_xi_c_ident(op.ic_site)}'
        negated_op = 'XI_OP_COUNT'
        if op.negated_op is not None:
            negated_op = f'XI_{_xi_op_ident(op.negated_op)}'
        backend_rewrite_name = (
            'NULL' if op.backend_rewrite_name is None else f'"{op.backend_rewrite_name}"')
        algebraic_traits = _xi_bit_expr('XI_GEN_ALGEBRAIC', op.algebraic)
        suffix = ' \\' if i + 1 < len(ops) else ''
        lines.append(
            f'    X({op.ident}, "{op.name}", XI_GEN_CLASS_{_xi_c_ident(op.cls)}, {arity}, '
            f'{len(op.operands)}, {len(op.results)}, {result_kind}, {result_ownership}, '
            f'{lowering_policy}, {speculation}, {vn_kind}, {tbaa_group}, {backend_rewrite}, {escape_use}, '
            f'{escape_alloc}, {own_use}, {ic_site}, {negated_op}, {flags}, {algebraic_traits}, {effects}, '
            f'{targets}, {backend_rewrite_name}, {native_type}){suffix}')
    lines.append('')
    lines.append('')

    lines.append('static inline const char *xi_generated_op_name(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return "{op.ident}";')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return "???";')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_arity(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        arity = 'XI_OP_ARITY_VARIADIC' if op.arity == 0xFF else str(op.arity)
        lines.append(f'        case XI_{op.ident}: return {arity};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_OP_ARITY_VARIADIC;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_class(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return XI_GEN_CLASS_{_xi_c_ident(op.cls)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_CLASS__COUNT;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_result_kind(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return XI_GEN_RESULT_{_xi_c_ident(op.result_kind)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_RESULT_VALUE;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_result_ownership(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(
            f'        case XI_{op.ident}: return XI_GEN_RESULT_OWNERSHIP_{_xi_c_ident(op.result_ownership)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_RESULT_OWNERSHIP_OWNED;')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_generated_op_result_native_type(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        if op.result_native_type == 'none':
            lines.append(f'        case XI_{op.ident}: return NULL;')
        else:
            lines.append(f'        case XI_{op.ident}: return "{op.result_native_type}";')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return NULL;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_lowering_policy(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return '
                     f'XI_GEN_LOWERING_{_xi_c_ident(op.lowering_policy)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_LOWERING__COUNT;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_speculation(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return '
                     f'XI_GEN_SPECULATION_{_xi_c_ident(op.speculation)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_SPECULATION_NEVER;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_value_numbering(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return XI_GEN_VN_{_xi_c_ident(op.vn_kind)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_VN_NONE;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_tbaa_group(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return XI_GEN_TBAA_{_xi_c_ident(op.tbaa_group)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_TBAA_NONE;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_backend_rewrite(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return '
                     f'XI_GEN_BACKEND_REWRITE_{_xi_c_ident(op.backend_rewrite)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_BACKEND_REWRITE__COUNT;')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_generated_op_backend_rewrite_name(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        if op.backend_rewrite_name is None:
            lines.append(f'        case XI_{op.ident}: return NULL;')
        else:
            lines.append(f'        case XI_{op.ident}: return "{op.backend_rewrite_name}";')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return NULL;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_generated_op_backend_legal(uint16_t op) {')
    lines.append('    return xi_generated_op_backend_rewrite(op) == XI_GEN_BACKEND_REWRITE_NONE &&')
    lines.append('           xi_generated_op_lowering_policy(op) != XI_GEN_LOWERING_VERIFIER_ONLY;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_escape_use(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return XI_GEN_ESCAPE_USE_{_xi_c_ident(op.escape_use)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_ESCAPE_USE_HEAP;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_escape_alloc(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(
            f'        case XI_{op.ident}: return XI_GEN_ESCAPE_ALLOC_{_xi_c_ident(op.escape_alloc)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_ESCAPE_ALLOC_NONE;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_own_use(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return XI_GEN_OWN_USE_{_xi_c_ident(op.own_use)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_OWN_USE_CONSUME;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_ic_site(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return XI_GEN_IC_SITE_{_xi_c_ident(op.ic_site)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_IC_SITE_NONE;')
    lines.append('}')
    lines.append('')
    lines.append('static inline XiOp xi_generated_op_negates_to(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        negated_op = 'XI_OP_COUNT'
        if op.negated_op is not None:
            negated_op = f'XI_{_xi_op_ident(op.negated_op)}'
        lines.append(f'        case XI_{op.ident}: return {negated_op};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_OP_COUNT;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint32_t xi_generated_op_algebraic_traits(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        algebraic_traits = _xi_bit_expr('XI_GEN_ALGEBRAIC', op.algebraic)
        lines.append(f'        case XI_{op.ident}: return {algebraic_traits};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint8_t xi_generated_op_default_flags(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        flags = _xi_effect_flag_expr(op.effects)
        lines.append(f'        case XI_{op.ident}: return {flags};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint32_t xi_generated_op_effects(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        effects = _xi_bit_expr('XI_EFFECT', op.effects)
        lines.append(f'        case XI_{op.ident}: return {effects};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    lines.append('#endif  /* XI_OPS_GEN_H */')
    lines.append('')
    return '\n'.join(lines)


# ============================================================
# L3: Xi lowering coverage metadata
# ============================================================

VALID_XI_LOWERING_TARGETS = {
    'aot-c',
    'aot-c-stmt',
    'aot-verify',
    'vm-bytecode',
}

VALID_XI_LOWERING_TARGET_ATTRS = {
    'fresh-dst',
    'handles-cell-dst',
    'raw-cell-args',
}

VALID_XI_LOWERING_TEMPLATES = {
    'value-binary',
    'value-unary',
    'compare',
    'narrow',
    'widen',
}

XI_VM_TEMPLATE_OPCODES = {
    'xi.add': 'OP_ADD',
    'xi.sub': 'OP_SUB',
    'xi.mul': 'OP_MUL',
    'xi.div': 'OP_DIV',
    'xi.mod': 'OP_MOD',
    'xi.neg': 'OP_UNM',
    'xi.band': 'OP_BAND',
    'xi.bor': 'OP_BOR',
    'xi.bxor': 'OP_BXOR',
    'xi.bnot': 'OP_BNOT',
    'xi.not': 'OP_NOT',
    'xi.shl': 'OP_SHL',
    'xi.shr': 'OP_SHR',
    'xi.eq': 'OP_CMP_EQ',
    'xi.ne': 'OP_CMP_NE',
    'xi.lt': 'OP_CMP_LT',
    'xi.le': 'OP_CMP_LE',
    'xi.gt': 'OP_CMP_LT',
    'xi.ge': 'OP_CMP_LE',
    'xi.narrow.i8': 'OP_NARROW_I8',
    'xi.narrow.u8': 'OP_NARROW_U8',
    'xi.narrow.i16': 'OP_NARROW_I16',
    'xi.narrow.u16': 'OP_NARROW_U16',
    'xi.narrow.i32': 'OP_NARROW_I32',
    'xi.narrow.u32': 'OP_NARROW_U32',
    'xi.narrow.f32': 'OP_NARROW_F32',
    'xi.widen.i8': 'OP_WIDEN_I8',
    'xi.widen.u8': 'OP_WIDEN_U8',
    'xi.widen.i16': 'OP_WIDEN_I16',
    'xi.widen.u16': 'OP_WIDEN_U16',
    'xi.widen.i32': 'OP_WIDEN_I32',
    'xi.widen.u32': 'OP_WIDEN_U32',
    'xi.widen.f32': 'OP_WIDEN_F32',
}

XI_VM_TEMPLATE_SWAP_ARGS = {
    'xi.gt',
    'xi.ge',
}

XI_VM_TEMPLATE_WIDTH = {
    'xi.narrow.i8': 'int8_t',
    'xi.narrow.u8': 'uint8_t',
    'xi.narrow.i16': 'int16_t',
    'xi.narrow.u16': 'uint16_t',
    'xi.narrow.i32': 'int32_t',
    'xi.narrow.u32': 'uint32_t',
    'xi.narrow.f32': '',
    'xi.widen.i8': 'int8_t',
    'xi.widen.u8': 'uint8_t',
    'xi.widen.i16': 'int16_t',
    'xi.widen.u16': 'uint16_t',
    'xi.widen.i32': 'int32_t',
    'xi.widen.u32': 'uint32_t',
    'xi.widen.f32': '',
}

XI_VM_TEMPLATE_BITWISE_BINARY = {
    'xi.band': ('&', '&&', 'xr_bigint_and', 'XR_OP_BAND_FLAG', 'SYMBOL_OP_BAND',
                '"&"', '"bitwise AND requires integer types"'),
    'xi.bor': ('|', '||', 'xr_bigint_or', 'XR_OP_BOR_FLAG', 'SYMBOL_OP_BOR',
               '"|"', '"bitwise OR requires integer types"'),
    'xi.bxor': ('^', None, 'xr_bigint_xor', 'XR_OP_BXOR_FLAG', 'SYMBOL_OP_BXOR',
                '"^"', '"bitwise XOR requires integer types"'),
}

XI_VM_TEMPLATE_BITWISE_UNARY = {
    'xi.bnot': ('~', 'SYMBOL_OP_BNOT', '"~"', '"bitwise NOT requires integer type"'),
}

XI_VM_TEMPLATE_UNARY = {
    'xi.neg': ('XVM_TEMPLATE_UNARY_NEG_CASE', '"-"'),
    'xi.not': ('XVM_TEMPLATE_UNARY_NOT_CASE', '"!"'),
}

XI_VM_TEMPLATE_ARITH_BINARY = {
    'xi.add': ('XVM_TEMPLATE_ARITH_ADD_CASE', '+', '+', 'xr_bigint_add',
               'XR_OP_ADD_FLAG', 'SYMBOL_OP_ADD', '"+"',
               '"operator \'+\' requires both operands to be numeric or both string, got \'%s\' and \'%s\'"'),
    'xi.sub': ('XVM_TEMPLATE_ARITH_NUMERIC_CASE', '-', '-', 'xr_bigint_sub',
               'XR_OP_SUB_FLAG', 'SYMBOL_OP_SUB', '"-"',
               '"subtraction requires numeric types"'),
    'xi.mul': ('XVM_TEMPLATE_ARITH_MUL_CASE', '*', '*', 'xr_bigint_mul',
               'XR_OP_MUL_FLAG', 'SYMBOL_OP_MUL', '"*"',
               '"multiplication requires numeric types"'),
    'xi.div': ('XVM_TEMPLATE_ARITH_DIV_CASE', 'xr_bigint_div',
               'XR_OP_DIV_FLAG', 'SYMBOL_OP_DIV', '"/"',
               '"division requires numeric types"'),
    'xi.mod': ('XVM_TEMPLATE_ARITH_MOD_CASE', 'xr_bigint_mod',
               'XR_OP_MOD_FLAG', 'SYMBOL_OP_MOD', '"%%"',
               'XR_ERROR_CORE_MODULO_REQUIRES_INTEGER_MSG'),
}

XI_VM_TEMPLATE_SHIFT = {
    'xi.shl': ('xr_int_shl_wrap', 'xr_bigint_shl'),
    'xi.shr': ('xr_int_shr_wrap', 'xr_bigint_shr'),
}

XI_VM_TEMPLATE_COMPARE_DEEP = {
    'xi.eq': ('false', 'XR_OP_EQ_FLAG', 'SYMBOL_OP_EQ', '"=="', 'vm_values_equal_deep'),
    'xi.ne': ('true', 'XR_OP_NE_FLAG', 'SYMBOL_OP_NE', '"!="', 'vm_values_equal_deep'),
}

XI_VM_TEMPLATE_COMPARE_ORDER = {
    'xi.lt': ('XR_OP_LT_FLAG', 'SYMBOL_OP_LT', '"<"', 'vm_numeric_less'),
    'xi.le': ('XR_OP_LE_FLAG', 'SYMBOL_OP_LE', '"<="', 'vm_numeric_less_equal'),
}

XI_VM_TEMPLATE_COMPARE_OPS = (
    set(XI_VM_TEMPLATE_COMPARE_DEEP) |
    set(XI_VM_TEMPLATE_COMPARE_ORDER) |
    {'xi.gt', 'xi.ge'}
)

XI_AOT_C_TEMPLATE_WIDTH = {
    'xi.narrow.i8': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'int8_t', False),
    'xi.narrow.u8': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'uint8_t', False),
    'xi.narrow.i16': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'int16_t', False),
    'xi.narrow.u16': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'uint16_t', False),
    'xi.narrow.i32': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'int32_t', False),
    'xi.narrow.u32': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'uint32_t', False),
    'xi.narrow.f32': ('AOT_WIDTH_TEMPLATE_F32_ROUNDTRIP', '', False),
    'xi.widen.i8': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'int8_t', False),
    'xi.widen.u8': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'uint8_t', False),
    'xi.widen.i16': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'int16_t', False),
    'xi.widen.u16': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'uint16_t', False),
    'xi.widen.i32': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'int32_t', False),
    'xi.widen.u32': ('AOT_WIDTH_TEMPLATE_CAST_I64', 'uint32_t', False),
    'xi.widen.f32': ('AOT_WIDTH_TEMPLATE_F32_ROUNDTRIP', '', True),
}

XI_AOT_C_TEMPLATE_ARITH = {
    'xi.add': ('xrt_add', '+', 'xrt_i64_add'),
    'xi.sub': ('xrt_sub', '-', 'xrt_i64_sub'),
    'xi.mul': ('xrt_mul', '*', 'xrt_i64_mul'),
}

XI_AOT_C_TEMPLATE_DIV_MOD = {
    'xi.div': ('xrt_div', 'xrt_int_div'),
    'xi.mod': ('xrt_mod', 'xrt_int_mod'),
}

XI_AOT_C_TEMPLATE_BITWISE_BINARY = {
    'xi.band': '&',
    'xi.bor': '|',
    'xi.bxor': '^',
}

XI_AOT_C_TEMPLATE_BITWISE_UNARY = {
    'xi.bnot': '~',
}

XI_AOT_C_TEMPLATE_SHIFT = {
    'xi.shl': 'xrt_i64_shl',
    'xi.shr': 'xrt_i64_shr',
}

XI_AOT_C_TEMPLATE_COMPARE = {
    'xi.eq': ('xrt_eq', '==', False),
    'xi.ne': ('!xrt_eq', '!=', False),
    'xi.lt': ('xrt_lt', '<', False),
    'xi.le': ('xrt_le', '<=', False),
    'xi.gt': ('xrt_lt', '>', True),
    'xi.ge': ('xrt_le', '>=', True),
}

@dataclass
class XiLoweringDef:
    op_name: str
    ident: str
    required_targets: list
    targets: list
    target_drivers: dict
    target_rejects: dict
    target_attrs: dict
    match: Optional[SList] = None
    template: str = 'custom'


def _xi_lowering_parse_bool_attr(value: SExpr, context: str) -> bool:
    text = _sexpr_atom_value(value, context)
    if text == 'yes':
        return True
    if text == 'no':
        return False
    die(f"{context}: expected yes or no")


def _xi_lowering_target_entry(form: SList, target: str,
                              op_name: str) -> tuple[Optional[str], bool, dict]:
    value = _xi_get_kw(form, ':' + target)
    if value is None:
        return None, False, {}
    if not isinstance(value, SList) or len(value.children) < 2:
        die(f"{op_name}:{target}: expected (driver name) or (reject name)")
    kind = _sexpr_atom_value(value.children[0], f"{op_name}:{target}")
    if kind not in {'driver', 'reject'}:
        die(f"{op_name}:{target}: expected driver or reject entry")
    attrs = {}
    if len(value.children) > 2:
        extras = value.children[2:]
        if len(extras) % 2 != 0:
            die(f"{op_name}:{target}: target attributes must be keyword/value pairs")
        for i in range(0, len(extras), 2):
            key_expr = extras[i]
            if not isinstance(key_expr, SAtom) or not key_expr.value.startswith(':'):
                die(f"{op_name}:{target}: target attribute key must start with ':'")
            key = key_expr.value[1:]
            if key not in VALID_XI_LOWERING_TARGET_ATTRS:
                die(f"{op_name}:{target}: unknown target attribute '{key}'")
            if target != 'vm-bytecode':
                die(f"{op_name}:{target}: {key} is only valid for vm-bytecode")
            attrs[key] = _xi_lowering_parse_bool_attr(extras[i + 1],
                                                      f"{op_name}:{target}:{key}")
    if any(attrs.values()) and kind == 'reject':
        die(f"{op_name}:{target}: rejected target cannot require VM target attributes")
    return _sexpr_atom_value(value.children[1], f"{op_name}:{target}"), kind == 'reject', attrs


def _xi_lowering_target_entries(form: SList, op_name: str) -> tuple[dict, dict, dict]:
    drivers = {}
    rejects = {}
    attrs = {}
    for target in sorted(VALID_XI_LOWERING_TARGETS):
        driver, reject, target_attrs = _xi_lowering_target_entry(form, target, op_name)
        if driver is not None:
            drivers[target] = driver
            if target_attrs:
                attrs[target] = target_attrs
            if reject:
                rejects[target] = driver
    return drivers, rejects, attrs


def _xi_lowering_template_from_match(match: Optional[SList], op_name: str) -> str:
    if match is None or not match.children:
        return 'custom'
    template = _sexpr_atom_value(match.children[0], f"{op_name}:match")
    if template not in VALID_XI_LOWERING_TEMPLATES:
        die(f"{op_name}:match: unknown lowering template '{template}'")
    return template


def _xi_validate_lowering_policy(entries: list[XiLoweringDef], ops: list[XiOpDef],
                                 path: str) -> None:
    entry_names = {entry.op_name for entry in entries}
    missing = [op.name for op in ops
               if op.lowering_policy == 'generated' and op.name not in entry_names]
    if missing:
        die(f"{path}: missing lowering entry for generated Xi op(s): {', '.join(missing)}")
    op_by_name = {op.name: op for op in ops}
    for entry in entries:
        op = op_by_name[entry.op_name]
        if op.lowering_policy != 'generated':
            die(f"{path}: Xi op '{entry.op_name}' has lowering-policy "
                f"'{op.lowering_policy}' but also has a lowering entry")
    for entry in entries:
        op = op_by_name[entry.op_name]
        op_targets = {target for target in op.targets if target != 'aot-verify'}
        lowering_targets = {
            'aot-c' if target == 'aot-c-stmt' else target
            for target in entry.targets
            if target != 'aot-verify'
        }
        if op_targets != lowering_targets:
            expected = ', '.join(sorted(op_targets)) if op_targets else 'none'
            actual = ', '.join(sorted(lowering_targets)) if lowering_targets else 'none'
            die(f"{path}: lowering '{entry.op_name}' target mismatch with ops.def: "
                f"expected {expected}, got {actual}")


def parse_xi_lowering_def(text: str, ops: list[XiOpDef], path: str = '<input>') -> list[XiLoweringDef]:
    known_ops = {op.name for op in ops}
    forms = parse_sexpr(tokenize_sexpr(text, path), path)
    entries = []
    seen = set()
    for form in forms:
        if not isinstance(form, SList) or not form.children:
            die(f"{path}: top-level form must be a list")
        head = _sexpr_atom_value(form.children[0], path)
        if head != 'lower':
            die(f"{path}:{form.line}:{form.col}: expected lower")
        if len(form.children) < 2:
            die(f"{path}:{form.line}:{form.col}: missing lowered Xi op name")
        op_name = _sexpr_atom_value(form.children[1], 'lower')
        if op_name not in known_ops:
            die(f"{path}: lowering references unknown Xi op '{op_name}'")
        ident = _xi_op_ident(op_name)
        if op_name in seen:
            die(f"{path}: duplicate lowering entry for '{op_name}'")
        seen.add(op_name)
        required = _xi_parse_atom_list(_xi_get_kw_list(form, ':required-targets'),
                                      f"{op_name}:required-targets")
        for target in required:
            if target not in VALID_XI_LOWERING_TARGETS:
                die(f"{path}: lowering '{op_name}' uses unknown required target '{target}'")
        target_drivers, target_rejects, target_attrs = _xi_lowering_target_entries(form, op_name)
        targets = sorted(target_drivers.keys())
        if not targets:
            die(f"{path}: lowering '{op_name}' has no target emit")
        missing = [target for target in required if target not in targets]
        if missing:
            die(f"{path}: lowering '{op_name}' missing required target(s): {', '.join(missing)}")
        match = _xi_get_kw_list(form, ':match')
        template = _xi_lowering_template_from_match(match, op_name)
        entries.append(XiLoweringDef(op_name=op_name, ident=ident, required_targets=required,
                                     targets=targets, target_drivers=target_drivers,
                                     target_rejects=target_rejects,
                                     target_attrs=target_attrs,
                                     match=match, template=template))
    _xi_validate_lowering_policy(entries, ops, path)
    return entries


def generate_xi_lowering_coverage_header(entries: list[XiLoweringDef]) -> str:
    main_targets = {'vm-bytecode', 'aot-c'}
    patterned_entries = [entry for entry in entries if entry.template != 'custom']
    main_backend_entries = [entry for entry in entries if main_targets <= set(entry.targets)]
    main_backend_patterned_entries = [
        entry for entry in patterned_entries
        if main_targets <= set(entry.targets)
    ]
    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('')
    lines.append('#ifndef XI_LOWERING_COVERAGE_GEN_H')
    lines.append('#define XI_LOWERING_COVERAGE_GEN_H')
    lines.append('')
    lines.append('#include <stdbool.h>')
    lines.append('#include <stdint.h>')
    lines.append('#include "xi.h"')
    lines.append('')
    lines.append('#define XI_LOWER_TARGET_NONE 0')
    for i, target in enumerate(sorted(VALID_XI_LOWERING_TARGETS)):
        lines.append(f'#define XI_LOWER_TARGET_{_xi_c_ident(target)} (1u << {i})')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_LOWER_TEMPLATE_CUSTOM = 0,')
    for i, template in enumerate(sorted(VALID_XI_LOWERING_TEMPLATES), start=1):
        lines.append(f'    XI_LOWER_TEMPLATE_{_xi_c_ident(template)} = {i},')
    lines.append('} XiLowerTemplateKind;')
    lines.append('')
    lines.append(f'enum {{ XI_LOWERING_ENTRY_COUNT = {len(entries)} }};')
    lines.append(f'enum {{ XI_LOWERING_PATTERNED_ENTRY_COUNT = {len(patterned_entries)} }};')
    lines.append(f'enum {{ XI_LOWERING_CUSTOM_ENTRY_COUNT = {len(entries) - len(patterned_entries)} }};')
    lines.append(f'enum {{ XI_LOWERING_MAIN_BACKEND_ENTRY_COUNT = {len(main_backend_entries)} }};')
    lines.append(
        f'enum {{ XI_LOWERING_MAIN_BACKEND_PATTERNED_ENTRY_COUNT = {len(main_backend_patterned_entries)} }};')
    lines.append(f'enum {{ XI_LOWERING_REJECTED_TARGET_COUNT = {sum(len(entry.target_rejects) for entry in entries)} }};')
    lines.append(f'enum {{ XI_LOWERING_REJECTED_ENTRY_COUNT = {sum(1 for entry in entries if entry.target_rejects)} }};')
    for target in sorted(VALID_XI_LOWERING_TARGETS):
        ident = _xi_c_ident(target)
        lines.append(
            f'enum {{ XI_LOWERING_{ident}_ENTRY_COUNT = {sum(1 for entry in entries if target in entry.targets)} }};')
        lines.append(
            f'enum {{ XI_LOWERING_{ident}_PATTERNED_ENTRY_COUNT = {sum(1 for entry in patterned_entries if target in entry.targets)} }};')
    lines.append('')
    lines.append('#define XI_LOWERING_COVERAGE_ENTRIES(X) \\')
    for i, entry in enumerate(entries):
        target_bits = _xi_bit_expr('XI_LOWER_TARGET', entry.targets)
        required_bits = _xi_bit_expr('XI_LOWER_TARGET', entry.required_targets)
        reject_bits = _xi_bit_expr('XI_LOWER_TARGET', sorted(entry.target_rejects.keys()))
        suffix = ' \\' if i + 1 < len(entries) else ''
        lines.append(f'    X({entry.ident}, "{entry.op_name}", {target_bits}, {required_bits}, {reject_bits}){suffix}')
    lines.append('')
    lines.append('')
    lines.append('static inline uint32_t xi_lowering_generated_targets(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in entries:
        target_bits = _xi_bit_expr('XI_LOWER_TARGET', entry.targets)
        lines.append(f'        case XI_{entry.ident}: return {target_bits};')
    lines.append('        case XI_OP_COUNT: return 0;')
    lines.append('        default: return 0;')
    lines.append('    }')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint32_t xi_lowering_required_targets(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in entries:
        required_bits = _xi_bit_expr('XI_LOWER_TARGET', entry.required_targets)
        lines.append(f'        case XI_{entry.ident}: return {required_bits};')
    lines.append('        case XI_OP_COUNT: return 0;')
    lines.append('        default: return 0;')
    lines.append('    }')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint32_t xi_lowering_rejected_targets(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in entries:
        reject_bits = _xi_bit_expr('XI_LOWER_TARGET', sorted(entry.target_rejects.keys()))
        lines.append(f'        case XI_{entry.ident}: return {reject_bits};')
    lines.append('        case XI_OP_COUNT: return 0;')
    lines.append('        default: return 0;')
    lines.append('    }')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    lines.append('static inline XiLowerTemplateKind xi_lowering_template_kind(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in entries:
        template = f'XI_LOWER_TEMPLATE_{_xi_c_ident(entry.template)}'
        lines.append(f'        case XI_{entry.ident}: return {template};')
    lines.append('        case XI_OP_COUNT: return XI_LOWER_TEMPLATE_CUSTOM;')
    lines.append('        default: return XI_LOWER_TEMPLATE_CUSTOM;')
    lines.append('    }')
    lines.append('    return XI_LOWER_TEMPLATE_CUSTOM;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_lowering_is_patterned(uint16_t op) {')
    lines.append('    return xi_lowering_template_kind(op) != XI_LOWER_TEMPLATE_CUSTOM;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_lowering_has_target(uint16_t op, uint32_t target) {')
    lines.append('    return (xi_lowering_generated_targets(op) & target) == target;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_lowering_target_is_rejected(uint16_t op, uint32_t target) {')
    lines.append('    return (xi_lowering_rejected_targets(op) & target) == target;')
    lines.append('}')
    lines.append('')
    lines.append('#endif  /* XI_LOWERING_COVERAGE_GEN_H */')
    lines.append('')
    return '\n'.join(lines)


def generate_xi_vm_dispatch_header(entries: list[XiLoweringDef]) -> str:
    vm_entries = [entry for entry in entries if 'vm-bytecode' in entry.target_drivers]
    template_entries = [
        entry for entry in vm_entries
        if entry.template != 'custom'
    ]
    missing_template_opcodes = [
        entry.op_name for entry in template_entries
        if entry.op_name not in XI_VM_TEMPLATE_OPCODES
    ]
    if missing_template_opcodes:
        die("xi-lowering: missing VM template opcode(s) for " +
            ", ".join(missing_template_opcodes))
    fresh_dst_entries = [
        entry for entry in vm_entries
        if entry.target_attrs.get('vm-bytecode', {}).get('fresh-dst', False)
    ]
    raw_cell_arg_entries = [
        entry for entry in vm_entries
        if entry.target_attrs.get('vm-bytecode', {}).get('raw-cell-args', False)
    ]
    cell_dst_entries = [
        entry for entry in vm_entries
        if entry.target_attrs.get('vm-bytecode', {}).get('handles-cell-dst', False)
    ]
    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('')
    lines.append('#ifndef XI_EMIT_VM_GEN_H')
    lines.append('#define XI_EMIT_VM_GEN_H')
    lines.append('')
    lines.append('#include <stdbool.h>')
    lines.append('#include <stdint.h>')
    lines.append('#include "xi.h"')
    lines.append('#include "../runtime/value/xchunk.h"')
    lines.append('')
    lines.append('#define XI_EMIT_VM_LOWERING_HANDLERS(X) \\')
    for i, entry in enumerate(vm_entries):
        driver = entry.target_drivers['vm-bytecode']
        suffix = ' \\' if i + 1 < len(vm_entries) else ''
        lines.append(f'    X({entry.ident}, {driver}){suffix}')
    lines.append('')
    lines.append('')
    lines.append('static inline OpCode xi_emit_vm_template_opcode(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in template_entries:
        lines.append(f'        case XI_{entry.ident}: return {XI_VM_TEMPLATE_OPCODES[entry.op_name]};')
    lines.append('        case XI_OP_COUNT: return OP_NOP;')
    lines.append('        default: return OP_NOP;')
    lines.append('    }')
    lines.append('    return OP_NOP;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_emit_vm_template_swaps_args(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in template_entries:
        if entry.op_name in XI_VM_TEMPLATE_SWAP_ARGS:
            lines.append(f'        case XI_{entry.ident}: return true;')
    lines.append('        case XI_OP_COUNT: return false;')
    lines.append('        default: return false;')
    lines.append('    }')
    lines.append('    return false;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_emit_vm_requires_fresh_dst(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in fresh_dst_entries:
        lines.append(f'        case XI_{entry.ident}: return true;')
    lines.append('        case XI_OP_COUNT: return false;')
    lines.append('        default: return false;')
    lines.append('    }')
    lines.append('    return false;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_emit_vm_uses_raw_cell_args(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in raw_cell_arg_entries:
        lines.append(f'        case XI_{entry.ident}: return true;')
    lines.append('        case XI_OP_COUNT: return false;')
    lines.append('        default: return false;')
    lines.append('    }')
    lines.append('    return false;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_emit_vm_handles_cell_dst(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in cell_dst_entries:
        lines.append(f'        case XI_{entry.ident}: return true;')
    lines.append('        case XI_OP_COUNT: return false;')
    lines.append('        default: return false;')
    lines.append('    }')
    lines.append('    return false;')
    lines.append('}')
    lines.append('')
    lines.append('#endif  /* XI_EMIT_VM_GEN_H */')
    lines.append('')
    return '\n'.join(lines)


def generate_xi_vm_template_width_dispatch(entries: list[XiLoweringDef]) -> str:
    width_entries = [
        entry for entry in entries
        if 'vm-bytecode' in entry.target_drivers and entry.template in {'narrow', 'widen'}
    ]
    missing_width_ops = [
        entry.op_name for entry in width_entries
        if entry.op_name not in XI_VM_TEMPLATE_WIDTH or entry.op_name not in XI_VM_TEMPLATE_OPCODES
    ]
    if missing_width_ops:
        die("xi-lowering: missing VM width template op(s) for " + ", ".join(missing_width_ops))

    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('/* Included inside xvm.c dispatch switch; relies on i, R, vmcase, vmbreak. */')
    lines.append('')
    lines.append('#define XVM_TEMPLATE_WIDTH_INT_CASE(op, ctype) \\')
    lines.append('    vmcase(op) { \\')
    lines.append('        int a = GETARG_A(i), b = GETARG_B(i); \\')
    lines.append('        R(a) = XR_FROM_INT((int64_t) (ctype) XR_TO_INT(R(b))); \\')
    lines.append('        vmbreak; \\')
    lines.append('    }')
    lines.append('')
    lines.append('#define XVM_TEMPLATE_WIDTH_F32_CASE(op) \\')
    lines.append('    vmcase(op) { \\')
    lines.append('        int a = GETARG_A(i), b = GETARG_B(i); \\')
    lines.append('        R(a) = XR_FROM_FLOAT((double) (float) XR_TO_FLOAT(R(b))); \\')
    lines.append('        vmbreak; \\')
    lines.append('    }')
    lines.append('')
    for entry in width_entries:
        opcode = XI_VM_TEMPLATE_OPCODES[entry.op_name]
        ctype = XI_VM_TEMPLATE_WIDTH[entry.op_name]
        if ctype:
            lines.append(f'XVM_TEMPLATE_WIDTH_INT_CASE({opcode}, {ctype})')
        else:
            lines.append(f'XVM_TEMPLATE_WIDTH_F32_CASE({opcode})')
    lines.append('')
    lines.append('#undef XVM_TEMPLATE_WIDTH_INT_CASE')
    lines.append('#undef XVM_TEMPLATE_WIDTH_F32_CASE')
    lines.append('')
    return '\n'.join(lines)


def generate_xi_vm_template_bitwise_binary_dispatch(entries: list[XiLoweringDef]) -> str:
    bitwise_entries = [
        entry for entry in entries
        if 'vm-bytecode' in entry.target_drivers and entry.op_name in XI_VM_TEMPLATE_BITWISE_BINARY
    ]
    missing_bitwise_ops = [
        entry.op_name for entry in bitwise_entries
        if entry.op_name not in XI_VM_TEMPLATE_OPCODES
    ]
    if missing_bitwise_ops:
        die("xi-lowering: missing VM bitwise binary template opcode(s) for " +
            ", ".join(missing_bitwise_ops))

    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('/* Included inside xvm_dispatch_bitwise.inc.c with template macros defined. */')
    lines.append('')
    lines.append('#ifndef XVM_TEMPLATE_BITWISE_BINARY_CASE')
    lines.append('#error "XVM_TEMPLATE_BITWISE_BINARY_CASE must be defined before including this file"')
    lines.append('#endif')
    lines.append('#ifndef XVM_TEMPLATE_BITWISE_BINARY_BOOL_CASE')
    lines.append('#error "XVM_TEMPLATE_BITWISE_BINARY_BOOL_CASE must be defined before including this file"')
    lines.append('#endif')
    lines.append('')
    for entry in bitwise_entries:
        opcode = XI_VM_TEMPLATE_OPCODES[entry.op_name]
        int_op, bool_op, bigint_fn, op_flag, op_symbol, op_name, error_msg = (
            XI_VM_TEMPLATE_BITWISE_BINARY[entry.op_name]
        )
        if bool_op is not None:
            lines.append(
                f'XVM_TEMPLATE_BITWISE_BINARY_BOOL_CASE({opcode}, {int_op}, {bool_op}, '
                f'{bigint_fn}, {op_flag}, {op_symbol}, {op_name}, {error_msg})')
        else:
            lines.append(
                f'XVM_TEMPLATE_BITWISE_BINARY_CASE({opcode}, {int_op}, {bigint_fn}, '
                f'{op_flag}, {op_symbol}, {op_name}, {error_msg})')
    lines.append('')
    return '\n'.join(lines)


def generate_xi_vm_template_bitwise_unary_dispatch(entries: list[XiLoweringDef]) -> str:
    bitwise_entries = [
        entry for entry in entries
        if 'vm-bytecode' in entry.target_drivers and entry.op_name in XI_VM_TEMPLATE_BITWISE_UNARY
    ]
    missing_bitwise_ops = [
        entry.op_name for entry in bitwise_entries
        if entry.op_name not in XI_VM_TEMPLATE_OPCODES
    ]
    if missing_bitwise_ops:
        die("xi-lowering: missing VM bitwise unary template opcode(s) for " +
            ", ".join(missing_bitwise_ops))

    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('/* Included inside xvm_dispatch_bitwise.inc.c with template macros defined. */')
    lines.append('')
    lines.append('#ifndef XVM_TEMPLATE_BITWISE_UNARY_CASE')
    lines.append('#error "XVM_TEMPLATE_BITWISE_UNARY_CASE must be defined before including this file"')
    lines.append('#endif')
    lines.append('')
    for entry in bitwise_entries:
        opcode = XI_VM_TEMPLATE_OPCODES[entry.op_name]
        int_op, op_symbol, op_name, error_msg = XI_VM_TEMPLATE_BITWISE_UNARY[entry.op_name]
        lines.append(
            f'XVM_TEMPLATE_BITWISE_UNARY_CASE({opcode}, {int_op}, {op_symbol}, '
            f'{op_name}, {error_msg})')
    lines.append('')
    return '\n'.join(lines)


def generate_xi_vm_template_unary_dispatch(entries: list[XiLoweringDef]) -> str:
    unary_entries = [
        entry for entry in entries
        if 'vm-bytecode' in entry.target_drivers and entry.op_name in XI_VM_TEMPLATE_UNARY
    ]
    missing_unary_ops = [
        entry.op_name for entry in unary_entries
        if entry.op_name not in XI_VM_TEMPLATE_OPCODES
    ]
    if missing_unary_ops:
        die("xi-lowering: missing VM unary template opcode(s) for " +
            ", ".join(missing_unary_ops))

    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('/* Included inside xvm_dispatch_arith.inc.c with template macros defined. */')
    lines.append('')
    lines.append('#ifndef XVM_TEMPLATE_UNARY_NEG_CASE')
    lines.append('#error "XVM_TEMPLATE_UNARY_NEG_CASE must be defined before including this file"')
    lines.append('#endif')
    lines.append('#ifndef XVM_TEMPLATE_UNARY_NOT_CASE')
    lines.append('#error "XVM_TEMPLATE_UNARY_NOT_CASE must be defined before including this file"')
    lines.append('#endif')
    lines.append('')
    for entry in unary_entries:
        opcode = XI_VM_TEMPLATE_OPCODES[entry.op_name]
        macro, op_name = XI_VM_TEMPLATE_UNARY[entry.op_name]
        lines.append(f'{macro}({opcode}, {op_name})')
    lines.append('')
    return '\n'.join(lines)


def generate_xi_vm_template_arith_binary_dispatch(entries: list[XiLoweringDef]) -> str:
    arith_entries = [
        entry for entry in entries
        if 'vm-bytecode' in entry.target_drivers and entry.op_name in XI_VM_TEMPLATE_ARITH_BINARY
    ]
    missing_arith_ops = [
        entry.op_name for entry in arith_entries
        if entry.op_name not in XI_VM_TEMPLATE_OPCODES
    ]
    if missing_arith_ops:
        die("xi-lowering: missing VM arithmetic template opcode(s) for " +
            ", ".join(missing_arith_ops))

    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('/* Included inside xvm_dispatch_arith.inc.c with template macros defined. */')
    lines.append('')
    required_macros = []
    for entry in arith_entries:
        macro = XI_VM_TEMPLATE_ARITH_BINARY[entry.op_name][0]
        if macro not in required_macros:
            required_macros.append(macro)
    for macro in required_macros:
        lines.append(f'#ifndef {macro}')
        lines.append(f'#error "{macro} must be defined before including this file"')
        lines.append('#endif')
    lines.append('')
    for entry in arith_entries:
        opcode = XI_VM_TEMPLATE_OPCODES[entry.op_name]
        macro, *macro_args = XI_VM_TEMPLATE_ARITH_BINARY[entry.op_name]
        lines.append(f'{macro}({opcode}, {", ".join(macro_args)})')
    lines.append('')
    return '\n'.join(lines)


def generate_xi_vm_template_shift_dispatch(entries: list[XiLoweringDef]) -> str:
    shift_entries = [
        entry for entry in entries
        if 'vm-bytecode' in entry.target_drivers and entry.op_name in XI_VM_TEMPLATE_SHIFT
    ]
    missing_shift_ops = [
        entry.op_name for entry in shift_entries
        if entry.op_name not in XI_VM_TEMPLATE_OPCODES
    ]
    if missing_shift_ops:
        die("xi-lowering: missing VM shift template opcode(s) for " +
            ", ".join(missing_shift_ops))

    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('/* Included inside xvm_dispatch_bitwise.inc.c with template macros defined. */')
    lines.append('')
    lines.append('#ifndef XVM_TEMPLATE_SHIFT_CASE')
    lines.append('#error "XVM_TEMPLATE_SHIFT_CASE must be defined before including this file"')
    lines.append('#endif')
    lines.append('')
    for entry in shift_entries:
        opcode = XI_VM_TEMPLATE_OPCODES[entry.op_name]
        int_fn, bigint_fn = XI_VM_TEMPLATE_SHIFT[entry.op_name]
        lines.append(f'XVM_TEMPLATE_SHIFT_CASE({opcode}, {int_fn}, {bigint_fn})')
    lines.append('')
    return '\n'.join(lines)


def generate_xi_vm_template_compare_dispatch(entries: list[XiLoweringDef]) -> str:
    compare_entries = [
        entry for entry in entries
        if 'vm-bytecode' in entry.target_drivers and entry.op_name in XI_VM_TEMPLATE_COMPARE_OPS
    ]
    missing_compare_ops = [
        entry.op_name for entry in compare_entries
        if entry.op_name not in XI_VM_TEMPLATE_OPCODES
    ]
    if missing_compare_ops:
        die("xi-lowering: missing VM compare template opcode(s) for " +
            ", ".join(missing_compare_ops))

    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('/* Included inside xvm_dispatch_compare.inc.c with template macros defined. */')
    lines.append('')
    lines.append('#ifndef XVM_TEMPLATE_COMPARE_DEEP_CASE')
    lines.append('#error "XVM_TEMPLATE_COMPARE_DEEP_CASE must be defined before including this file"')
    lines.append('#endif')
    lines.append('#ifndef XVM_TEMPLATE_COMPARE_ORDER_CASE')
    lines.append('#error "XVM_TEMPLATE_COMPARE_ORDER_CASE must be defined before including this file"')
    lines.append('#endif')
    lines.append('')

    emitted_opcodes: set[str] = set()
    for entry in compare_entries:
        opcode = XI_VM_TEMPLATE_OPCODES[entry.op_name]
        if opcode in emitted_opcodes:
            continue
        emitted_opcodes.add(opcode)
        if entry.op_name in XI_VM_TEMPLATE_COMPARE_DEEP:
            negate, op_flag, op_symbol, op_name, deep_fn = XI_VM_TEMPLATE_COMPARE_DEEP[entry.op_name]
            lines.append(
                f'XVM_TEMPLATE_COMPARE_DEEP_CASE({opcode}, {negate}, {op_flag}, '
                f'{op_symbol}, {op_name}, {deep_fn})')
        elif entry.op_name in XI_VM_TEMPLATE_COMPARE_ORDER:
            op_flag, op_symbol, op_name, compare_fn = XI_VM_TEMPLATE_COMPARE_ORDER[entry.op_name]
            lines.append(
                f'XVM_TEMPLATE_COMPARE_ORDER_CASE({opcode}, {op_flag}, '
                f'{op_symbol}, {op_name}, {compare_fn})')
    lines.append('')
    return '\n'.join(lines)


def generate_xi_target_dispatch_header(entries: list[XiLoweringDef], target: str,
                                       guard: str, macro: str) -> str:
    target_entries = [entry for entry in entries if target in entry.target_drivers]
    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('')
    lines.append(f'#ifndef {guard}')
    lines.append(f'#define {guard}')
    lines.append('')
    lines.append(f'#define {macro}(X) \\')
    for i, entry in enumerate(target_entries):
        driver = entry.target_drivers[target]
        suffix = ' \\' if i + 1 < len(target_entries) else ''
        lines.append(f'    X({entry.ident}, "{entry.op_name}", {driver}){suffix}')
    lines.append('')
    lines.append('')
    lines.append(f'#endif  /* {guard} */')
    lines.append('')
    return '\n'.join(lines)


def generate_xi_to_c_dispatch_header(entries: list[XiLoweringDef]) -> str:
    target = 'aot-c'
    target_entries = [entry for entry in entries if target in entry.target_drivers]
    width_entries = [
        entry for entry in target_entries
        if entry.template in {'narrow', 'widen'}
    ]
    arith_entries = [
        entry for entry in target_entries
        if entry.op_name in XI_AOT_C_TEMPLATE_ARITH
    ]
    div_mod_entries = [
        entry for entry in target_entries
        if entry.op_name in XI_AOT_C_TEMPLATE_DIV_MOD
    ]
    bitwise_binary_entries = [
        entry for entry in target_entries
        if entry.op_name in XI_AOT_C_TEMPLATE_BITWISE_BINARY
    ]
    bitwise_unary_entries = [
        entry for entry in target_entries
        if entry.op_name in XI_AOT_C_TEMPLATE_BITWISE_UNARY
    ]
    shift_entries = [
        entry for entry in target_entries
        if entry.op_name in XI_AOT_C_TEMPLATE_SHIFT
    ]
    compare_entries = [
        entry for entry in target_entries
        if entry.op_name in XI_AOT_C_TEMPLATE_COMPARE
    ]
    value_binary_templates = [
        entry for entry in target_entries
        if entry.template == 'value-binary'
    ]
    missing_value_binary_ops = [
        entry.op_name for entry in value_binary_templates
        if entry.op_name not in XI_AOT_C_TEMPLATE_ARITH
        and entry.op_name not in XI_AOT_C_TEMPLATE_DIV_MOD
        and entry.op_name not in XI_AOT_C_TEMPLATE_BITWISE_BINARY
        and entry.op_name not in XI_AOT_C_TEMPLATE_SHIFT
    ]
    if missing_value_binary_ops:
        die("xi-lowering: missing AOT C value-binary template op(s) for " +
            ", ".join(missing_value_binary_ops))
    compare_templates = [
        entry for entry in target_entries
        if entry.template == 'compare'
    ]
    missing_compare_ops = [
        entry.op_name for entry in compare_templates
        if entry.op_name not in XI_AOT_C_TEMPLATE_COMPARE
    ]
    if missing_compare_ops:
        die("xi-lowering: missing AOT C compare template op(s) for " +
            ", ".join(missing_compare_ops))
    missing_width_ops = [
        entry.op_name for entry in width_entries
        if entry.op_name not in XI_AOT_C_TEMPLATE_WIDTH
    ]
    if missing_width_ops:
        die("xi-lowering: missing AOT C width template op(s) for " +
            ", ".join(missing_width_ops))

    def emit_template_driver_macro(lines: list[str], macro: str,
                                   macro_entries: list[XiLoweringDef]) -> None:
        lines.append(f'#define {macro}(X) \\')
        for i, entry in enumerate(macro_entries):
            driver = entry.target_drivers[target]
            suffix = ' \\' if i + 1 < len(macro_entries) else ''
            lines.append(f'    X({entry.ident}, {driver}){suffix}')
        lines.append('')
        lines.append('')

    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('')
    lines.append('#ifndef XI_TO_C_DISPATCH_GEN_H')
    lines.append('#define XI_TO_C_DISPATCH_GEN_H')
    lines.append('')
    lines.append('#include <stdbool.h>')
    lines.append('#include <stdint.h>')
    lines.append('#include "../ir/xi.h"')
    lines.append('')
    lines.append('#define XI_TO_C_LOWERING_DRIVERS(X) \\')
    for i, entry in enumerate(target_entries):
        driver = entry.target_drivers[target]
        suffix = ' \\' if i + 1 < len(target_entries) else ''
        lines.append(f'    X({entry.ident}, "{entry.op_name}", {driver}){suffix}')
    lines.append('')
    lines.append('')
    emit_template_driver_macro(lines, 'XI_TO_C_TEMPLATE_ARITH_DRIVERS', arith_entries)
    emit_template_driver_macro(lines, 'XI_TO_C_TEMPLATE_DIV_MOD_DRIVERS', div_mod_entries)
    emit_template_driver_macro(lines, 'XI_TO_C_TEMPLATE_BITWISE_BINARY_DRIVERS',
                               bitwise_binary_entries)
    emit_template_driver_macro(lines, 'XI_TO_C_TEMPLATE_BITWISE_UNARY_DRIVERS',
                               bitwise_unary_entries)
    emit_template_driver_macro(lines, 'XI_TO_C_TEMPLATE_SHIFT_DRIVERS', shift_entries)
    emit_template_driver_macro(lines, 'XI_TO_C_TEMPLATE_COMPARE_DRIVERS', compare_entries)
    emit_template_driver_macro(lines, 'XI_TO_C_TEMPLATE_WIDTH_DRIVERS', width_entries)
    lines.append('static inline const char *xi_to_c_template_arith_runtime_fn(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in arith_entries:
        runtime_fn, _, _ = XI_AOT_C_TEMPLATE_ARITH[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{runtime_fn}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_to_c_template_arith_native_op(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in arith_entries:
        _, native_op, _ = XI_AOT_C_TEMPLATE_ARITH[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{native_op}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_to_c_template_arith_i64_wrap_fn(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in arith_entries:
        _, _, wrap_fn = XI_AOT_C_TEMPLATE_ARITH[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{wrap_fn}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_to_c_template_div_mod_runtime_fn(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in div_mod_entries:
        runtime_fn, _ = XI_AOT_C_TEMPLATE_DIV_MOD[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{runtime_fn}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_to_c_template_div_mod_int_fn(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in div_mod_entries:
        _, int_fn = XI_AOT_C_TEMPLATE_DIV_MOD[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{int_fn}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_to_c_template_bitwise_binary_op(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in bitwise_binary_entries:
        op_text = XI_AOT_C_TEMPLATE_BITWISE_BINARY[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{op_text}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_to_c_template_bitwise_unary_op(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in bitwise_unary_entries:
        op_text = XI_AOT_C_TEMPLATE_BITWISE_UNARY[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{op_text}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_to_c_template_shift_fn(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in shift_entries:
        fn = XI_AOT_C_TEMPLATE_SHIFT[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{fn}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_to_c_template_compare_runtime_fn(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in compare_entries:
        runtime_fn, _, _ = XI_AOT_C_TEMPLATE_COMPARE[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{runtime_fn}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_to_c_template_compare_native_op(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in compare_entries:
        _, native_op, _ = XI_AOT_C_TEMPLATE_COMPARE[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{native_op}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_to_c_template_compare_swaps_tagged_args(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in compare_entries:
        _, _, swaps = XI_AOT_C_TEMPLATE_COMPARE[entry.op_name]
        if swaps:
            lines.append(f'        case XI_{entry.ident}: return true;')
    lines.append('        case XI_OP_COUNT: return false;')
    lines.append('        default: return false;')
    lines.append('    }')
    lines.append('    return false;')
    lines.append('}')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    AOT_WIDTH_TEMPLATE_INVALID = 0,')
    lines.append('    AOT_WIDTH_TEMPLATE_CAST_I64 = 1,')
    lines.append('    AOT_WIDTH_TEMPLATE_F32_ROUNDTRIP = 2,')
    lines.append('} XiToCWidthTemplateKind;')
    lines.append('')
    lines.append('static inline XiToCWidthTemplateKind xi_to_c_template_width_kind(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in width_entries:
        kind, _, _ = XI_AOT_C_TEMPLATE_WIDTH[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return {kind};')
    lines.append('        case XI_OP_COUNT: return AOT_WIDTH_TEMPLATE_INVALID;')
    lines.append('        default: return AOT_WIDTH_TEMPLATE_INVALID;')
    lines.append('    }')
    lines.append('    return AOT_WIDTH_TEMPLATE_INVALID;')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xi_to_c_template_width_cast_type(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in width_entries:
        _, ctype, _ = XI_AOT_C_TEMPLATE_WIDTH[entry.op_name]
        if ctype:
            lines.append(f'        case XI_{entry.ident}: return "{ctype}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_to_c_template_width_preserves_loaded_f32(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in width_entries:
        _, _, preserve = XI_AOT_C_TEMPLATE_WIDTH[entry.op_name]
        if preserve:
            lines.append(f'        case XI_{entry.ident}: return true;')
    lines.append('        case XI_OP_COUNT: return false;')
    lines.append('        default: return false;')
    lines.append('    }')
    lines.append('    return false;')
    lines.append('}')
    lines.append('')
    lines.append('#endif  /* XI_TO_C_DISPATCH_GEN_H */')
    lines.append('')
    return '\n'.join(lines)


def generate_xi_lowering_test(entries: list[XiLoweringDef]) -> str:
    main_targets = {'vm-bytecode', 'aot-c'}
    patterned_entries = [entry for entry in entries if entry.template != 'custom']
    main_backend_entries = [entry for entry in entries if main_targets <= set(entry.targets)]
    main_backend_patterned_entries = [
        entry for entry in patterned_entries
        if main_targets <= set(entry.targets)
    ]
    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('')
    lines.append('#include "../../../src/ir/xi_lowering_coverage_gen.h"')
    lines.append('#include "../../../src/ir/xi_emit_vm_gen.h"')
    lines.append('#include "../../../src/aot/xi_to_c_dispatch_gen.h"')
    lines.append('')
    lines.append('#include <assert.h>')
    lines.append('#include <stdio.h>')
    lines.append('#include <string.h>')
    lines.append('')
    lines.append('int main(void) {')
    lines.append(f'    assert(XI_LOWERING_ENTRY_COUNT == {len(entries)});')
    lines.append(f'    assert(XI_LOWERING_PATTERNED_ENTRY_COUNT == {len(patterned_entries)});')
    lines.append(f'    assert(XI_LOWERING_CUSTOM_ENTRY_COUNT == {len(entries) - len(patterned_entries)});')
    lines.append(f'    assert(XI_LOWERING_MAIN_BACKEND_ENTRY_COUNT == {len(main_backend_entries)});')
    lines.append(
        f'    assert(XI_LOWERING_MAIN_BACKEND_PATTERNED_ENTRY_COUNT == {len(main_backend_patterned_entries)});')
    lines.append(
        f'    assert(XI_LOWERING_REJECTED_TARGET_COUNT == {sum(len(entry.target_rejects) for entry in entries)});')
    lines.append(
        f'    assert(XI_LOWERING_REJECTED_ENTRY_COUNT == {sum(1 for entry in entries if entry.target_rejects)});')
    for target in sorted(VALID_XI_LOWERING_TARGETS):
        ident = _xi_c_ident(target)
        lines.append(
            f'    assert(XI_LOWERING_{ident}_ENTRY_COUNT == {sum(1 for entry in entries if target in entry.targets)});')
        lines.append(
            f'    assert(XI_LOWERING_{ident}_PATTERNED_ENTRY_COUNT == {sum(1 for entry in patterned_entries if target in entry.targets)});')
    for entry in entries:
        target_bits = _xi_bit_expr('XI_LOWER_TARGET', entry.targets)
        required_bits = _xi_bit_expr('XI_LOWER_TARGET', entry.required_targets)
        reject_bits = _xi_bit_expr('XI_LOWER_TARGET', sorted(entry.target_rejects.keys()))
        template = f'XI_LOWER_TEMPLATE_{_xi_c_ident(entry.template)}'
        patterned = 'true' if entry.template != 'custom' else 'false'
        lines.append(f'    assert(xi_lowering_generated_targets(XI_{entry.ident}) == ({target_bits}));')
        lines.append(f'    assert(xi_lowering_required_targets(XI_{entry.ident}) == ({required_bits}));')
        lines.append(f'    assert(xi_lowering_rejected_targets(XI_{entry.ident}) == ({reject_bits}));')
        lines.append(f'    assert(xi_lowering_template_kind(XI_{entry.ident}) == {template});')
        lines.append(f'    assert(xi_lowering_is_patterned(XI_{entry.ident}) == {patterned});')
        if 'vm-bytecode' in entry.target_drivers and entry.template != 'custom':
            opcode = XI_VM_TEMPLATE_OPCODES[entry.op_name]
            swaps = 'true' if entry.op_name in XI_VM_TEMPLATE_SWAP_ARGS else 'false'
            lines.append(f'    assert(xi_emit_vm_template_opcode(XI_{entry.ident}) == {opcode});')
            lines.append(f'    assert(xi_emit_vm_template_swaps_args(XI_{entry.ident}) == {swaps});')
        if 'aot-c' in entry.target_drivers and entry.op_name in XI_AOT_C_TEMPLATE_ARITH:
            runtime_fn, native_op, wrap_fn = XI_AOT_C_TEMPLATE_ARITH[entry.op_name]
            lines.append(
                f'    assert(strcmp(xi_to_c_template_arith_runtime_fn(XI_{entry.ident}), "{runtime_fn}") == 0);')
            lines.append(
                f'    assert(strcmp(xi_to_c_template_arith_native_op(XI_{entry.ident}), "{native_op}") == 0);')
            lines.append(
                f'    assert(strcmp(xi_to_c_template_arith_i64_wrap_fn(XI_{entry.ident}), "{wrap_fn}") == 0);')
        if 'aot-c' in entry.target_drivers and entry.op_name in XI_AOT_C_TEMPLATE_DIV_MOD:
            runtime_fn, int_fn = XI_AOT_C_TEMPLATE_DIV_MOD[entry.op_name]
            lines.append(
                f'    assert(strcmp(xi_to_c_template_div_mod_runtime_fn(XI_{entry.ident}), "{runtime_fn}") == 0);')
            lines.append(
                f'    assert(strcmp(xi_to_c_template_div_mod_int_fn(XI_{entry.ident}), "{int_fn}") == 0);')
        if 'aot-c' in entry.target_drivers and entry.op_name in XI_AOT_C_TEMPLATE_BITWISE_BINARY:
            op_text = XI_AOT_C_TEMPLATE_BITWISE_BINARY[entry.op_name]
            lines.append(
                f'    assert(strcmp(xi_to_c_template_bitwise_binary_op(XI_{entry.ident}), "{op_text}") == 0);')
        if 'aot-c' in entry.target_drivers and entry.op_name in XI_AOT_C_TEMPLATE_BITWISE_UNARY:
            op_text = XI_AOT_C_TEMPLATE_BITWISE_UNARY[entry.op_name]
            lines.append(
                f'    assert(strcmp(xi_to_c_template_bitwise_unary_op(XI_{entry.ident}), "{op_text}") == 0);')
        if 'aot-c' in entry.target_drivers and entry.op_name in XI_AOT_C_TEMPLATE_SHIFT:
            fn = XI_AOT_C_TEMPLATE_SHIFT[entry.op_name]
            lines.append(
                f'    assert(strcmp(xi_to_c_template_shift_fn(XI_{entry.ident}), "{fn}") == 0);')
        if 'aot-c' in entry.target_drivers and entry.op_name in XI_AOT_C_TEMPLATE_COMPARE:
            runtime_fn, native_op, swaps = XI_AOT_C_TEMPLATE_COMPARE[entry.op_name]
            swaps_c = 'true' if swaps else 'false'
            lines.append(
                f'    assert(strcmp(xi_to_c_template_compare_runtime_fn(XI_{entry.ident}), "{runtime_fn}") == 0);')
            lines.append(
                f'    assert(strcmp(xi_to_c_template_compare_native_op(XI_{entry.ident}), "{native_op}") == 0);')
            lines.append(
                f'    assert(xi_to_c_template_compare_swaps_tagged_args(XI_{entry.ident}) == {swaps_c});')
        if 'aot-c' in entry.target_drivers and entry.template in {'narrow', 'widen'}:
            kind, ctype, preserve = XI_AOT_C_TEMPLATE_WIDTH[entry.op_name]
            preserve_c = 'true' if preserve else 'false'
            lines.append(f'    assert(xi_to_c_template_width_kind(XI_{entry.ident}) == {kind});')
            lines.append(
                f'    assert(strcmp(xi_to_c_template_width_cast_type(XI_{entry.ident}), "{ctype}") == 0);')
            lines.append(
                f'    assert(xi_to_c_template_width_preserves_loaded_f32(XI_{entry.ident}) == {preserve_c});')
        fresh_dst = 'true' if entry.target_attrs.get('vm-bytecode', {}).get('fresh-dst',
                                                                            False) else 'false'
        lines.append(f'    assert(xi_emit_vm_requires_fresh_dst(XI_{entry.ident}) == {fresh_dst});')
        raw_cell_args = 'true' if entry.target_attrs.get('vm-bytecode', {}).get(
            'raw-cell-args', False) else 'false'
        cell_dst = 'true' if entry.target_attrs.get('vm-bytecode', {}).get('handles-cell-dst',
                                                                           False) else 'false'
        lines.append(f'    assert(xi_emit_vm_uses_raw_cell_args(XI_{entry.ident}) == {raw_cell_args});')
        lines.append(f'    assert(xi_emit_vm_handles_cell_dst(XI_{entry.ident}) == {cell_dst});')
    lines.append('    printf("Xi lowering generated coverage: %d entries\\n", XI_LOWERING_ENTRY_COUNT);')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    return '\n'.join(lines)


def write_xi_lowering_outputs(output_root: str, entries: list[XiLoweringDef]) -> list[str]:
    outputs = [
        ('src/ir/xi_lowering_coverage_gen.h', generate_xi_lowering_coverage_header(entries)),
        ('src/ir/xi_emit_vm_gen.h', generate_xi_vm_dispatch_header(entries)),
        ('src/vm/xvm_template_width_gen.inc.c', generate_xi_vm_template_width_dispatch(entries)),
        ('src/vm/xvm_template_bitwise_binary_gen.inc.c',
         generate_xi_vm_template_bitwise_binary_dispatch(entries)),
        ('src/vm/xvm_template_bitwise_unary_gen.inc.c',
         generate_xi_vm_template_bitwise_unary_dispatch(entries)),
        ('src/vm/xvm_template_unary_gen.inc.c',
         generate_xi_vm_template_unary_dispatch(entries)),
        ('src/vm/xvm_template_arith_binary_gen.inc.c',
         generate_xi_vm_template_arith_binary_dispatch(entries)),
        ('src/vm/xvm_template_shift_gen.inc.c', generate_xi_vm_template_shift_dispatch(entries)),
        ('src/vm/xvm_template_compare_gen.inc.c',
         generate_xi_vm_template_compare_dispatch(entries)),
        ('src/aot/xi_to_c_dispatch_gen.h', generate_xi_to_c_dispatch_header(entries)),
        ('src/aot/xi_to_c_stmt_dispatch_gen.h',
         generate_xi_target_dispatch_header(entries, 'aot-c-stmt',
                                            'XI_TO_C_STMT_DISPATCH_GEN_H',
                                            'XI_TO_C_STMT_LOWERING_DRIVERS')),
        ('tests/unit/ir/test_xi_lowering_gen.c', generate_xi_lowering_test(entries)),
    ]
    written = []
    for relpath, content in outputs:
        path = os.path.join(output_root, relpath)
        write_file(path, content)
        written.append(path)
    return written


# ============================================================
# L3: Xi verifier metadata
# ============================================================

VALID_XI_VERIFY_CHECKS = {
    'bool-result',
    'obsolete',
    'select-contract',
}


@dataclass
class XiVerifierRule:
    op_name: str
    ident: str
    checks: list


def parse_xi_verifier_def(text: str, ops: list[XiOpDef], path: str = '<input>') -> list[XiVerifierRule]:
    known_ops = {op.name for op in ops}
    forms = parse_sexpr(tokenize_sexpr(text, path), path)
    rules = []
    seen = set()
    for form in forms:
        if not isinstance(form, SList) or not form.children:
            die(f"{path}: top-level form must be a list")
        head = _sexpr_atom_value(form.children[0], path)
        if head != 'verify-xi-op':
            die(f"{path}:{form.line}:{form.col}: expected verify-xi-op")
        if len(form.children) < 2:
            die(f"{path}:{form.line}:{form.col}: missing verified Xi op name")
        op_name = _sexpr_atom_value(form.children[1], 'verify-xi-op')
        if op_name not in known_ops:
            die(f"{path}: verifier references unknown Xi op '{op_name}'")
        if op_name in seen:
            die(f"{path}: duplicate verifier rule for '{op_name}'")
        seen.add(op_name)
        checks = _xi_parse_atom_list(_xi_get_kw_list(form, ':checks'), f"{op_name}:checks")
        if not checks:
            die(f"{path}: verifier rule '{op_name}' is missing :checks")
        for check in checks:
            if check not in VALID_XI_VERIFY_CHECKS:
                die(f"{path}: verifier rule '{op_name}' uses unknown check '{check}'")
        rules.append(XiVerifierRule(op_name=op_name, ident=_xi_op_ident(op_name),
                                    checks=checks))
    return rules


def _xi_verify_check_flag_expr(values: list) -> str:
    return _xi_bit_expr('XI_VERIFY_CHECK', values)


def generate_xi_verify_header(rules: list[XiVerifierRule]) -> str:
    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/verifier.def */')
    lines.append('')
    lines.append('#ifndef XI_VERIFY_GEN_H')
    lines.append('#define XI_VERIFY_GEN_H')
    lines.append('')
    lines.append('#include <stdbool.h>')
    lines.append('#include <stdint.h>')
    lines.append('#include "xi.h"')
    lines.append('')
    lines.append('#define XI_VERIFY_CHECK_NONE 0')
    for i, check in enumerate(sorted(VALID_XI_VERIFY_CHECKS)):
        lines.append(f'#define XI_VERIFY_CHECK_{_xi_c_ident(check)} (1u << {i})')
    lines.append('')
    lines.append(f'enum {{ XI_VERIFY_RULE_COUNT = {len(rules)} }};')
    lines.append('')
    lines.append('#define XI_VERIFY_RULES(X) \\')
    for i, rule in enumerate(rules):
        checks = _xi_verify_check_flag_expr(rule.checks)
        suffix = ' \\' if i + 1 < len(rules) else ''
        lines.append(f'    X({rule.ident}, "{rule.op_name}", {checks}){suffix}')
    lines.append('')
    lines.append('')
    lines.append('static inline uint32_t xi_verify_generated_op_checks(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for rule in rules:
        checks = _xi_verify_check_flag_expr(rule.checks)
        lines.append(f'        case XI_{rule.ident}: return {checks};')
    lines.append('        case XI_OP_COUNT: return 0;')
    lines.append('        default: return 0;')
    lines.append('    }')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_verify_generated_op_has_check(uint16_t op, uint32_t check) {')
    lines.append('    return (xi_verify_generated_op_checks(op) & check) == check;')
    lines.append('}')
    lines.append('')
    lines.append('#endif  /* XI_VERIFY_GEN_H */')
    lines.append('')
    return '\n'.join(lines)

# ============================================================
# L3: AOT representation metadata
# ============================================================

VALID_AOT_DYNAMIC_KINDS = {
    'aggregate',
    'pointer',
    'scalar',
    'tagged',
    'void',
}

VALID_AOT_STORAGE_REPS = {
    'XR_REP_F64',
    'XR_REP_I64',
    'XR_REP_PTR',
    'XR_REP_RAWPTR',
    'XR_REP_TAGGED',
    'XR_REP_VOID',
}


@dataclass
class AotRepDef:
    name: str
    ident: str
    c_type: str
    size: int
    align: int
    signed: bool
    integer: bool
    boxed: bool
    dynamic_kind: str
    native_type: str
    storage_rep: str


def _aot_bool_atom(form: SList, keyword: str, context: str) -> bool:
    value = _xi_get_kw(form, keyword)
    if value is None:
        die(f"{context}: missing {keyword}")
    text = _sexpr_atom_value(value, context)
    if text == 'yes':
        return True
    if text == 'no':
        return False
    die(f"{context}: {keyword} must be yes or no")


def _aot_int_atom(form: SList, keyword: str, context: str) -> int:
    value = _xi_get_kw(form, keyword)
    if value is None:
        die(f"{context}: missing {keyword}")
    if not isinstance(value, SAtom) or not value.is_number:
        die(f"{context}: {keyword} must be a number")
    return value.int_value


def parse_aot_rep_def(text: str, path: str = '<input>') -> list[AotRepDef]:
    forms = parse_sexpr(tokenize_sexpr(text, path), path)
    reps = []
    seen_names = set()
    seen_native_types = set()
    for form in forms:
        if not isinstance(form, SList) or not form.children:
            die(f"{path}: top-level form must be a list")
        head = _sexpr_atom_value(form.children[0], path)
        if head != 'define-aot-rep':
            die(f"{path}:{form.line}:{form.col}: expected define-aot-rep")
        if len(form.children) < 2:
            die(f"{path}:{form.line}:{form.col}: missing AOT rep name")
        name = _sexpr_atom_value(form.children[1], 'define-aot-rep')
        if name in seen_names:
            die(f"{path}: duplicate AOT rep '{name}'")
        seen_names.add(name)
        context = f"{path}:{name}"
        c_type = _xi_get_kw_str(form, ':c-type')
        if not c_type:
            die(f"{context}: missing :c-type")
        size = _aot_int_atom(form, ':size', context)
        align = _aot_int_atom(form, ':align', context)
        if size < 0:
            die(f"{context}: :size must be non-negative")
        if align <= 0:
            die(f"{context}: :align must be positive")
        signed = _aot_bool_atom(form, ':signed', context)
        integer = _aot_bool_atom(form, ':integer', context)
        boxed = _aot_bool_atom(form, ':boxed', context)
        dynamic_kind = _xi_get_kw_str(form, ':dynamic-kind')
        if dynamic_kind not in VALID_AOT_DYNAMIC_KINDS:
            die(f"{context}: unknown :dynamic-kind '{dynamic_kind}'")
        if dynamic_kind == 'void' and size != 0:
            die(f"{context}: void representation must have size 0")
        if dynamic_kind != 'void' and size == 0:
            die(f"{context}: non-void representation must have positive size")
        native_type = _xi_get_kw_str(form, ':native-type')
        if not native_type:
            die(f"{context}: missing :native-type")
        if native_type != 'none':
            if not native_type.startswith('XR_NATIVE_'):
                die(f"{context}: :native-type must be XR_NATIVE_* or none")
            if native_type in seen_native_types:
                die(f"{context}: duplicate native type '{native_type}'")
            seen_native_types.add(native_type)
        storage_rep = _xi_get_kw_str(form, ':storage-rep')
        if storage_rep not in VALID_AOT_STORAGE_REPS:
            die(f"{context}: unknown :storage-rep '{storage_rep}'")
        reps.append(AotRepDef(name=name, ident=_xi_c_ident(name), c_type=c_type, size=size,
                              align=align, signed=signed, integer=integer, boxed=boxed,
                              dynamic_kind=dynamic_kind, native_type=native_type,
                              storage_rep=storage_rep))
    return reps


def _c_bool(value: bool) -> str:
    return 'true' if value else 'false'


def _aot_int_fits_expr(rep: AotRepDef) -> str:
    if not rep.integer:
        return 'false'
    if rep.signed:
        if rep.size == 1:
            return 'value >= INT8_MIN && value <= INT8_MAX'
        if rep.size == 2:
            return 'value >= INT16_MIN && value <= INT16_MAX'
        if rep.size == 4:
            return 'value >= INT32_MIN && value <= INT32_MAX'
        if rep.size == 8:
            return 'true'
    else:
        if rep.size == 1:
            return 'value >= 0 && value <= UINT8_MAX'
        if rep.size == 2:
            return 'value >= 0 && value <= UINT16_MAX'
        if rep.size == 4:
            return 'value >= 0 && (uint64_t) value <= UINT32_MAX'
        if rep.size == 8:
            return 'value >= 0'
    return 'false'


def _aot_elem_name(rep: AotRepDef) -> str:
    return f'XR_ELEM_{rep.ident}'


def generate_aot_rep_header(reps: list[AotRepDef]) -> str:
    dynamic_kinds = []
    for rep in reps:
        if rep.dynamic_kind not in dynamic_kinds:
            dynamic_kinds.append(rep.dynamic_kind)
    native_reps = [rep for rep in reps if rep.native_type != 'none']

    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/aot/rep.def */')
    lines.append('')
    lines.append('#ifndef XAOT_REP_GEN_H')
    lines.append('#define XAOT_REP_GEN_H')
    lines.append('')
    lines.append('#include <stdbool.h>')
    lines.append('#include <stdint.h>')
    lines.append('#include "../runtime/value/xtype.h"')
    lines.append('')
    lines.append('typedef enum {')
    for i, rep in enumerate(reps):
        lines.append(f'    XAOT_REP_{rep.ident} = {i},')
    lines.append('    XAOT_REP_COUNT')
    lines.append('} XaotRep;')
    lines.append('')
    lines.append('typedef enum {')
    for i, kind in enumerate(dynamic_kinds):
        lines.append(f'    XAOT_DYNAMIC_{_xi_c_ident(kind)} = {i},')
    lines.append('    XAOT_DYNAMIC_COUNT')
    lines.append('} XaotDynamicKind;')
    lines.append('')
    lines.append('typedef struct {')
    lines.append('    const char *name;')
    lines.append('    const char *c_type;')
    lines.append('    uint8_t size;')
    lines.append('    uint8_t align;')
    lines.append('    bool is_signed;')
    lines.append('    bool is_integer;')
    lines.append('    bool is_boxed;')
    lines.append('    uint8_t dynamic_kind;')
    lines.append('    bool has_native_type;')
    lines.append('    uint8_t native_type;')
    lines.append('    XrRep storage_rep;')
    lines.append('} XaotRepInfo;')
    lines.append('')
    lines.append('#define XAOT_REP_ENTRIES(X) \\')
    for i, rep in enumerate(reps):
        native_type = rep.native_type if rep.native_type != 'none' else '0'
        suffix = ' \\' if i + 1 < len(reps) else ''
        lines.append(
            f'    X({rep.ident}, "{rep.name}", "{rep.c_type}", {rep.size}, {rep.align}, '
            f'{_c_bool(rep.signed)}, {_c_bool(rep.integer)}, {_c_bool(rep.boxed)}, '
            f'XAOT_DYNAMIC_{_xi_c_ident(rep.dynamic_kind)}, {_c_bool(rep.native_type != "none")}, '
            f'{native_type}, {rep.storage_rep}){suffix}')
    lines.append('')
    lines.append('')
    lines.append('static inline const XaotRepInfo *xaot_rep_info(XaotRep rep) {')
    lines.append('    static const XaotRepInfo table[XAOT_REP_COUNT] = {')
    for rep in reps:
        native_type = rep.native_type if rep.native_type != 'none' else '0'
        lines.append(f'        [XAOT_REP_{rep.ident}] = {{"{rep.name}", "{rep.c_type}",')
        lines.append(f'                                      {rep.size}, {rep.align},')
        lines.append(f'                                      {_c_bool(rep.signed)}, {_c_bool(rep.integer)},')
        lines.append(f'                                      {_c_bool(rep.boxed)},')
        lines.append(f'                                      XAOT_DYNAMIC_{_xi_c_ident(rep.dynamic_kind)},')
        lines.append(f'                                      {_c_bool(rep.native_type != "none")}, {native_type},')
        lines.append(f'                                      {rep.storage_rep}}},')
    lines.append('    };')
    lines.append('    if ((unsigned) rep >= XAOT_REP_COUNT)')
    lines.append('        return NULL;')
    lines.append('    return &table[rep];')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xaot_rep_from_native_type(uint8_t native_type, XaotRep *out) {')
    lines.append('    switch (native_type) {')
    for rep in native_reps:
        lines.append(f'        case {rep.native_type}:')
        lines.append(f'            if (out) *out = XAOT_REP_{rep.ident};')
        lines.append('            return true;')
    lines.append('        default:')
    lines.append('            return false;')
    lines.append('    }')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xaot_c_type_for_native_type(uint8_t native_type) {')
    lines.append('    XaotRep rep;')
    lines.append('    const XaotRepInfo *info;')
    lines.append('    if (!xaot_rep_from_native_type(native_type, &rep))')
    lines.append('        return NULL;')
    lines.append('    info = xaot_rep_info(rep);')
    lines.append('    return info ? info->c_type : NULL;')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xaot_c_type_for_native_int_type(uint8_t native_type) {')
    lines.append('    XaotRep rep;')
    lines.append('    const XaotRepInfo *info;')
    lines.append('    if (!xaot_rep_from_native_type(native_type, &rep))')
    lines.append('        return NULL;')
    lines.append('    info = xaot_rep_info(rep);')
    lines.append('    return (info && info->is_integer) ? info->c_type : NULL;')
    lines.append('}')
    lines.append('')
    lines.append('static inline XrRep xaot_storage_rep_for_native_type(uint8_t native_type) {')
    lines.append('    XaotRep rep;')
    lines.append('    const XaotRepInfo *info;')
    lines.append('    if (!xaot_rep_from_native_type(native_type, &rep))')
    lines.append('        return XR_REP_TAGGED;')
    lines.append('    info = xaot_rep_info(rep);')
    lines.append('    return info ? info->storage_rep : XR_REP_TAGGED;')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xaot_elem_name_for_native_type(uint8_t native_type) {')
    lines.append('    switch (native_type) {')
    for rep in native_reps:
        lines.append(f'        case {rep.native_type}:')
        lines.append(f'            return "{_aot_elem_name(rep)}";')
    lines.append('        default:')
    lines.append('            return NULL;')
    lines.append('    }')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xaot_native_int_const_fits(uint8_t native_type, int64_t value) {')
    lines.append('    switch (native_type) {')
    for rep in native_reps:
        if not rep.integer:
            continue
        lines.append(f'        case {rep.native_type}:')
        lines.append(f'            return {_aot_int_fits_expr(rep)};')
    lines.append('        default:')
    lines.append('            return false;')
    lines.append('    }')
    lines.append('}')
    lines.append('')
    lines.append('#endif  /* XAOT_REP_GEN_H */')
    lines.append('')
    return '\n'.join(lines)


# ============================================================
# L3: AOT ABI metadata
# ============================================================

VALID_AOT_ABI_CLASSES = {
    'pointer',
    'scalar',
    'tagged',
    'void',
}


@dataclass
class AotAbiDef:
    name: str
    ident: str
    type_kind: str
    nullable: bool
    abi_class: str
    default_rep: str
    native_width: bool
    typed_boundary: bool


def _aot_rep_names(reps: list[AotRepDef]) -> set:
    return {rep.name for rep in reps}


def parse_aot_abi_def(text: str, reps: list[AotRepDef], path: str = '<input>') -> list[AotAbiDef]:
    known_reps = _aot_rep_names(reps)
    forms = parse_sexpr(tokenize_sexpr(text, path), path)
    entries = []
    seen_names = set()
    seen_kinds = set()
    for form in forms:
        if not isinstance(form, SList) or not form.children:
            die(f"{path}: top-level form must be a list")
        head = _sexpr_atom_value(form.children[0], path)
        if head != 'define-aot-abi':
            die(f"{path}:{form.line}:{form.col}: expected define-aot-abi")
        if len(form.children) < 2:
            die(f"{path}:{form.line}:{form.col}: missing AOT ABI name")
        name = _sexpr_atom_value(form.children[1], 'define-aot-abi')
        if name in seen_names:
            die(f"{path}: duplicate AOT ABI '{name}'")
        seen_names.add(name)
        context = f"{path}:{name}"
        type_kind = _xi_get_kw_str(form, ':type-kind')
        if not type_kind.startswith('XR_KIND_'):
            die(f"{context}: :type-kind must be XR_KIND_*")
        if type_kind in seen_kinds:
            die(f"{context}: duplicate type kind '{type_kind}'")
        seen_kinds.add(type_kind)
        abi_class = _xi_get_kw_str(form, ':abi-class')
        if abi_class not in VALID_AOT_ABI_CLASSES:
            die(f"{context}: unknown :abi-class '{abi_class}'")
        default_rep = _xi_get_kw_str(form, ':default-rep')
        if default_rep not in known_reps:
            die(f"{context}: unknown :default-rep '{default_rep}'")
        entries.append(AotAbiDef(name=name, ident=_xi_c_ident(name), type_kind=type_kind,
                                 nullable=_aot_bool_atom(form, ':nullable', context),
                                 abi_class=abi_class, default_rep=default_rep,
                                 native_width=_aot_bool_atom(form, ':native-width', context),
                                 typed_boundary=_aot_bool_atom(form, ':typed-boundary', context)))
    return entries


def generate_aot_abi_header(entries: list[AotAbiDef]) -> str:
    classes = []
    for entry in entries:
        if entry.abi_class not in classes:
            classes.append(entry.abi_class)
    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/aot/abi.def */')
    lines.append('')
    lines.append('#ifndef XAOT_ABI_GEN_H')
    lines.append('#define XAOT_ABI_GEN_H')
    lines.append('')
    lines.append('#include <stdbool.h>')
    lines.append('#include <stddef.h>')
    lines.append('#include <stdint.h>')
    lines.append('#include "../runtime/value/xtype.h"')
    lines.append('#include "xaot_rep_gen.h"')
    lines.append('')
    lines.append('typedef enum {')
    for i, cls in enumerate(classes):
        lines.append(f'    XAOT_ABI_CLASS_{_xi_c_ident(cls)} = {i},')
    lines.append('    XAOT_ABI_CLASS_COUNT')
    lines.append('} XaotAbiClass;')
    lines.append('')
    lines.append('typedef struct {')
    lines.append('    const char *name;')
    lines.append('    XrTypeKind type_kind;')
    lines.append('    uint8_t abi_class;')
    lines.append('    XaotRep default_rep;')
    lines.append('    bool allows_nullable;')
    lines.append('    bool uses_native_width;')
    lines.append('    bool typed_boundary;')
    lines.append('} XaotAbiInfo;')
    lines.append('')
    lines.append('#define XAOT_ABI_ENTRIES(X) \\')
    for i, entry in enumerate(entries):
        suffix = ' \\' if i + 1 < len(entries) else ''
        lines.append(
            f'    X({entry.ident}, "{entry.name}", {entry.type_kind}, '
            f'XAOT_ABI_CLASS_{_xi_c_ident(entry.abi_class)}, XAOT_REP_{_xi_c_ident(entry.default_rep)}, '
            f'{_c_bool(entry.nullable)}, {_c_bool(entry.native_width)}, '
            f'{_c_bool(entry.typed_boundary)}){suffix}')
    lines.append('')
    lines.append('')
    lines.append('static inline const XaotAbiInfo *xaot_abi_for_type_kind(XrTypeKind kind) {')
    lines.append('    static const XaotAbiInfo table[] = {')
    for entry in entries:
        lines.append(f'        {{"{entry.name}", {entry.type_kind},')
        lines.append(f'         XAOT_ABI_CLASS_{_xi_c_ident(entry.abi_class)},')
        lines.append(f'         XAOT_REP_{_xi_c_ident(entry.default_rep)}, {_c_bool(entry.nullable)},')
        lines.append(f'         {_c_bool(entry.native_width)}, {_c_bool(entry.typed_boundary)}}},')
    lines.append('    };')
    lines.append('    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {')
    lines.append('        if (table[i].type_kind == kind)')
    lines.append('            return &table[i];')
    lines.append('    }')
    lines.append('    return NULL;')
    lines.append('}')
    lines.append('')
    lines.append('static inline XaotRep xaot_abi_rep_for_type(const XrType *type) {')
    lines.append('    const XaotAbiInfo *abi;')
    lines.append('    XaotRep rep;')
    lines.append('    if (!type)')
    lines.append('        return XAOT_REP_TAGGED;')
    lines.append('    abi = xaot_abi_for_type_kind(type->kind);')
    lines.append('    if (!abi || (type->is_nullable && !abi->allows_nullable))')
    lines.append('        return XAOT_REP_TAGGED;')
    lines.append('    if (abi->uses_native_width && type->native_width != 0 &&')
    lines.append('        xaot_rep_from_native_type(type->native_width, &rep))')
    lines.append('        return rep;')
    lines.append('    return abi->default_rep;')
    lines.append('}')
    lines.append('')
    lines.append('static inline XrRep xaot_abi_storage_rep_for_type(const XrType *type) {')
    lines.append('    const XaotRepInfo *info = xaot_rep_info(xaot_abi_rep_for_type(type));')
    lines.append('    return info ? info->storage_rep : XR_REP_TAGGED;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xaot_abi_type_can_use_typed_boundary(const XrType *type) {')
    lines.append('    const XaotAbiInfo *abi;')
    lines.append('    XrRep storage;')
    lines.append('    if (!type)')
    lines.append('        return false;')
    lines.append('    abi = xaot_abi_for_type_kind(type->kind);')
    lines.append('    if (!abi || !abi->typed_boundary || (type->is_nullable && !abi->allows_nullable))')
    lines.append('        return false;')
    lines.append('    storage = xaot_abi_storage_rep_for_type(type);')
    lines.append('    return storage == XR_REP_I64 || storage == XR_REP_F64 || storage == XR_REP_PTR ||')
    lines.append('           storage == XR_REP_RAWPTR;')
    lines.append('}')
    lines.append('')
    lines.append('#endif  /* XAOT_ABI_GEN_H */')
    lines.append('')
    return '\n'.join(lines)


# ============================================================
# L3: AOT layout metadata
# ============================================================

VALID_AOT_LAYOUT_FIELD_KINDS = {
    'boxed-ref',
    'inline-array',
    'nested-aggregate',
    'pointer-ref',
    'scalar',
}

VALID_AOT_LAYOUT_HEAP_POLICIES = {
    'elem',
    'nested',
    'no',
    'yes',
}


@dataclass
class AotLayoutDef:
    name: str
    ident: str
    native_type: str
    field_kind: str
    rep: str
    c_type: str
    heap_field: str
    ref_tag: str


def parse_aot_layout_def(text: str, reps: list[AotRepDef],
                         path: str = '<input>') -> list[AotLayoutDef]:
    known_reps = _aot_rep_names(reps)
    forms = parse_sexpr(tokenize_sexpr(text, path), path)
    entries = []
    seen_names = set()
    seen_native_types = set()
    for form in forms:
        if not isinstance(form, SList) or not form.children:
            die(f"{path}: top-level form must be a list")
        head = _sexpr_atom_value(form.children[0], path)
        if head != 'define-aot-layout':
            die(f"{path}:{form.line}:{form.col}: expected define-aot-layout")
        if len(form.children) < 2:
            die(f"{path}:{form.line}:{form.col}: missing AOT layout name")
        name = _sexpr_atom_value(form.children[1], 'define-aot-layout')
        if name in seen_names:
            die(f"{path}: duplicate AOT layout '{name}'")
        seen_names.add(name)
        context = f"{path}:{name}"
        native_type = _xi_get_kw_str(form, ':native-type')
        if not native_type.startswith('XR_NATIVE_'):
            die(f"{context}: :native-type must be XR_NATIVE_*")
        if native_type in seen_native_types:
            die(f"{context}: duplicate native type '{native_type}'")
        seen_native_types.add(native_type)
        field_kind = _xi_get_kw_str(form, ':field-kind')
        if field_kind not in VALID_AOT_LAYOUT_FIELD_KINDS:
            die(f"{context}: unknown :field-kind '{field_kind}'")
        rep = _xi_get_kw_str(form, ':rep')
        if rep not in known_reps:
            die(f"{context}: unknown :rep '{rep}'")
        c_type = _xi_get_kw_str(form, ':c-type')
        if not c_type:
            die(f"{context}: missing :c-type")
        heap_field = _xi_get_kw_str(form, ':heap-field')
        if heap_field not in VALID_AOT_LAYOUT_HEAP_POLICIES:
            die(f"{context}: unknown :heap-field '{heap_field}'")
        ref_tag = _xi_get_kw_str(form, ':ref-tag')
        if not ref_tag:
            die(f"{context}: missing :ref-tag")
        if ref_tag != 'none' and not ref_tag.startswith('XR_TAG_'):
            die(f"{context}: :ref-tag must be XR_TAG_* or none")
        entries.append(AotLayoutDef(name=name, ident=_xi_c_ident(name), native_type=native_type,
                                    field_kind=field_kind, rep=rep, c_type=c_type,
                                    heap_field=heap_field, ref_tag=ref_tag))
    return entries


def _c_string_or_null(value: str) -> str:
    return 'NULL' if value == 'none' else f'"{value}"'


def generate_aot_layout_header(entries: list[AotLayoutDef]) -> str:
    field_kinds = []
    heap_policies = []
    for entry in entries:
        if entry.field_kind not in field_kinds:
            field_kinds.append(entry.field_kind)
        if entry.heap_field not in heap_policies:
            heap_policies.append(entry.heap_field)
    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/aot/layout.def */')
    lines.append('')
    lines.append('#ifndef XAOT_LAYOUT_GEN_H')
    lines.append('#define XAOT_LAYOUT_GEN_H')
    lines.append('')
    lines.append('#include <stdbool.h>')
    lines.append('#include <stddef.h>')
    lines.append('#include <stdint.h>')
    lines.append('#include "xaot_rep_gen.h"')
    lines.append('')
    lines.append('typedef enum {')
    for i, kind in enumerate(field_kinds):
        lines.append(f'    XAOT_LAYOUT_FIELD_{_xi_c_ident(kind)} = {i},')
    lines.append('    XAOT_LAYOUT_FIELD_COUNT')
    lines.append('} XaotLayoutFieldKind;')
    lines.append('')
    lines.append('typedef enum {')
    for i, policy in enumerate(heap_policies):
        lines.append(f'    XAOT_LAYOUT_HEAP_{_xi_c_ident(policy)} = {i},')
    lines.append('    XAOT_LAYOUT_HEAP_COUNT')
    lines.append('} XaotLayoutHeapPolicy;')
    lines.append('')
    lines.append('typedef struct {')
    lines.append('    const char *name;')
    lines.append('    uint8_t native_type;')
    lines.append('    uint8_t field_kind;')
    lines.append('    XaotRep rep;')
    lines.append('    const char *c_type;')
    lines.append('    uint8_t heap_policy;')
    lines.append('    const char *ref_tag;')
    lines.append('} XaotLayoutInfo;')
    lines.append('')
    lines.append('#define XAOT_LAYOUT_ENTRIES(X) \\')
    for i, entry in enumerate(entries):
        suffix = ' \\' if i + 1 < len(entries) else ''
        lines.append(
            f'    X({entry.ident}, "{entry.name}", {entry.native_type}, '
            f'XAOT_LAYOUT_FIELD_{_xi_c_ident(entry.field_kind)}, '
            f'XAOT_REP_{_xi_c_ident(entry.rep)}, {_c_string_or_null(entry.c_type)}, '
            f'XAOT_LAYOUT_HEAP_{_xi_c_ident(entry.heap_field)}, '
            f'{_c_string_or_null(entry.ref_tag)}){suffix}')
    lines.append('')
    lines.append('')
    lines.append('static inline const XaotLayoutInfo *xaot_layout_for_native_type(uint8_t native_type) {')
    lines.append('    static const XaotLayoutInfo table[] = {')
    for entry in entries:
        lines.append(f'        {{"{entry.name}", {entry.native_type},')
        lines.append(f'         XAOT_LAYOUT_FIELD_{_xi_c_ident(entry.field_kind)},')
        lines.append(f'         XAOT_REP_{_xi_c_ident(entry.rep)}, {_c_string_or_null(entry.c_type)},')
        lines.append(f'         XAOT_LAYOUT_HEAP_{_xi_c_ident(entry.heap_field)},')
        lines.append(f'         {_c_string_or_null(entry.ref_tag)}}},')
    lines.append('    };')
    lines.append('    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {')
    lines.append('        if (table[i].native_type == native_type)')
    lines.append('            return &table[i];')
    lines.append('    }')
    lines.append('    return NULL;')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xaot_layout_c_type_for_native_type(uint8_t native_type) {')
    lines.append('    const XaotLayoutInfo *info = xaot_layout_for_native_type(native_type);')
    lines.append('    return info ? info->c_type : NULL;')
    lines.append('}')
    lines.append('')
    lines.append('static inline XrRep xaot_layout_storage_rep_for_native_type(uint8_t native_type) {')
    lines.append('    const XaotLayoutInfo *layout = xaot_layout_for_native_type(native_type);')
    lines.append('    const XaotRepInfo *rep = layout ? xaot_rep_info(layout->rep) : NULL;')
    lines.append('    return rep ? rep->storage_rep : XR_REP_TAGGED;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xaot_layout_native_field_direct_heap_supported(uint8_t native_type) {')
    lines.append('    const XaotLayoutInfo *info = xaot_layout_for_native_type(native_type);')
    lines.append('    return info && info->heap_policy == XAOT_LAYOUT_HEAP_YES;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xaot_layout_native_field_uses_nested_layout(uint8_t native_type) {')
    lines.append('    const XaotLayoutInfo *info = xaot_layout_for_native_type(native_type);')
    lines.append('    return info && info->heap_policy == XAOT_LAYOUT_HEAP_NESTED;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xaot_layout_native_field_uses_elem_layout(uint8_t native_type) {')
    lines.append('    const XaotLayoutInfo *info = xaot_layout_for_native_type(native_type);')
    lines.append('    return info && info->heap_policy == XAOT_LAYOUT_HEAP_ELEM;')
    lines.append('}')
    lines.append('')
    lines.append('static inline const char *xaot_layout_ref_tag_name_for_native_type(uint8_t native_type) {')
    lines.append('    const XaotLayoutInfo *info = xaot_layout_for_native_type(native_type);')
    lines.append('    return info ? info->ref_tag : NULL;')
    lines.append('}')
    lines.append('')
    lines.append('#endif  /* XAOT_LAYOUT_GEN_H */')
    lines.append('')
    return '\n'.join(lines)

# ============================================================
# Command-line entry points
# ============================================================


def cmd_xi_ops(args: list[str]):
    if len(args) != 2:
        die("usage: xisagen.py xi-ops <ops.def> <output.h>")
    ops = parse_xi_ops_def(read_file(args[0]), args[0])
    if not ops:
        die(f"no Xi ops parsed from {args[0]}")
    content = generate_xi_ops_header(ops)
    write_file(args[1], content)
    print(f"xisagen: parsed {len(ops)} Xi ops from {args[0]}", file=sys.stderr)
    print(f"xisagen: generated {args[1]}", file=sys.stderr)

def cmd_xi_lowering(args: list[str]):
    if len(args) != 3:
        die("usage: xisagen.py xi-lowering <ops.def> <lowering.def> <output-root>")
    ops = parse_xi_ops_def(read_file(args[0]), args[0])
    entries = parse_xi_lowering_def(read_file(args[1]), ops, args[1])
    if not entries:
        die(f"no Xi lowering entries parsed from {args[1]}")
    outputs = write_xi_lowering_outputs(args[2], entries)
    print(f"xisagen: parsed {len(entries)} Xi lowering entries from {args[1]}", file=sys.stderr)
    for path in outputs:
        print(f"xisagen: generated {path}", file=sys.stderr)


def cmd_xi_verify(args: list[str]):
    if len(args) != 3:
        die("usage: xisagen.py xi-verify <ops.def> <verifier.def> <output.h>")
    ops = parse_xi_ops_def(read_file(args[0]), args[0])
    rules = parse_xi_verifier_def(read_file(args[1]), ops, args[1])
    if not rules:
        die(f"no Xi verifier rules parsed from {args[1]}")
    content = generate_xi_verify_header(rules)
    write_file(args[2], content)
    print(f"xisagen: parsed {len(rules)} Xi verifier rules from {args[1]}", file=sys.stderr)
    print(f"xisagen: generated {args[2]}", file=sys.stderr)


def cmd_aot_rep(args: list[str]):
    if len(args) != 2:
        die("usage: xisagen.py aot-rep <rep.def> <output.h>")
    reps = parse_aot_rep_def(read_file(args[0]), args[0])
    if not reps:
        die(f"no AOT reps parsed from {args[0]}")
    content = generate_aot_rep_header(reps)
    write_file(args[1], content)
    print(f"xisagen: parsed {len(reps)} AOT reps from {args[0]}", file=sys.stderr)
    print(f"xisagen: generated {args[1]}", file=sys.stderr)

def cmd_aot_abi(args: list[str]):
    if len(args) != 3:
        die("usage: xisagen.py aot-abi <rep.def> <abi.def> <output.h>")
    reps = parse_aot_rep_def(read_file(args[0]), args[0])
    entries = parse_aot_abi_def(read_file(args[1]), reps, args[1])
    if not entries:
        die(f"no AOT ABI entries parsed from {args[1]}")
    content = generate_aot_abi_header(entries)
    write_file(args[2], content)
    print(f"xisagen: parsed {len(entries)} AOT ABI entries from {args[1]}", file=sys.stderr)
    print(f"xisagen: generated {args[2]}", file=sys.stderr)

def cmd_aot_layout(args: list[str]):
    if len(args) != 3:
        die("usage: xisagen.py aot-layout <rep.def> <layout.def> <output.h>")
    reps = parse_aot_rep_def(read_file(args[0]), args[0])
    entries = parse_aot_layout_def(read_file(args[1]), reps, args[1])
    if not entries:
        die(f"no AOT layout entries parsed from {args[1]}")
    content = generate_aot_layout_header(entries)
    write_file(args[2], content)
    print(f"xisagen: parsed {len(entries)} AOT layout entries from {args[1]}", file=sys.stderr)
    print(f"xisagen: generated {args[2]}", file=sys.stderr)


def cmd_test(args: list[str]):
    """Run self-tests."""
    print("xisagen self-test:", file=sys.stderr)
    _test_sexpr_parser()
    _test_xi_ops_parser()
    _test_xi_lowering_parser()
    _test_xi_verifier_parser()
    _test_aot_rep_parser()
    _test_aot_abi_parser()
    _test_aot_layout_parser()
    _test_error_paths()
    print("All xisagen self-tests passed.", file=sys.stderr)

def _test_sexpr_parser():
    print("  test_sexpr_parser...", end='', file=sys.stderr)
    tokens = tokenize_sexpr('(add 1 2)')
    forms = parse_sexpr(tokens)
    assert len(forms) == 1
    assert isinstance(forms[0], SList)
    assert len(forms[0].children) == 3
    assert isinstance(forms[0].children[0], SAtom) and forms[0].children[0].value == 'add'
    assert forms[0].children[1].int_value == 1
    assert forms[0].children[2].int_value == 2

    # Keywords and variables
    tokens = tokenize_sexpr('(:key $var "str" 0xFF 0b1101)')
    forms = parse_sexpr(tokens)
    lst = forms[0]
    assert lst.children[0].is_keyword
    assert lst.children[1].is_variable
    assert lst.children[2].is_string
    assert lst.children[3].int_value == 255
    assert lst.children[4].int_value == 13

    # Comments
    tokens = tokenize_sexpr('foo ; comment\nbar')
    forms = parse_sexpr(tokens)
    assert len(forms) == 2

    # Nested
    tokens = tokenize_sexpr('(a (b c) d)')
    forms = parse_sexpr(tokens)
    assert len(forms[0].children) == 3
    assert isinstance(forms[0].children[1], SList)

    # Keyword lookup
    tokens = tokenize_sexpr('(foo :min-bytes 3 :name "test")')
    forms = parse_sexpr(tokens)
    lst = forms[0]
    assert lst.get_kw_int(':min-bytes') == 3
    assert lst.get_kw_str(':name') == 'test'

    print(" PASS", file=sys.stderr)

def _test_xi_ops_parser():
    print("  test_xi_ops_parser...", end='', file=sys.stderr)
    text = '''
    (define-xi-op xi.add
      :class pure
      :operands ((value $lhs :type int) (value $rhs :type int))
      :results ((value $result :type int))
      :own-use borrow
      :speculation safe
      :vn-kind pure
      :algebraic (associative commutative)
      :effects ()
      :requires (same-numeric-type)
      :observable (integer-wrap)
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.mem.load
      :class memory
      :operands ((address $addr :type pointer))
      :results ((value $result))
      :result-ownership borrowed
      :effects (memory-read may-throw)
      :tbaa-group array
      :requires (valid-address)
      :observable (load-width)
      :targets (aot-c aot-verify)
      :lowering-policy pass-local)
    (define-xi-op xi.narrow.i8
      :class conversion
      :arity 1
      :result-kind value
      :result-native-type i8
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.store.field
      :class memory-write
      :arity 2
      :result-kind void
      :result-ownership none
      :effects (side-effect memory-write)
      :tbaa-group field
      :escape-use heap
      :own-use stored-value
      :ic-site field
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.iter.new
      :class iterator
      :arity 1
      :effects (side-effect memory-read memory-write)
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-verify)
      :backend-rewrite (builtin "iter_new"))
    (define-xi-op xi.eq
      :class comparison
      :arity 2
      :speculation safe
      :vn-kind pure
      :algebraic (commutative)
      :negates-to xi.ne
      :own-use borrow
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.ne
      :class comparison
      :arity 2
      :speculation safe
      :vn-kind pure
      :algebraic (commutative)
      :negates-to xi.eq
      :own-use borrow
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.array.new
      :class allocation
      :arity variadic
      :escape-alloc heap
      :effects (side-effect memory-write allocates)
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    '''
    ops = parse_xi_ops_def(text)
    assert len(ops) == 8
    assert ops[0].name == 'xi.add'
    assert ops[0].ident == 'ADD'
    assert ops[0].arity == 2
    assert ops[0].operands[0].attrs['type'] == 'int'
    assert ops[0].speculation == 'safe'
    assert ops[0].vn_kind == 'pure'
    assert ops[0].algebraic == ['associative', 'commutative']
    assert ops[0].own_use == 'borrow'
    assert ops[1].ident == 'MEM_LOAD'
    assert ops[1].arity == 1
    assert ops[1].effects == ['memory-read', 'may-throw']
    assert ops[1].result_kind == 'value'
    assert ops[1].result_ownership == 'borrowed'
    assert ops[1].lowering_policy == 'pass-local'
    assert ops[1].tbaa_group == 'array'
    assert ops[2].result_native_type == 'i8'
    assert ops[3].result_kind == 'void'
    assert ops[3].result_ownership == 'none'
    assert ops[3].tbaa_group == 'field'
    assert ops[3].escape_use == 'heap'
    assert ops[3].own_use == 'stored-value'
    assert ops[3].ic_site == 'field'
    assert ops[4].backend_rewrite == 'builtin'
    assert ops[4].backend_rewrite_name == 'iter_new'
    assert ops[5].negated_op == 'xi.ne'
    assert ops[6].negated_op == 'xi.eq'
    assert ops[7].escape_alloc == 'heap'
    header = generate_xi_ops_header(ops)
    assert 'case XI_ADD: return "ADD";' in header
    assert 'case XI_MEM_LOAD: return 1;' in header
    assert 'XI_EFFECT_MAY_THROW' in header
    assert 'XI_TARGET_AOT_C' in header
    assert 'xi_generated_op_arity' in header
    assert 'xi_generated_op_class' in header
    assert 'case XI_MEM_LOAD: return XI_GEN_CLASS_MEMORY;' in header
    assert 'xi_generated_op_result_kind' in header
    assert 'case XI_STORE_FIELD: return XI_GEN_RESULT_VOID;' in header
    assert 'xi_generated_op_result_ownership' in header
    assert 'case XI_MEM_LOAD: return XI_GEN_RESULT_OWNERSHIP_BORROWED;' in header
    assert 'case XI_STORE_FIELD: return XI_GEN_RESULT_OWNERSHIP_NONE;' in header
    assert 'xi_generated_op_result_native_type' in header
    assert 'case XI_NARROW_I8: return "i8";' in header
    assert 'xi_generated_op_lowering_policy' in header
    assert 'case XI_MEM_LOAD: return XI_GEN_LOWERING_PASS_LOCAL;' in header
    assert 'xi_generated_op_speculation' in header
    assert 'case XI_ADD: return XI_GEN_SPECULATION_SAFE;' in header
    assert 'case XI_MEM_LOAD: return XI_GEN_SPECULATION_NEVER;' in header
    assert 'xi_generated_op_value_numbering' in header
    assert 'case XI_ADD: return XI_GEN_VN_PURE;' in header
    assert 'case XI_MEM_LOAD: return XI_GEN_VN_NONE;' in header
    assert 'xi_generated_op_tbaa_group' in header
    assert 'case XI_MEM_LOAD: return XI_GEN_TBAA_ARRAY;' in header
    assert 'xi_generated_op_backend_rewrite' in header
    assert 'case XI_ITER_NEW: return XI_GEN_BACKEND_REWRITE_BUILTIN;' in header
    assert 'xi_generated_op_backend_rewrite_name' in header
    assert 'case XI_ITER_NEW: return "iter_new";' in header
    assert 'xi_generated_op_backend_legal' in header
    assert 'xi_generated_op_escape_use' in header
    assert 'case XI_STORE_FIELD: return XI_GEN_ESCAPE_USE_HEAP;' in header
    assert 'xi_generated_op_escape_alloc' in header
    assert 'case XI_ARRAY_NEW: return XI_GEN_ESCAPE_ALLOC_HEAP;' in header
    assert 'xi_generated_op_own_use' in header
    assert 'case XI_ADD: return XI_GEN_OWN_USE_BORROW;' in header
    assert 'case XI_STORE_FIELD: return XI_GEN_OWN_USE_STORED_VALUE;' in header
    assert 'xi_generated_op_ic_site' in header
    assert 'case XI_STORE_FIELD: return XI_GEN_IC_SITE_FIELD;' in header
    assert 'xi_generated_op_negates_to' in header
    assert 'case XI_EQ: return XI_NE;' in header
    assert 'xi_generated_op_algebraic_traits' in header
    assert 'case XI_ADD: return XI_GEN_ALGEBRAIC_ASSOCIATIVE | XI_GEN_ALGEBRAIC_COMMUTATIVE;' in header
    assert 'xi_generated_op_default_flags' in header
    assert 'xi_generated_op_effects' in header
    assert 'case XI_MEM_LOAD: return XI_EFFECT_MEMORY_READ | XI_EFFECT_MAY_THROW;' in header
    print(" PASS", file=sys.stderr)

def _test_xi_lowering_parser():
    print("  test_xi_lowering_parser...", end='', file=sys.stderr)
    ops_text = '''
    (define-xi-op xi.add
      :class arithmetic
      :arity 2
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.copy
      :class pure
      :arity 1
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.phi
      :class pure
      :arity variadic
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify)
      :lowering-policy special)
    (define-xi-op xi.extract
      :class call
      :arity 1
      :effects ()
      :requires ()
      :observable ()
      :targets (aot-verify)
      :lowering-policy verifier-only)
    (define-xi-op xi.vec.add
      :class vector
      :arity 2
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify)
      :lowering-policy pass-local)
    '''
    lowering_text = '''
    (lower xi.add
      :match (value-binary)
      :required-targets (vm-bytecode aot-c)
      :vm-bytecode (driver xi_emit_add :fresh-dst yes :raw-cell-args yes :handles-cell-dst yes)
      :aot-c (driver xicgen_add))
    (lower xi.copy
      :match ()
      :required-targets (vm-bytecode aot-c aot-c-stmt)
      :vm-bytecode (driver xi_emit_copy)
      :aot-c (driver xicgen_copy)
      :aot-c-stmt (driver xicgen_stmt_copy))
    '''
    ops = parse_xi_ops_def(ops_text)
    entries = parse_xi_lowering_def(lowering_text, ops)
    assert len(entries) == 2
    assert entries[0].ident == 'ADD'
    assert entries[0].targets == ['aot-c', 'vm-bytecode']
    assert entries[0].template == 'value-binary'
    assert entries[0].target_attrs['vm-bytecode']['fresh-dst'] is True
    assert entries[0].target_attrs['vm-bytecode']['raw-cell-args'] is True
    assert entries[0].target_attrs['vm-bytecode']['handles-cell-dst'] is True
    assert entries[1].target_drivers['aot-c-stmt'] == 'xicgen_stmt_copy'
    assert entries[1].template == 'custom'
    header = generate_xi_lowering_coverage_header(entries)
    assert 'XI_LOWERING_ENTRY_COUNT = 2' in header
    assert 'XI_LOWERING_PATTERNED_ENTRY_COUNT = 1' in header
    assert 'XI_LOWERING_MAIN_BACKEND_PATTERNED_ENTRY_COUNT = 1' in header
    assert 'case XI_ADD: return XI_LOWER_TEMPLATE_VALUE_BINARY;' in header
    assert 'case XI_ADD: return XI_LOWER_TARGET_AOT_C | XI_LOWER_TARGET_VM_BYTECODE;' in header
    vm_header = generate_xi_vm_dispatch_header(entries)
    assert 'X(ADD, xi_emit_add)' in vm_header
    assert 'case XI_ADD: return OP_ADD;' in vm_header
    assert 'xi_emit_vm_template_swaps_args' in vm_header
    assert 'case XI_ADD: return true;' in vm_header
    assert 'xi_emit_vm_uses_raw_cell_args' in vm_header
    assert 'xi_emit_vm_handles_cell_dst' in vm_header
    vm_bitwise = generate_xi_vm_template_bitwise_binary_dispatch([
        XiLoweringDef('xi.band', 'BAND', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-binary'),
        XiLoweringDef('xi.bxor', 'BXOR', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-binary'),
    ])
    assert 'XVM_TEMPLATE_BITWISE_BINARY_BOOL_CASE(OP_BAND, &, &&' in vm_bitwise
    assert 'XVM_TEMPLATE_BITWISE_BINARY_CASE(OP_BXOR, ^' in vm_bitwise
    vm_bitwise_unary = generate_xi_vm_template_bitwise_unary_dispatch([
        XiLoweringDef('xi.bnot', 'BNOT', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_bnot'}, {}, {},
                      template='value-unary'),
    ])
    assert 'XVM_TEMPLATE_BITWISE_UNARY_CASE(OP_BNOT, ~, SYMBOL_OP_BNOT' in vm_bitwise_unary
    vm_unary = generate_xi_vm_template_unary_dispatch([
        XiLoweringDef('xi.neg', 'NEG', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-unary'),
        XiLoweringDef('xi.not', 'NOT', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-unary'),
    ])
    assert 'XVM_TEMPLATE_UNARY_NEG_CASE(OP_UNM, "-")' in vm_unary
    assert 'XVM_TEMPLATE_UNARY_NOT_CASE(OP_NOT, "!")' in vm_unary
    vm_arith_binary = generate_xi_vm_template_arith_binary_dispatch([
        XiLoweringDef('xi.add', 'ADD', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-binary'),
        XiLoweringDef('xi.sub', 'SUB', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-binary'),
        XiLoweringDef('xi.mul', 'MUL', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-binary'),
        XiLoweringDef('xi.div', 'DIV', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-binary'),
        XiLoweringDef('xi.mod', 'MOD', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-binary'),
    ])
    assert 'XVM_TEMPLATE_ARITH_ADD_CASE(OP_ADD, +, +' in vm_arith_binary
    assert 'XVM_TEMPLATE_ARITH_NUMERIC_CASE(OP_SUB, -, -' in vm_arith_binary
    assert 'XVM_TEMPLATE_ARITH_MUL_CASE(OP_MUL, *, *' in vm_arith_binary
    assert 'XVM_TEMPLATE_ARITH_DIV_CASE(OP_DIV, xr_bigint_div' in vm_arith_binary
    assert 'XVM_TEMPLATE_ARITH_MOD_CASE(OP_MOD, xr_bigint_mod' in vm_arith_binary
    vm_shift = generate_xi_vm_template_shift_dispatch([
        XiLoweringDef('xi.shl', 'SHL', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-binary'),
        XiLoweringDef('xi.shr', 'SHR', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-binary'),
    ])
    assert 'XVM_TEMPLATE_SHIFT_CASE(OP_SHL, xr_int_shl_wrap, xr_bigint_shl)' in vm_shift
    assert 'XVM_TEMPLATE_SHIFT_CASE(OP_SHR, xr_int_shr_wrap, xr_bigint_shr)' in vm_shift
    vm_compare = generate_xi_vm_template_compare_dispatch([
        XiLoweringDef('xi.eq', 'EQ', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_cmp'}, {}, {},
                      template='compare'),
        XiLoweringDef('xi.ne', 'NE', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_cmp'}, {}, {},
                      template='compare'),
        XiLoweringDef('xi.lt', 'LT', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_cmp'}, {}, {},
                      template='compare'),
        XiLoweringDef('xi.gt', 'GT', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_cmp'}, {}, {},
                      template='compare'),
    ])
    assert 'XVM_TEMPLATE_COMPARE_DEEP_CASE(OP_CMP_EQ, false, XR_OP_EQ_FLAG' in vm_compare
    assert 'XVM_TEMPLATE_COMPARE_DEEP_CASE(OP_CMP_NE, true, XR_OP_NE_FLAG' in vm_compare
    assert 'XVM_TEMPLATE_COMPARE_ORDER_CASE(OP_CMP_LT, XR_OP_LT_FLAG' in vm_compare
    assert vm_compare.count('OP_CMP_LT') == 1
    lowering_test = generate_xi_lowering_test(entries)
    assert 'xi_emit_vm_requires_fresh_dst(XI_ADD) == true' in lowering_test
    assert 'xi_emit_vm_requires_fresh_dst(XI_COPY) == false' in lowering_test
    assert 'xi_lowering_template_kind(XI_ADD) == XI_LOWER_TEMPLATE_VALUE_BINARY' in lowering_test
    assert 'xi_emit_vm_uses_raw_cell_args(XI_ADD) == true' in lowering_test
    assert 'xi_emit_vm_handles_cell_dst(XI_COPY) == false' in lowering_test
    aot_header = generate_xi_to_c_dispatch_header(entries)
    assert 'X(ADD, "xi.add", xicgen_add)' in aot_header
    assert 'XI_TO_C_TEMPLATE_ARITH_DRIVERS' in aot_header
    assert 'case XI_ADD: return "xrt_add";' in aot_header
    assert 'xi_to_c_template_arith_native_op' in aot_header
    assert 'XI_TO_C_TEMPLATE_WIDTH_DRIVERS' in aot_header
    assert 'XiToCWidthTemplateKind' in aot_header
    assert 'xi_to_c_template_width_kind' in aot_header
    assert 'xi_to_c_template_width_cast_type' in aot_header
    stmt_header = generate_xi_target_dispatch_header(entries, 'aot-c-stmt', 'TEST_STMT_H',
                                                     'TEST_STMT')
    assert 'X(COPY, "xi.copy", xicgen_stmt_copy)' in stmt_header
    try:
        parse_xi_lowering_def('''
        (lower xi.add
          :match (bogus-template)
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (driver xi_emit_add)
          :aot-c (driver xicgen_add))
        (lower xi.copy
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (driver xi_emit_copy)
          :aot-c (driver xicgen_copy))
        ''', ops)
        assert False, "unknown lowering template should be rejected"
    except SystemExit:
        pass
    try:
        parse_xi_lowering_def('''
        (lower xi.add
          :required-targets (vm-bytecode)
          :vm-bytecode (driver xi_emit_add))
        ''', ops)
        assert False, "missing required lowering target should be rejected"
    except SystemExit:
        pass
    try:
        parse_xi_lowering_def('''
        (lower xi.add
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (driver xi_emit_add :fresh-dst maybe)
          :aot-c (driver xicgen_add))
        (lower xi.copy
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (driver xi_emit_copy)
          :aot-c (driver xicgen_copy))
        ''', ops)
        assert False, "invalid target bool attribute should be rejected"
    except SystemExit:
        pass
    try:
        parse_xi_lowering_def('''
        (lower xi.add
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (driver xi_emit_add :window yes)
          :aot-c (driver xicgen_add))
        (lower xi.copy
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (driver xi_emit_copy)
          :aot-c (driver xicgen_copy))
        ''', ops)
        assert False, "unknown target attribute should be rejected"
    except SystemExit:
        pass
    try:
        parse_xi_lowering_def('''
        (lower xi.add
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (reject xi_emit_add_reject :fresh-dst yes)
          :aot-c (driver xicgen_add))
        (lower xi.copy
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (driver xi_emit_copy)
          :aot-c (driver xicgen_copy))
        ''', ops)
        assert False, "reject target cannot require fresh-dst"
    except SystemExit:
        pass
    try:
        parse_xi_lowering_def('''
        (lower xi.add
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (driver xi_emit_add)
          :aot-c (driver xicgen_add))
        ''', ops)
        assert False, "missing generated lowering entry should be rejected"
    except SystemExit:
        pass
    try:
        parse_xi_lowering_def('''
        (lower xi.add
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (driver xi_emit_add)
          :aot-c (driver xicgen_add))
        (lower xi.copy
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (driver xi_emit_copy)
          :aot-c (driver xicgen_copy))
        (lower xi.phi
          :required-targets (vm-bytecode)
          :vm-bytecode (driver xi_emit_phi))
        ''', ops)
        assert False, "special lowering policy should reject normal lowering entries"
    except SystemExit:
        pass
    mismatch_ops_text = '''
    (define-xi-op xi.add
      :class arithmetic
      :arity 2
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-verify))
    '''
    try:
        parse_xi_lowering_def('''
        (lower xi.add
          :required-targets (vm-bytecode aot-c)
          :vm-bytecode (driver xi_emit_add)
          :aot-c (driver xicgen_add))
        ''', parse_xi_ops_def(mismatch_ops_text))
        assert False, "lowering target mismatch should be rejected"
    except SystemExit:
        pass
    print(" PASS", file=sys.stderr)


def _test_xi_verifier_parser():
    print("  test_xi_verifier_parser...", end='', file=sys.stderr)
    ops_text = '''
    (define-xi-op xi.eq
      :class comparison
      :arity 2
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.select
      :class pure
      :arity 3
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.extract
      :class call
      :arity 1
      :effects ()
      :requires ()
      :observable ()
      :targets (aot-verify)
      :lowering-policy verifier-only)
    '''
    verifier_text = '''
    (verify-xi-op xi.eq
      :checks (bool-result))
    (verify-xi-op xi.select
      :checks (select-contract))
    (verify-xi-op xi.extract
      :checks (obsolete))
    '''
    ops = parse_xi_ops_def(ops_text)
    rules = parse_xi_verifier_def(verifier_text, ops)
    assert len(rules) == 3
    assert rules[0].ident == 'EQ'
    assert rules[0].checks == ['bool-result']
    header = generate_xi_verify_header(rules)
    assert 'XI_VERIFY_RULE_COUNT = 3' in header
    assert 'case XI_EQ: return XI_VERIFY_CHECK_BOOL_RESULT;' in header
    assert 'case XI_SELECT: return XI_VERIFY_CHECK_SELECT_CONTRACT;' in header
    assert 'xi_verify_generated_op_has_check' in header
    try:
        parse_xi_verifier_def('''
        (verify-xi-op xi.missing
          :checks (bool-result))
        ''', ops)
        assert False, "unknown verifier op should be rejected"
    except SystemExit:
        pass
    try:
        parse_xi_verifier_def('''
        (verify-xi-op xi.eq
          :checks (shape-shift))
        ''', ops)
        assert False, "unknown verifier check should be rejected"
    except SystemExit:
        pass
    try:
        parse_xi_verifier_def('''
        (verify-xi-op xi.eq
          :checks (bool-result))
        (verify-xi-op xi.eq
          :checks (obsolete))
        ''', ops)
        assert False, "duplicate verifier rule should be rejected"
    except SystemExit:
        pass
    print(" PASS", file=sys.stderr)


def _test_aot_rep_parser():
    print("  test_aot_rep_parser...", end='', file=sys.stderr)
    text = '''
    (define-aot-rep i8
      :c-type "int8_t"
      :size 1
      :align 1
      :signed yes
      :integer yes
      :boxed no
      :dynamic-kind scalar
      :native-type XR_NATIVE_I8
      :storage-rep XR_REP_I64)
    (define-aot-rep tagged
      :c-type "XrValue"
      :size 16
      :align 8
      :signed no
      :integer no
      :boxed yes
      :dynamic-kind tagged
      :native-type none
      :storage-rep XR_REP_TAGGED)
    '''
    reps = parse_aot_rep_def(text)
    assert len(reps) == 2
    assert reps[0].ident == 'I8'
    assert reps[0].c_type == 'int8_t'
    assert reps[0].integer
    header = generate_aot_rep_header(reps)
    assert 'XAOT_REP_I8' in header
    assert 'xaot_c_type_for_native_int_type' in header
    assert 'xaot_elem_name_for_native_type' in header
    assert 'case XR_NATIVE_I8:' in header
    try:
        parse_aot_rep_def('''
        (define-aot-rep bad
          :size 1
          :align 1
          :signed yes
          :integer yes
          :boxed no
          :dynamic-kind scalar
          :native-type XR_NATIVE_I8
          :storage-rep XR_REP_I64)
        ''')
        assert False, "missing c-type should be rejected"
    except SystemExit:
        pass
    print(" PASS", file=sys.stderr)

def _test_aot_abi_parser():
    print("  test_aot_abi_parser...", end='', file=sys.stderr)
    reps = parse_aot_rep_def('''
    (define-aot-rep i64
      :c-type "int64_t" :size 8 :align 8 :signed yes :integer yes
      :boxed no :dynamic-kind scalar :native-type XR_NATIVE_I64 :storage-rep XR_REP_I64)
    (define-aot-rep tagged
      :c-type "XrValue" :size 16 :align 8 :signed no :integer no
      :boxed yes :dynamic-kind tagged :native-type none :storage-rep XR_REP_TAGGED)
    ''')
    text = '''
    (define-aot-abi int
      :type-kind XR_KIND_INT
      :nullable no
      :abi-class scalar
      :default-rep i64
      :native-width yes
      :typed-boundary yes)
    '''
    entries = parse_aot_abi_def(text, reps)
    assert len(entries) == 1
    assert entries[0].ident == 'INT'
    assert entries[0].native_width
    header = generate_aot_abi_header(entries)
    assert 'xaot_abi_storage_rep_for_type' in header
    assert 'XAOT_REP_I64' in header
    try:
        parse_aot_abi_def('''
        (define-aot-abi bad
          :type-kind XR_KIND_INT
          :nullable no
          :abi-class scalar
          :default-rep missing
          :native-width no
          :typed-boundary yes)
        ''', reps)
        assert False, "unknown ABI rep should be rejected"
    except SystemExit:
        pass
    print(" PASS", file=sys.stderr)

def _test_aot_layout_parser():
    print("  test_aot_layout_parser...", end='', file=sys.stderr)
    reps = parse_aot_rep_def('''
    (define-aot-rep i64
      :c-type "int64_t" :size 8 :align 8 :signed yes :integer yes
      :boxed no :dynamic-kind scalar :native-type XR_NATIVE_I64 :storage-rep XR_REP_I64)
    (define-aot-rep ptr
      :c-type "void *" :size 8 :align 8 :signed no :integer no
      :boxed no :dynamic-kind pointer :native-type none :storage-rep XR_REP_PTR)
    ''')
    text = '''
    (define-aot-layout i64
      :native-type XR_NATIVE_I64
      :field-kind scalar
      :rep i64
      :c-type "int64_t"
      :heap-field yes
      :ref-tag none)
    (define-aot-layout array-ref
      :native-type XR_NATIVE_ARRAY_REF
      :field-kind pointer-ref
      :rep ptr
      :c-type "xrt_array_t *"
      :heap-field yes
      :ref-tag XR_TAG_ARRAY)
    '''
    entries = parse_aot_layout_def(text, reps)
    assert len(entries) == 2
    assert entries[1].ref_tag == 'XR_TAG_ARRAY'
    header = generate_aot_layout_header(entries)
    assert 'xaot_layout_c_type_for_native_type' in header
    assert 'xaot_layout_ref_tag_name_for_native_type' in header
    try:
        parse_aot_layout_def('''
        (define-aot-layout bad
          :native-type XR_NATIVE_I64
          :field-kind scalar
          :rep missing
          :c-type "int64_t"
          :heap-field yes
          :ref-tag none)
        ''', reps)
        assert False, "unknown layout rep should be rejected"
    except SystemExit:
        pass
    print(" PASS", file=sys.stderr)


def _test_error_paths():
    print("  test_error_paths...", end='', file=sys.stderr)

    # Unterminated string
    try:
        tokenize_sexpr('"hello', 'test')
        assert False, "should have died"
    except SystemExit:
        pass

    # Unclosed paren
    try:
        tokens = tokenize_sexpr('(a b', 'test')
        parse_sexpr(tokens, 'test')
        assert False, "should have died"
    except SystemExit:
        pass

    # Unexpected )
    try:
        tokens = tokenize_sexpr(')', 'test')
        parse_sexpr(tokens, 'test')
        assert False, "should have died"
    except SystemExit:
        pass

    # Unexpected character
    try:
        tokenize_sexpr('`', 'test')
        assert False, "should have died"
    except SystemExit:
        pass

    print(" PASS", file=sys.stderr)


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        sys.exit(1)

    cmd = sys.argv[1]
    args = sys.argv[2:]

    commands = {
        'xi-ops': cmd_xi_ops,
        'xi-lowering': cmd_xi_lowering,
        'xi-verify': cmd_xi_verify,
        'aot-rep': cmd_aot_rep,
        'aot-abi': cmd_aot_abi,
        'aot-layout': cmd_aot_layout,
        'test': cmd_test,
    }

    if cmd not in commands:
        die(f"unknown command '{cmd}'. Available: {', '.join(commands)}")

    commands[cmd](args)


if __name__ == '__main__':
    main()
