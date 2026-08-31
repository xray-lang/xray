#!/usr/bin/env python3
"""
xisagen — XISA code generator (Python rewrite)

Reads declarative .def files, generates C headers for Xi IR and AOT metadata:
  - xi-ops:  xisa/xi/ops.def     → xi_ops_gen.h
  - semantic-ops: xisa/xi/ops.def → semantic owner registry artifacts
  - xi-lowering: xisa/xi/lowering.def → Xi target lowering headers and tests
  - xi-verify: xisa/xi/verifier.def → xi_verify_gen.h
  - aot-rep: xisa/aot/rep.def    → xaot_rep_gen.h
  - aot-abi: xisa/aot/abi.def    → xaot_abi_gen.h
  - aot-layout: xisa/aot/layout.def → xaot_layout_gen.h
  - aot-c-emission-rules: xisa/aot/c_emission_rules.def → independent evaluators
  - target-vm-ops: xisa/target/vm_ops.def → target contract and dispatch

Usage:
  python3 xisagen.py xi-ops  <ops.def>     <output.h>
  <python-3.11+> xisagen.py semantic-ops <ops.def> <output-root>
  python3 xisagen.py xi-lowering <ops.def> <lowering.def> <output-root>
  python3 xisagen.py xi-lowering-check <ops.def> <lowering.def> <output-root> <validation-stamp>
  python3 xisagen.py xi-lowering-validate <ops.def> <lowering.def> <stamp> <depfile>
  python3 xisagen.py xi-verify <ops.def> <verifier.def> <output.h>
  python3 xisagen.py aot-rep <rep.def>     <output.h>
  python3 xisagen.py aot-abi <rep.def> <abi.def> <output.h>
  python3 xisagen.py aot-layout <rep.def> <layout.def> <output.h>
  python3 xisagen.py aot-c-emission-rules <rules.def> <output-root>
  python3 xisagen.py target-vm-ops <vm_ops.def> <output-root>
  python3 xisagen.py test
"""

from __future__ import annotations

import sys
import re
import os
import hashlib
import json
import tempfile
import functools
import stat
import errno
import subprocess
import time
import shlex
import shutil
import contextlib
import io
from collections import Counter
from dataclasses import dataclass, field, replace
from pathlib import Path

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

def write_file_if_changed(path: str, content: str) -> bool:
    """Publish generated text only when its canonical content changed."""
    try:
        with open(path, 'r', encoding='utf-8') as stream:
            if stream.read() == content:
                return False
    except FileNotFoundError:
        pass
    except OSError as error:
        die(f"cannot compare generated output {path}: {error}")
    write_file(path, content)
    return True

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
    def head(self) -> SExpr | None:
        return self.children[0] if self.children else None

    def get_kw(self, keyword: str) -> SExpr | None:
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

    def get_kw_list(self, keyword: str) -> SList | None:
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

    def parse_one() -> SExpr | None:
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

# TBAA groups partition the memory an op may touch.
#
#   'none'  -- the op touches no memory at all.  An op declaring a
#              memory-read/memory-write effect may NOT use it (see the
#              memory-scope rule in parse_xi_ops_def): conflating "no memory"
#              with "memory I cannot classify" makes such an op invisible to
#              xi_tbaa_may_alias, which is unsound rather than conservative.
#   'fresh' -- the op writes only into storage it allocates itself, which no
#              other value can reach yet.  Aliases nothing, by construction.
#   'top'   -- the op touches memory outside the lattice.  Aliases everything.
#   others  -- a concrete disjoint region.
VALID_XI_TBAA_GROUPS = {
    'array',
    'chan',
    'const',
    'field',
    'fresh',
    'global',
    'object',
    'none',
    'shared',
    'struct',
    'tls',
    'top',
    'tuple',
    'upval',
}

# Language-level synchronisation edge established by an op (spec §16.9.2).
# This is the *memory model* dimension, distinct from TBAA: it says which
# happens-before edge the op creates, which in turn tells the optimiser which
# direction ordinary memory operations must not move across it.
#
#   'none'     -- establishes no happens-before edge.
#   'acquire'  -- later memory operations must not move above this op.
#   'release'  -- earlier memory operations must not move below this op.
#   'acq-rel'  -- both.
#   'seq-cst'  -- both, and participates in a single total order.
#
# Ops whose ordering is chosen at runtime by an `Ordering` argument declare the
# strongest edge they can carry; this is the fail-closed upper bound.
VALID_XI_SYNC_ORDERS = {
    'none',
    'acquire',
    'release',
    'acq-rel',
    'seq-cst',
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
    'pass',
    'stored-value',
}

VALID_XI_IC_SITES = {
    'field',
    'method',
    'none',
}

VALID_XI_SEMANTIC_OWNERS = {
    'capability-provider',
    'declarative-primitive',
    'generated-specialization',
    'shared-semantic-kernel',
}

XI_SEMANTIC_CONSUMERS = {
    'semantic-plan': 1 << 0,
    'vm': 1 << 1,
    'aot-hosted': 1 << 2,
    'aot-freestanding': 1 << 3,
    'cgen': 1 << 4,
    'runtime': 1 << 5,
}

DEFAULT_SEMANTIC_PLAN_BINDING = (
    'src/plan/semantic/xr_semantic_ops.c',
    'xr_semantic_op_contract',
)


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
    observable_contract: str
    result_kind: str
    result_ownership: str
    result_native_type: str
    lowering_policy: str
    speculation: str
    vn_kind: str
    algebraic: list
    tbaa_group: str
    sync_order: str
    backend_rewrite: str
    backend_rewrite_name: str | None
    escape_use: str
    escape_alloc: str
    own_use: str
    ic_site: str
    negated_op: str | None


@dataclass(frozen=True)
class XiProductionBinding:
    consumer: str
    path: str
    symbol: str


@dataclass(frozen=True)
class XiObservableOwnerDef:
    name: str
    category: str
    operations: tuple[str, ...]
    consumers: tuple[str, ...]
    cgen_adapter: str
    production_bindings: tuple[XiProductionBinding, ...]


@dataclass(frozen=True)
class XiObservableOperation:
    operation: str
    category: str
    owner: str
    operation_id_hi: int
    operation_id_lo: int
    owner_id_hi: int
    owner_id_lo: int
    observable_contract: str
    consumers: tuple[str, ...]
    consumer_bits: int
    cgen_adapter: str
    production_bindings: tuple[XiProductionBinding, ...]


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


def _xi_get_kw(form: SList, keyword: str) -> SExpr | None:
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


def _xi_get_kw_list(form: SList, keyword: str) -> SList | None:
    value = _xi_get_kw(form, keyword)
    if value is None:
        return None
    if not isinstance(value, SList):
        die(f"{keyword}: expected list")
    return value


def _xi_parse_atom_list(expr: SList | None, context: str) -> list:
    if expr is None:
        return []
    values = []
    for child in expr.children:
        values.append(_sexpr_atom_value(child, context))
    return values


def _xi_parse_arity(expr: SExpr | None, default: int, context: str) -> int:
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


def _xi_parse_value_defs(expr: SList | None, context: str, result: bool) -> list:
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


def _xi_parse_backend_rewrite(expr: SList | None,
                              context: str) -> tuple[str, str | None]:
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
        if head in {'define-xi-semantic-owner', 'define-xi-observable-owner'}:
            continue
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
        observable_contract = _xi_get_kw_str(
            form, ':observable-contract', 'contracts/xi-canonical-ops.md')
        contract_path = Path(observable_contract)
        if (not observable_contract or '\\' in observable_contract or contract_path.is_absolute()
                or '..' in contract_path.parts):
            die(f"{path}: Xi op '{name}' has invalid observable contract "
                f"'{observable_contract}'")
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
        # Memory-scope rule (fail-closed): an op that reads or writes memory
        # must say which memory.  'none' means "touches no memory"; using it
        # for unclassified memory makes the op invisible to alias analysis,
        # so stores through it stop killing loads and the op silently stops
        # being a barrier.  Declare a concrete group, 'fresh' (writes only
        # into storage this op allocates), or 'top' (unknown memory).
        touches_memory = bool({'memory-read', 'memory-write'} & set(effects))
        if touches_memory and tbaa_group == 'none':
            die(f"{path}: Xi op '{name}' declares a memory effect but no "
                f":tbaa-group; use a concrete group, 'fresh' (writes only "
                f"into its own fresh allocation) or 'top' (unclassified "
                f"memory). 'none' means 'touches no memory' and would make "
                f"this op invisible to alias analysis")
        if not touches_memory and tbaa_group != 'none':
            die(f"{path}: Xi op '{name}' declares :tbaa-group "
                f"'{tbaa_group}' without any memory effect; a TBAA group "
                f"only classifies memory an op actually touches")
        if tbaa_group == 'fresh' and 'memory-read' in effects:
            die(f"{path}: Xi op '{name}' cannot use :tbaa-group fresh with a "
                f"memory-read effect; 'fresh' claims the op only writes "
                f"storage nothing else can reach, so it has nothing to read")
        sync_order = _xi_get_kw_str(form, ':sync', 'none')
        if sync_order not in VALID_XI_SYNC_ORDERS:
            die(f"{path}: Xi op '{name}' uses unknown sync order "
                f"'{sync_order}'")
        if sync_order != 'none' and not (touches_memory or
                                         {'side-effect', 'may-suspend'} & set(effects)):
            die(f"{path}: Xi op '{name}' declares :sync '{sync_order}' but "
                f"has no memory, side-effect or suspension effect; a "
                f"synchronisation edge needs an observable operation to "
                f"attach to")
        # A barrier that can be speculated or value-numbered is not a barrier:
        # duplicating it invents an edge and eliminating it removes one. Both
        # columns must pin the op in place, and both are checked here rather
        # than per-value in the verifier because they are facts about the op.
        if sync_order != 'none' and speculation != 'never':
            die(f"{path}: Xi op '{name}' declares :sync '{sync_order}' with "
                f":speculation '{speculation}'; a synchronisation edge must "
                f"not be speculatable, since executing it on a path that "
                f"would not have reached it invents an ordering edge")
        if sync_order != 'none' and vn_kind != 'none':
            die(f"{path}: Xi op '{name}' declares :sync '{sync_order}' with "
                f":vn-kind '{vn_kind}'; a synchronisation edge must not be "
                f"value-numbered, since CSE-ing two of them removes an edge")
        escape_use = _xi_get_kw_str(form, ':escape-use', 'none')
        if escape_use not in VALID_XI_ESCAPE_USES:
            die(f"{path}: Xi op '{name}' uses unknown escape-use "
                f"'{escape_use}'")
        escape_alloc = _xi_get_kw_str(form, ':escape-alloc', 'none')
        if escape_alloc not in VALID_XI_ESCAPE_ALLOCS:
            die(f"{path}: Xi op '{name}' uses unknown escape-alloc "
                f"'{escape_alloc}'")
        # Task 219 C5 (fail-closed operand ownership): every Xi op MUST declare
        # its operand-ownership column explicitly. There is no default guess —
        # an undeclared op is a compile error. This structurally eliminates the
        # incident-5 class (a receiver silently defaulting to `consume`).
        if _xi_get_kw(form, ':own-use') is None:
            die(f"{path}: Xi op '{name}' is missing required :own-use "
                f"(one of {sorted(VALID_XI_OWN_USES)}); ownership must be "
                f"declared explicitly (task 219 C5, no default)")
        own_use = _xi_get_kw_str(form, ':own-use')
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
                           observable_contract=observable_contract,
                           result_kind=result_kind,
                           result_ownership=result_ownership,
                           result_native_type=result_native_type,
                           lowering_policy=lowering_policy,
                           speculation=speculation,
                           vn_kind=vn_kind,
                           algebraic=algebraic,
                           tbaa_group=tbaa_group,
                           sync_order=sync_order,
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


def parse_xi_semantic_owners(text: str, ops: list[XiOpDef],
                             path: str = '<input>') -> dict[str, str]:
    forms = parse_sexpr(tokenize_sexpr(text, path), path)
    op_names = {op.name for op in ops}
    owners: dict[str, str] = {}
    category_count = {category: 0 for category in VALID_XI_SEMANTIC_OWNERS}
    for form in forms:
        if not isinstance(form, SList) or not form.children:
            die(f"{path}: top-level form must be a list")
        head = _sexpr_atom_value(form.children[0], path)
        if head != 'define-xi-semantic-owner':
            continue
        if len(form.children) < 2:
            die(f"{path}:{form.line}:{form.col}: missing semantic owner category")
        category = _sexpr_atom_value(form.children[1], 'define-xi-semantic-owner')
        if category not in VALID_XI_SEMANTIC_OWNERS:
            die(f"{path}: unknown Xi semantic owner category '{category}'")
        category_count[category] += 1
        if category_count[category] != 1:
            die(f"{path}: duplicate Xi semantic owner category '{category}'")
        operation_list = _xi_get_kw_list(form, ':operations')
        if operation_list is None:
            die(f"{path}: semantic owner category '{category}' is missing :operations")
        for operation in _xi_parse_atom_list(operation_list, f"{category}:operations"):
            if operation not in op_names:
                die(f"{path}: semantic owner category '{category}' names unknown op "
                    f"'{operation}'")
            if operation in owners:
                die(f"{path}: Xi op '{operation}' has multiple semantic owners")
            owners[operation] = category
    missing_categories = sorted(category for category, count in category_count.items()
                                if count == 0)
    if missing_categories:
        die(f"{path}: missing Xi semantic owner categories: "
            f"{', '.join(missing_categories)}")
    missing_ops = sorted(op_names - owners.keys())
    if missing_ops:
        die(f"{path}: Xi ops have no semantic owner: {', '.join(missing_ops)}")
    return owners


def _semantic_stable_id(name: str, hasher=None) -> tuple[int, int]:
    if hasher is None:
        digest = hashlib.sha256(b'xray-semantic-id-v1\0' + name.encode('utf-8')).digest()[:16]
    else:
        digest = hasher(name)
    if not isinstance(digest, bytes) or len(digest) != 16:
        die(f"stable ID hasher returned an invalid digest for '{name}'")
    return int.from_bytes(digest[:8], 'big'), int.from_bytes(digest[8:], 'big')


def _parse_production_bindings(expr: SList | None, consumers: tuple[str, ...],
                               context: str) -> tuple[XiProductionBinding, ...]:
    if expr is None:
        die(f"{context}: missing :production-bindings")
    bindings = []
    seen = set()
    for entry in expr.children:
        if not isinstance(entry, SList) or len(entry.children) != 3:
            die(f"{context}: production binding must be (consumer \"path\" \"symbol\")")
        consumer = _sexpr_atom_value(entry.children[0], context)
        path_expr = entry.children[1]
        symbol_expr = entry.children[2]
        if consumer not in XI_SEMANTIC_CONSUMERS:
            die(f"{context}: production binding names unknown consumer '{consumer}'")
        if consumer in seen:
            die(f"{context}: duplicate production binding for consumer '{consumer}'")
        if (not isinstance(path_expr, SAtom) or not path_expr.is_string or
                not isinstance(symbol_expr, SAtom) or not symbol_expr.is_string):
            die(f"{context}: production binding path and symbol must be strings")
        path_value = path_expr.str_value
        symbol_value = symbol_expr.str_value
        if not path_value or not symbol_value:
            die(f"{context}: production binding path and symbol cannot be empty")
        seen.add(consumer)
        bindings.append(XiProductionBinding(consumer, path_value, symbol_value))
    expected = set(consumers)
    if seen != expected:
        missing = sorted(expected - seen)
        extra = sorted(seen - expected)
        detail = []
        if missing:
            detail.append(f"missing bindings for {', '.join(missing)}")
        if extra:
            detail.append(f"unknown bindings for {', '.join(extra)}")
        die(f"{context}: {'; '.join(detail)}")
    return tuple(bindings)


def parse_xi_observable_owners(text: str, ops: list[XiOpDef],
                               semantic_owners: dict[str, str], path: str = '<input>',
                               id_hasher=None) -> tuple[list[XiObservableOwnerDef],
                                                        list[XiObservableOperation]]:
    forms = parse_sexpr(tokenize_sexpr(text, path), path)
    op_names = {op.name for op in ops}
    explicit_owners = []
    explicit_by_operation = {}
    owner_names = set()
    for form in forms:
        if not isinstance(form, SList) or not form.children:
            die(f"{path}: top-level form must be a list")
        head = _sexpr_atom_value(form.children[0], path)
        if head != 'define-xi-observable-owner':
            continue
        if len(form.children) < 2:
            die(f"{path}:{form.line}:{form.col}: missing observable owner name")
        name = _sexpr_atom_value(form.children[1], 'define-xi-observable-owner')
        if not name or name in owner_names:
            die(f"{path}: duplicate observable owner '{name}'")
        owner_names.add(name)
        category = _xi_get_kw_str(form, ':category')
        if category not in VALID_XI_SEMANTIC_OWNERS:
            die(f"{path}: observable owner '{name}' has unknown category '{category}'")
        operations = tuple(_xi_parse_atom_list(_xi_get_kw_list(form, ':operations'),
                                               f"{name}:operations"))
        if not operations:
            die(f"{path}: observable owner '{name}' has no operations")
        for operation in operations:
            if operation not in op_names:
                die(f"{path}: observable owner '{name}' names unknown operation '{operation}'")
            if operation in explicit_by_operation:
                die(f"{path}: Xi op '{operation}' has multiple observable owners")
            if semantic_owners.get(operation) != category:
                die(f"{path}: observable owner '{name}' category does not match '{operation}'")
            explicit_by_operation[operation] = name
        consumer_values = _xi_parse_atom_list(_xi_get_kw_list(form, ':consumers'),
                                              f"{name}:consumers")
        if not consumer_values:
            die(f"{path}: observable owner '{name}' has no consumers")
        consumers = tuple(consumer_values)
        if len(set(consumers)) != len(consumers):
            die(f"{path}: observable owner '{name}' has duplicate consumers")
        for consumer in consumers:
            if consumer not in XI_SEMANTIC_CONSUMERS:
                die(f"{path}: observable owner '{name}' names unknown consumer '{consumer}'")
        cgen_adapter = _xi_get_kw_str(form, ':cgen-adapter')
        if 'cgen' in consumers and not cgen_adapter:
            die(f"{path}: observable owner '{name}' is missing :cgen-adapter")
        if cgen_adapter and 'cgen' not in consumers:
            die(f"{path}: observable owner '{name}' has a cgen adapter without a cgen consumer")
        if cgen_adapter and not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', cgen_adapter):
            die(f"{path}: observable owner '{name}' has invalid cgen adapter '{cgen_adapter}'")
        bindings = _parse_production_bindings(_xi_get_kw_list(form, ':production-bindings'),
                                              consumers, name)
        explicit_owners.append(XiObservableOwnerDef(name, category, operations, consumers,
                                                    cgen_adapter, bindings))

    owner_by_name = {owner.name: owner for owner in explicit_owners}
    claimed_ids = {}

    def claim_stable_id(name: str) -> tuple[int, int]:
        stable_id = _semantic_stable_id(name, id_hasher)
        previous = claimed_ids.get(stable_id)
        if previous is not None and previous != name:
            die(f"{path}: stable semantic ID collision between '{previous}' and '{name}'")
        claimed_ids[stable_id] = name
        return stable_id

    operation_ids = {op.name: claim_stable_id(op.name) for op in ops}
    for owner in explicit_owners:
        claim_stable_id(owner.name)

    rows = []
    for op in ops:
        explicit_name = explicit_by_operation.get(op.name)
        if explicit_name:
            owner = owner_by_name[explicit_name]
            owner_id = claim_stable_id(owner.name)
            consumers = owner.consumers
            cgen_adapter = owner.cgen_adapter
            bindings = owner.production_bindings
        else:
            owner = None
            owner_id = operation_ids[op.name]
            consumers = ('semantic-plan',)
            cgen_adapter = ''
            bindings = (XiProductionBinding('semantic-plan', *DEFAULT_SEMANTIC_PLAN_BINDING),)
        consumer_bits = 0
        for consumer in consumers:
            consumer_bits |= XI_SEMANTIC_CONSUMERS[consumer]
        rows.append(XiObservableOperation(
            operation=op.name,
            category=semantic_owners[op.name],
            owner=owner.name if owner else op.name,
            operation_id_hi=operation_ids[op.name][0],
            operation_id_lo=operation_ids[op.name][1],
            owner_id_hi=owner_id[0],
            owner_id_lo=owner_id[1],
            observable_contract=op.observable_contract,
            consumers=consumers,
            consumer_bits=consumer_bits,
            cgen_adapter=cgen_adapter,
            production_bindings=bindings,
        ))
    return explicit_owners, rows


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


def _c_string_literal(value: str) -> str:
    escaped = (value.replace('\\', '\\\\')
                    .replace('"', '\\"')
                    .replace('\n', '\\n')
                    .replace('\r', '\\r')
                    .replace('\t', '\\t'))
    return f'"{escaped}"'


def _xi_value_contract(values: list[XiOperandDef] | list[XiResultDef]) -> str:
    records = []
    for value in values:
        attrs = ','.join(f'{key}={value.attrs[key]}' for key in sorted(value.attrs))
        records.append(f'{value.kind}:{value.name}:{attrs}')
    return ';'.join(records)


def _c_u64(value: int) -> str:
    return f'UINT64_C(0x{value:016x})'


def _c_u32(value: int) -> str:
    return f'UINT32_C(0x{value:08x})'


def _semantic_id_hex(hi: int, lo: int) -> str:
    return f'{hi:016x}{lo:016x}'


def generate_xi_semantic_ops_header(ops: list[XiOpDef], owners: dict[str, str],
                                    observable_rows: list[XiObservableOperation]) -> str:
    observable_by_operation = {row.operation: row for row in observable_rows}
    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/ops.def (target-neutral fields only) */')
    lines.append('')
    lines.append('#ifndef XR_SEMANTIC_OPS_GEN_H')
    lines.append('#define XR_SEMANTIC_OPS_GEN_H')
    lines.append('')
    lines.append('#include "../../shared/xr_semantic_owner_ids_gen.h"')
    lines.append('')
    lines.append('#define XR_SEM_EFFECT_NONE 0')
    for i, effect in enumerate(sorted(VALID_XI_EFFECTS)):
        lines.append(f'#define XR_SEM_EFFECT_{_xi_c_ident(effect)} (1u << {i})')
    lines.append('')
    lines.append('#define XR_SEMANTIC_OPERATION_CONTRACTS(X) \\')
    for i, op in enumerate(ops):
        owner = owners.get(op.name)
        if owner is None:
            die(f"Xi op '{op.name}' has no semantic owner")
        observable = observable_by_operation.get(op.name)
        if observable is None:
            die(f"Xi op '{op.name}' has no observable owner binding")
        if observable.owner != op.name:
            owner_ident = _xi_c_ident(observable.owner)
            owner_id_hi = f'XR_SEM_OWNER_ID_{owner_ident}_HI'
            owner_id_lo = f'XR_SEM_OWNER_ID_{owner_ident}_LO'
        else:
            owner_id_hi = _c_u64(observable.owner_id_hi)
            owner_id_lo = _c_u64(observable.owner_id_lo)
        arity = 'XR_SEMANTIC_OP_ARITY_VARIADIC' if op.arity == 0xFF else str(op.arity)
        effects = _xi_bit_expr('XR_SEM_EFFECT', op.effects)
        fields = [
            op.ident,
            _c_string_literal(op.name),
            f'XR_SEM_OWNER_{_xi_c_ident(owner)}',
            _c_string_literal(observable.owner),
            _c_u64(observable.operation_id_hi),
            _c_u64(observable.operation_id_lo),
            owner_id_hi,
            owner_id_lo,
            _c_string_literal(op.cls),
            arity,
            str(len(op.operands)),
            str(len(op.results)),
            _c_string_literal(_xi_value_contract(op.operands)),
            _c_string_literal(_xi_value_contract(op.results)),
            _c_string_literal(op.result_kind),
            f'XR_SEM_RESULT_OWNERSHIP_{_xi_c_ident(op.result_ownership)}',
            _c_string_literal(op.speculation),
            _c_string_literal(op.vn_kind),
            _c_string_literal(','.join(op.algebraic)),
            _c_string_literal(op.tbaa_group),
            _c_string_literal(op.sync_order),
            _c_string_literal(op.escape_use),
            _c_string_literal(op.escape_alloc),
            f'XR_SEM_OWN_USE_{_xi_c_ident(op.own_use)}',
            effects,
            _c_string_literal(','.join(op.requires)),
            _c_string_literal(','.join(op.observable)),
            _c_string_literal(op.negated_op or ''),
        ]
        suffix = ' \\' if i + 1 < len(ops) else ''
        lines.append(f"    X({', '.join(fields)}){suffix}")
    lines.append('')
    lines.append('')
    lines.append('#endif  /* XR_SEMANTIC_OPS_GEN_H */')
    lines.append('')
    return '\n'.join(lines)


def build_semantic_owner_registry(rows: list[XiObservableOperation]) -> dict:
    operations = []
    owners_by_name = {}
    for row in rows:
        binding_rows = [
            {
                'consumer': binding.consumer,
                'path': binding.path,
                'symbol': binding.symbol,
            }
            for binding in row.production_bindings
        ]
        operation_row = {
            'operation': row.operation,
            'operation_id': _semantic_id_hex(row.operation_id_hi, row.operation_id_lo),
            'operation_id_hi': f'0x{row.operation_id_hi:016x}',
            'operation_id_lo': f'0x{row.operation_id_lo:016x}',
            'owner': row.owner,
            'owner_id': _semantic_id_hex(row.owner_id_hi, row.owner_id_lo),
            'owner_id_hi': f'0x{row.owner_id_hi:016x}',
            'owner_id_lo': f'0x{row.owner_id_lo:016x}',
            'observable_contract': row.observable_contract,
            'category': row.category,
            'consumers': list(row.consumers),
            'consumer_bits': f'0x{row.consumer_bits:08x}',
            'cgen_adapter': row.cgen_adapter or None,
            'production_bindings': binding_rows,
        }
        operations.append(operation_row)
        owner_row = owners_by_name.get(row.owner)
        if owner_row is None:
            owner_row = {
                'owner': row.owner,
                'owner_id': operation_row['owner_id'],
                'owner_id_hi': operation_row['owner_id_hi'],
                'owner_id_lo': operation_row['owner_id_lo'],
                'category': row.category,
                'operations': [],
                'consumers': list(row.consumers),
                'consumer_bits': operation_row['consumer_bits'],
                'cgen_adapter': operation_row['cgen_adapter'],
                'production_bindings': binding_rows,
            }
            owners_by_name[row.owner] = owner_row
        owner_row['operations'].append(row.operation)

    payload = {
        'schema': 1,
        'source': 'xisa/xi/ops.def',
        'consumers': {
            name: f'0x{bit:08x}' for name, bit in XI_SEMANTIC_CONSUMERS.items()
        },
        'owners': [owners_by_name[name] for name in sorted(owners_by_name)],
        'operations': operations,
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(',', ':'),
                           ensure_ascii=False).encode('utf-8')
    fingerprint = hashlib.sha256(canonical).hexdigest()
    return {
        'schema': payload['schema'],
        'source': payload['source'],
        'canonical_fingerprint': fingerprint,
        'consumers': payload['consumers'],
        'owners': payload['owners'],
        'operations': payload['operations'],
    }


def generate_semantic_owner_registry_json(rows: list[XiObservableOperation]) -> str:
    registry = build_semantic_owner_registry(rows)
    return json.dumps(registry, indent=2, ensure_ascii=False) + '\n'


def generate_semantic_owner_ids_header(explicit_owners: list[XiObservableOwnerDef],
                                       rows: list[XiObservableOperation]) -> str:
    registry = build_semantic_owner_registry(rows)
    row_by_operation = {row.operation: row for row in rows}
    explicit_rows = [(owner, row_by_operation[owner.operations[0]])
                     for owner in explicit_owners]

    lines = [
        '/* AUTO-GENERATED by xisagen - DO NOT EDIT */',
        '/* Source: xisa/xi/ops.def */',
        '',
        '#ifndef XR_SEMANTIC_OWNER_IDS_GEN_H',
        '#define XR_SEMANTIC_OWNER_IDS_GEN_H',
        '',
        '#include <stdbool.h>',
        '#include <stddef.h>',
        '#include <stdint.h>',
        '',
    ]
    for consumer, bit in XI_SEMANTIC_CONSUMERS.items():
        lines.append(f'#define XR_SEM_CONSUMER_{_xi_c_ident(consumer)} {_c_u32(bit)}')
    lines.extend([
        '',
        f'#define XR_SEMANTIC_OWNER_REGISTRY_FINGERPRINT "{registry["canonical_fingerprint"]}"',
        '',
    ])
    for owner in explicit_owners:
        row = row_by_operation[owner.operations[0]]
        ident = _xi_c_ident(owner.name)
        lines.append(f'#define XR_SEM_OWNER_ID_{ident}_HI {_c_u64(row.owner_id_hi)}')
        lines.append(f'#define XR_SEM_OWNER_ID_{ident}_LO {_c_u64(row.owner_id_lo)}')
        lines.append(f'#define XR_SEM_OWNER_ID_{ident}_CONSUMERS {_c_u32(row.consumer_bits)}')
    lines.extend([
        '',
        'static inline uint32_t xr_semantic_owner_consumer_bits(uint64_t owner_id_hi,',
        '                                                        uint64_t owner_id_lo) {',
    ])
    for owner, row in explicit_rows:
        ident = _xi_c_ident(owner.name)
        lines.extend([
            f'    if (owner_id_hi == XR_SEM_OWNER_ID_{ident}_HI &&',
            f'        owner_id_lo == XR_SEM_OWNER_ID_{ident}_LO)',
            f'        return XR_SEM_OWNER_ID_{ident}_CONSUMERS;',
        ])
    lines.extend([
        '    return 0;',
        '}',
        '',
        'static inline bool xr_semantic_owner_has_consumer(uint64_t owner_id_hi,',
        '                                                   uint64_t owner_id_lo,',
        '                                                   uint32_t consumer_bit) {',
        '    uint32_t bits = xr_semantic_owner_consumer_bits(owner_id_hi, owner_id_lo);',
        '    return bits != 0 && consumer_bit != 0 && (bits & consumer_bit) != 0;',
        '}',
        '',
        'static inline const char *xr_semantic_owner_cgen_adapter(uint64_t owner_id_hi,',
        '                                                           uint64_t owner_id_lo) {',
    ])
    for owner, row in explicit_rows:
        if not row.cgen_adapter:
            continue
        ident = _xi_c_ident(owner.name)
        lines.extend([
            f'    if (owner_id_hi == XR_SEM_OWNER_ID_{ident}_HI &&',
            f'        owner_id_lo == XR_SEM_OWNER_ID_{ident}_LO)',
            f'        return {_c_string_literal(row.cgen_adapter)};',
        ])
    lines.extend([
        '    return NULL;',
        '}',
        '',
        '#endif  /* XR_SEMANTIC_OWNER_IDS_GEN_H */',
        '',
    ])
    return '\n'.join(lines)


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
    lines.append('    XI_GEN_TBAA_OBJECT = 10,')
    lines.append('    XI_GEN_TBAA_TUPLE = 11,')
    lines.append('    XI_GEN_TBAA_CHAN = 12,')
    lines.append('    XI_GEN_TBAA_FRESH = 13,')
    lines.append('    XI_GEN_TBAA__COUNT')
    lines.append('} XiGeneratedTbaaGroup;')
    lines.append('')
    lines.append('/* ========== Synchronisation Orders (spec §16.9.2) ========== */')
    lines.append('')
    lines.append('typedef enum {')
    lines.append('    XI_GEN_SYNC_NONE = 0,')
    lines.append('    XI_GEN_SYNC_ACQUIRE = 1,')
    lines.append('    XI_GEN_SYNC_RELEASE = 2,')
    lines.append('    XI_GEN_SYNC_ACQ_REL = 3,')
    lines.append('    XI_GEN_SYNC_SEQ_CST = 4,')
    lines.append('    XI_GEN_SYNC__COUNT')
    lines.append('} XiGeneratedSyncOrder;')
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
    lines.append('    XI_GEN_OWN_USE_PASS = 4,')
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
    lines.append('    uint8_t sync_order;')
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
        sync_order = f'XI_GEN_SYNC_{_xi_c_ident(op.sync_order)}'
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
            f'{lowering_policy}, {speculation}, {vn_kind}, {tbaa_group}, {sync_order}, '
            f'{backend_rewrite}, {escape_use}, '
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
    lines.append('static inline uint8_t xi_generated_op_sync_order(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for op in ops:
        lines.append(f'        case XI_{op.ident}: return XI_GEN_SYNC_{_xi_c_ident(op.sync_order)};')
    lines.append('        case XI_OP_COUNT: break;')
    lines.append('    }')
    lines.append('    return XI_GEN_SYNC_NONE;')
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

VALID_XI_LOWERING_KEYS = {
    ':match',
    ':required-targets',
    ':aot-c-consumer',
} | {':' + target for target in VALID_XI_LOWERING_TARGETS}

VALID_XI_LOWERING_TARGET_ATTRS = {
    'fresh-dst',
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

XI_NUMERIC_WIDTH_KERNELS = {
    op_name: 'xr_numeric_' + op_name.removeprefix('xi.').replace('.', '_')
    for op_name in {
        'xi.narrow.i8', 'xi.narrow.u8', 'xi.narrow.i16', 'xi.narrow.u16',
        'xi.narrow.i32', 'xi.narrow.u32', 'xi.narrow.f32',
        'xi.widen.i8', 'xi.widen.u8', 'xi.widen.i16', 'xi.widen.u16',
        'xi.widen.i32', 'xi.widen.u32', 'xi.widen.f32',
    }
}

XI_VM_TEMPLATE_BITWISE_BINARY = {
    'xi.band': ('XR_BITWISE_BINARY_AND', 'true', '"bitwise AND requires integer types"'),
    'xi.bor': ('XR_BITWISE_BINARY_OR', 'true', '"bitwise OR requires integer types"'),
    'xi.bxor': ('XR_BITWISE_BINARY_XOR', 'false', '"bitwise XOR requires integer types"'),
}

XI_VM_TEMPLATE_BITWISE_UNARY = {
    'xi.bnot': '"bitwise NOT requires integer type"',
}

XI_VM_TEMPLATE_UNARY = {
    'xi.neg': ('XVM_TEMPLATE_UNARY_NEG_CASE', None),
    'xi.not': ('XVM_TEMPLATE_UNARY_NOT_CASE', '"!"'),
}

XI_VM_TEMPLATE_ARITH_BINARY = {
    'xi.add': ('XVM_TEMPLATE_ARITH_ADD_CASE', 'xr_i64_add_wrap', '+', 'xr_bigint_add',
               'XR_OP_ADD_FLAG', 'SYMBOL_OP_ADD', '"+"',
               '"operator \'+\' requires both operands to be numeric or both string, got \'%s\' and \'%s\'"'),
    'xi.sub': ('XVM_TEMPLATE_ARITH_NUMERIC_CASE', 'xr_i64_sub_wrap', '-', 'xr_bigint_sub',
               'XR_OP_SUB_FLAG', 'SYMBOL_OP_SUB', '"-"',
               '"subtraction requires numeric types"'),
    'xi.mul': ('XVM_TEMPLATE_ARITH_MUL_CASE', 'xr_i64_mul_wrap', '*', 'xr_bigint_mul',
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
    'xi.shl': 'XR_SHIFT_LEFT',
    'xi.shr': 'XR_SHIFT_RIGHT_SIGNED',
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
    'xi.band': 'XR_BITWISE_BINARY_AND',
    'xi.bor': 'XR_BITWISE_BINARY_OR',
    'xi.bxor': 'XR_BITWISE_BINARY_XOR',
}

XI_AOT_C_TEMPLATE_BITWISE_UNARY = {'xi.bnot'}

XI_AOT_C_TEMPLATE_SHIFT = {
    'xi.shl': 'XR_SHIFT_LEFT',
    'xi.shr': 'XR_SHIFT_RIGHT_SIGNED',
}

# (tagged runtime relation, shared-owner relation token, tagged args swap).
# The relation token names the owner's relation; the raw C operator is spelled
# once inside the shared compare kernel and never in generated code.
XI_AOT_C_TEMPLATE_COMPARE = {
    'xi.eq': ('xrt_eq', 'EQ', False),
    'xi.ne': ('!xrt_eq', 'NE', False),
    'xi.lt': ('xrt_lt', 'LT', False),
    'xi.le': ('xrt_le', 'LE', False),
    'xi.gt': ('xrt_lt', 'GT', True),
    'xi.ge': ('xrt_le', 'GE', True),
}

@dataclass
class XiConsumerRouterWitness:
    source_path: str
    symbol: str


@dataclass(frozen=True)
class XiConsumerActivationWitness:
    source_path: str
    caller: str
    args: tuple[str, ...]
    count: int


@dataclass(frozen=True)
class XiConsumerEmitterWitness:
    source_path: str
    symbol: str


@dataclass(frozen=True)
class XiConsumerPredicateWitness:
    source_path: str
    symbol: str
    domain_source_path: str | None = None
    domain_symbol: str | None = None


@dataclass(frozen=True)
class XiConsumerOutputCallWitness:
    symbol: str
    args: tuple[str, ...]
    prefix: bool


@dataclass(frozen=True)
class XiConsumerOutputSequenceWitness:
    calls: tuple[XiConsumerOutputCallWitness, ...]
    count: int


@dataclass
class XiConsumerBinding:
    source_path: str
    symbol: str
    witness_kind: str
    routers: tuple[XiConsumerRouterWitness, ...] = ()
    structural_family: str | None = None
    structural_category: str | None = None
    activations: tuple[XiConsumerActivationWitness, ...] = ()
    output_sequences: tuple[XiConsumerOutputSequenceWitness, ...] = ()
    emitters: tuple[XiConsumerEmitterWitness, ...] = ()
    predicates: tuple[XiConsumerPredicateWitness, ...] = ()


@dataclass
class XiLoweringDef:
    op_name: str
    ident: str
    required_targets: list
    targets: list
    target_drivers: dict
    target_rejects: dict
    target_attrs: dict
    target_consumers: dict = field(default_factory=dict)
    match: SList | None = None
    template: str = 'custom'


XI_C11_KEYWORDS = {
    '_Alignas', '_Alignof', '_Atomic', '_Bool', '_Complex', '_Generic',
    '_Imaginary', '_Noreturn', '_Static_assert', '_Thread_local', 'auto',
    'break', 'case', 'char', 'const', 'continue', 'default', 'do', 'double',
    'else', 'enum', 'extern', 'float', 'for', 'goto', 'if', 'inline', 'int',
    'long', 'register', 'restrict', 'return', 'short', 'signed', 'sizeof',
    'static', 'struct', 'switch', 'typedef', 'union', 'unsigned', 'void',
    'volatile', 'while',
}

XI_CONSUMER_STRUCTURAL_CATEGORIES = {
    'edge-parallel-copy',
    'sync-storage',
    'coroutine-frame-storage',
    'coroutine-local-storage',
}

XI_CONSUMER_STRUCTURAL_FAMILIES = {
    'phi': XI_CONSUMER_STRUCTURAL_CATEGORIES,
}


def _xi_consumer_c_ident(expr: SExpr, context: str) -> str:
    value = _sexpr_atom_value(expr, context)
    if (not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', value) or
            value in XI_C11_KEYWORDS):
        die(f"{context}: invalid C identifier '{value}'")
    return value


def _xi_consumer_source_path(expr: SExpr, context: str) -> str:
    if not isinstance(expr, SAtom) or not expr.is_string:
        die(f"{context}: consumer source path must be a string")
    value = expr.str_value
    path = Path(value)
    if (not value or '\\' in value or path.is_absolute() or
            path.as_posix() != value or any(part in {'', '.', '..'} for part in path.parts) or
            any(char in value for char in '*?[]') or
            len(path.parts) < 3 or path.parts[0:2] != ('src', 'aot') or
            path.suffix != '.c'):
        die(f"{context}: invalid canonical AOT consumer source path '{value}'")
    return value


def _xi_consumer_arg_list(expr: SExpr, context: str) -> tuple[str, ...]:
    if (not isinstance(expr, SList) or not expr.children or
            _sexpr_atom_value(expr.children[0], context) != 'args'):
        die(f"{context}: expected (args ...)")
    args = tuple(_sexpr_atom_value(value, context) for value in expr.children[1:])
    if any(not value or value != value.strip() for value in args):
        die(f"{context}: call arguments must be exact nonempty tokens")
    return args


def _xi_consumer_positive_count(expr: SExpr, context: str) -> int:
    if (not isinstance(expr, SList) or len(expr.children) != 2 or
            _sexpr_atom_value(expr.children[0], context) != 'count'):
        die(f"{context}: expected (count positive-integer)")
    value = _sexpr_atom_value(expr.children[1], context)
    if not re.fullmatch(r'[1-9][0-9]*', value):
        die(f"{context}: count must be a positive integer")
    return int(value)


def _xi_parse_activation_witness(expr: SList, context: str
                                 ) -> XiConsumerActivationWitness:
    if len(expr.children) != 5:
        die(f"{context}: activation must name path, caller, args, and count")
    return XiConsumerActivationWitness(
        _xi_consumer_source_path(expr.children[1], context),
        _xi_consumer_c_ident(expr.children[2], context),
        _xi_consumer_arg_list(expr.children[3], context),
        _xi_consumer_positive_count(expr.children[4], context),
    )


def _xi_parse_emitter_witness(expr: SList, context: str
                              ) -> XiConsumerEmitterWitness:
    if len(expr.children) != 3:
        die(f"{context}: emitter must name one path and symbol")
    return XiConsumerEmitterWitness(
        _xi_consumer_source_path(expr.children[1], context),
        _xi_consumer_c_ident(expr.children[2], context),
    )


def _xi_parse_predicate_witness(expr: SList, context: str
                                ) -> XiConsumerPredicateWitness:
    if len(expr.children) not in {3, 4}:
        die(f"{context}: predicate must name one path and symbol with an "
            "optional exact domain")
    domain_source_path = None
    domain_symbol = None
    if len(expr.children) == 4:
        domain = expr.children[3]
        if (not isinstance(domain, SList) or len(domain.children) != 3 or
                _sexpr_atom_value(domain.children[0], context) != 'domain'):
            die(f"{context}: predicate domain must name one path and symbol")
        domain_source_path = _xi_consumer_source_path(domain.children[1], context)
        domain_symbol = _xi_consumer_c_ident(domain.children[2], context)
    return XiConsumerPredicateWitness(
        _xi_consumer_source_path(expr.children[1], context),
        _xi_consumer_c_ident(expr.children[2], context),
        domain_source_path,
        domain_symbol,
    )


def _xi_parse_output_sequence_witness(expr: SList, context: str
                                      ) -> XiConsumerOutputSequenceWitness:
    if len(expr.children) < 3:
        die(f"{context}: output-sequence requires call entries and a count")
    calls: list[XiConsumerOutputCallWitness] = []
    for call in expr.children[1:-1]:
        if not isinstance(call, SList) or len(call.children) != 3:
            die(f"{context}: output sequence call shape is not exact")
        kind = _sexpr_atom_value(call.children[0], context)
        if kind not in {'call', 'call-prefix'}:
            die(f"{context}: output sequence uses unknown call kind '{kind}'")
        calls.append(XiConsumerOutputCallWitness(
            _xi_consumer_c_ident(call.children[1], context),
            _xi_consumer_arg_list(call.children[2], context),
            kind == 'call-prefix',
        ))
    if not calls:
        die(f"{context}: output-sequence cannot be empty")
    return XiConsumerOutputSequenceWitness(
        tuple(calls), _xi_consumer_positive_count(expr.children[-1], context))


def _xi_parse_consumer_binding(expr: SExpr, op_name: str,
                               target: str) -> XiConsumerBinding:
    context = f"{op_name}:{target}:consumer"
    if not isinstance(expr, SList) or len(expr.children) < 4:
        die(f"{context}: binding must be (binding \"path\" symbol witness ...)")
    if _sexpr_atom_value(expr.children[0], context) != 'binding':
        die(f"{context}: expected binding entry")
    source_path = _xi_consumer_source_path(expr.children[1], context)
    symbol = _xi_consumer_c_ident(expr.children[2], context)
    witnesses = expr.children[3:]
    if any(not isinstance(witness, SList) or not witness.children
           for witness in witnesses):
        die(f"{context}: binding witness must be selector, guarded-selector, "
            "selected-by, structural, predicate, activation, or output-sequence")
    kinds = [_sexpr_atom_value(witness.children[0], context) for witness in witnesses]
    primary = [witness for witness, kind in zip(witnesses, kinds)
               if kind in {'selector', 'guarded-selector', 'selected-by', 'structural'}]
    activations = tuple(
        _xi_parse_activation_witness(witness, context)
        for witness, kind in zip(witnesses, kinds) if kind == 'activation')
    emitters = tuple(
        _xi_parse_emitter_witness(witness, context)
        for witness, kind in zip(witnesses, kinds) if kind == 'emitter')
    predicates = tuple(
        _xi_parse_predicate_witness(witness, context)
        for witness, kind in zip(witnesses, kinds) if kind == 'predicate')
    output_sequences = tuple(
        _xi_parse_output_sequence_witness(witness, context)
        for witness, kind in zip(witnesses, kinds) if kind == 'output-sequence')
    unknown = sorted(set(kinds) - {
        'selector', 'guarded-selector', 'selected-by', 'structural',
        'predicate', 'activation', 'emitter', 'output-sequence',
    })
    if unknown:
        die(f"{context}: unknown binding witness kind(s): {', '.join(unknown)}")
    primary_kinds = [_sexpr_atom_value(witness.children[0], context)
                     for witness in primary]
    if primary_kinds == ['selector']:
        if len(primary[0].children) != 1:
            die(f"{context}: selector witness takes no arguments")
        binding = XiConsumerBinding(source_path, symbol, 'selector')
    elif primary_kinds == ['guarded-selector']:
        if len(primary[0].children) != 1:
            die(f"{context}: guarded-selector witness takes no arguments")
        binding = XiConsumerBinding(source_path, symbol, 'guarded-selector')
    elif primary_kinds and set(primary_kinds) == {'selected-by'}:
        routers = []
        for witness in primary:
            if len(witness.children) != 3:
                die(f"{context}: selected-by witness must name one router path and symbol")
            routers.append(XiConsumerRouterWitness(
                _xi_consumer_source_path(witness.children[1], context),
                _xi_consumer_c_ident(witness.children[2], context),
            ))
        identities = [(router.source_path, router.symbol) for router in routers]
        if len(set(identities)) != len(identities):
            die(f"{context}: duplicate selected-by router witness")
        binding = XiConsumerBinding(source_path, symbol, 'selected-by', tuple(routers))
    elif primary_kinds == ['structural']:
        witness = primary[0]
        if len(witness.children) != 3:
            die(f"{context}: structural witness must name a family and category")
        family = _sexpr_atom_value(witness.children[1], context)
        category = _sexpr_atom_value(witness.children[2], context)
        allowed_categories = XI_CONSUMER_STRUCTURAL_FAMILIES.get(family)
        if allowed_categories is None or op_name != 'xi.' + family:
            die(f"{context}: structural family '{family}' does not match the Xi operation")
        if category not in allowed_categories:
            die(f"{context}: unknown structural witness category '{category}'")
        binding = XiConsumerBinding(source_path, symbol, 'structural', (), family, category)
    else:
        die(f"{context}: consumer binding mixes incompatible primary witness kinds")
    if len(set(activations)) != len(activations):
        die(f"{context}: duplicate activation witness")
    if len(set(emitters)) != len(emitters):
        die(f"{context}: duplicate emitter witness")
    if len(set(predicates)) != len(predicates):
        die(f"{context}: duplicate predicate witness")
    if binding.witness_kind in {'selector', 'guarded-selector'}:
        if not emitters:
            die(f"{context}: selector witness requires an exact terminal emitter")
    elif emitters:
        die(f"{context}: emitter witness is only valid with selector ownership")
    if binding.witness_kind != 'selector' and predicates:
        die(f"{context}: predicate witness is only valid with selector ownership")
    if len(set(output_sequences)) != len(output_sequences):
        die(f"{context}: duplicate output-sequence witness")
    binding.activations = activations
    binding.output_sequences = output_sequences
    binding.emitters = emitters
    binding.predicates = predicates
    return binding


def _xi_parse_consumer(expr: SList, op_name: str,
                       target: str) -> list[XiConsumerBinding]:
    context = f"{op_name}:{target}:consumer"
    if len(expr.children) < 3:
        die(f"{context}: expected (consumer xi-cgen-direct (binding ...))")
    consumer = _sexpr_atom_value(expr.children[1], context)
    if consumer != 'xi-cgen-direct':
        die(f"{context}: unknown direct consumer '{consumer}'")
    bindings = [
        _xi_parse_consumer_binding(binding, op_name, target)
        for binding in expr.children[2:]
    ]
    identities = [(binding.source_path, binding.symbol) for binding in bindings]
    if len(set(identities)) != len(identities):
        die(f"{context}: duplicate consumer owner binding")
    structural = [binding.structural_category for binding in bindings
                  if binding.witness_kind == 'structural']
    if len(set(structural)) != len(structural):
        die(f"{context}: duplicate structural witness category")
    return bindings


def _xi_lowering_parse_bool_attr(value: SExpr, context: str) -> bool:
    text = _sexpr_atom_value(value, context)
    if text == 'yes':
        return True
    if text == 'no':
        return False
    die(f"{context}: expected yes or no")


def _xi_lowering_target_entry(form: SList, target: str,
                              op_name: str) -> tuple[str | None, list, dict]:
    target_key = ':' + target
    if sum(1 for item in form.children
           if isinstance(item, SAtom) and item.value == target_key) > 1:
        die(f"{op_name}:{target}: duplicate target entry")
    value = _xi_get_kw(form, target_key)
    if value is None:
        return None, [], {}
    if not isinstance(value, SList) or len(value.children) < 2:
        die(f"{op_name}:{target}: expected (driver name), (reject name), or "
            "(consumer xi-cgen-direct (binding ...))")
    kind = _sexpr_atom_value(value.children[0], f"{op_name}:{target}")
    if kind not in {'driver', 'reject', 'consumer'}:
        die(f"{op_name}:{target}: expected driver, reject, or consumer entry")
    if kind == 'consumer':
        if target != 'aot-c':
            die(f"{op_name}:{target}: direct consumer is only valid for aot-c")
        return kind, _xi_parse_consumer(value, op_name, target), {}
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
    return kind, [_sexpr_atom_value(value.children[1], f"{op_name}:{target}")], attrs


def _xi_lowering_target_entries(form: SList, op_name: str) -> tuple[dict, dict, dict, dict]:
    drivers = {}
    rejects = {}
    consumers = {}
    attrs = {}
    for target in sorted(VALID_XI_LOWERING_TARGETS):
        kind, names, target_attrs = _xi_lowering_target_entry(form, target, op_name)
        if kind is None:
            continue
        if kind == 'driver':
            drivers[target] = names[0]
        elif kind == 'reject':
            rejects[target] = names[0]
        else:
            consumers[target] = names
        if target_attrs:
            attrs[target] = target_attrs
    supplemental = _xi_get_kw(form, ':aot-c-consumer')
    if supplemental is not None:
        if 'aot-c' in consumers:
            die(f"{op_name}:aot-c-consumer: duplicate direct consumer authority")
        if (not isinstance(supplemental, SList) or
                not supplemental.children or
                _sexpr_atom_value(supplemental.children[0],
                                  f"{op_name}:aot-c-consumer") != 'consumer'):
            die(f"{op_name}:aot-c-consumer: expected "
                "(consumer xi-cgen-direct (binding ...))")
        consumers['aot-c'] = _xi_parse_consumer(
            supplemental, op_name, 'aot-c')
    return drivers, rejects, consumers, attrs


def _xi_lowering_template_from_match(match: SList | None, op_name: str) -> str:
    if match is None or not match.children:
        return 'custom'
    template = _sexpr_atom_value(match.children[0], f"{op_name}:match")
    if template not in VALID_XI_LOWERING_TEMPLATES:
        die(f"{op_name}:match: unknown lowering template '{template}'")
    return template


def _xi_normalized_lowering_targets(targets) -> set[str]:
    return {
        'aot-c' if target == 'aot-c-stmt' else target
        for target in targets
    }


def _xi_canonical_lowering_target(target: str) -> str:
    return 'aot-c' if target == 'aot-c-stmt' else target


def _xi_validate_closed_lowering_form(form: SList, op_name: str) -> None:
    fields = form.children[2:]
    if len(fields) % 2 != 0:
        die(f"{op_name}: lowering fields must be exact keyword/value pairs")
    seen: set[str] = set()
    for index in range(0, len(fields), 2):
        key = fields[index]
        if (not isinstance(key, SAtom) or key.is_string or
                not key.value.startswith(':')):
            die(f"{op_name}: lowering field key must be a non-string keyword")
        if key.value not in VALID_XI_LOWERING_KEYS:
            die(f"{op_name}: unknown lowering field '{key.value}'")
        if key.value in seen:
            die(f"{op_name}: duplicate lowering field '{key.value}'")
        seen.add(key.value)


def _xi_validate_canonical_target_ownership(
        op_name: str, required: list[str], drivers: dict[str, str],
        rejects: dict[str, str], consumers: dict[str, list[XiConsumerBinding]]) -> None:
    normalized_required = [_xi_canonical_lowering_target(target) for target in required]
    if len(normalized_required) != len(set(normalized_required)):
        die(f"{op_name}: required targets contain a canonical target collision")
    owners: dict[str, list[str]] = {}
    for kind, entries in (
            ('driver', drivers), ('reject', rejects), ('consumer', consumers)):
        for target in entries:
            canonical = _xi_canonical_lowering_target(target)
            owners.setdefault(canonical, []).append(f"{target}:{kind}")
    collisions = {target: entries for target, entries in owners.items()
                  if len(entries) > 1}
    for target, entries in list(collisions.items()):
        if target == 'aot-c' and set(entries) in (
                {'aot-c:driver', 'aot-c:consumer'},
                {'aot-c-stmt:driver', 'aot-c:consumer'}):
            del collisions[target]
    if collisions:
        details = '; '.join(
            f"{target}=" + ','.join(entries)
            for target, entries in sorted(collisions.items())
        )
        die(f"{op_name}: canonical lowering target has multiple authorities: {details}")


def _xi_expected_stmt_driver(op_name: str) -> str:
    suffix = op_name.removeprefix('xi.').replace('.', '_').replace('-', '_')
    expected = 'xicgen_stmt_' + suffix
    if not re.fullmatch(r'[a-z_][a-z0-9_]*', expected):
        die(f"{op_name}: cannot derive a canonical AOT statement driver")
    return expected


def _xi_validate_lowering_policy(entries: list[XiLoweringDef], ops: list[XiOpDef],
                                 path: str) -> None:
    entry_names = {entry.op_name for entry in entries}
    missing = [op.name for op in ops
               if op.lowering_policy == 'generated' and op.name not in entry_names]
    if missing:
        die(f"{path}: missing lowering entry for generated Xi op(s): {', '.join(missing)}")
    missing_special = [op.name for op in ops
                       if op.lowering_policy == 'special' and 'aot-c' in op.targets and
                       op.name not in entry_names]
    if missing_special:
        die(f"{path}: missing direct consumer entry for special AOT Xi op(s): "
            f"{', '.join(missing_special)}")
    op_by_name = {op.name: op for op in ops}
    for entry in entries:
        op = op_by_name[entry.op_name]
        if op.lowering_policy not in {'generated', 'special'}:
            die(f"{path}: Xi op '{entry.op_name}' has lowering-policy "
                f"'{op.lowering_policy}' but also has a lowering entry")
        if op.lowering_policy == 'special':
            if (set(entry.target_consumers) != {'aot-c'} or entry.target_drivers or
                    entry.target_rejects or entry.target_attrs or
                    entry.required_targets != ['aot-c']):
                die(f"{path}: special AOT Xi op '{entry.op_name}' must have exactly one "
                    "required aot-c direct consumer target")
            continue
    for entry in entries:
        op = op_by_name[entry.op_name]
        if op.lowering_policy == 'special':
            continue
        op_targets = {target for target in op.targets if target != 'aot-verify'}
        lowering_targets = _xi_normalized_lowering_targets(
            target for target in entry.targets if target != 'aot-verify')
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
        _xi_validate_closed_lowering_form(form, op_name)
        required = _xi_parse_atom_list(_xi_get_kw_list(form, ':required-targets'),
                                      f"{op_name}:required-targets")
        for target in required:
            if target not in VALID_XI_LOWERING_TARGETS:
                die(f"{path}: lowering '{op_name}' uses unknown required target '{target}'")
        target_drivers, target_rejects, target_consumers, target_attrs = \
            _xi_lowering_target_entries(form, op_name)
        _xi_validate_canonical_target_ownership(
            op_name, required, target_drivers, target_rejects, target_consumers)
        stmt_driver = target_drivers.get('aot-c-stmt')
        if stmt_driver is not None and stmt_driver != _xi_expected_stmt_driver(op_name):
            die(f"{path}: {op_name}:aot-c-stmt driver must be "
                f"{_xi_expected_stmt_driver(op_name)}, got {stmt_driver}")
        targets = sorted(set(target_drivers) | set(target_rejects) | set(target_consumers))
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
                                     target_consumers=target_consumers,
                                     match=match, template=template))
    _xi_validate_lowering_policy(entries, ops, path)
    return entries


@functools.lru_cache(maxsize=16384)
def _xi_c_translation_phase_1_2(text: str) -> str:
    """Apply C trigraph replacement and backslash-newline deletion."""
    trigraphs = {
        '??=': '#', '??/': '\\', "??'": '^', '??(': '[', '??)': ']',
        '??!': '|', '??<': '{', '??>': '}', '??-': '~',
    }
    translated = []
    index = 0
    while index < len(text):
        spelling = text[index:index + 3]
        if spelling in trigraphs:
            translated.append(trigraphs[spelling])
            index += 3
        else:
            translated.append(text[index])
            index += 1
    return re.sub(r'\\(?:\r\n|\n|\r)', '', ''.join(translated))


@functools.lru_cache(maxsize=16384)
def _xi_c_source_without_literals(text: str, *, blank_preprocessor: bool = True) -> str:
    """Normalize translation phases, then blank C comments and literals."""
    text = _xi_c_translation_phase_1_2(text)
    chars = list(text)
    i = 0
    state = 'normal'
    while i < len(chars):
        current = chars[i]
        following = chars[i + 1] if i + 1 < len(chars) else ''
        if state == 'normal':
            if current == '/' and following == '/':
                chars[i] = chars[i + 1] = ' '
                i += 2
                state = 'line-comment'
                continue
            if current == '/' and following == '*':
                chars[i] = chars[i + 1] = ' '
                i += 2
                state = 'block-comment'
                continue
            if current == '"':
                chars[i] = ' '
                state = 'string'
            elif current == "'":
                chars[i] = ' '
                state = 'char'
        elif state == 'line-comment':
            if current == '\n':
                state = 'normal'
            else:
                chars[i] = ' '
        elif state == 'block-comment':
            if current == '*' and following == '/':
                chars[i] = chars[i + 1] = ' '
                i += 2
                state = 'normal'
                continue
            if current != '\n':
                chars[i] = ' '
        elif state in {'string', 'char'}:
            delimiter = '"' if state == 'string' else "'"
            if current == '\\' and following:
                if current != '\n':
                    chars[i] = ' '
                if following != '\n':
                    chars[i + 1] = ' '
                i += 2
                continue
            if current == delimiter:
                chars[i] = ' '
                state = 'normal'
            elif current != '\n':
                chars[i] = ' '
        i += 1
    sanitized = ''.join(chars)
    if not blank_preprocessor:
        return sanitized
    lines = sanitized.splitlines(keepends=True)
    continuation = False
    for index, line in enumerate(lines):
        stripped = line.lstrip()
        if continuation or re.match(r'(?:#|%:)', stripped):
            continuation = line.rstrip('\r\n').endswith('\\')
            lines[index] = ''.join('\n' if char == '\n' else ' ' for char in line)
        else:
            continuation = False
    return ''.join(lines)


def _xi_matching_delimiter(text: str, start: int, opening: str,
                           closing: str) -> int | None:
    depth = 0
    for index in range(start, len(text)):
        if text[index] == opening:
            depth += 1
        elif text[index] == closing:
            depth -= 1
            if depth == 0:
                return index
    return None


@functools.lru_cache(maxsize=512)
def _xi_c_file_scope_function_body_ranges(text: str) -> dict[str, list[tuple[int, int]]]:
    source = _xi_c_source_without_literals(text)
    brace_depth = 0
    depths = [0] * (len(source) + 1)
    for index, char in enumerate(source):
        depths[index] = brace_depth
        if char == '{':
            brace_depth += 1
        elif char == '}':
            brace_depth = max(0, brace_depth - 1)
    ranges = {}
    for match in re.finditer(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(', source):
        if depths[match.start()] != 0:
            continue
        symbol = match.group(1)
        open_paren = source.find('(', match.start())
        close_paren = _xi_matching_delimiter(source, open_paren, '(', ')')
        if close_paren is None:
            continue
        cursor = close_paren + 1
        while cursor < len(source) and source[cursor].isspace():
            cursor += 1
        while cursor < len(source) and source[cursor] not in '{;':
            cursor += 1
        if cursor >= len(source) or source[cursor] != '{':
            continue
        close_brace = _xi_matching_delimiter(source, cursor, '{', '}')
        if close_brace is None:
            continue
        ranges.setdefault(symbol, []).append((cursor + 1, close_brace))
    return ranges


def _xi_c_file_scope_function_bodies(text: str) -> dict[str, list[str]]:
    source = _xi_c_source_without_literals(text)
    return {
        symbol: [source[start:end] for start, end in ranges]
        for symbol, ranges in _xi_c_file_scope_function_body_ranges(text).items()
    }


@functools.lru_cache(maxsize=256)
def _xi_c_file_scope_function_value_parameters(text: str) -> dict[str, set[str]]:
    """Return XiValue pointer parameter names for file-scope definitions."""
    source = _xi_c_source_without_literals(text)
    depths = _xi_brace_depths(source)
    parameters: dict[str, set[str]] = {}
    for match in re.finditer(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(', source):
        if depths[match.start()] != 0:
            continue
        symbol = match.group(1)
        open_paren = source.find('(', match.start())
        close_paren = _xi_matching_delimiter(source, open_paren, '(', ')')
        if close_paren is None:
            continue
        cursor = close_paren + 1
        while cursor < len(source) and source[cursor].isspace():
            cursor += 1
        while cursor < len(source) and source[cursor] not in '{;':
            cursor += 1
        if cursor >= len(source) or source[cursor] != '{':
            continue
        for parameter in _xi_split_c_arguments(
                source[open_paren + 1:close_paren]):
            value = re.fullmatch(
                r'(?:const)?XiValue(?:const)?\*+(?:const)?'
                r'([A-Za-z_][A-Za-z0-9_]*)',
                _xi_normalize_c_expr(parameter))
            if value is not None:
                parameters.setdefault(symbol, set()).add(value.group(1))
    return parameters


def _xi_conditional_preprocessor_depth(text: str, end: int) -> int:
    sanitized = _xi_c_source_without_literals(text, blank_preprocessor=False)[:end]
    depth = 0
    for line in sanitized.splitlines():
        match = re.match(r'^\s*#\s*(if|ifdef|ifndef|endif)\b', line)
        if not match:
            continue
        if match.group(1) == 'endif':
            depth = max(0, depth - 1)
        else:
            depth += 1
    return depth


def _xi_c_function_bodies(text: str, symbol: str) -> list[str]:
    return _xi_c_file_scope_function_bodies(text).get(symbol, [])


def _xi_c_call_present(text: str, symbol: str) -> bool:
    return re.search(rf'\b{re.escape(symbol)}\s*\(', text) is not None


def _xi_c_call_count(text: str, symbol: str) -> int:
    return len(re.findall(rf'\b{re.escape(symbol)}\s*\(', text))


def _xi_normalize_c_expr(text: str) -> str:
    return re.sub(r'\s+', '', text)


def _xi_split_c_arguments(text: str) -> tuple[str, ...]:
    if not text.strip():
        return ()
    parts = []
    start = 0
    paren = bracket = brace = 0
    for index, char in enumerate(text):
        if char == '(':
            paren += 1
        elif char == ')':
            paren = max(0, paren - 1)
        elif char == '[':
            bracket += 1
        elif char == ']':
            bracket = max(0, bracket - 1)
        elif char == '{':
            brace += 1
        elif char == '}':
            brace = max(0, brace - 1)
        elif char == ',' and paren == 0 and bracket == 0 and brace == 0:
            parts.append(_xi_normalize_c_expr(text[start:index]))
            start = index + 1
    parts.append(_xi_normalize_c_expr(text[start:]))
    return tuple(parts)


def _xi_c_call_records(text: str, symbol: str
                       ) -> list[tuple[int, int, tuple[str, ...]]]:
    records = []
    for match in re.finditer(rf'\b{re.escape(symbol)}\s*\(', text):
        open_paren = text.find('(', match.start())
        close_paren = _xi_matching_delimiter(text, open_paren, '(', ')')
        if close_paren is None:
            continue
        records.append((match.start(), close_paren + 1,
                        _xi_split_c_arguments(text[open_paren + 1:close_paren])))
    return records


def _xi_exact_guard_call_count(body: str, symbol: str,
                               expected_args: tuple[str, ...]) -> int:
    expected = _xi_normalize_c_expr(symbol + '(' + ','.join(expected_args) + ')')
    return sum(
        1 for _, _, condition, _, _ in _xi_if_statement_records(body)
        if _xi_normalize_c_expr(condition) == expected
    )


def _xi_brace_depths(text: str) -> list[int]:
    brace_depth = 0
    depths = [0] * (len(text) + 1)
    for index, char in enumerate(text):
        depths[index] = brace_depth
        if char == '{':
            brace_depth += 1
        elif char == '}':
            brace_depth = max(0, brace_depth - 1)
    return depths


def _xi_top_level_match_positions(text: str, pattern: str) -> list[int]:
    depths = _xi_brace_depths(text)
    return [match.start() for match in re.finditer(pattern, text)
            if depths[match.start()] == 0]


def _xi_top_level_bare_call_positions(text: str, callee_pattern: str) -> list[int]:
    depths = _xi_brace_depths(text)
    positions = []
    pattern = rf'(?:^|[;}}])\s*(?P<call>\b(?:{callee_pattern})\s*\()'
    for match in re.finditer(pattern, text):
        position = match.start('call')
        if depths[position] == 0:
            positions.append(position)
    return positions


def _xi_top_level_call_present(text: str, symbol: str) -> bool:
    return bool(_xi_top_level_bare_call_positions(text, re.escape(symbol)))


def _xi_top_level_bare_return_positions(text: str, expression: str) -> list[int]:
    depths = _xi_brace_depths(text)
    positions = []
    pattern = rf'(?:^|[;}}])\s*(?P<return>\breturn{expression}\s*;)'
    for match in re.finditer(pattern, text):
        position = match.start('return')
        if depths[position] == 0:
            positions.append(position)
    return positions


def _xi_top_level_any_return_positions(text: str) -> list[int]:
    return _xi_top_level_bare_return_positions(text, r'(?:\s+[^;]+)?')


def _xi_top_level_call_reachable(text: str, symbol: str) -> bool:
    calls = _xi_top_level_bare_call_positions(text, re.escape(symbol))
    returns = _xi_top_level_any_return_positions(text)
    return any(not any(ret < call for ret in returns) for call in calls)


def _xi_top_level_call_precedes_return(text: str, symbol: str) -> bool:
    calls = _xi_top_level_bare_call_positions(text, re.escape(symbol))
    all_returns = _xi_top_level_any_return_positions(text)
    returns = _xi_top_level_bare_return_positions(text, r'(?:\s+true)?')
    return any(not any(ret < call for ret in all_returns) and
               any(call < ret for ret in returns) for call in calls)


def _xi_call_record_reachable(text: str, position: int,
                              terminators: set[str]) -> bool:
    """Reject calls preceded by an unconditional stop in their lexical branch."""
    depths = _xi_brace_depths(text)
    target_depth = depths[position]
    block_start = 0
    for index in range(position - 1, -1, -1):
        if text[index] == '{' and depths[index] + 1 == target_depth:
            block_start = index + 1
            break
    prefix = text[block_start:position]
    label_positions = _xi_top_level_match_positions(
        prefix, r'(?:^|[;}]|\n)\s*(?:case\b[^:]*|default)\s*:')
    if label_positions:
        label_start = label_positions[-1]
        colon = prefix.find(':', label_start)
        if colon >= 0:
            prefix = prefix[colon + 1:]
    if _xi_top_level_any_return_positions(prefix):
        return False
    return not any(
        _xi_top_level_bare_call_positions(prefix, re.escape(terminator))
        for terminator in terminators)


def _xi_reachable_activation_callers(
        aot_functions: tuple[tuple[str, str, str], ...],
        activation_callers: set[tuple[str, str]],
        terminators: set[str]) -> set[tuple[str, str]]:
    """Trace declared activation callers from the two CGen translation roots."""
    bodies = {(source_path, symbol): body
              for source_path, symbol, body in aot_functions}
    roots = {key for key in activation_callers
             if key[1] in {'xi_cgen_func', 'xi_cgen_coro_func'}}
    reachable = set(roots)
    pending = list(sorted(roots))
    while pending:
        body = bodies[pending.pop()]
        for key in sorted(activation_callers - reachable):
            calls = _xi_c_call_records(body, key[1])
            if any(_xi_call_record_reachable(body, position, terminators)
                   for position, _, _ in calls):
                reachable.add(key)
                pending.append(key)
    return reachable


def _xi_selector_matches(body: str, selector: str) -> list[tuple[int, int]]:
    root_value = r'\bv\s*->\s*op\b'
    selector_token = rf'\b{re.escape(selector)}\b'
    patterns = [
        rf'\bcase\s+{selector_token}\s*:',
        rf'{root_value}\s*==\s*{selector_token}',
        rf'{selector_token}\s*==\s*{root_value}',
    ]
    matches = []
    for pattern in patterns:
        matches.extend((match.start(), match.end()) for match in re.finditer(pattern, body))
    return sorted(set(matches))


def _xi_switch_statements(text: str) -> list[tuple[str, str]]:
    brace_depth = 0
    depths = [0] * (len(text) + 1)
    for index, char in enumerate(text):
        depths[index] = brace_depth
        if char == '{':
            brace_depth += 1
        elif char == '}':
            brace_depth = max(0, brace_depth - 1)
    switches = []
    for match in re.finditer(r'\bswitch\s*\(', text):
        if depths[match.start()] != 0:
            continue
        open_paren = text.find('(', match.start())
        close_paren = _xi_matching_delimiter(text, open_paren, '(', ')')
        if close_paren is None:
            continue
        cursor = close_paren + 1
        while cursor < len(text) and text[cursor].isspace():
            cursor += 1
        if cursor >= len(text) or text[cursor] != '{':
            continue
        close_brace = _xi_matching_delimiter(text, cursor, '{', '}')
        if close_brace is None:
            continue
        switches.append((text[open_paren + 1:close_paren],
                         text[cursor + 1:close_brace]))
    return switches


def _xi_switch_case_branches(body: str) -> dict[str, list[str]]:
    brace_depth = 0
    depths = [0] * (len(body) + 1)
    for index, char in enumerate(body):
        depths[index] = brace_depth
        if char == '{':
            brace_depth += 1
        elif char == '}':
            brace_depth = max(0, brace_depth - 1)
    labels = []
    for match in re.finditer(r'\bcase\s+(XI_[A-Z0-9_]+)\s*:|\bdefault\s*:', body):
        if depths[match.start()] == 0:
            labels.append((match.start(), match.end(), match.group(1)))
    branches = {}
    for index, (_, end, selector) in enumerate(labels):
        if selector is None:
            continue
        branch_end = labels[index + 1][0] if index + 1 < len(labels) else len(body)
        branches.setdefault(selector, []).append(body[end:branch_end])
    return branches


def _xi_switch_routes_to_owner(body: str, selector: str,
                               owner: str | None,
                               known_emitters: set[str] | None = None,
                               terminators: set[str] | None = None) -> bool:
    for discriminant, switch_body in _xi_switch_statements(body):
        if re.fullmatch(r'\s*v\s*->\s*op\s*', discriminant) is None:
            continue
        for branch in _xi_switch_case_branches(switch_body).get(selector, []):
            foreign_emitter = owner is not None and any(
                emitter != owner and _xi_c_call_present(branch, emitter)
                for emitter in known_emitters or set())
            if ((owner is None and _xi_terminal_emission_branch(
                    branch, known_emitters, terminators)) or
                    (owner is not None and not foreign_emitter and
                     _xi_top_level_call_precedes_return(branch, owner))):
                return True
    return False


def _xi_positive_emission_precedes_return(
        branch: str, return_pattern: str,
        known_emitters: set[str] | None = None,
        terminators: set[str] | None = None) -> bool:
    if _xi_top_level_match_positions(
            branch, r'\bctx\s*->\s*error\s*=\s*true\b'):
        return False
    semantic_emission_pattern = (
        r'(?!(?:emit_codegen_abort|emit_thread_spawn_abort_stmt|'
        r'emit_value_generated_line_reset|emit_value_source_line|'
        r'emit_debug_source_var_sync)\b)'
        r'(?:emit_[A-Za-z0-9_]*|[A-Za-z_][A-Za-z0-9_]*_emit_[A-Za-z0-9_]*)'
    )
    if known_emitters is not None:
        if not known_emitters:
            return False
        semantic_emission_pattern = '(?:' + '|'.join(
            re.escape(symbol) for symbol in sorted(known_emitters)) + ')'
    semantic_emissions = [position for position in _xi_top_level_bare_call_positions(
        branch, semantic_emission_pattern)
        if re.match(r'[^;]*\bout\b[^;]*\)', branch[position:])]
    any_emissions = [position for position in _xi_top_level_bare_call_positions(
        branch,
        r'(?:emit_[A-Za-z0-9_]*|[A-Za-z_][A-Za-z0-9_]*_emit_[A-Za-z0-9_]*)')
        if re.match(r'[^;]*\bout\b[^;]*\)', branch[position:])]
    generated_text = [position for position in _xi_top_level_bare_call_positions(
        branch, r'fprintf')
        if re.match(r'fprintf\s*\(\s*out\b', branch[position:])]
    emissions = semantic_emissions
    if known_emitters is None and not emissions and any_emissions and generated_text:
        emissions = [min(any_emissions + generated_text)]
    return_expression = r'' if return_pattern == r'\breturn\s*;' else r'\s+true'
    returns = _xi_top_level_bare_return_positions(branch, return_expression)
    all_returns = _xi_top_level_any_return_positions(branch)
    terminal_positions = []
    for symbol in terminators or set():
        terminal_positions.extend(
            _xi_top_level_bare_call_positions(branch, re.escape(symbol)))
    return any(not any(stop < emission for stop in all_returns + terminal_positions) and
               any(emission < ret for ret in returns) for emission in emissions)


def _xi_terminal_emission_branch(
        branch: str, known_emitters: set[str] | None = None,
        terminators: set[str] | None = None) -> bool:
    return _xi_positive_emission_precedes_return(
        branch, r'\breturn\s*;', known_emitters, terminators)


def _xi_strip_balanced_parentheses(text: str) -> str:
    value = text.strip()
    while value.startswith('('):
        end = _xi_matching_delimiter(value, 0, '(', ')')
        if end != len(value) - 1:
            break
        value = value[1:-1].strip()
    return value


def _xi_split_top_level(text: str, operator: str) -> list[str]:
    parts = []
    start = 0
    paren_depth = 0
    index = 0
    while index < len(text):
        if text[index] == '(':
            paren_depth += 1
        elif text[index] == ')':
            paren_depth = max(0, paren_depth - 1)
        elif paren_depth == 0 and text.startswith(operator, index):
            parts.append(text[start:index])
            index += len(operator)
            start = index
            continue
        index += 1
    parts.append(text[start:])
    return parts


def _xi_exact_positive_selector(term: str) -> str | None:
    value = _xi_strip_balanced_parentheses(term)
    root_value = r'v\s*->\s*op'
    selector = r'(XI_[A-Z0-9_]+)'
    for pattern in (rf'{root_value}\s*==\s*{selector}',
                    rf'{selector}\s*==\s*{root_value}'):
        match = re.fullmatch(pattern, value)
        if match:
            return match.group(1)
    return None


def _xi_positive_selector_disjunction(condition: str) -> set[str] | None:
    value = _xi_strip_balanced_parentheses(condition)
    selectors = []
    for term in _xi_split_top_level(value, '||'):
        selector = _xi_exact_positive_selector(term)
        if selector is None:
            return None
        selectors.append(selector)
    return set(selectors)


def _xi_condition_has_selector_disjunct(condition: str, selector: str) -> bool:
    value = _xi_strip_balanced_parentheses(condition)
    return any(_xi_exact_positive_selector(term) == selector
               for term in _xi_split_top_level(value, '||'))


def _xi_condition_has_static_false_gate(condition: str) -> bool:
    value = _xi_strip_balanced_parentheses(condition)
    return any(_xi_strip_balanced_parentheses(term) in {'false', '0'}
               for term in _xi_split_top_level(value, '&&'))


def _xi_direct_selector_present(
        body: str, selector: str, known_emitters: set[str] | None = None,
        terminators: set[str] | None = None,
        allow_nested_emitter_route: bool = False) -> bool:
    if allow_nested_emitter_route and known_emitters is not None and any(
            _xi_selector_routes_to_owner(body, selector, emitter, known_emitters)
            for emitter in known_emitters):
        return True
    if _xi_switch_routes_to_owner(
            body, selector, None, known_emitters, terminators):
        return True
    for condition, then_branch, _ in _xi_if_statements(body, top_level_only=True):
        if (_xi_condition_has_selector_disjunct(condition, selector) and
                _xi_terminal_emission_branch(
                    then_branch, known_emitters, terminators)):
            return True
    return False


def _xi_guarded_selector_present(
        body: str, selector: str, known_emitters: set[str] | None = None,
        terminators: set[str] | None = None) -> bool:
    guard = re.match(
        rf'^\s*if\s*\(\s*!\s*v\s*\|\|\s*v\s*->\s*op\s*!=\s*'
        rf'\b{re.escape(selector)}\b\s*\)\s*return\s+false\s*;',
        body,
    )
    return guard is not None and _xi_positive_emission_precedes_return(
        body[guard.end():], r'\breturn\s+true\s*;', known_emitters, terminators)


def _xi_if_statement_records(
        text: str, *, top_level_only: bool = False
) -> list[tuple[int, int, str, str, str]]:
    statements = []
    depths = _xi_brace_depths(text)
    for match in re.finditer(r'\bif\s*\(', text):
        if top_level_only and depths[match.start()] != 0:
            continue
        open_paren = text.find('(', match.start())
        close_paren = _xi_matching_delimiter(text, open_paren, '(', ')')
        if close_paren is None:
            continue
        cursor = close_paren + 1
        while cursor < len(text) and text[cursor].isspace():
            cursor += 1
        if cursor < len(text) and text[cursor] == '{':
            then_end = _xi_matching_delimiter(text, cursor, '{', '}')
            if then_end is None:
                continue
            then_branch = text[cursor + 1:then_end]
            after_then = then_end + 1
        else:
            then_end = text.find(';', cursor)
            if then_end < 0:
                continue
            then_branch = text[cursor:then_end + 1]
            after_then = then_end + 1
        while after_then < len(text) and text[after_then].isspace():
            after_then += 1
        else_branch = ''
        statement_end = after_then
        if re.match(r'else\b', text[after_then:]):
            else_cursor = after_then + 4
            while else_cursor < len(text) and text[else_cursor].isspace():
                else_cursor += 1
            if else_cursor < len(text) and text[else_cursor] == '{':
                else_end = _xi_matching_delimiter(text, else_cursor, '{', '}')
                if else_end is not None:
                    else_branch = text[else_cursor + 1:else_end]
                    statement_end = else_end + 1
            else:
                else_end = text.find(';', else_cursor)
                if else_end >= 0:
                    else_branch = text[else_cursor:else_end + 1]
                    statement_end = else_end + 1
        statements.append((match.start(), statement_end,
                           text[open_paren + 1:close_paren],
                           then_branch, else_branch))
    return statements


def _xi_if_statements(text: str, *, top_level_only: bool = False
                      ) -> list[tuple[str, str, str]]:
    return [(condition, then_branch, else_branch)
            for _, _, condition, then_branch, else_branch in
            _xi_if_statement_records(text, top_level_only=top_level_only)]


def _xi_root_op_selectors(condition: str) -> set[str]:
    return _xi_positive_selector_disjunction(condition) or set()


def _xi_selector_routes_to_owner(body: str, selector: str, owner: str,
                                 known_emitters: set[str] | None = None) -> bool:
    if _xi_switch_routes_to_owner(
            body, selector, owner, known_emitters=known_emitters):
        return True
    for _, _, condition, then_branch, else_branch in \
            _xi_if_statement_records(body, top_level_only=True):
        outer_selectors = _xi_root_op_selectors(condition)
        if selector not in outer_selectors:
            continue
        if outer_selectors == {selector} and _xi_c_call_present(then_branch, owner):
            foreign_emitter = any(
                emitter != owner and _xi_c_call_present(then_branch, emitter)
                for emitter in known_emitters or set())
            if (not foreign_emitter and
                    _xi_top_level_call_precedes_return(then_branch, owner)):
                return True
        for nested_start, nested_end, nested_condition, nested_then, nested_else in \
                _xi_if_statement_records(then_branch, top_level_only=True):
            nested_selectors = _xi_root_op_selectors(nested_condition)
            routed = (
                selector in nested_selectors and
                _xi_top_level_call_reachable(nested_then, owner) and
                not any(emitter != owner and
                        _xi_c_call_present(nested_then, emitter)
                        for emitter in known_emitters or set())
            ) or (
                selector not in nested_selectors and
                outer_selectors == nested_selectors | {selector} and
                _xi_top_level_call_reachable(nested_else, owner) and
                not any(emitter != owner and
                        _xi_c_call_present(nested_else, emitter)
                        for emitter in known_emitters or set())
            )
            if routed:
                returns = _xi_top_level_bare_return_positions(then_branch, r'')
                if (not any(ret < nested_start for ret in returns) and
                        any(nested_end < ret for ret in returns)):
                    return True
    return False


def _xi_require_function_body(source_root: Path, source_path: str, symbol: str,
                              context: str,
                              source_text: dict[str, str]) -> str:
    del source_root
    text = source_text.get(source_path)
    if text is None:
        die(f"{context}: consumer source is outside the validated xi_cgen.c "
            f"translation unit: {source_path}")
    ranges = _xi_c_file_scope_function_body_ranges(text).get(symbol, [])
    if len(ranges) != 1:
        die(f"{context}: expected one file-scope definition of {source_path}::{symbol}, "
            f"found {len(ranges)}")
    start, end = ranges[0]
    if _xi_conditional_preprocessor_depth(text, start) != 0:
        die(f"{context}: governed consumer function {source_path}::{symbol} "
            "is enclosed by conditional preprocessing")
    directive_source = _xi_c_source_without_literals(
        text, blank_preprocessor=False)[start:end]
    if re.search(r'^\s*#\s*(?:if|ifdef|ifndef|elif|else|endif)\b',
                 directive_source, re.MULTILINE):
        die(f"{context}: governed consumer function {source_path}::{symbol} "
            "contains conditional preprocessing")
    body = _xi_c_source_without_literals(text)[start:end]
    if any(_xi_condition_has_static_false_gate(condition)
           for condition, _, _ in _xi_if_statements(body)):
        die(f"{context}: governed consumer function {source_path}::{symbol} "
            "contains a statically false conditional branch")
    return body


def _xi_validate_activation_census(
        source_root: Path, binding: XiConsumerBinding,
        aot_functions: list[tuple[str, str, str]], context: str,
        source_text: dict[str, str], terminators: set[str],
        reachable_activation_callers: set[tuple[str, str]]) -> None:
    if not binding.activations:
        return
    declared: Counter[tuple[str, str, tuple[str, ...]]] = Counter()
    for activation in binding.activations:
        _xi_require_function_body(source_root, activation.source_path,
                                  activation.caller, context, source_text)
        if (activation.source_path, activation.caller) not in \
                reachable_activation_callers:
            die(f"{context}: activation census for {activation.source_path}::"
                f"{activation.caller} differs from lowering.def: caller is not "
                "reachable from a CGen root")
        declared[(activation.source_path, activation.caller,
                  tuple(_xi_normalize_c_expr(arg) for arg in activation.args))] += \
            activation.count
    discovered: Counter[tuple[str, str, tuple[str, ...]]] = Counter()
    for source_path, caller, body in aot_functions:
        calls = _xi_c_call_records(body, binding.symbol)
        if not calls:
            continue
        for position, _, args in calls:
            if not _xi_call_record_reachable(body, position, terminators):
                continue
            discovered[(source_path, caller, args)] += 1
    if discovered != declared:
        missing = sorted((key, count) for key, count in (declared - discovered).items())
        extra = sorted((key, count) for key, count in (discovered - declared).items())
        details = []
        if missing:
            details.append('missing ' + ', '.join(
                f'{path}::{caller}{args} x{count}'
                for (path, caller, args), count in missing))
        if extra:
            details.append('undeclared ' + ', '.join(
                f'{path}::{caller}{args} x{count}'
                for (path, caller, args), count in extra))
        die(f"{context}: activation census for {binding.source_path}::{binding.symbol} "
            "differs from lowering.def: " + '; '.join(details))
    if binding.witness_kind == 'guarded-selector':
        for activation in binding.activations:
            caller_body = next((body for source_path, symbol, body in aot_functions
                                if source_path == activation.source_path and
                                symbol == activation.caller), None)
            if caller_body is None:
                die(f"{context}: activation caller is missing: "
                    f"{activation.source_path}::{activation.caller}")
            actual = _xi_exact_guard_call_count(
                caller_body, binding.symbol,
                tuple(_xi_normalize_c_expr(arg) for arg in activation.args))
            if actual != activation.count:
                die(f"{context}: guarded activation must be the complete if predicate in "
                    f"{activation.source_path}::{activation.caller}; expected "
                    f"{activation.count}, found {actual}")


def _xi_output_call_matches(actual: tuple[str, ...],
                            expected: XiConsumerOutputCallWitness) -> bool:
    wanted = tuple(_xi_normalize_c_expr(arg) for arg in expected.args)
    return actual[:len(wanted)] == wanted if expected.prefix else actual == wanted


def _xi_validate_output_sequences(binding: XiConsumerBinding, body: str,
                                  context: str, terminators: set[str]) -> None:
    for sequence in binding.output_sequences:
        records = []
        for call in sequence.calls:
            records.append([
                (start, end, args)
                for start, end, args in _xi_c_call_records(body, call.symbol)
                if _xi_output_call_matches(args, call)
            ])
        matches = 0
        for candidate in records[0]:
            if not _xi_call_record_reachable(body, candidate[0], terminators):
                continue
            previous = candidate
            complete = True
            for following in records[1:]:
                adjacent = next((record for record in following
                                 if record[0] > previous[1] and
                                 re.fullmatch(r'\s*;\s*', body[previous[1]:record[0]])), None)
                if adjacent is None:
                    complete = False
                    break
                previous = adjacent
            if complete:
                matches += 1
        if matches != sequence.count:
            die(f"{context}: {binding.source_path}::{binding.symbol} output sequence "
                f"expected {sequence.count} exact occurrence(s), found {matches}")


def _xi_validate_structural_consumer(source_root: Path, binding: XiConsumerBinding,
                                     context: str,
                                     source_text: dict[str, str],
                                     terminators: set[str]) -> None:
    body = _xi_require_function_body(source_root, binding.source_path,
                                     binding.symbol, context, source_text)
    common = ('XiPhi', 'phis', 'next', 'emit_phi_ref')
    category_tokens = {
        'edge-parallel-copy': ('emit_phi_tmp_ref', 'emit_phi_incoming_as_rep',
                               'cg_phi_copy_should_emit'),
        'sync-storage': ('cg_phi_has_storage', 'emit_phi_tmp_ref'),
        'coroutine-frame-storage': ('cg_coro_phi_needs_frame', 'cg_coro_decl_ctype'),
        'coroutine-local-storage': ('cg_phi_has_storage', 'cg_coro_phi_needs_frame',
                                    'emit_phi_tmp_ref'),
    }
    required = common + category_tokens[binding.structural_category]
    missing = [token for token in required
               if re.search(rf'\b{re.escape(token)}\b', body) is None]
    if missing:
        die(f"{context}: structural owner {binding.source_path}::{binding.symbol} "
            f"lacks {binding.structural_category} witness token(s): {', '.join(missing)}")
    _xi_validate_output_sequences(binding, body, context, terminators)


def xi_lowering_consumer_source_paths(entries: list[XiLoweringDef]) -> set[str]:
    paths = {
        binding.source_path
        for entry in entries
        for bindings in entry.target_consumers.values()
        for binding in bindings
    }
    paths.update(
        router.source_path
        for entry in entries
        for bindings in entry.target_consumers.values()
        for binding in bindings
        for router in binding.routers
    )
    paths.update(
        activation.source_path
        for entry in entries
        for bindings in entry.target_consumers.values()
        for binding in bindings
        for activation in binding.activations
    )
    paths.update(
        emitter.source_path
        for entry in entries
        for bindings in entry.target_consumers.values()
        for binding in bindings
        for emitter in binding.emitters
    )
    paths.update(
        predicate.source_path
        for entry in entries
        for bindings in entry.target_consumers.values()
        for binding in bindings
        for predicate in binding.predicates
    )
    paths.update(
        predicate.domain_source_path
        for entry in entries
        for bindings in entry.target_consumers.values()
        for binding in bindings
        for predicate in binding.predicates
        if predicate.domain_source_path is not None
    )
    return paths


@dataclass(frozen=True)
class XiFileSnapshot:
    path: str
    identity: tuple[int, ...]
    data: bytes


@dataclass(frozen=True)
class XiLoweringValidationSnapshot:
    sources: tuple[tuple[str, bytes], ...]
    include_directories: tuple[str, ...] = ()
    directories: tuple[tuple[str, tuple[str, ...]], ...] = ()
    source_identities: tuple[tuple[str, tuple[int, ...]], ...] = ()
    directory_identities: tuple[tuple[str, tuple[int, ...]], ...] = ()

    def source_bytes(self) -> dict[str, bytes]:
        return dict(self.sources)

    def source_text(self) -> dict[str, str]:
        texts = {}
        for relative, data in self.sources:
            try:
                texts[relative] = data.decode('utf-8', errors='strict')
            except UnicodeDecodeError as error:
                die(f"xi-lowering: source is not UTF-8: {relative}: {error}")
        return texts

    def fingerprint(self) -> str:
        digest = hashlib.sha256()
        digest.update(b'xi-cgen-quoted-include-directories/1\0')
        for relative in self.include_directories:
            digest.update(relative.encode('utf-8'))
            digest.update(b'\0')
        digest.update(b'xi-cgen-quoted-include-directory-entries/1\0')
        for relative, entries in self.directories:
            digest.update(relative.encode('utf-8'))
            digest.update(b'\0')
            for entry in entries:
                digest.update(entry.encode('utf-8'))
                digest.update(b'\0')
        digest.update(b'xi-cgen-quoted-include-sources/1\0')
        for relative, data in self.sources:
            digest.update(relative.encode('utf-8'))
            digest.update(b'\0')
            digest.update(len(data).to_bytes(8, 'big'))
            digest.update(data)
        return digest.hexdigest()

    def identity_fingerprint(self) -> str:
        digest = hashlib.sha256()
        digest.update(b'xi-cgen-closure-identities/1\0')
        for relative, identity in self.source_identities:
            digest.update(relative.encode('utf-8'))
            digest.update(b'\0')
            digest.update(repr(identity).encode('ascii'))
            digest.update(b'\0')
        for relative, identity in self.directory_identities:
            digest.update(relative.encode('utf-8'))
            digest.update(b'\0')
            digest.update(repr(identity).encode('ascii'))
            digest.update(b'\0')
        return digest.hexdigest()


@dataclass(frozen=True)
class XiAotDiscoverySnapshot:
    sources: tuple[tuple[str, bytes], ...]
    directories: tuple[tuple[str, tuple[str, ...]], ...]
    source_identities: tuple[tuple[str, tuple[int, ...]], ...] = ()
    directory_identities: tuple[tuple[str, tuple[int, ...]], ...] = ()

    def source_text(self) -> dict[str, str]:
        texts = {}
        for relative, data in self.sources:
            try:
                texts[relative] = data.decode('utf-8', errors='strict')
            except UnicodeDecodeError as error:
                die(f"xi-lowering: discovery source is not UTF-8: {relative}: {error}")
        return texts

    def fingerprint(self) -> str:
        digest = hashlib.sha256()
        digest.update(b'xi-aot-discovery-sources/1\0')
        for relative, data in self.sources:
            digest.update(relative.encode('utf-8'))
            digest.update(b'\0')
            digest.update(len(data).to_bytes(8, 'big'))
            digest.update(data)
        digest.update(b'xi-aot-discovery-directories/1\0')
        for relative, entries in self.directories:
            digest.update(relative.encode('utf-8'))
            digest.update(b'\0')
            for entry in entries:
                digest.update(entry.encode('utf-8'))
                digest.update(b'\0')
        return digest.hexdigest()

    def identity_fingerprint(self) -> str:
        digest = hashlib.sha256()
        digest.update(b'xi-aot-discovery-identities/1\0')
        for relative, identity in self.source_identities:
            digest.update(relative.encode('utf-8'))
            digest.update(b'\0')
            digest.update(repr(identity).encode('ascii'))
            digest.update(b'\0')
        for relative, identity in self.directory_identities:
            digest.update(relative.encode('utf-8'))
            digest.update(b'\0')
            digest.update(repr(identity).encode('ascii'))
            digest.update(b'\0')
        return digest.hexdigest()


@functools.lru_cache(maxsize=16)
def _xi_aot_discovery_functions(
        snapshot: XiAotDiscoverySnapshot) -> tuple[tuple[str, str, str], ...]:
    functions = []
    for relative, data in snapshot.sources:
        for symbol, bodies in _xi_aot_source_functions(data):
            functions.extend((relative, symbol, body) for body in bodies)
    return tuple(functions)


@functools.lru_cache(maxsize=512)
def _xi_aot_source_functions(
        data: bytes) -> tuple[tuple[str, tuple[str, ...]], ...]:
    text = data.decode('utf-8', errors='strict')
    return tuple(sorted(
        (symbol, tuple(bodies))
        for symbol, bodies in _xi_c_file_scope_function_bodies(text).items()))


def _xi_repository_relative_name(source_root: Path, path: Path,
                                 context: str) -> str:
    """Return one canonical in-repository name without resolving any component."""
    source_root = Path(os.path.abspath(os.fspath(source_root)))
    raw = Path(os.fspath(path))
    if ('..' in raw.parts or
            (os.name != 'nt' and '\\' in os.fspath(path))):
        die(f"{context} uses parent traversal or backslashes: {path}")
    lexical = Path(os.path.abspath(os.fspath(
        raw if raw.is_absolute() else source_root / raw)))
    try:
        relative = lexical.relative_to(source_root)
    except ValueError:
        die(f"{context} escapes repository root: {path}")
    if (not relative.parts or
            any(component in {'', '.', '..'} for component in relative.parts)):
        die(f"{context} is not a canonical repository file name: {path}")
    return relative.as_posix()


def _xi_repository_relative_parts(relative: str, context: str) -> tuple[str, ...]:
    path = Path(relative)
    if (not relative or '\\' in relative or path.is_absolute() or
            path.as_posix() != relative or
            any(component in {'', '.', '..'} for component in path.parts)):
        die(f"{context} is not a canonical repository-relative path: {relative}")
    return path.parts


def _xi_posix_open_repository_object(
        source_root: Path, relative: str, context: str, *, directory: bool,
        before_open_hook=None) -> int:
    """Open one object through anchored descriptors without following symlinks."""
    parts = _xi_repository_relative_parts(relative, context)
    root_flags = (os.O_RDONLY | getattr(os, 'O_CLOEXEC', 0) |
                  getattr(os, 'O_DIRECTORY', 0) |
                  getattr(os, 'O_NOFOLLOW', 0))
    if not getattr(os, 'O_NOFOLLOW', 0) or os.open not in os.supports_dir_fd:
        die("xi-lowering: descriptor-relative no-follow opens are unavailable")
    opened = [os.open(os.fspath(source_root), root_flags)]
    try:
        if not stat.S_ISDIR(os.fstat(opened[-1]).st_mode):
            die(f"{context} repository root is not a directory: {source_root}")
        for index, component in enumerate(parts):
            final = index + 1 == len(parts)
            if before_open_hook is not None:
                before_open_hook(relative, index, component, final)
            expect_directory = not final or directory
            flags = (os.O_RDONLY | getattr(os, 'O_CLOEXEC', 0) |
                     getattr(os, 'O_NOFOLLOW', 0))
            if expect_directory:
                flags |= getattr(os, 'O_DIRECTORY', 0)
            else:
                flags |= getattr(os, 'O_NONBLOCK', 0)
            try:
                child = os.open(component, flags, dir_fd=opened[-1])
            except OSError as error:
                die(f"{context} cannot open {relative} without following links: {error}")
            opened.append(child)
            child_stat = os.fstat(child)
            expected = stat.S_ISDIR(child_stat.st_mode) if expect_directory \
                else stat.S_ISREG(child_stat.st_mode)
            if not expected:
                kind = 'directory' if expect_directory else 'regular file'
                die(f"{context} is not a {kind}: {relative}")
        final = opened[-1]
        for parent in reversed(opened[:-1]):
            os.close(parent)
        opened = [final]
        return opened.pop()
    except BaseException:
        for descriptor in reversed(opened):
            try:
                os.close(descriptor)
            except OSError:
                pass
        raise


@functools.lru_cache(maxsize=1)
def _xi_windows_file_info_type():
    import ctypes
    from ctypes import wintypes

    class ByHandleFileInformation(ctypes.Structure):
        _fields_ = [
            ('dwFileAttributes', wintypes.DWORD),
            ('ftCreationTime', wintypes.FILETIME),
            ('ftLastAccessTime', wintypes.FILETIME),
            ('ftLastWriteTime', wintypes.FILETIME),
            ('dwVolumeSerialNumber', wintypes.DWORD),
            ('nFileSizeHigh', wintypes.DWORD),
            ('nFileSizeLow', wintypes.DWORD),
            ('nNumberOfLinks', wintypes.DWORD),
            ('nFileIndexHigh', wintypes.DWORD),
            ('nFileIndexLow', wintypes.DWORD),
        ]

    return ByHandleFileInformation


@functools.lru_cache(maxsize=1)
def _xi_windows_kernel32():
    import ctypes
    from ctypes import wintypes

    info_type = _xi_windows_file_info_type()
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.GetFileInformationByHandle.argtypes = [
        wintypes.HANDLE, ctypes.POINTER(info_type)]
    kernel32.GetFileInformationByHandle.restype = wintypes.BOOL
    kernel32.ReadFile.argtypes = [
        wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    kernel32.ReadFile.restype = wintypes.BOOL
    kernel32.GetFileType.argtypes = [wintypes.HANDLE]
    kernel32.GetFileType.restype = wintypes.DWORD
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    return kernel32


def _xi_windows_handle_info(handle: int):
    import ctypes
    from ctypes import wintypes

    info = _xi_windows_file_info_type()()
    if not _xi_windows_kernel32().GetFileInformationByHandle(
            wintypes.HANDLE(handle), ctypes.byref(info)):
        raise ctypes.WinError(ctypes.get_last_error())
    return info


def _xi_windows_component_access(expect_directory: bool | None) -> int:
    file_read_attributes = 0x0080
    generic_read = 0x80000000
    return file_read_attributes if expect_directory is True \
        else generic_read | file_read_attributes


def _xi_windows_open_repository_object(
        source_root: Path, relative: str, context: str, *, directory: bool | None,
        before_open_hook=None) -> tuple[list[int], object]:
    """Lock every Windows path component and reject reparse points."""
    import ctypes
    from ctypes import wintypes

    parts = _xi_repository_relative_parts(relative, context)
    kernel32 = _xi_windows_kernel32()
    create_file = kernel32.CreateFileW
    close_handle = kernel32.CloseHandle
    invalid = ctypes.c_void_p(-1).value
    file_attribute_directory = 0x00000010
    file_attribute_reparse_point = 0x00000400
    file_flag_backup_semantics = 0x02000000
    file_flag_open_reparse_point = 0x00200000
    file_read_attributes = 0x0080
    file_share_read = 0x00000001
    open_existing = 3
    handles: list[int] = []
    cursor = Path(os.path.abspath(os.fspath(source_root)))
    try:
        root_handle = create_file(
            os.fspath(cursor), file_read_attributes, file_share_read, None,
            open_existing,
            file_flag_open_reparse_point | file_flag_backup_semantics, None)
        if root_handle == invalid:
            raise ctypes.WinError(ctypes.get_last_error())
        handles.append(root_handle)
        root_info = _xi_windows_handle_info(root_handle)
        if (root_info.dwFileAttributes & file_attribute_reparse_point or
                not root_info.dwFileAttributes & file_attribute_directory):
            die(f"{context} repository root is not a regular directory: {cursor}")
        for index, component in enumerate(parts):
            final = index + 1 == len(parts)
            if before_open_hook is not None:
                before_open_hook(relative, index, component, final)
            cursor /= component
            expect_directory = True if not final else directory
            access = _xi_windows_component_access(expect_directory)
            flags = file_flag_open_reparse_point | file_flag_backup_semantics
            handle = create_file(
                os.fspath(cursor), access, file_share_read, None, open_existing,
                flags, None)
            if handle == invalid:
                raise ctypes.WinError(ctypes.get_last_error())
            handles.append(handle)
            info = _xi_windows_handle_info(handle)
            if info.dwFileAttributes & file_attribute_reparse_point:
                die(f"{context} uses a reparse-point path component: {cursor}")
            is_directory = bool(info.dwFileAttributes & file_attribute_directory)
            if expect_directory is not None and is_directory != expect_directory:
                kind = 'directory' if expect_directory else 'regular file'
                die(f"{context} is not a {kind}: {cursor}")
            if not is_directory and kernel32.GetFileType(
                    wintypes.HANDLE(handle)) != 0x0001:
                die(f"{context} is not a regular disk file: {cursor}")
        return handles, info
    except BaseException:
        for handle in reversed(handles):
            close_handle(wintypes.HANDLE(handle))
        raise


def _xi_stable_file_identity(file_stat) -> tuple[int, ...]:
    return (file_stat.st_dev, file_stat.st_ino, file_stat.st_mode,
            file_stat.st_size, file_stat.st_mtime_ns, file_stat.st_ctime_ns)


def _xi_windows_stable_file_identity(info) -> tuple[int, ...]:
    return (
        info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
        info.dwFileAttributes, info.nFileSizeHigh, info.nFileSizeLow,
        info.ftLastWriteTime.dwHighDateTime,
        info.ftLastWriteTime.dwLowDateTime,
    )


def _xi_windows_read_open_file(handle: int, before, relative: str,
                               context: str) -> bytes:
    import ctypes
    from ctypes import wintypes

    chunks = []
    read_file = _xi_windows_kernel32().ReadFile
    while True:
        buffer = ctypes.create_string_buffer(65536)
        count = wintypes.DWORD()
        if not read_file(wintypes.HANDLE(handle), buffer, len(buffer),
                         ctypes.byref(count), None):
            raise ctypes.WinError(ctypes.get_last_error())
        if count.value == 0:
            break
        chunks.append(buffer.raw[:count.value])
    after = _xi_windows_handle_info(handle)
    before_identity = _xi_windows_stable_file_identity(before)
    after_identity = _xi_windows_stable_file_identity(after)
    if before_identity != after_identity:
        die(f"{context} changed while it was being captured: {relative}")
    return b''.join(chunks)


def _xi_secure_repository_file_snapshot(
        source_root: Path, relative: str, context: str,
        before_open_hook=None) -> tuple[tuple[int, ...], bytes]:
    """Capture one stable file identity and bytes through bound handles."""
    source_root = Path(os.path.abspath(os.fspath(source_root)))
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes

        handles, before = _xi_windows_open_repository_object(
            source_root, relative, context, directory=False,
            before_open_hook=before_open_hook)
        handle = handles[-1]
        try:
            data = _xi_windows_read_open_file(handle, before, relative, context)
            return _xi_windows_stable_file_identity(before), data
        finally:
            for open_handle in reversed(handles):
                _xi_windows_kernel32().CloseHandle(wintypes.HANDLE(open_handle))

    file_fd = _xi_posix_open_repository_object(
        source_root, relative, context, directory=False,
        before_open_hook=before_open_hook)
    try:
        before = os.fstat(file_fd)
        chunks = []
        while True:
            chunk = os.read(file_fd, 65536)
            if not chunk:
                break
            chunks.append(chunk)
        after = os.fstat(file_fd)
        if _xi_stable_file_identity(before) != _xi_stable_file_identity(after):
            die(f"{context} changed while it was being captured: {relative}")
        return _xi_stable_file_identity(before), b''.join(chunks)
    finally:
        os.close(file_fd)


def _xi_secure_repository_file_bytes(
        source_root: Path, relative: str, context: str,
        before_open_hook=None) -> bytes:
    """Read one stable regular file through a no-follow, component-bound handle."""
    return _xi_secure_repository_file_snapshot(
        source_root, relative, context, before_open_hook)[1]


def _xi_capture_repository_file(
        source_root: Path, relative: str, context: str) -> XiFileSnapshot:
    identity, data = _xi_secure_repository_file_snapshot(
        source_root, relative, context)
    return XiFileSnapshot(relative, identity, data)


def _xi_capture_aot_discovery_census_posix(
        source_root: Path, before_open_hook=None) -> XiAotDiscoverySnapshot:
    sources: dict[str, bytes] = {}
    source_identities: dict[str, tuple[int, ...]] = {}
    directories: list[tuple[str, tuple[str, ...]]] = []
    directory_identities: dict[str, tuple[int, ...]] = {}
    root_fd = _xi_posix_open_repository_object(
        source_root, 'src/aot', 'xi-lowering: AOT discovery root', directory=True,
        before_open_hook=before_open_hook)

    def capture(directory_fd: int, relative_directory: str) -> None:
        before = os.fstat(directory_fd)
        try:
            entries = tuple(sorted(os.listdir(directory_fd)))
        except OSError as error:
            die(f"xi-lowering: cannot enumerate AOT discovery directory "
                f"{relative_directory}: {error}")
        directories.append((relative_directory, entries))
        for child in entries:
            if child in {'', '.', '..'} or '/' in child or '\\' in child:
                die(f"xi-lowering: invalid AOT discovery entry: {child}")
            relative = f"{relative_directory}/{child}"
            if before_open_hook is not None:
                parts = Path(relative).parts
                before_open_hook(relative, len(parts) - 1, child, True)
            flags = (os.O_RDONLY | getattr(os, 'O_CLOEXEC', 0) |
                     getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0))
            try:
                child_fd = os.open(child, flags, dir_fd=directory_fd)
            except OSError as error:
                die(f"xi-lowering: cannot open AOT discovery entry {relative} "
                    f"without following links: {error}")
            try:
                child_stat = os.fstat(child_fd)
                if stat.S_ISDIR(child_stat.st_mode):
                    capture(child_fd, relative)
                elif stat.S_ISREG(child_stat.st_mode) and child.endswith('.c'):
                    file_before = _xi_stable_file_identity(child_stat)
                    chunks = []
                    while True:
                        chunk = os.read(child_fd, 65536)
                        if not chunk:
                            break
                        chunks.append(chunk)
                    file_after = _xi_stable_file_identity(os.fstat(child_fd))
                    if file_before != file_after:
                        die("xi-lowering: AOT discovery source changed while it "
                            f"was captured: {relative}")
                    data = b''.join(chunks)
                    try:
                        data.decode('utf-8', errors='strict')
                    except UnicodeDecodeError as error:
                        die(f"xi-lowering: cannot capture AOT discovery source "
                            f"{relative}: {error}")
                    sources[relative] = data
                    source_identities[relative] = file_before
                elif not stat.S_ISREG(child_stat.st_mode):
                    die(f"xi-lowering: AOT discovery entry is not a regular "
                        f"file or directory: {relative}")
            finally:
                os.close(child_fd)
        after = os.fstat(directory_fd)
        if _xi_stable_file_identity(before) != _xi_stable_file_identity(after):
            die("xi-lowering: AOT discovery directory changed while it was "
                f"captured: {relative_directory}")
        directory_identities[relative_directory] = _xi_stable_file_identity(before)

    try:
        capture(root_fd, 'src/aot')
    finally:
        os.close(root_fd)
    return XiAotDiscoverySnapshot(
        tuple(sorted(sources.items())), tuple(sorted(directories)),
        tuple(sorted(source_identities.items())),
        tuple(sorted(directory_identities.items())))


def _xi_capture_aot_discovery_census_windows(
        source_root: Path, before_open_hook=None) -> XiAotDiscoverySnapshot:
    import ctypes
    from ctypes import wintypes

    sources: dict[str, bytes] = {}
    source_identities: dict[str, tuple[int, ...]] = {}
    directories: list[tuple[str, tuple[str, ...]]] = []
    directory_identities: dict[str, tuple[int, ...]] = {}
    directory_attribute = 0x00000010

    def capture(relative_directory: str, handles=None, before=None) -> None:
        if handles is None:
            handles, before = _xi_windows_open_repository_object(
                source_root, relative_directory,
                'xi-lowering: AOT discovery directory', directory=True,
                before_open_hook=before_open_hook)
        try:
            path = source_root / Path(relative_directory)
            entries = tuple(sorted(os.listdir(path)))
            directories.append((relative_directory, entries))
            for child in entries:
                if child in {'', '.', '..'} or '/' in child or '\\' in child:
                    die(f"xi-lowering: invalid AOT discovery entry: {child}")
                relative = f"{relative_directory}/{child}"
                child_handles, info = _xi_windows_open_repository_object(
                    source_root, relative, 'xi-lowering: AOT discovery entry',
                    directory=None, before_open_hook=before_open_hook)
                is_directory = bool(info.dwFileAttributes & directory_attribute)
                if is_directory:
                    capture(relative, child_handles, info)
                elif child.endswith('.c'):
                    try:
                        data = _xi_windows_read_open_file(
                            child_handles[-1], info, relative,
                            'xi-lowering: AOT discovery source')
                        try:
                            data.decode('utf-8', errors='strict')
                        except UnicodeDecodeError as error:
                            die(f"xi-lowering: cannot capture AOT discovery source "
                                f"{relative}: {error}")
                        sources[relative] = data
                        source_identities[relative] = \
                            _xi_windows_stable_file_identity(info)
                    finally:
                        for handle in reversed(child_handles):
                            _xi_windows_kernel32().CloseHandle(
                                wintypes.HANDLE(handle))
                else:
                    for handle in reversed(child_handles):
                        _xi_windows_kernel32().CloseHandle(
                            wintypes.HANDLE(handle))
            after = _xi_windows_handle_info(handles[-1])
            before_identity = _xi_windows_stable_file_identity(before)
            after_identity = _xi_windows_stable_file_identity(after)
            if before_identity != after_identity:
                die("xi-lowering: AOT discovery directory changed while it was "
                    f"captured: {relative_directory}")
            directory_identities[relative_directory] = before_identity
        finally:
            for handle in reversed(handles):
                _xi_windows_kernel32().CloseHandle(wintypes.HANDLE(handle))

    capture('src/aot')
    return XiAotDiscoverySnapshot(
        tuple(sorted(sources.items())), tuple(sorted(directories)),
        tuple(sorted(source_identities.items())),
        tuple(sorted(directory_identities.items())))


def _xi_capture_aot_discovery_census(
        source_root: Path, before_open_hook=None) -> XiAotDiscoverySnapshot:
    """Capture every AOT C source and directory without pathname re-resolution."""
    source_root = Path(os.path.abspath(os.fspath(source_root)))
    if os.name == 'nt':
        return _xi_capture_aot_discovery_census_windows(
            source_root, before_open_hook)
    return _xi_capture_aot_discovery_census_posix(
        source_root, before_open_hook)


@functools.lru_cache(maxsize=1024)
def _xi_preprocessor_logical_source(source: str, context: str) -> str:
    """Apply the preprocessing phases needed to identify include directives."""
    source = _xi_c_translation_phase_1_2(source)
    output = []
    index = 0
    state = 'normal'
    while index < len(source):
        current = source[index]
        following = source[index + 1] if index + 1 < len(source) else ''
        if state == 'normal':
            if current == '/' and following == '/':
                output.append(' ')
                index += 2
                state = 'line-comment'
                continue
            if current == '/' and following == '*':
                output.append(' ')
                index += 2
                state = 'block-comment'
                continue
            output.append(current)
            if current == '"':
                state = 'string'
            elif current == "'":
                state = 'char'
        elif state == 'line-comment':
            if current in {'\r', '\n'}:
                output.append(current)
                state = 'normal'
        elif state == 'block-comment':
            if current == '*' and following == '/':
                output.append(' ')
                index += 2
                state = 'normal'
                continue
            if current in {'\r', '\n'}:
                output.append(current)
        else:
            output.append(current)
            delimiter = '"' if state == 'string' else "'"
            if current == '\\' and following:
                output.append(following)
                index += 2
                continue
            if current == delimiter:
                state = 'normal'
        index += 1
    if state == 'block-comment':
        die(f"{context}: unterminated preprocessing comment")
    if state in {'string', 'char'}:
        die(f"{context}: unterminated preprocessing literal")
    return ''.join(output)


def _xi_literal_c_includes(source: str, context: str
                           ) -> list[tuple[str, str]]:
    """Parse every compiler-visible literal include or fail closed."""
    includes: list[tuple[str, str]] = []
    logical = _xi_preprocessor_logical_source(source, context)
    directive = re.compile(
        r'(?m)^[^\S\r\n]*(?:#|%:)[^\S\r\n]*include\b([^\r\n]*)$')
    for match in directive.finditer(logical):
        operand = match.group(1).strip()
        if operand.startswith('<'):
            angle = re.fullmatch(r'<([^>\r\n]*)>', operand)
            if angle is None:
                die(f"{context}: malformed angle-bracket include")
            includes.append(('angle', angle.group(1)))
            continue
        if not operand.startswith('"'):
            die(f"{context}: include operands must be literal quoted or "
                "angle-bracket header names; macro-expanded includes are "
                "forbidden")
        closing = operand.find('"', 1)
        if closing < 0:
            die(f"{context}: unterminated local C include path")
        spelling = operand[1:closing]
        trailing = operand[closing + 1:]
        if trailing.strip():
            die(f"{context}: local C include has non-comment trailing tokens")
        includes.append(('quoted', spelling))
    return includes


def _xi_local_c_include_spellings(source: str, context: str) -> list[str]:
    """Return quoted include spellings for callers that do not resolve angles."""
    return [spelling for style, spelling in _xi_literal_c_includes(source, context)
            if style == 'quoted']


def _xi_cgen_cmake_include_directories(source_root: Path) -> tuple[str, ...]:
    """Read the single governed repository search order from the C compile edge."""
    cmake_path = source_root / 'CMakeLists.txt'
    try:
        os.lstat(cmake_path)
    except FileNotFoundError:
        die("xi-lowering: CMakeLists.txt is required to project the Xi CGen "
            "quoted-include search order")
    data = _xi_secure_repository_file_bytes(
        source_root, 'CMakeLists.txt', 'xi-lowering: C include search order')
    try:
        text = data.decode('utf-8', errors='strict')
    except UnicodeDecodeError as error:
        die(f"xi-lowering: CMake include search order is not UTF-8: {error}")
    matches = list(re.finditer(
        r'(?ms)^set\(XRAY_COMMON_INCLUDES[^\S\r\n]*\r?\n'
        r'(?P<body>.*?)^\)', text))
    if len(matches) != 1:
        die("xi-lowering: CMake must define exactly one literal "
            "XRAY_COMMON_INCLUDES search order")
    prefix = '${CMAKE_CURRENT_SOURCE_DIR}/'
    parsed = []
    for raw_line in matches[0].group('body').splitlines():
        line = raw_line.split('#', 1)[0].strip()
        if not line:
            continue
        if not line.startswith(prefix):
            die("xi-lowering: XRAY_COMMON_INCLUDES must contain only literal "
                f"repository directories: {line}")
        relative = line[len(prefix):]
        _xi_repository_relative_parts(
            relative, 'xi-lowering: C include search directory')
        parsed.append(relative)
    actual = tuple(parsed)
    if not actual or len(actual) != len(set(actual)):
        die("xi-lowering: XRAY_COMMON_INCLUDES must be non-empty and contain "
            "no duplicate repository directories")
    return actual


def _xi_secure_repository_directory_snapshot(
        source_root: Path, relative: str, context: str,
        before_open_hook=None) -> tuple[tuple[int, ...], tuple[str, ...]]:
    """Capture one stable directory identity and listing through a held handle."""
    if relative == '.':
        if os.name == 'nt':
            import ctypes
            from ctypes import wintypes

            kernel32 = _xi_windows_kernel32()
            handle = kernel32.CreateFileW(
                os.fspath(source_root), 0x0080, 0x00000001, None, 3,
                0x00200000 | 0x02000000, None)
            if handle == ctypes.c_void_p(-1).value:
                raise ctypes.WinError(ctypes.get_last_error())
            try:
                before = _xi_windows_handle_info(handle)
                if before.dwFileAttributes & 0x00000400 or not \
                        before.dwFileAttributes & 0x00000010:
                    die(f"{context} repository root is not a regular directory")
                entries = tuple(sorted(os.listdir(source_root)))
                after = _xi_windows_handle_info(handle)
                before_identity = _xi_windows_stable_file_identity(before)
                after_identity = _xi_windows_stable_file_identity(after)
                if before_identity != after_identity:
                    die(f"{context} changed while captured: {relative}")
                return _xi_windows_stable_file_identity(before), entries
            finally:
                kernel32.CloseHandle(wintypes.HANDLE(handle))
        flags = (os.O_RDONLY | getattr(os, 'O_CLOEXEC', 0) |
                 getattr(os, 'O_DIRECTORY', 0) |
                 getattr(os, 'O_NOFOLLOW', 0))
        directory_fd = os.open(os.fspath(source_root), flags)
    elif os.name == 'nt':
        import ctypes
        from ctypes import wintypes

        handles, before = _xi_windows_open_repository_object(
            source_root, relative, context, directory=True,
            before_open_hook=before_open_hook)
        try:
            entries = tuple(sorted(os.listdir(source_root / relative)))
            after = _xi_windows_handle_info(handles[-1])
            before_identity = _xi_windows_stable_file_identity(before)
            after_identity = _xi_windows_stable_file_identity(after)
            if before_identity != after_identity:
                die(f"{context} changed while captured: {relative}")
            return _xi_windows_stable_file_identity(before), entries
        finally:
            for handle in reversed(handles):
                _xi_windows_kernel32().CloseHandle(wintypes.HANDLE(handle))
    else:
        directory_fd = _xi_posix_open_repository_object(
            source_root, relative, context, directory=True,
            before_open_hook=before_open_hook)
    try:
        before = _xi_stable_file_identity(os.fstat(directory_fd))
        entries = tuple(sorted(os.listdir(directory_fd)))
        after = _xi_stable_file_identity(os.fstat(directory_fd))
        if before != after:
            die(f"{context} changed while captured: {relative}")
        return before, entries
    finally:
        os.close(directory_fd)


def _xi_include_candidate(base: str, spelling: str) -> str:
    """Join one compiler search directory without erasing a checked child."""
    stack = [] if base == '.' else list(Path(base).parts)
    saw_child = False
    for component in Path(spelling).parts:
        if component == '..':
            if saw_child or not stack:
                die("xi-lowering: local include parent traversal may only precede "
                    f"all child components and remain in-repository: {spelling}")
            stack.pop()
            continue
        saw_child = True
        stack.append(component)
    if not stack:
        die(f"xi-lowering: local include resolves to the repository root: {spelling}")
    return Path(*stack).as_posix()


def _xi_canonical_repository_include(
        source_root: Path, including: str, spelling: str,
        include_directories: tuple[str, ...],
        captured_directories: dict[str, tuple[str, ...]],
        captured_directory_identities: dict[str, tuple[int, ...]],
        *, include_current_directory: bool, missing_ok: bool = False,
        before_open_hook=None) -> tuple[str, tuple[int, ...], bytes] | None:
    """Resolve one repository include through the governed compiler order."""
    path = Path(spelling)
    if (not spelling or path.is_absolute() or '\\' in spelling or
            any(component in {'', '.'} for component in path.parts)):
        die("xi-lowering: local include must use a canonical relative spelling: "
            f"{spelling}")

    def capture_directory(relative: str) -> tuple[str, ...] | None:
        if relative in captured_directories:
            return captured_directories[relative]
        try:
            mode = os.lstat(source_root if relative == '.' else source_root / relative).st_mode
        except FileNotFoundError:
            return None
        if stat.S_ISLNK(mode):
            die(f"xi-lowering: local include search directory is a symlink: {relative}")
        if not stat.S_ISDIR(mode):
            return None
        identity, entries = _xi_secure_repository_directory_snapshot(
            source_root, relative, 'xi-lowering: local include search directory',
            before_open_hook=before_open_hook)
        captured_directories[relative] = entries
        captured_directory_identities[relative] = identity
        return entries

    search_directories = include_directories
    if include_current_directory:
        search_directories = (Path(including).parent.as_posix(), *search_directories)
    for search_directory in dict.fromkeys(search_directories):
        if capture_directory(search_directory) is None:
            continue
        candidate = _xi_include_candidate(search_directory, spelling)
        parent = Path(candidate).parent.as_posix()
        if capture_directory(parent) is None:
            continue
        try:
            mode = os.lstat(source_root / candidate).st_mode
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(mode):
            die(f"xi-lowering: local include resolves through a symlink: {candidate}")
        if not stat.S_ISREG(mode):
            continue
        identity, data = _xi_secure_repository_file_snapshot(
            source_root, candidate, 'xi-lowering: local C include',
            before_open_hook=before_open_hook)
        return candidate, identity, data
    if missing_ok:
        return None
    die(f"xi-lowering: local include cannot be resolved in-repository: {spelling}")


def capture_xi_lowering_validation_snapshot(
        source_root: Path, before_open_hook=None) -> XiLoweringValidationSnapshot:
    """Capture repository-local quoted includes through governed ordered roots.

    Build-tree include roots are intentionally outside this descriptor-bound
    domain, so a quoted include that would require one fails closed.
    """
    source_root = Path(os.path.abspath(os.fspath(source_root)))
    include_directories = _xi_cgen_cmake_include_directories(source_root)
    captured_directories: dict[str, tuple[str, ...]] = {}
    captured_directory_identities: dict[str, tuple[int, ...]] = {}
    for relative in include_directories:
        try:
            mode = os.lstat(source_root / relative).st_mode
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(mode):
            die(f"xi-lowering: C include search directory is a symlink: {relative}")
        if not stat.S_ISDIR(mode):
            die(f"xi-lowering: C include search path is not a directory: {relative}")
        identity, entries = _xi_secure_repository_directory_snapshot(
            source_root, relative, 'xi-lowering: C include search directory',
            before_open_hook=before_open_hook)
        captured_directories[relative] = entries
        captured_directory_identities[relative] = identity
    pending: list[tuple[str, tuple[int, ...] | None, bytes | None]] = [
        ('src/aot/xi_cgen.c', None, None)]
    visited: dict[str, bytes] = {}
    visited_identities: dict[str, tuple[int, ...]] = {}
    while pending:
        relative, captured_identity, captured_data = pending.pop()
        canonical = _xi_repository_relative_name(
            source_root, source_root / relative, 'xi-lowering: local C include')
        if canonical in visited:
            continue
        try:
            data = captured_data
            identity = captured_identity
            if data is None:
                identity, data = _xi_secure_repository_file_snapshot(
                    source_root, canonical, 'xi-lowering: local C include',
                    before_open_hook=before_open_hook)
            source = data.decode('utf-8', errors='strict')
        except UnicodeDecodeError as error:
            die(f"xi-lowering: cannot capture local C include {canonical}: {error}")
        visited[canonical] = data
        assert identity is not None
        visited_identities[canonical] = identity
        for style, include_spelling in _xi_literal_c_includes(
                source, f"xi-lowering: local C include in {canonical}"):
            included = _xi_canonical_repository_include(
                source_root, canonical, include_spelling, include_directories,
                captured_directories, captured_directory_identities,
                include_current_directory=style == 'quoted',
                missing_ok=style == 'angle',
                before_open_hook=before_open_hook)
            if included is None:
                continue
            included_relative, included_identity, included_data = included
            pending.append((included_relative, included_identity, included_data))
    return XiLoweringValidationSnapshot(
        tuple(sorted(visited.items())), include_directories,
        tuple(sorted(captured_directories.items())),
        tuple(sorted(visited_identities.items())),
        tuple(sorted(captured_directory_identities.items())))


def xi_lowering_aot_validation_paths(entries: list[XiLoweringDef],
                                     source_root: Path,
                                     snapshot: XiLoweringValidationSnapshot | None = None
                                     ) -> list[Path]:
    source_root = Path(os.path.abspath(os.fspath(source_root)))
    snapshot = snapshot or capture_xi_lowering_validation_snapshot(source_root)
    closure = {relative for relative, _ in snapshot.sources}
    declared = xi_lowering_consumer_source_paths(entries)
    missing = sorted(declared - closure)
    if missing:
        die("xi-lowering: direct consumer witness source is outside the xi_cgen.c "
            "translation unit: " + ', '.join(missing))
    return [source_root / relative for relative in sorted(closure)]


def xi_lowering_validation_dependency_paths(
        entries: list[XiLoweringDef], source_root: Path,
        snapshot: XiLoweringValidationSnapshot,
        discovery_snapshot: XiAotDiscoverySnapshot) -> list[Path]:
    """Return closure and discovery inputs without creating a second owner list."""
    xi_lowering_aot_validation_paths(entries, source_root, snapshot)
    relatives = {relative for relative, _ in snapshot.sources}
    relatives.update(relative for relative, _ in snapshot.directories)
    relatives.update(relative for relative, _ in discovery_snapshot.sources)
    relatives.update(relative for relative, _ in discovery_snapshot.directories)
    if (source_root / 'CMakeLists.txt').exists():
        relatives.add('CMakeLists.txt')
    return [source_root / relative for relative in sorted(relatives)]


def xi_lowering_noreturn_symbols(source_text: dict[str, str]) -> set[str]:
    symbols = {'abort', 'exit', '_Exit', 'quick_exit',
               '__builtin_trap', '__builtin_unreachable'}
    declaration = re.compile(
        r'(?m)^\s*(?:_Noreturn|XR_NORETURN|noreturn|\[\[noreturn\]\])\b'
        r'[^;{}]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\(')
    for text in source_text.values():
        source = _xi_c_source_without_literals(
            text, blank_preprocessor=False)
        symbols.update(match.group(1) for match in declaration.finditer(source))
    return symbols


def _xi_validate_governed_token_aliases(
        entries: list[XiLoweringDef], source_text: dict[str, str],
        context: str,
        predicate_helpers: dict[str, set[str]],
        passthrough_helpers: set[str]) -> None:
    """Reject semantic aliases while leaving non-routing observations alone."""
    selectors = {'XI_' + entry.ident for entry in entries}
    symbols = {
        symbol
        for entry in entries
        for bindings in entry.target_consumers.values()
        for binding in bindings
        for symbol in (
            binding.symbol,
            *(router.symbol for router in binding.routers),
            *(activation.caller for activation in binding.activations),
            *(emitter.symbol for emitter in binding.emitters),
            *(predicate.symbol for predicate in binding.predicates),
        )
    }
    generated_macro_projections = {
        'src/ir/xi_lowering_coverage_gen.h',
        'src/ir/xi_ops_gen.h',
        'src/plan/target/xr_target_instruction_gen.h',
    }
    governed = selectors | symbols
    governed_pattern = re.compile(
        r'\b(?:' + '|'.join(re.escape(token) for token in sorted(governed)) + r')\b')
    symbol_pattern = re.compile(
        r'\b(?:' + '|'.join(re.escape(symbol) for symbol in sorted(symbols)) + r')\b')
    macro_definitions: dict[str, str] = {}
    definition_pattern = re.compile(
        r'^\s*(?:#|%:)\s*define\s+([A-Za-z_][A-Za-z0-9_]*)'
        r'(?:\s*\([^\r\n]*?\))?\s*(.*?)\s*$')
    for relative, text in sorted(source_text.items()):
        logical = _xi_preprocessor_logical_source(
            text, f'{context}: {relative}')
        for line in logical.splitlines():
            if not re.match(r'\s*(?:#|%:)', line):
                continue
            definition = definition_pattern.match(line)
            if definition is not None:
                macro_definitions[definition.group(1)] = definition.group(2)
            if relative not in generated_macro_projections:
                hidden = sorted(set(governed_pattern.findall(line)))
                if hidden:
                    die(f"{context}: governed token appears in preprocessing "
                        f"directive in {relative}: {', '.join(hidden)}")

    behavioral_macros = {
        name for name, replacement in macro_definitions.items()
        if (re.search(r'(?:->|\.)\s*op\b|\bcase\b', replacement) is not None or
            symbol_pattern.search(replacement) is not None)
    }
    changed = True
    while changed:
        changed = False
        for name, replacement in macro_definitions.items():
            if name in behavioral_macros:
                continue
            if any(re.search(rf'\b{re.escape(target)}\s*\(', replacement)
                   for target in behavioral_macros):
                behavioral_macros.add(name)
                changed = True

    behavioral_functions = set()
    for text in source_text.values():
        for symbol, bodies in _xi_c_file_scope_function_bodies(text).items():
            if any(re.search(r'(?:->|\.)\s*op\b', body) is not None and
                   symbol_pattern.search(body) is not None
                   for body in bodies):
                behavioral_functions.add(symbol)

    selector_pattern = re.compile(
        r'\b(?:' + '|'.join(re.escape(selector) for selector in sorted(selectors)) + r')\b')
    behavioral_calls = behavioral_macros | behavioral_functions
    behavioral_call_pattern = None
    if behavioral_calls:
        behavioral_call_pattern = re.compile(
            r'\b(?:' + '|'.join(
                re.escape(symbol) for symbol in sorted(behavioral_calls)) + r')\s*\(')

    predicate_ranges: dict[tuple[str, str], list[tuple[int, int]]] = {}
    for relative, text in source_text.items():
        ranges = _xi_c_file_scope_function_body_ranges(text)
        for selector, helpers in predicate_helpers.items():
            predicate_ranges[(relative, selector)] = [
                item
                for helper in helpers
                for item in ranges.get(helper, [])
            ]

    for relative, text in sorted(source_text.items()):
        code = _xi_c_source_without_literals(text)
        for match in symbol_pattern.finditer(code):
            if re.match(r'\s*\(', code[match.end():]) is None:
                die(f"{context}: governed function token is used through an "
                    f"alias in {relative}: {match.group(0)}")
        for match in selector_pattern.finditer(code):
            prefix = code[max(code.rfind(';', 0, match.start()),
                              code.rfind('{', 0, match.start()),
                              code.rfind('}', 0, match.start()),
                              code.rfind('\n', 0, match.start())) + 1:match.start()]
            designated_op = re.search(
                r'(?:^|[,;{])\s*\.\s*op\s*=\s*'
                r'(?:\(\s*[A-Za-z_][A-Za-z0-9_\s*]*\)\s*)*$',
                prefix) is not None
            aggregate_open = code.rfind('{', 0, match.start())
            aggregate_boundary = max(
                code.rfind(';', 0, match.start()),
                code.rfind('}', 0, match.start()),
                code.rfind('\n', 0, match.start()))
            aggregate_prefix = ''
            if aggregate_open > aggregate_boundary:
                aggregate_prefix = code[
                    max(code.rfind(';', 0, aggregate_open),
                        code.rfind('}', 0, aggregate_open),
                        code.rfind('\n', 0, aggregate_open)) + 1:aggregate_open]
            aggregate_alias = (
                not designated_op and aggregate_open > aggregate_boundary and
                re.search(r'=\s*$', aggregate_prefix) is not None)
            suffix = code[match.end():code.find('\n', match.end())
                          if code.find('\n', match.end()) >= 0 else len(code)]
            direct_field_comparison = (
                re.search(r'(?:->|\.)\s*[A-Za-z_][A-Za-z0-9_]*\s*==\s*$',
                          prefix) is not None or
                re.match(r'\s*==\s*[A-Za-z_][A-Za-z0-9_]*'
                         r'\s*(?:->|\.)\s*[A-Za-z_][A-Za-z0-9_]*\b',
                         suffix) is not None)
            unmatched_call = None
            call_matches = list(re.finditer(
                r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\(', prefix))
            for call_match in reversed(call_matches):
                if prefix[call_match.end():].count('(') + 1 > \
                        prefix[call_match.end():].count(')'):
                    unmatched_call = call_match.group(1)
                    break
            selector_typed_storage = re.search(
                r'\bXiOp\b[^;{}=]*=', prefix) is not None
            assignment_rhs = (
                not designated_op and not direct_field_comparison and re.search(
                    r'(?<![.!>])\b[A-Za-z_][A-Za-z0-9_]*\s*'
                    r'(?<![=!<>])=(?!=)', prefix) is not None and
                (unmatched_call is None or selector_typed_storage or
                 unmatched_call in passthrough_helpers))
            alias_form = (re.search(
                    r'(?<![.!>])\b[A-Za-z_][A-Za-z0-9_]*\s*='
                    r'\s*(?:\(\s*[A-Za-z_][A-Za-z0-9_\s*]*\)\s*)*'
                    r'&?\s*$', prefix) or re.search(r'&\s*$', prefix) or
                    aggregate_alias or assignment_rhs)
            selector = match.group(0)
            predicate_alias = any(
                start <= match.start() < end
                for start, end in predicate_ranges.get((relative, selector), []))
            if alias_form and not predicate_alias:
                line = code.count('\n', 0, match.start()) + 1
                die(f"{context}: governed selector is used through an "
                    f"initializer alias in {relative}:{line}: {match.group(0)}")
        if behavioral_call_pattern is not None:
            for match in behavioral_call_pattern.finditer(code):
                open_paren = match.end() - 1
                end = _xi_matching_delimiter(code, open_paren, '(', ')')
                if end is None:
                    die(f"{context}: behavioral macro or function call is "
                        f"unmatched in {relative}")
                hidden = sorted(set(
                    selector_pattern.findall(code[open_paren + 1:end])))
                if hidden:
                    die(f"{context}: governed selector is hidden in a macro argument "
                        f"in {relative}: {', '.join(hidden)}")


@functools.lru_cache(maxsize=4096)
def _xi_function_selector_helper_tokens(
        body: str) -> tuple[frozenset[str], frozenset[str]]:
    """Extract predicate and factory selector literals from one function body."""
    source = _xi_c_source_without_literals(body)
    returns = re.findall(r'\breturn\s+([^;]+);', source)
    comparison = re.compile(
        r'(?:\b[A-Za-z_][A-Za-z0-9_]*(?:\s*->\s*op)?\b)\s*==\s*'
        r'\b(XI_[A-Z0-9_]+)\b|\b(XI_[A-Z0-9_]+)\b\s*==\s*'
        r'(?:\b[A-Za-z_][A-Za-z0-9_]*(?:\s*->\s*op)?\b)')
    predicates = set()
    factories = set()
    for expression in returns:
        for match in comparison.finditer(expression):
            predicates.add(match.group(1) or match.group(2))
        value = expression.strip()
        while True:
            cast = re.match(
                r'^\(\s*[A-Za-z_][A-Za-z0-9_\s*]*\)\s*', value)
            if cast is None:
                break
            value = value[cast.end():].strip()
        value = _xi_strip_balanced_parentheses(value)
        if re.fullmatch(r'XI_[A-Z0-9_]+', value):
            factories.add(value)
    return frozenset(predicates), frozenset(factories)


def _xi_returned_call_symbols(body: str) -> set[str]:
    source = _xi_c_source_without_literals(body)
    symbols = set()
    for expression in re.findall(r'\breturn\s+([^;]+);', source):
        value = _xi_strip_balanced_parentheses(expression.strip())
        match = re.fullmatch(
            r'(?:\(\s*[A-Za-z_][A-Za-z0-9_\s*]*\)\s*)*'
            r'([A-Za-z_][A-Za-z0-9_]*)\s*\(.*\)', value, re.S)
        if match is not None:
            symbols.add(match.group(1))
    return symbols


def _xi_transitive_selector_helper_symbols(
        aot_functions: tuple[tuple[str, str, str], ...], selectors: set[str]
) -> tuple[dict[str, set[str]], dict[str, set[str]]]:
    """Close predicate and factory summaries over returned helper calls."""
    function_bodies: dict[str, list[str]] = {}
    for _, symbol, body in aot_functions:
        function_bodies.setdefault(symbol, []).append(body)
    predicates_by_symbol: dict[str, set[str]] = {}
    factories_by_symbol: dict[str, set[str]] = {}
    returned_calls: dict[str, set[str]] = {}
    for symbol, bodies in function_bodies.items():
        predicates_by_symbol[symbol] = set()
        factories_by_symbol[symbol] = set()
        returned_calls[symbol] = set()
        for body in bodies:
            predicates, factories = _xi_function_selector_helper_tokens(body)
            predicates_by_symbol[symbol].update(selectors.intersection(predicates))
            factories_by_symbol[symbol].update(selectors.intersection(factories))
            returned_calls[symbol].update(_xi_returned_call_symbols(body))
    for _ in range(len(function_bodies) + 1):
        changed = False
        for symbol in sorted(function_bodies):
            for callee in returned_calls[symbol]:
                before = (len(predicates_by_symbol[symbol]),
                          len(factories_by_symbol[symbol]))
                predicates_by_symbol[symbol].update(
                    predicates_by_symbol.get(callee, set()))
                factories_by_symbol[symbol].update(
                    factories_by_symbol.get(callee, set()))
                changed |= before != (len(predicates_by_symbol[symbol]),
                                      len(factories_by_symbol[symbol]))
        if not changed:
            break
    else:
        die("xi-lowering: selector helper transitive closure did not converge")
    predicates = {
        selector: {symbol for symbol, owned in predicates_by_symbol.items()
                   if selector in owned}
        for selector in selectors
    }
    factories = {
        selector: {symbol for symbol, owned in factories_by_symbol.items()
                   if selector in owned}
        for selector in selectors
    }
    return predicates, factories


def _xi_selector_passthrough_helpers(
        aot_functions: tuple[tuple[str, str, str], ...]) -> set[str]:
    """Close exact return-value passthrough helpers for alias classification."""
    bodies: dict[str, list[str]] = {}
    for _, symbol, body in aot_functions:
        bodies.setdefault(symbol, []).append(body)
    passthrough = {
        symbol
        for symbol, definitions in bodies.items()
        if any(re.fullmatch(
            r'\s*return\s+[A-Za-z_][A-Za-z0-9_]*\s*;\s*',
            _xi_c_source_without_literals(body)) is not None
            for body in definitions)
    }
    returned_calls = {
        symbol: set().union(*(_xi_returned_call_symbols(body)
                              for body in definitions))
        for symbol, definitions in bodies.items()
    }
    for _ in range(len(bodies) + 1):
        added = {
            symbol for symbol, callees in returned_calls.items()
            if symbol not in passthrough and callees.intersection(passthrough)
        }
        if not added:
            break
        passthrough.update(added)
    else:
        die("xi-lowering: selector passthrough closure did not converge")
    return passthrough


def _xi_selector_predicate_helpers(
        aot_functions: tuple[tuple[str, str, str], ...],
        selectors: set[str]) -> dict[str, set[str]]:
    """Map selector predicates that can hide a caller's terminal branch."""
    return _xi_transitive_selector_helper_symbols(
        aot_functions, selectors)[0]


def _xi_selector_factory_helpers(
        aot_functions: tuple[tuple[str, str, str], ...],
        selectors: set[str]) -> dict[str, set[str]]:
    """Map helpers that return a governed selector for a caller comparison."""
    return _xi_transitive_selector_helper_symbols(
        aot_functions, selectors)[1]


def _xi_exact_positive_selector_for_roots(
        term: str, root_values: set[str]) -> tuple[str, str] | None:
    value = _xi_strip_balanced_parentheses(term)
    selector = r'(XI_[A-Z0-9_]+)'
    for root in sorted(root_values):
        root_op = rf'\b{re.escape(root)}\s*->\s*op\b'
        for pattern in (rf'{root_op}\s*==\s*{selector}',
                        rf'{selector}\s*==\s*{root_op}'):
            match = re.fullmatch(pattern, value)
            if match:
                return root, match.group(1)
    return None


def _xi_exact_positive_selector_set(
        expression: str, root_values: set[str]) -> tuple[str, frozenset[str]] | None:
    value = _xi_strip_balanced_parentheses(expression)
    matches = [
        _xi_exact_positive_selector_for_roots(term, root_values)
        for term in _xi_split_top_level(value, '||')
    ]
    if not matches or any(match is None for match in matches):
        return None
    roots = {match[0] for match in matches if match is not None}
    selectors = [match[1] for match in matches if match is not None]
    if len(roots) != 1 or len(selectors) != len(set(selectors)):
        return None
    return next(iter(roots)), frozenset(selectors)


def _xi_declared_positive_selector_predicate_present(
        body: str, selectors: set[str], root_values: set[str]) -> bool:
    """Prove one helper returns exactly a declared positive selector set."""
    source = _xi_c_source_without_literals(body)
    returns = [expression.strip()
               for expression in re.findall(r'\breturn\s+([^;]+);', source)]
    positive = [expression for expression in returns
                if _xi_normalize_c_expr(expression) != 'false']
    if len(positive) != 1 or not root_values:
        return False
    parsed = _xi_exact_positive_selector_set(positive[0], root_values)
    if parsed is None or parsed[1] != frozenset(selectors):
        return False
    if any(_xi_normalize_c_expr(expression) not in {'false'}
           for expression in returns if expression != positive[0]):
        return False
    return set(re.findall(r'\bXI_[A-Z0-9_]+\b', source)) == selectors


def _xi_exact_negative_selector_set(
        expression: str, root_values: set[str]) -> tuple[str, frozenset[str]] | None:
    value = _xi_strip_balanced_parentheses(expression)
    matches: list[tuple[str, str]] = []
    selector = r'(XI_[A-Z0-9_]+)'
    for term in _xi_split_top_level(value, '&&'):
        stripped = _xi_strip_balanced_parentheses(term)
        found = None
        for root in sorted(root_values):
            root_op = rf'\b{re.escape(root)}\s*->\s*op\b'
            for pattern in (rf'{root_op}\s*!=\s*{selector}',
                            rf'{selector}\s*!=\s*{root_op}'):
                match = re.fullmatch(pattern, stripped)
                if match:
                    found = (root, match.group(1))
                    break
            if found is not None:
                break
        if found is None:
            return None
        matches.append(found)
    roots = {root for root, _ in matches}
    selector_values = [selector_value for _, selector_value in matches]
    if (len(roots) != 1 or
            len(selector_values) != len(set(selector_values))):
        return None
    return next(iter(roots)), frozenset(selector_values)


def _xi_declared_selector_domain_present(
        body: str, selectors: set[str], root_values: set[str]) -> str | None:
    """Return the guarded Xi root when a leading false guard is exact."""
    source = _xi_c_source_without_literals(body)
    records = _xi_if_statement_records(source, top_level_only=True)
    if not records or source[:records[0][0]].strip():
        return None
    start, end, condition, then_branch, _ = records[0]
    if start != 0 and source[:start].strip():
        return None
    if _xi_normalize_c_expr(then_branch) != 'returnfalse;':
        return None
    terms = [_xi_strip_balanced_parentheses(term)
             for term in _xi_split_top_level(
                 _xi_strip_balanced_parentheses(condition), '||')]
    domains = [parsed for parsed in (
        _xi_exact_negative_selector_set(term, root_values) for term in terms)
               if parsed is not None]
    if len(domains) != 1 or domains[0][1] != frozenset(selectors):
        return None
    root = domains[0][0]
    if _xi_normalize_c_expr('!' + root) not in {
            _xi_normalize_c_expr(term) for term in terms}:
        return None
    if any(_xi_strip_balanced_parentheses(term) in {'true', 'false'}
           for term in terms):
        return None
    returns = [(match.start(), _xi_normalize_c_expr(match.group(1)))
               for match in re.finditer(r'\breturn\s+([^;]+);', source)]
    if not any(value == 'true' and position >= end
               for position, value in returns):
        return None
    if any(value not in {'false', 'true'} for _, value in returns):
        return None
    if set(re.findall(r'\bXI_[A-Z0-9_]+\b', source)) != selectors:
        return None
    return root


def _xi_declared_selector_predicate_delegates(
        body: str, domain_symbol: str, root_value: str) -> bool:
    """Prove an outer predicate is dominated by its exact domain helper."""
    source = _xi_c_source_without_literals(body)
    records = _xi_if_statement_records(source, top_level_only=True)
    if not records or source[:records[0][0]].strip():
        return False
    _, end, condition, then_branch, _ = records[0]
    if _xi_normalize_c_expr(then_branch) != 'returnfalse;':
        return False
    matching_roots = set()
    for term in _xi_split_top_level(
            _xi_strip_balanced_parentheses(condition), '||'):
        value = _xi_strip_balanced_parentheses(term)
        match = re.fullmatch(
            rf'!\s*{re.escape(domain_symbol)}\s*\((.*)\)', value, re.S)
        if match is None:
            continue
        arguments = _xi_split_c_arguments(match.group(1))
        if _xi_normalize_c_expr(root_value) in arguments:
            matching_roots.add(root_value)
    if matching_roots != {root_value}:
        return False
    returns = [(match.start(), _xi_normalize_c_expr(match.group(1)))
               for match in re.finditer(r'\breturn\s+([^;]+);', source)]
    return (any(value == 'true' and position >= end
                for position, value in returns) and
            all(value in {'false', 'true'} for _, value in returns))


def _xi_predicate_terminal_emission_precedes_return(
        branch: str, known_emitters: set[str], terminators: set[str]) -> bool:
    """Prove a declared predicate branch reaches an exact nested emitter."""
    returns = _xi_top_level_bare_return_positions(branch, r'')
    if not returns:
        return False
    for emitter in known_emitters:
        for position, _, args in _xi_c_call_records(branch, emitter):
            if ('out' in args and
                    _xi_call_record_reachable(branch, position, terminators) and
                    any(position < terminal_return for terminal_return in returns)):
                return True
    return False


def _xi_hidden_selector_route_present(
        body: str, selector: str, predicate_symbols: set[str],
        factory_symbols: set[str], root_values: set[str],
        known_emitters: set[str], terminators: set[str]) -> bool:
    """Find terminal branches whose selector was wrapped or cast away."""
    token = re.compile(rf'\b{re.escape(selector)}\b')

    def predicate_uses_root_value(condition: str) -> bool:
        normalized_roots = {
            _xi_normalize_c_expr(expression)
            for name in root_values
            for expression in (name, f'{name}->op')
        }
        for helper in predicate_symbols:
            for match in re.finditer(rf'\b{re.escape(helper)}\s*\(', condition):
                open_paren = condition.find('(', match.start())
                close_paren = _xi_matching_delimiter(
                    condition, open_paren, '(', ')')
                if close_paren is None:
                    return True
                arguments = _xi_split_c_arguments(
                    condition[open_paren + 1:close_paren])
                if any(_xi_normalize_c_expr(
                        _xi_strip_balanced_parentheses(argument)) in normalized_roots
                       for argument in arguments):
                    return True
        return False

    factory_pattern = None
    if factory_symbols:
        factory_pattern = re.compile(
            r'\b(?:' + '|'.join(
                re.escape(symbol) for symbol in sorted(factory_symbols)) +
            r')\s*\(')
    for condition, then_branch, _ in _xi_if_statements(body, top_level_only=True):
        direct_field_selector = re.search(
            rf'(?:->|\.)\s*op\s*==\s*\b{re.escape(selector)}\b',
            condition) is not None
        negative_field_selector = re.search(
            rf'(?:->|\.)\s*op\s*!=\s*'
            rf'(?:\(\s*[A-Za-z_][A-Za-z0-9_\s*]*\)\s*)*'
            rf'\b{re.escape(selector)}\b|\b{re.escape(selector)}\b\s*!=\s*'
            rf'(?:[A-Za-z_][A-Za-z0-9_]*\s*(?:->|\.)\s*op)',
            condition) is not None
        hidden_literal = (
            token.search(condition) is not None and
            not direct_field_selector and
            not negative_field_selector and
            not _xi_condition_has_selector_disjunct(condition, selector))
        hidden_helper = (
            predicate_uses_root_value(condition) or
            (factory_pattern is not None and
             factory_pattern.search(condition) is not None))
        terminal = _xi_terminal_emission_branch(
            then_branch, known_emitters, terminators)
        if (hidden_helper and predicate_symbols and
                _xi_predicate_terminal_emission_precedes_return(
                    then_branch, known_emitters, terminators)):
            terminal = True
        if (hidden_literal or hidden_helper) and terminal:
            return True
    return False


def _xi_transitive_terminal_emitters(
        aot_functions: tuple[tuple[str, str, str], ...], seeds: set[str],
        terminators: set[str]) -> set[str]:
    """Close exact terminal-emitter wrappers over bounded helper call edges."""
    terminal = set(seeds)
    symbols = {symbol for _, symbol, _ in aot_functions}
    for _ in range(len(symbols) + 1):
        added = set()
        for _, symbol, body in aot_functions:
            if symbol in terminal:
                continue
            for callee in terminal:
                direct_call = rf'{re.escape(callee)}\s*\([^;{{}}]*\)'
                if (re.fullmatch(
                        rf'\s*(?:{direct_call}\s*;|return\s+{direct_call}\s*;)'
                        r'\s*(?:return\s*;\s*)?', body, re.S) is not None and
                        not any(_xi_c_call_records(body, terminator)
                                for terminator in terminators)):
                    added.add(symbol)
                    break
        if not added:
            break
        terminal.update(added)
    else:
        die("xi-lowering: terminal emitter transitive closure did not converge")
    return terminal


def _xi_terminal_selector_census(
        aot_functions: tuple[tuple[str, str, str], ...], selectors: set[str],
        known_emitters: set[str], terminators: set[str],
        declared_routes: dict[str, set[tuple[str, str]]],
        predicate_helpers: dict[str, set[str]],
        factory_helpers: dict[str, set[str]],
        root_values: dict[tuple[str, str], set[str]],
) -> dict[str, set[tuple[str, str]]]:
    """Find terminal selectors by source function without an entries x functions scan."""
    census = {selector: set() for selector in selectors}
    if not known_emitters:
        return census
    token_pattern = re.compile(r'\bXI_[A-Z0-9_]+\b')
    emitter_pattern = re.compile(
        r'\b(?:' + '|'.join(
            re.escape(symbol) for symbol in sorted(known_emitters)) + r')\s*\(')
    for source_path, symbol, function_body in aot_functions:
        if emitter_pattern.search(function_body) is None:
            continue
        candidates = set(token_pattern.findall(function_body)) & selectors
        candidates.update(
            selector for selector, helpers in predicate_helpers.items()
            if any(re.search(rf'\b{re.escape(helper)}\s*\(', function_body)
                   for helper in helpers))
        candidates.update(
            selector for selector, helpers in factory_helpers.items()
            if any(re.search(rf'\b{re.escape(helper)}\s*\(', function_body)
                   for helper in helpers))
        for selector in candidates:
            if (source_path, symbol) in declared_routes.get(selector, set()):
                continue
            if _xi_direct_selector_present(
                    function_body, selector, known_emitters, terminators):
                census[selector].add((source_path, symbol))
            elif _xi_hidden_selector_route_present(
                    function_body, selector, predicate_helpers[selector],
                    factory_helpers[selector],
                    root_values.get((source_path, symbol), set()),
                    known_emitters, terminators):
                census[selector].add((source_path, symbol))
    return census


def validate_xi_lowering_consumer_sources(
        entries: list[XiLoweringDef], source_root: Path, path: str = '<input>',
        snapshot: XiLoweringValidationSnapshot | None = None,
        discovery_snapshot: XiAotDiscoverySnapshot | None = None
) -> XiLoweringValidationSnapshot:
    source_root = Path(os.path.abspath(os.fspath(source_root)))
    snapshot = snapshot or capture_xi_lowering_validation_snapshot(source_root)
    validation_paths = xi_lowering_aot_validation_paths(entries, source_root, snapshot)
    source_text = snapshot.source_text()
    discovery_snapshot = discovery_snapshot or _xi_capture_aot_discovery_census(
        source_root)
    discovery_text = discovery_snapshot.source_text()
    combined_sources = dict(discovery_snapshot.sources)
    combined_sources.update(snapshot.sources)
    combined_identities = dict(discovery_snapshot.source_identities)
    combined_identities.update(snapshot.source_identities)
    combined_snapshot = XiAotDiscoverySnapshot(
        tuple(sorted(combined_sources.items())), discovery_snapshot.directories,
        tuple(sorted(combined_identities.items())),
        discovery_snapshot.directory_identities)
    combined_text = combined_snapshot.source_text()
    aot_functions = _xi_aot_discovery_functions(combined_snapshot)
    governed_selectors = {'XI_' + entry.ident for entry in entries}
    declared_selector_predicate_helpers = {
        'XI_' + entry.ident: {
            predicate.symbol
            for bindings in entry.target_consumers.values()
            for binding in bindings
            for predicate in binding.predicates
        }
        for entry in entries
    }
    declared_predicate_selectors: dict[
        XiConsumerPredicateWitness, set[str]] = {}
    outer_predicate_domains: dict[tuple[str, str], set[tuple[str | None,
                                                              str | None]]] = {}
    for entry in entries:
        selector = 'XI_' + entry.ident
        for bindings in entry.target_consumers.values():
            for binding in bindings:
                for predicate in binding.predicates:
                    declared_predicate_selectors.setdefault(
                        predicate, set()).add(selector)
                    outer_predicate_domains.setdefault(
                        (predicate.source_path, predicate.symbol), set()).add(
                            (predicate.domain_source_path,
                             predicate.domain_symbol))
    inconsistent_predicates = sorted(
        f'{source_path}::{symbol}'
        for (source_path, symbol), domains in outer_predicate_domains.items()
        if len(domains) != 1)
    if inconsistent_predicates:
        die(f"{path}: predicate domain declaration differs across Xi rows: "
            + ', '.join(inconsistent_predicates))
    inferred_selector_predicate_helpers, selector_factory_helpers = \
        _xi_transitive_selector_helper_symbols(
            aot_functions, governed_selectors)
    selector_predicate_helpers = {
        selector: (declared_selector_predicate_helpers[selector] |
                   inferred_selector_predicate_helpers[selector])
        for selector in governed_selectors
    }
    selector_passthrough_helpers = _xi_selector_passthrough_helpers(aot_functions)
    _xi_validate_governed_token_aliases(
        entries, combined_text, path, declared_selector_predicate_helpers,
        selector_passthrough_helpers)
    declared_locations: dict[str, set[str]] = {}
    for entry in entries:
        for bindings in entry.target_consumers.values():
            for binding in bindings:
                declared_locations.setdefault(binding.symbol, set()).add(binding.source_path)
                for router in binding.routers:
                    declared_locations.setdefault(router.symbol, set()).add(router.source_path)
                for activation in binding.activations:
                    declared_locations.setdefault(activation.caller, set()).add(
                        activation.source_path)
                for emitter in binding.emitters:
                    declared_locations.setdefault(emitter.symbol, set()).add(
                        emitter.source_path)
                for predicate in binding.predicates:
                    declared_locations.setdefault(predicate.symbol, set()).add(
                        predicate.source_path)
                    if predicate.domain_symbol is not None:
                        declared_locations.setdefault(
                            predicate.domain_symbol, set()).add(
                                predicate.domain_source_path)
    definition_paths: dict[str, set[str]] = {}
    for relative, symbol, _ in aot_functions:
        definition_paths.setdefault(symbol, set()).add(relative)
    for symbol, expected_paths in declared_locations.items():
        discovered_paths = definition_paths.get(symbol, set())
        if discovered_paths != expected_paths:
            die(f"{path}: repository AOT source census for {symbol} differs from "
                f"lowering.def: expected {sorted(expected_paths)}, "
                f"found {sorted(discovered_paths)}")
    known_emitters = {
        emitter.symbol
        for entry in entries
        for bindings in entry.target_consumers.values()
        for binding in bindings
        for emitter in binding.emitters
    }
    terminal_emitter_seeds = known_emitters | {
        call.symbol
        for entry in entries
        for bindings in entry.target_consumers.values()
        for binding in bindings
        for sequence in binding.output_sequences
        for call in sequence.calls
        if call.symbol != 'fprintf'
    } | {
        binding.symbol
        for entry in entries
        for bindings in entry.target_consumers.values()
        for binding in bindings
        if binding.witness_kind == 'selected-by'
    }
    terminators = xi_lowering_noreturn_symbols(source_text)
    terminal_emitters = _xi_transitive_terminal_emitters(
        aot_functions, terminal_emitter_seeds, terminators)
    function_root_values = {
        (relative, symbol): values
        for relative, text in combined_text.items()
        for symbol, values in _xi_c_file_scope_function_value_parameters(text).items()
    }
    for predicate, selectors in declared_predicate_selectors.items():
        context = f"{path}: predicate {predicate.source_path}::{predicate.symbol}"
        predicate_body = _xi_require_function_body(
            source_root, predicate.source_path, predicate.symbol,
            context, source_text)
        predicate_roots = function_root_values.get(
            (predicate.source_path, predicate.symbol), set())
        if predicate.domain_symbol is None:
            if not _xi_declared_positive_selector_predicate_present(
                    predicate_body, selectors, predicate_roots):
                die(f"{context} does not return exactly the declared positive "
                    f"selector set {sorted(selectors)}")
            continue
        domain_body = _xi_require_function_body(
            source_root, predicate.domain_source_path,
            predicate.domain_symbol, context, source_text)
        domain_roots = function_root_values.get(
            (predicate.domain_source_path, predicate.domain_symbol), set())
        domain_root = _xi_declared_selector_domain_present(
            domain_body, selectors, domain_roots)
        if domain_root is None:
            die(f"{context} domain {predicate.domain_source_path}::"
                f"{predicate.domain_symbol} does not admit exactly the "
                f"declared selector set {sorted(selectors)}")
        if not _xi_declared_selector_predicate_delegates(
                predicate_body, predicate.domain_symbol, domain_root):
            die(f"{context} is not fail-closed through its declared "
                f"selector domain {predicate.domain_symbol}")
    activation_callers = {
        (activation.source_path, activation.caller)
        for entry in entries
        for bindings in entry.target_consumers.values()
        for binding in bindings
        for activation in binding.activations
    }
    reachable_activation_callers = _xi_reachable_activation_callers(
        aot_functions, activation_callers, terminators)
    declared_routes: dict[str, set[tuple[str, str]]] = {}
    for entry in entries:
        selector = 'XI_' + entry.ident
        declared_routes[selector] = {
            (router.source_path, router.symbol)
            for bindings in entry.target_consumers.values()
            for binding in bindings
            for router in binding.routers
        } - {
            (binding.source_path, binding.symbol)
            for bindings in entry.target_consumers.values()
            for binding in bindings
            if binding.witness_kind == 'selector'
        }
    terminal_selector_census = _xi_terminal_selector_census(
        aot_functions, governed_selectors, terminal_emitters, terminators,
        declared_routes, selector_predicate_helpers,
        selector_factory_helpers, function_root_values)
    for entry in entries:
        selector = 'XI_' + entry.ident
        for target, bindings in entry.target_consumers.items():
            structural = [binding for binding in bindings
                          if binding.witness_kind == 'structural']
            if structural:
                categories = {binding.structural_category for binding in structural}
                if len(structural) != len(bindings) or categories != XI_CONSUMER_STRUCTURAL_CATEGORIES:
                    die(f"{path}: {entry.op_name}:{target}: structural consumer must bind "
                        "the complete four-category ownership set")
            for binding in bindings:
                context = f"{path}: {entry.op_name}:{target}"
                body = _xi_require_function_body(source_root, binding.source_path,
                                                 binding.symbol, context, source_text)
                if binding.witness_kind in {'guarded-selector', 'structural'} and \
                        not binding.activations:
                    die(f"{context}: {binding.source_path}::{binding.symbol} must declare "
                        "its complete activation census")
                if (binding.witness_kind == 'structural' and
                        binding.structural_category != 'edge-parallel-copy' and
                        not binding.output_sequences):
                    die(f"{context}: {binding.source_path}::{binding.symbol} must declare "
                        "its exact structural output sequence")
                _xi_validate_activation_census(
                    source_root, binding, aot_functions, context, source_text,
                    terminators, reachable_activation_callers)
                binding_emitters = {emitter.symbol for emitter in binding.emitters}
                if binding.witness_kind == 'selector':
                    direct = _xi_direct_selector_present(
                        body, selector, binding_emitters, terminators,
                        allow_nested_emitter_route=True)
                    hidden = _xi_hidden_selector_route_present(
                        body, selector,
                        {predicate.symbol for predicate in binding.predicates},
                        selector_factory_helpers[selector],
                        function_root_values.get(
                            (binding.source_path, binding.symbol), set()),
                        binding_emitters, terminators)
                    if not direct and not hidden:
                        die(f"{context}: {binding.source_path}::{binding.symbol} does not "
                            f"select {selector} from v->op")
                elif binding.witness_kind == 'guarded-selector':
                    if not _xi_guarded_selector_present(
                            body, selector, binding_emitters, terminators):
                        die(f"{context}: {binding.source_path}::{binding.symbol} lacks the "
                            f"leading fail-closed guard for {selector}")
                elif binding.witness_kind == 'selected-by':
                    for router in binding.routers:
                        router_body = _xi_require_function_body(
                            source_root, router.source_path, router.symbol, context,
                            source_text)
                        if not _xi_selector_routes_to_owner(router_body, selector,
                                                           binding.symbol,
                                                           known_emitters):
                            die(f"{context}: {router.source_path}::{router.symbol} does not "
                                f"route {selector} to {binding.source_path}::{binding.symbol}")
                    declared = {(router.source_path, router.symbol)
                                for router in binding.routers}
                    discovered = {
                        (source_path, symbol)
                        for source_path, symbol, function_body in aot_functions
                        if selector in function_body and
                        binding.symbol in function_body and
                        _xi_selector_routes_to_owner(function_body, selector,
                                                     binding.symbol,
                                                     known_emitters)
                    }
                    if discovered != declared:
                        missing = sorted(discovered - declared)
                        stale = sorted(declared - discovered)
                        details = []
                        if missing:
                            details.append('undeclared route(s): ' + ', '.join(
                                f'{source_path}::{symbol}' for source_path, symbol in missing))
                        if stale:
                            details.append('stale route(s): ' + ', '.join(
                                f'{source_path}::{symbol}' for source_path, symbol in stale))
                        die(f"{context}: direct-consumer router census mismatch: " +
                            '; '.join(details))
                else:
                    _xi_validate_structural_consumer(
                        source_root, binding, context, source_text, terminators)
            declared_guarded = {
                (binding.source_path, binding.symbol)
                for binding in bindings
                if binding.witness_kind == 'guarded-selector'
            }
            if declared_guarded:
                discovered_guarded = {
                    (source_path, symbol)
                    for source_path, symbol, function_body in aot_functions
                    if selector in function_body and _xi_guarded_selector_present(
                        function_body, selector, known_emitters, terminators)
                }
                if discovered_guarded != declared_guarded:
                    die(f"{path}: {entry.op_name}:{target}: guarded-selector census mismatch")
        declared_selectors = {
            (binding.source_path, binding.symbol)
            for bindings in entry.target_consumers.values()
            for binding in bindings
            if binding.witness_kind == 'selector'
        }
        discovered_selectors = terminal_selector_census[selector]
        if discovered_selectors != declared_selectors:
            missing = sorted(discovered_selectors - declared_selectors)
            stale = sorted(declared_selectors - discovered_selectors)
            details = []
            if missing:
                details.append('undeclared terminal selector(s): ' + ', '.join(
                    f'{source_path}::{symbol}' for source_path, symbol in missing))
            if stale:
                details.append('stale terminal selector(s): ' + ', '.join(
                    f'{source_path}::{symbol}' for source_path, symbol in stale))
            die(f"{path}: {entry.op_name}:aot-c: direct-consumer selector census "
                "mismatch: " + '; '.join(details))
    return snapshot


def generate_xi_lowering_coverage_header(entries: list[XiLoweringDef],
                                         ops: list[XiOpDef]) -> str:
    main_targets = {'vm-bytecode', 'aot-c'}
    patterned_entries = [entry for entry in entries if entry.template != 'custom']
    main_backend_entries = [
        entry for entry in entries
        if main_targets <= _xi_normalized_lowering_targets(entry.targets)
    ]
    main_backend_patterned_entries = [
        entry for entry in patterned_entries
        if main_targets <= _xi_normalized_lowering_targets(entry.targets)
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
    lines.append('#include "xi_ops_gen.h"')
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
    lines.append(f'enum {{ XI_LOWERING_CONSUMER_TARGET_COUNT = {sum(len(entry.target_consumers) for entry in entries)} }};')
    lines.append(f'enum {{ XI_LOWERING_CONSUMER_ENTRY_COUNT = {sum(1 for entry in entries if entry.target_consumers)} }};')
    lines.append(f'enum {{ XI_LOWERING_CONSUMER_BINDING_COUNT = {sum(len(bindings) for entry in entries for bindings in entry.target_consumers.values())} }};')
    lines.append(f'enum {{ XI_LOWERING_CONSUMER_ROUTER_WITNESS_COUNT = {sum(len(binding.routers) for entry in entries for bindings in entry.target_consumers.values() for binding in bindings)} }};')
    lines.append(f'enum {{ XI_LOWERING_CONSUMER_EMITTER_WITNESS_COUNT = {sum(len(binding.emitters) for entry in entries for bindings in entry.target_consumers.values() for binding in bindings)} }};')
    lines.append(f'enum {{ XI_LOWERING_CONSUMER_PREDICATE_WITNESS_COUNT = {sum(len(binding.predicates) for entry in entries for bindings in entry.target_consumers.values() for binding in bindings)} }};')
    lines.append(f'enum {{ XI_LOWERING_CONSUMER_PREDICATE_DOMAIN_WITNESS_COUNT = {sum(1 for entry in entries for bindings in entry.target_consumers.values() for binding in bindings for predicate in binding.predicates if predicate.domain_symbol is not None)} }};')
    lines.append(f'enum {{ XI_LOWERING_CONSUMER_GUARDED_SELECTOR_COUNT = {sum(1 for entry in entries for bindings in entry.target_consumers.values() for binding in bindings if binding.witness_kind == "guarded-selector")} }};')
    lines.append(f'enum {{ XI_LOWERING_CONSUMER_ACTIVATION_EDGE_COUNT = {sum(len(binding.activations) for entry in entries for bindings in entry.target_consumers.values() for binding in bindings)} }};')
    lines.append(f'enum {{ XI_LOWERING_CONSUMER_ACTIVATION_CALL_COUNT = {sum(activation.count for entry in entries for bindings in entry.target_consumers.values() for binding in bindings for activation in binding.activations)} }};')
    lines.append(f'enum {{ XI_LOWERING_CONSUMER_OUTPUT_SEQUENCE_COUNT = {sum(len(binding.output_sequences) for entry in entries for bindings in entry.target_consumers.values() for binding in bindings)} }};')
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
        consumer_bits = _xi_bit_expr('XI_LOWER_TARGET', sorted(entry.target_consumers.keys()))
        suffix = ' \\' if i + 1 < len(entries) else ''
        lines.append(f'    X({entry.ident}, "{entry.op_name}", {target_bits}, {required_bits}, '
                     f'{reject_bits}, {consumer_bits}){suffix}')
    lines.append('')
    lines.append('')
    lines.append('#define XI_LOWERING_CONSUMER_BINDINGS(X) \\')
    consumer_rows = [
        (entry, binding)
        for entry in entries
        for _, bindings in sorted(entry.target_consumers.items())
        for binding in bindings
    ]
    for i, (entry, binding) in enumerate(consumer_rows):
        suffix = ' \\' if i + 1 < len(consumer_rows) else ''
        witness = ('XI_LOWERING_CONSUMER_WITNESS_' +
                   _xi_c_ident(binding.witness_kind if binding.witness_kind != 'structural'
                               else 'structural-' + binding.structural_category))
        lines.append(f'    X({entry.ident}, "{entry.op_name}", "{binding.source_path}", '
                     f'{binding.symbol}, {witness}){suffix}')
    lines.append('')
    lines.append('')
    lines.append('#define XI_LOWERING_CONSUMER_ROUTER_WITNESSES(X) \\')
    router_rows = [
        (entry, binding, router)
        for entry in entries
        for _, bindings in sorted(entry.target_consumers.items())
        for binding in bindings
        for router in binding.routers
    ]
    for i, (entry, binding, router) in enumerate(router_rows):
        suffix = ' \\' if i + 1 < len(router_rows) else ''
        lines.append(f'    X({entry.ident}, "{entry.op_name}", "{router.source_path}", '
                     f'{router.symbol}, "{binding.source_path}", {binding.symbol}){suffix}')
    lines.append('')
    lines.append('')
    lines.append('#define XI_LOWERING_CONSUMER_EMITTER_WITNESSES(X) \\')
    emitter_rows = [
        (entry, binding, emitter)
        for entry in entries
        for _, bindings in sorted(entry.target_consumers.items())
        for binding in bindings
        for emitter in binding.emitters
    ]
    for i, (entry, binding, emitter) in enumerate(emitter_rows):
        suffix = ' \\' if i + 1 < len(emitter_rows) else ''
        lines.append(f'    X({entry.ident}, "{entry.op_name}", "{binding.source_path}", '
                     f'{binding.symbol}, "{emitter.source_path}", {emitter.symbol}){suffix}')
    lines.append('')
    lines.append('')
    lines.append('static inline uint32_t xi_lowering_covered_targets(uint16_t op) {')
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
    lines.append('static inline uint32_t xi_lowering_driver_targets(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in entries:
        driver_bits = _xi_bit_expr('XI_LOWER_TARGET', sorted(entry.target_drivers.keys()))
        lines.append(f'        case XI_{entry.ident}: return {driver_bits};')
    lines.append('        case XI_OP_COUNT: return 0;')
    lines.append('        default: return 0;')
    lines.append('    }')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    lines.append('static inline uint32_t xi_lowering_consumer_targets(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in entries:
        consumer_bits = _xi_bit_expr('XI_LOWER_TARGET', sorted(entry.target_consumers.keys()))
        lines.append(f'        case XI_{entry.ident}: return {consumer_bits};')
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
    lines.append('    return (xi_lowering_covered_targets(op) & target) == target;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_lowering_target_is_rejected(uint16_t op, uint32_t target) {')
    lines.append('    return (xi_lowering_rejected_targets(op) & target) == target;')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_lowering_op_backend_legal(uint16_t op) {')
    lines.append('    if (xi_generated_op_backend_rewrite(op) != XI_GEN_BACKEND_REWRITE_NONE)')
    lines.append('        return false;')
    lines.append('    if (xi_generated_op_lowering_policy(op) == XI_GEN_LOWERING_VERIFIER_ONLY)')
    lines.append('        return false;')
    lines.append('    uint32_t aot_implementations =')
    lines.append('        xi_lowering_covered_targets(op) &')
    lines.append('        (XI_LOWER_TARGET_AOT_C | XI_LOWER_TARGET_AOT_C_STMT);')
    lines.append('    uint32_t aot_rejections =')
    lines.append('        xi_lowering_rejected_targets(op) &')
    lines.append('        (XI_LOWER_TARGET_AOT_C | XI_LOWER_TARGET_AOT_C_STMT);')
    lines.append('    return aot_implementations != 0 && aot_rejections == 0;')
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
        if entry.op_name not in XI_NUMERIC_WIDTH_KERNELS or
        entry.op_name not in XI_VM_TEMPLATE_OPCODES
    ]
    if missing_width_ops:
        die("xi-lowering: missing VM width template op(s) for " + ", ".join(missing_width_ops))

    lines = []
    lines.append('/* AUTO-GENERATED by xisagen - DO NOT EDIT */')
    lines.append('/* Source: xisa/xi/lowering.def */')
    lines.append('/* Included inside xvm.c dispatch switch; relies on i, R, vmcase, vmbreak. */')
    lines.append('')
    lines.append('#define XVM_TEMPLATE_NUMERIC_WIDTH_INT_CASE(op, kernel) \\')
    lines.append('    vmcase(op) { \\')
    lines.append('        int a = GETARG_A(i), b = GETARG_B(i); \\')
    lines.append('        R(a) = XR_FROM_INT(XR_NUMERIC_WIDTH_OWNER_APPLY( \\')
    lines.append('            XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI, \\')
    lines.append('            XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, \\')
    lines.append('            XR_SEM_CONSUMER_VM, kernel, XR_TO_INT(R(b)))); \\')
    lines.append('        vmbreak; \\')
    lines.append('    }')
    lines.append('')
    lines.append('#define XVM_TEMPLATE_NUMERIC_WIDTH_F32_CASE(op, kernel) \\')
    lines.append('    vmcase(op) { \\')
    lines.append('        int a = GETARG_A(i), b = GETARG_B(i); \\')
    lines.append('        R(a) = XR_FROM_FLOAT(XR_NUMERIC_WIDTH_OWNER_APPLY( \\')
    lines.append('            XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_HI, \\')
    lines.append('            XR_SEM_OWNER_ID_SHARED_NUMERIC_CONVERSION_LO, \\')
    lines.append('            XR_SEM_CONSUMER_VM, kernel, XR_TO_FLOAT(R(b)))); \\')
    lines.append('        vmbreak; \\')
    lines.append('    }')
    lines.append('')
    for entry in width_entries:
        opcode = XI_VM_TEMPLATE_OPCODES[entry.op_name]
        kernel = XI_NUMERIC_WIDTH_KERNELS[entry.op_name]
        if entry.op_name.endswith('.f32'):
            lines.append(f'XVM_TEMPLATE_NUMERIC_WIDTH_F32_CASE({opcode}, {kernel})')
        else:
            lines.append(f'XVM_TEMPLATE_NUMERIC_WIDTH_INT_CASE({opcode}, {kernel})')
    lines.append('')
    lines.append('#undef XVM_TEMPLATE_NUMERIC_WIDTH_INT_CASE')
    lines.append('#undef XVM_TEMPLATE_NUMERIC_WIDTH_F32_CASE')
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
    for entry in bitwise_entries:
        opcode = XI_VM_TEMPLATE_OPCODES[entry.op_name]
        kind, allows_bool, error_msg = XI_VM_TEMPLATE_BITWISE_BINARY[entry.op_name]
        lines.append(
            f'XVM_TEMPLATE_BITWISE_BINARY_CASE({opcode}, {kind}, {allows_bool}, {error_msg})')
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
        error_msg = XI_VM_TEMPLATE_BITWISE_UNARY[entry.op_name]
        lines.append(f'XVM_TEMPLATE_BITWISE_UNARY_CASE({opcode}, {error_msg})')
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
        if op_name is None:
            lines.append(f'{macro}({opcode})')
        else:
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
        kind = XI_VM_TEMPLATE_SHIFT[entry.op_name]
        lines.append(f'XVM_TEMPLATE_SHIFT_CASE({opcode}, {kind})')
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
        if entry.op_name not in XI_NUMERIC_WIDTH_KERNELS
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
    lines.append('#include "../shared/xr_native_type_core.h"')
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
    lines.append('static inline const char *xi_to_c_template_bitwise_binary_kind(uint16_t op) {')
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
    lines.append('static inline const char *xi_to_c_template_shift_kind(uint16_t op) {')
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
    lines.append('static inline const char *xi_to_c_template_compare_relation(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in compare_entries:
        _, relation, _ = XI_AOT_C_TEMPLATE_COMPARE[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{relation}";')
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
    lines.append('static inline const char *xi_to_c_template_width_numeric_kernel(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in width_entries:
        kernel = XI_NUMERIC_WIDTH_KERNELS[entry.op_name]
        lines.append(f'        case XI_{entry.ident}: return "{kernel}";')
    lines.append('        case XI_OP_COUNT: return "";')
    lines.append('        default: return "";')
    lines.append('    }')
    lines.append('    return "";')
    lines.append('}')
    lines.append('')
    lines.append('static inline bool xi_to_c_template_width_uses_f64_lane(uint16_t op) {')
    lines.append('    switch ((XiOp) op) {')
    for entry in width_entries:
        if entry.op_name.endswith('.f32'):
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


def generate_xi_lowering_test(entries: list[XiLoweringDef], ops: list[XiOpDef]) -> str:
    main_targets = {'vm-bytecode', 'aot-c'}
    patterned_entries = [entry for entry in entries if entry.template != 'custom']
    main_backend_entries = [
        entry for entry in entries
        if main_targets <= _xi_normalized_lowering_targets(entry.targets)
    ]
    main_backend_patterned_entries = [
        entry for entry in patterned_entries
        if main_targets <= _xi_normalized_lowering_targets(entry.targets)
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
    lines.append(
        f'    assert(XI_LOWERING_CONSUMER_TARGET_COUNT == {sum(len(entry.target_consumers) for entry in entries)});')
    lines.append(
        f'    assert(XI_LOWERING_CONSUMER_ENTRY_COUNT == {sum(1 for entry in entries if entry.target_consumers)});')
    lines.append(
        f'    assert(XI_LOWERING_CONSUMER_BINDING_COUNT == {sum(len(bindings) for entry in entries for bindings in entry.target_consumers.values())});')
    lines.append(
        f'    assert(XI_LOWERING_CONSUMER_ROUTER_WITNESS_COUNT == {sum(len(binding.routers) for entry in entries for bindings in entry.target_consumers.values() for binding in bindings)});')
    lines.append(
        f'    assert(XI_LOWERING_CONSUMER_EMITTER_WITNESS_COUNT == {sum(len(binding.emitters) for entry in entries for bindings in entry.target_consumers.values() for binding in bindings)});')
    lines.append(
        f'    assert(XI_LOWERING_CONSUMER_PREDICATE_WITNESS_COUNT == {sum(len(binding.predicates) for entry in entries for bindings in entry.target_consumers.values() for binding in bindings)});')
    lines.append(
        f'    assert(XI_LOWERING_CONSUMER_PREDICATE_DOMAIN_WITNESS_COUNT == {sum(1 for entry in entries for bindings in entry.target_consumers.values() for binding in bindings for predicate in binding.predicates if predicate.domain_symbol is not None)});')
    lines.append(
        f'    assert(XI_LOWERING_CONSUMER_GUARDED_SELECTOR_COUNT == {sum(1 for entry in entries for bindings in entry.target_consumers.values() for binding in bindings if binding.witness_kind == "guarded-selector")});')
    lines.append(
        f'    assert(XI_LOWERING_CONSUMER_ACTIVATION_EDGE_COUNT == {sum(len(binding.activations) for entry in entries for bindings in entry.target_consumers.values() for binding in bindings)});')
    lines.append(
        f'    assert(XI_LOWERING_CONSUMER_ACTIVATION_CALL_COUNT == {sum(activation.count for entry in entries for bindings in entry.target_consumers.values() for binding in bindings for activation in binding.activations)});')
    lines.append(
        f'    assert(XI_LOWERING_CONSUMER_OUTPUT_SEQUENCE_COUNT == {sum(len(binding.output_sequences) for entry in entries for bindings in entry.target_consumers.values() for binding in bindings)});')
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
        driver_bits = _xi_bit_expr('XI_LOWER_TARGET', sorted(entry.target_drivers.keys()))
        consumer_bits = _xi_bit_expr('XI_LOWER_TARGET', sorted(entry.target_consumers.keys()))
        template = f'XI_LOWER_TEMPLATE_{_xi_c_ident(entry.template)}'
        patterned = 'true' if entry.template != 'custom' else 'false'
        aot_targets = {'aot-c', 'aot-c-stmt'}
        aot_implementations = set(entry.targets) & aot_targets
        aot_rejections = set(entry.target_rejects) & aot_targets
        backend_legal = ('true' if aot_implementations and not aot_rejections
                         else 'false')
        lines.append(f'    assert(xi_lowering_covered_targets(XI_{entry.ident}) == ({target_bits}));')
        lines.append(f'    assert(xi_lowering_driver_targets(XI_{entry.ident}) == ({driver_bits}));')
        lines.append(f'    assert(xi_lowering_consumer_targets(XI_{entry.ident}) == ({consumer_bits}));')
        lines.append(f'    assert(xi_lowering_required_targets(XI_{entry.ident}) == ({required_bits}));')
        lines.append(f'    assert(xi_lowering_rejected_targets(XI_{entry.ident}) == ({reject_bits}));')
        lines.append(f'    assert(xi_lowering_op_backend_legal(XI_{entry.ident}) == {backend_legal});')
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
                f'    assert(strcmp(xi_to_c_template_bitwise_binary_kind(XI_{entry.ident}), "{op_text}") == 0);')
        if 'aot-c' in entry.target_drivers and entry.op_name in XI_AOT_C_TEMPLATE_SHIFT:
            fn = XI_AOT_C_TEMPLATE_SHIFT[entry.op_name]
            lines.append(
                f'    assert(strcmp(xi_to_c_template_shift_kind(XI_{entry.ident}), "{fn}") == 0);')
        if 'aot-c' in entry.target_drivers and entry.op_name in XI_AOT_C_TEMPLATE_COMPARE:
            runtime_fn, relation, swaps = XI_AOT_C_TEMPLATE_COMPARE[entry.op_name]
            swaps_c = 'true' if swaps else 'false'
            lines.append(
                f'    assert(strcmp(xi_to_c_template_compare_runtime_fn(XI_{entry.ident}), "{runtime_fn}") == 0);')
            lines.append(
                f'    assert(strcmp(xi_to_c_template_compare_relation(XI_{entry.ident}), "{relation}") == 0);')
            lines.append(
                f'    assert(xi_to_c_template_compare_swaps_tagged_args(XI_{entry.ident}) == {swaps_c});')
        if 'aot-c' in entry.target_drivers and entry.template in {'narrow', 'widen'}:
            expected_kernel = XI_NUMERIC_WIDTH_KERNELS[entry.op_name]
            uses_f64 = 'true' if entry.op_name.endswith('.f32') else 'false'
            lines.append(
                f'    assert(strcmp(xi_to_c_template_width_numeric_kernel(XI_{entry.ident}), '
                f'"{expected_kernel}") == 0);')
            lines.append(
                f'    assert(xi_to_c_template_width_uses_f64_lane(XI_{entry.ident}) == {uses_f64});')
        fresh_dst = 'true' if entry.target_attrs.get('vm-bytecode', {}).get('fresh-dst',
                                                                            False) else 'false'
        lines.append(f'    assert(xi_emit_vm_requires_fresh_dst(XI_{entry.ident}) == {fresh_dst});')
    for op in ops:
        if op.lowering_policy == 'special':
            legal = 'true' if 'aot-c' in op.targets else 'false'
            lines.append(f'    assert(xi_lowering_op_backend_legal(XI_{op.ident}) == {legal});')
        elif op.lowering_policy == 'verifier-only':
            lines.append(f'    assert(!xi_lowering_op_backend_legal(XI_{op.ident}));')
    lines.append('    printf("Xi lowering generated coverage: %d entries\\n", XI_LOWERING_ENTRY_COUNT);')
    lines.append('    return 0;')
    lines.append('}')
    lines.append('')
    return '\n'.join(lines)


def _xi_lowering_output_contents(
        entries: list[XiLoweringDef],
        ops: list[XiOpDef]) -> list[tuple[str, str]]:
    return [
        ('src/ir/xi_lowering_coverage_gen.h', generate_xi_lowering_coverage_header(entries, ops)),
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
        ('tests/unit/ir/test_xi_lowering_gen.c', generate_xi_lowering_test(entries, ops)),
    ]


def write_xi_lowering_outputs(output_root: str, entries: list[XiLoweringDef],
                              ops: list[XiOpDef]) -> list[str]:
    written = []
    for relpath, content in _xi_lowering_output_contents(entries, ops):
        path = os.path.join(output_root, relpath)
        write_file_if_changed(path, content)
        written.append(path)
    return written


def _xi_capture_lowering_projection_snapshot(
        root: Path, expected: list[tuple[str, str]]) -> tuple[XiFileSnapshot, ...]:
    snapshots = []
    for relative, _ in expected:
        identity, data = _xi_secure_repository_file_snapshot(
            root, relative, 'xi-lowering: checked-in projection')
        snapshots.append(XiFileSnapshot(relative, identity, data))
    return tuple(snapshots)


def check_xi_lowering_outputs(output_root: str, entries: list[XiLoweringDef],
                              ops: list[XiOpDef],
                              after_first_compare_hook=None) -> list[str]:
    """Compare all projections as one identity-bound, recaptured snapshot."""
    root = Path(os.path.abspath(output_root))
    expected = _xi_lowering_output_contents(entries, ops)
    initial = _xi_capture_lowering_projection_snapshot(root, expected)
    for index, ((relpath, content), actual) in enumerate(zip(expected, initial)):
        if actual.data != content.encode('utf-8'):
            die("xi-lowering: checked-in projection differs from canonical "
                f"content: {relpath}; regenerate it before building")
        if index == 0 and after_first_compare_hook is not None:
            after_first_compare_hook()
    if _xi_capture_lowering_projection_snapshot(root, expected) != initial:
        die("xi-lowering: checked-in projection identity, bytes, or output set "
            "changed while the complete projection snapshot was checked")
    return [os.fspath(root / relative) for relative, _ in expected]


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
    if rep.name == 'isize':
        return 'XR_ELEM_I64'
    if rep.name == 'usize':
        return 'XR_ELEM_U64'
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
    'aggregate',
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
    scalar_rep: bool
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
                                 scalar_rep=_aot_bool_atom(form, ':scalar-rep', context),
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
    lines.append('    bool uses_scalar_rep;')
    lines.append('    bool typed_boundary;')
    lines.append('} XaotAbiInfo;')
    lines.append('')
    lines.append('#define XAOT_ABI_ENTRIES(X) \\')
    for i, entry in enumerate(entries):
        suffix = ' \\' if i + 1 < len(entries) else ''
        lines.append(
            f'    X({entry.ident}, "{entry.name}", {entry.type_kind}, '
            f'XAOT_ABI_CLASS_{_xi_c_ident(entry.abi_class)}, XAOT_REP_{_xi_c_ident(entry.default_rep)}, '
            f'{_c_bool(entry.nullable)}, {_c_bool(entry.scalar_rep)}, '
            f'{_c_bool(entry.typed_boundary)}){suffix}')
    lines.append('')
    lines.append('')
    lines.append('static inline const XaotAbiInfo *xaot_abi_for_type_kind(XrTypeKind kind) {')
    lines.append('    static const XaotAbiInfo table[] = {')
    for entry in entries:
        lines.append(f'        {{"{entry.name}", {entry.type_kind},')
        lines.append(f'         XAOT_ABI_CLASS_{_xi_c_ident(entry.abi_class)},')
        lines.append(f'         XAOT_REP_{_xi_c_ident(entry.default_rep)}, {_c_bool(entry.nullable)},')
        lines.append(f'         {_c_bool(entry.scalar_rep)}, {_c_bool(entry.typed_boundary)}}},')
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
    lines.append('    if (!type->is_nullable && xr_type_is_enum_metadata(type))')
    lines.append('        return XAOT_REP_I64;')
    lines.append('    abi = xaot_abi_for_type_kind(type->kind);')
    lines.append('    if (!abi || (type->is_nullable && !abi->allows_nullable))')
    lines.append('        return XAOT_REP_TAGGED;')
    lines.append('    if (abi->uses_scalar_rep &&')
    lines.append('        xaot_rep_from_native_type(type->scalar_rep, &rep))')
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
    lines.append('    if (!type->is_nullable && xr_type_is_enum_metadata(type))')
    lines.append('        return true;')
    lines.append('    abi = xaot_abi_for_type_kind(type->kind);')
    lines.append('    if (!abi || !abi->typed_boundary || (type->is_nullable && !abi->allows_nullable))')
    lines.append('        return false;')
    lines.append('    storage = xaot_abi_storage_rep_for_type(type);')
    lines.append('    const XaotRepInfo *rep_info = xaot_rep_info(abi->default_rep);')
    lines.append('    if (rep_info && rep_info->dynamic_kind == XAOT_DYNAMIC_AGGREGATE)')
    lines.append('        return true;')
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
# Typed TargetPlan instruction contract
# ============================================================

TARGET_REP_FAMILIES = {'none', 'u8', 'i64', 'bool', 'dyn-value', 'aggregate'}
TARGET_RESULT_OWNERSHIPS = {'none', 'trivial', 'borrow', 'owned'}
TARGET_OPERAND_OWNERSHIPS = {'borrow', 'consume'}
TARGET_EFFECTS = {'control', 'may-error', 'may-suspend', 'memory-write'}
TARGET_ERRORS = {'none', 'divide-by-zero', 'modulo-by-zero', 'entry-call', 'array-push'}
TARGET_IMMEDIATE_KINDS = {
    'none', 'i64', 'parameter-ordinal', 'jump-target', 'branch-targets',
    'call-record', 'entry-expectation', 'coroutine-state', 'field-record',
    'layout-record', 'overflow-predicate-record',
}
TARGET_CONTROL_KINDS = {'none', 'return', 'jump', 'branch', 'suspend'}
TARGET_DISPATCH_ARGUMENTS = {
    'const': {'none'},
    'param': {'none'},
    'copy': {'none'},
    'unary': {'neg', 'bnot'},
    'binary': {'add', 'sub', 'mul', 'band', 'bor', 'bxor'},
    'shift': {'left', 'right'},
    'divmod': {'div', 'mod'},
    'compare': {'eq', 'ne', 'lt', 'le', 'gt', 'ge'},
    'return': {'none'},
    'branch': {'jump', 'i64', 'bool'},
    'call': {'none'},
    'entry-call': {'none'},
    'suspend': {'none'},
    'array-push': {'none'},
    'return-unit': {'none'},
    'aggregate-get': {'none'},
    'aggregate-make': {'none'},
    'call-aggregate': {'none'},
    'return-aggregate': {'none'},
    'const-u8': {'none'},
    'value-product-init': {'none'},
    'value-product-set-i64': {'none'},
    'value-product-set-u8': {'none'},
    'value-product-get-u8': {'none'},
    'overflow': {'none'},
    'native-leaf': {'none'},
}


@dataclass(frozen=True)
class TargetInstructionDef:
    source_name: str
    ident: str
    stable_id: int
    name: str
    arity: int
    terminator: bool
    result_rep: str
    operand_reps: tuple[str, ...]
    result_ownership: str
    operand_ownership: tuple[str, ...]
    effects: tuple[str, ...]
    error: str
    suspend: bool
    immediate: str
    control: str
    semantic: str
    dispatch: str
    dispatch_arg: str


def _target_required_atom(form: SList, keyword: str, context: str) -> str:
    value = _xi_get_kw(form, keyword)
    if value is None:
        die(f"{context}: missing {keyword}")
    return _sexpr_atom_value(value, context)


def _target_required_int(form: SList, keyword: str, context: str) -> int:
    value = _xi_get_kw(form, keyword)
    if value is None or not isinstance(value, SAtom) or not value.is_number:
        die(f"{context}: {keyword} must be an integer")
    return value.int_value


def parse_target_instruction_def(text: str,
                                 path: str = '<input>') -> list[TargetInstructionDef]:
    forms = parse_sexpr(tokenize_sexpr(text, path), path)
    entries = []
    seen_source_names = set()
    seen_names = set()
    seen_idents = set()
    seen_ids = set()
    for form in forms:
        if not isinstance(form, SList) or not form.children:
            die(f"{path}: top-level form must be a list")
        head = _sexpr_atom_value(form.children[0], path)
        if head != 'define-target-instruction':
            die(f"{path}:{form.line}:{form.col}: expected define-target-instruction")
        if len(form.children) < 2:
            die(f"{path}:{form.line}:{form.col}: missing instruction name")
        source_name = _sexpr_atom_value(form.children[1], 'define-target-instruction')
        ident = _xi_c_ident(source_name)
        context = f"{path}:{source_name}"
        stable_id = _target_required_int(form, ':id', context)
        name = _target_required_atom(form, ':name', context)
        arity = _target_required_int(form, ':arity', context)
        terminator = _aot_bool_atom(form, ':terminator', context)
        result_rep = _target_required_atom(form, ':result-rep', context)
        operand_reps = tuple(_xi_parse_atom_list(
            _xi_get_kw_list(form, ':operand-reps'), f"{context}:operand-reps"))
        result_ownership = _target_required_atom(form, ':result-ownership', context)
        operand_ownership = tuple(_xi_parse_atom_list(
            _xi_get_kw_list(form, ':operand-ownership'),
            f"{context}:operand-ownership"))
        effects = tuple(_xi_parse_atom_list(
            _xi_get_kw_list(form, ':effects'), f"{context}:effects"))
        error = _target_required_atom(form, ':error', context)
        suspend = _aot_bool_atom(form, ':suspend', context)
        immediate = _target_required_atom(form, ':immediate', context)
        control = _target_required_atom(form, ':control', context)
        semantic = _target_required_atom(form, ':semantic', context)
        dispatch = _target_required_atom(form, ':dispatch', context)
        dispatch_arg = _target_required_atom(form, ':dispatch-arg', context)

        if not source_name or source_name in seen_source_names:
            die(f"{context}: duplicate or empty source name")
        if not ident or ident in seen_idents:
            die(f"{context}: duplicate or invalid C identifier '{ident}'")
        if not name or name in seen_names:
            die(f"{context}: duplicate or empty canonical name '{name}'")
        if stable_id <= 0 or stable_id > 65534 or stable_id in seen_ids:
            die(f"{context}: stable ID must be unique and in [1, 65534]")
        if arity < 0 or arity > 2 or arity != len(operand_reps):
            die(f"{context}: arity must exactly match zero to two operand reps")
        if result_rep not in TARGET_REP_FAMILIES:
            die(f"{context}: unknown result rep '{result_rep}'")
        if any(rep not in TARGET_REP_FAMILIES or rep == 'none'
               for rep in operand_reps):
            die(f"{context}: operand reps must be concrete target families")
        if result_ownership not in TARGET_RESULT_OWNERSHIPS:
            die(f"{context}: unknown result ownership '{result_ownership}'")
        if len(operand_ownership) != arity or any(
                ownership not in TARGET_OPERAND_OWNERSHIPS
                for ownership in operand_ownership):
            die(f"{context}: operand ownership must exactly classify every operand")
        if len(set(effects)) != len(effects) or any(
                effect not in TARGET_EFFECTS for effect in effects):
            die(f"{context}: effects contain an unknown or duplicate value")
        if error not in TARGET_ERRORS:
            die(f"{context}: unknown error kind '{error}'")
        if immediate not in TARGET_IMMEDIATE_KINDS:
            die(f"{context}: unknown immediate kind '{immediate}'")
        if control not in TARGET_CONTROL_KINDS:
            die(f"{context}: unknown control kind '{control}'")
        if dispatch not in TARGET_DISPATCH_ARGUMENTS or \
                dispatch_arg not in TARGET_DISPATCH_ARGUMENTS[dispatch]:
            die(f"{context}: invalid dispatch binding '{dispatch}/{dispatch_arg}'")
        if semantic != 'none':
            _xi_op_ident(semantic)

        has_result = result_rep != 'none'
        if terminator and has_result:
            die(f"{context}: terminators cannot produce a result")
        if (not has_result and result_ownership != 'none') or \
                (has_result and result_ownership == 'none'):
            die(f"{context}: result ownership does not match result presence")
        if terminator != (control != 'none') or terminator != ('control' in effects):
            die(f"{context}: terminator, control kind, and control effect disagree")
        if (error != 'none') != ('may-error' in effects):
            die(f"{context}: error kind and may-error effect disagree")
        if suspend != ('may-suspend' in effects):
            die(f"{context}: suspend bit and may-suspend effect disagree")
        if control == 'jump' and immediate != 'jump-target':
            die(f"{context}: jump control requires a jump target immediate")
        if control == 'branch' and immediate != 'branch-targets':
            die(f"{context}: branch control requires branch target immediates")
        if control == 'return' and immediate != 'none':
            die(f"{context}: return control cannot carry an immediate")
        if control == 'suspend' and immediate != 'coroutine-state':
            die(f"{context}: suspend control requires a coroutine state immediate")

        seen_source_names.add(source_name)
        seen_names.add(name)
        seen_idents.add(ident)
        seen_ids.add(stable_id)
        entries.append(TargetInstructionDef(
            source_name, ident, stable_id, name, arity, terminator, result_rep,
            operand_reps, result_ownership, operand_ownership, effects, error,
            suspend, immediate, control, semantic, dispatch, dispatch_arg))

    entries.sort(key=lambda entry: entry.stable_id)
    expected_ids = list(range(1, len(entries) + 1))
    if [entry.stable_id for entry in entries] != expected_ids:
        die(f"{path}: stable instruction IDs must be dense from 1")
    return entries


def _target_enum(prefix: str, value: str) -> str:
    return f"{prefix}_{_xi_c_ident(value)}"


def _target_effect_expr(effects: tuple[str, ...]) -> str:
    if not effects:
        return 'XR_TARGET_INSTRUCTION_EFFECT_NONE'
    return ' | '.join(_target_enum('XR_TARGET_INSTRUCTION_EFFECT', effect)
                      for effect in effects)


def generate_target_instruction_header(entries: list[TargetInstructionDef]) -> str:
    lines = [
        '/* AUTO-GENERATED by xisagen - DO NOT EDIT */',
        '/* Source: xisa/target/vm_ops.def */',
        '',
        '#ifndef XR_TARGET_INSTRUCTION_GEN_H',
        '#define XR_TARGET_INSTRUCTION_GEN_H',
        '',
        '#include <stdbool.h>',
        '#include <stddef.h>',
        '#include <stdint.h>',
        '',
        'typedef enum XrTargetInstructionOpcode {',
        '    XR_TARGET_INSTRUCTION_INVALID = 0,',
    ]
    for entry in entries:
        lines.append(f'    XR_TARGET_INSTRUCTION_{entry.ident} = {entry.stable_id},')
    lines.extend([
        f'    XR_TARGET_INSTRUCTION_COUNT = {len(entries) + 1}',
        '} XrTargetInstructionOpcode;',
        '',
        f'#define XR_TARGET_INSTRUCTION_CONTRACT_COUNT {len(entries)}u',
        f'#define XR_TARGET_INSTRUCTION_MAX_STABLE_ID {entries[-1].stable_id}u',
        '',
        'typedef enum XrTargetInstructionRepFamily {',
        '    XR_TARGET_INSTRUCTION_REP_NONE = 0,',
        '    XR_TARGET_INSTRUCTION_REP_U8,',
        '    XR_TARGET_INSTRUCTION_REP_I64,',
        '    XR_TARGET_INSTRUCTION_REP_BOOL,',
        '    XR_TARGET_INSTRUCTION_REP_DYN_VALUE,',
        '    XR_TARGET_INSTRUCTION_REP_AGGREGATE,',
        '} XrTargetInstructionRepFamily;',
        '',
        'typedef enum XrTargetInstructionResultOwnership {',
        '    XR_TARGET_INSTRUCTION_RESULT_OWNERSHIP_NONE = 0,',
        '    XR_TARGET_INSTRUCTION_RESULT_OWNERSHIP_TRIVIAL,',
        '    XR_TARGET_INSTRUCTION_RESULT_OWNERSHIP_BORROW,',
        '    XR_TARGET_INSTRUCTION_RESULT_OWNERSHIP_OWNED,',
        '} XrTargetInstructionResultOwnership;',
        '',
        'typedef enum XrTargetInstructionOperandOwnership {',
        '    XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_NONE = 0,',
        '    XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_BORROW,',
        '    XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_CONSUME,',
        '} XrTargetInstructionOperandOwnership;',
        '',
        'typedef enum XrTargetInstructionEffect {',
        '    XR_TARGET_INSTRUCTION_EFFECT_NONE = 0,',
        '    XR_TARGET_INSTRUCTION_EFFECT_CONTROL = 1u << 0,',
        '    XR_TARGET_INSTRUCTION_EFFECT_MAY_ERROR = 1u << 1,',
        '    XR_TARGET_INSTRUCTION_EFFECT_MAY_SUSPEND = 1u << 2,',
        '    XR_TARGET_INSTRUCTION_EFFECT_MEMORY_WRITE = 1u << 3,',
        '} XrTargetInstructionEffect;',
        '',
        'typedef enum XrTargetInstructionErrorKind {',
        '    XR_TARGET_INSTRUCTION_ERROR_NONE = 0,',
        '    XR_TARGET_INSTRUCTION_ERROR_DIVIDE_BY_ZERO,',
        '    XR_TARGET_INSTRUCTION_ERROR_MODULO_BY_ZERO,',
        '    XR_TARGET_INSTRUCTION_ERROR_ENTRY_CALL,',
        '    XR_TARGET_INSTRUCTION_ERROR_ARRAY_PUSH,',
        '} XrTargetInstructionErrorKind;',
        '',
        'typedef enum XrTargetInstructionImmediateKind {',
        '    XR_TARGET_INSTRUCTION_IMMEDIATE_NONE = 0,',
        '    XR_TARGET_INSTRUCTION_IMMEDIATE_I64,',
        '    XR_TARGET_INSTRUCTION_IMMEDIATE_PARAMETER_ORDINAL,',
        '    XR_TARGET_INSTRUCTION_IMMEDIATE_JUMP_TARGET,',
        '    XR_TARGET_INSTRUCTION_IMMEDIATE_BRANCH_TARGETS,',
        '    XR_TARGET_INSTRUCTION_IMMEDIATE_CALL_RECORD,',
        '    XR_TARGET_INSTRUCTION_IMMEDIATE_ENTRY_EXPECTATION,',
        '    XR_TARGET_INSTRUCTION_IMMEDIATE_COROUTINE_STATE,',
        '    XR_TARGET_INSTRUCTION_IMMEDIATE_FIELD_RECORD,',
        '    XR_TARGET_INSTRUCTION_IMMEDIATE_LAYOUT_RECORD,',
        '    XR_TARGET_INSTRUCTION_IMMEDIATE_OVERFLOW_PREDICATE_RECORD,',
        '} XrTargetInstructionImmediateKind;',
        '',
        'typedef enum XrTargetInstructionControlKind {',
        '    XR_TARGET_INSTRUCTION_CONTROL_NONE = 0,',
        '    XR_TARGET_INSTRUCTION_CONTROL_RETURN,',
        '    XR_TARGET_INSTRUCTION_CONTROL_JUMP,',
        '    XR_TARGET_INSTRUCTION_CONTROL_BRANCH,',
        '    XR_TARGET_INSTRUCTION_CONTROL_SUSPEND,',
        '} XrTargetInstructionControlKind;',
        '',
        'typedef enum XrTargetInstructionDispatchKind {',
        '    XR_TARGET_INSTRUCTION_DISPATCH_CONST = 0,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_PARAM,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_COPY,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_UNARY,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_BINARY,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_SHIFT,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_DIVMOD,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_COMPARE,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_RETURN,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_BRANCH,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_CALL,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ENTRY_CALL,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_SUSPEND,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARRAY_PUSH,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_RETURN_UNIT,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_AGGREGATE_GET,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_AGGREGATE_MAKE,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_CALL_AGGREGATE,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_RETURN_AGGREGATE,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_CONST_U8,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_VALUE_PRODUCT_INIT,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_VALUE_PRODUCT_SET_I64,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_VALUE_PRODUCT_SET_U8,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_VALUE_PRODUCT_GET_U8,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_OVERFLOW,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_NATIVE_LEAF,',
        '} XrTargetInstructionDispatchKind;',
        '',
        'typedef enum XrTargetInstructionDispatchArgument {',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_NONE = 0,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_ADD,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_SUB,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_MUL,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BAND,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BOR,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BXOR,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_NEG,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BNOT,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_LEFT,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_RIGHT,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_DIV,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_MOD,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_EQ,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_NE,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_LT,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_LE,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_GT,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_GE,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_JUMP,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_I64,',
        '    XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_BOOL,',
        '} XrTargetInstructionDispatchArgument;',
        '',
        'typedef struct XrTargetInstructionContract {',
        '    const char *name;',
        '    const char *semantic_name;',
        '    uint8_t arity;',
        '    uint8_t terminator;',
        '    uint8_t result_rep;',
        '    uint8_t operand_rep[2];',
        '    uint8_t result_ownership;',
        '    uint8_t operand_ownership[2];',
        '    uint8_t effects;',
        '    uint8_t error_kind;',
        '    uint8_t may_suspend;',
        '    uint8_t immediate_kind;',
        '    uint8_t control_kind;',
        '    uint8_t dispatch_kind;',
        '    uint8_t dispatch_argument;',
        '} XrTargetInstructionContract;',
        '',
        'static inline const XrTargetInstructionContract *',
        'xr_target_instruction_contract(uint16_t opcode) {',
        '    static const XrTargetInstructionContract contracts[] = {',
        '        {NULL, NULL, 0, false, XR_TARGET_INSTRUCTION_REP_NONE,',
        '         {XR_TARGET_INSTRUCTION_REP_NONE, XR_TARGET_INSTRUCTION_REP_NONE},',
        '         XR_TARGET_INSTRUCTION_RESULT_OWNERSHIP_NONE,',
        '         {XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_NONE,',
        '          XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP_NONE},',
        '         XR_TARGET_INSTRUCTION_EFFECT_NONE, XR_TARGET_INSTRUCTION_ERROR_NONE,',
        '         false, XR_TARGET_INSTRUCTION_IMMEDIATE_NONE,',
        '         XR_TARGET_INSTRUCTION_CONTROL_NONE,',
        '         XR_TARGET_INSTRUCTION_DISPATCH_CONST,',
        '         XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT_NONE},',
    ])
    for entry in entries:
        operand_reps = list(entry.operand_reps) + ['none'] * (2 - entry.arity)
        operand_ownership = list(entry.operand_ownership) + ['none'] * (2 - entry.arity)
        semantic_name = 'NULL' if entry.semantic == 'none' else f'"{entry.semantic}"'
        lines.extend([
            f'        {{"{entry.name}", {semantic_name}, {entry.arity},',
            f'         {str(entry.terminator).lower()}, '
            f'{_target_enum("XR_TARGET_INSTRUCTION_REP", entry.result_rep)},',
            f'         {{{_target_enum("XR_TARGET_INSTRUCTION_REP", operand_reps[0])},',
            f'          {_target_enum("XR_TARGET_INSTRUCTION_REP", operand_reps[1])}}},',
            f'         {_target_enum("XR_TARGET_INSTRUCTION_RESULT_OWNERSHIP", entry.result_ownership)},',
            f'         {{{_target_enum("XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP", operand_ownership[0])},',
            f'          {_target_enum("XR_TARGET_INSTRUCTION_OPERAND_OWNERSHIP", operand_ownership[1])}}},',
            f'         {_target_effect_expr(entry.effects)},',
            f'         {_target_enum("XR_TARGET_INSTRUCTION_ERROR", entry.error)},',
            f'         {str(entry.suspend).lower()},',
            f'         {_target_enum("XR_TARGET_INSTRUCTION_IMMEDIATE", entry.immediate)},',
            f'         {_target_enum("XR_TARGET_INSTRUCTION_CONTROL", entry.control)},',
            f'         {_target_enum("XR_TARGET_INSTRUCTION_DISPATCH", entry.dispatch)},',
            f'         {_target_enum("XR_TARGET_INSTRUCTION_DISPATCH_ARGUMENT", entry.dispatch_arg)}}},',
        ])
    lines.extend([
        '    };',
        '    if (opcode <= XR_TARGET_INSTRUCTION_INVALID ||',
        '        opcode >= XR_TARGET_INSTRUCTION_COUNT)',
        '        return NULL;',
        '    return &contracts[opcode];',
        '}',
        '',
        'static inline const char *xr_target_instruction_opcode_name(uint16_t opcode) {',
        '    const XrTargetInstructionContract *contract =',
        '        xr_target_instruction_contract(opcode);',
        '    return contract ? contract->name : NULL;',
        '}',
        '',
        'static inline bool xr_target_instruction_is_terminator(uint16_t opcode) {',
        '    const XrTargetInstructionContract *contract =',
        '        xr_target_instruction_contract(opcode);',
        '    return contract && contract->terminator;',
        '}',
        '',
    ])
    # This macro is the canonical default lowering map. More specialized
    # instructions may share the same semantic opcode but are selected only
    # after their extra TargetPlan authority has been materialized, so the
    # first stable-ID binding remains the unique default.
    seen_semantics = set()
    semantic_entries = []
    for entry in entries:
        if entry.semantic == 'none' or entry.semantic in seen_semantics:
            continue
        seen_semantics.add(entry.semantic)
        semantic_entries.append(entry)
    lines.append('#define XR_TARGET_INSTRUCTION_SEMANTIC_BINDINGS(X) \\')
    for index, entry in enumerate(semantic_entries):
        suffix = ' \\' if index + 1 < len(semantic_entries) else ''
        lines.append(
            f'    X(XI_{_xi_op_ident(entry.semantic)}, {entry.ident}){suffix}')
    lines.extend(['', '#endif  /* XR_TARGET_INSTRUCTION_GEN_H */', ''])
    return '\n'.join(lines)


def generate_target_vm_ops(entries: list[TargetInstructionDef]) -> str:
    lines = [
        '/* AUTO-GENERATED by xisagen - DO NOT EDIT */',
        '/* Source: xisa/target/vm_ops.def */',
        '',
    ]
    for entry in entries:
        handler = re.sub(r'[^0-9A-Za-z_]', '_', entry.dispatch).lower()
        lines.append(f'XR_VM_OP({entry.ident}, {handler}, '
                     f'{_xi_c_ident(entry.dispatch)}, '
                     f'{_xi_c_ident(entry.dispatch_arg)})')
    lines.append('')
    return '\n'.join(lines)


def write_target_vm_outputs(output_root: str,
                            entries: list[TargetInstructionDef]) -> list[str]:
    outputs = {
        'src/plan/target/xr_target_instruction_gen.h':
            generate_target_instruction_header(entries),
        'src/vm/xr_vm_ops.def': generate_target_vm_ops(entries),
    }
    written = []
    for relative, content in outputs.items():
        output = os.path.join(output_root, *relative.split('/'))
        write_file(output, content)
        written.append(output)
    return written


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


def cmd_semantic_ops(args: list[str]):
    if len(args) != 2:
        die("usage: xisagen.py semantic-ops <ops.def> <output-root>")
    source = read_file(args[0])
    ops = parse_xi_ops_def(source, args[0])
    if not ops:
        die(f"no Xi ops parsed from {args[0]}")
    owners = parse_xi_semantic_owners(source, ops, args[0])
    explicit_owners, observable_rows = parse_xi_observable_owners(
        source, ops, owners, args[0])
    output_root = args[1]
    outputs = {
        'src/plan/semantic/xr_semantic_ops_gen.h':
            generate_xi_semantic_ops_header(ops, owners, observable_rows),
        'src/shared/xr_semantic_owner_ids_gen.h':
            generate_semantic_owner_ids_header(explicit_owners, observable_rows),
        'contracts/semantic-owner-registry.json':
            generate_semantic_owner_registry_json(observable_rows),
    }
    for relative, content in outputs.items():
        output = os.path.join(output_root, *relative.split('/'))
        write_file(output, content)
        print(f"xisagen: generated {output}", file=sys.stderr)
    print(f"xisagen: generated {len(ops)} target-neutral semantic op contracts",
          file=sys.stderr)

def cmd_xi_lowering(args: list[str]):
    if len(args) != 3:
        die("usage: xisagen.py xi-lowering <ops.def> <lowering.def> <output-root>")
    ops_path = Path(os.path.abspath(args[0]))
    lowering_path = Path(os.path.abspath(args[1]))
    source_root = ops_path.parents[2]
    ops_relative = _xi_repository_relative_name(
        source_root, ops_path, 'xi-lowering: ops schema')
    lowering_relative = _xi_repository_relative_name(
        source_root, lowering_path, 'xi-lowering: lowering schema')
    ops_source = _xi_secure_repository_file_bytes(
        source_root, ops_relative, 'xi-lowering: ops schema')
    lowering_source = _xi_secure_repository_file_bytes(
        source_root, lowering_relative, 'xi-lowering: lowering schema')
    try:
        ops_text = ops_source.decode('utf-8', errors='strict')
        lowering_text = lowering_source.decode('utf-8', errors='strict')
    except UnicodeDecodeError as error:
        die(f"xi-lowering: schema input is not UTF-8: {error}")
    ops = parse_xi_ops_def(ops_text, args[0])
    entries = parse_xi_lowering_def(lowering_text, ops, args[1])
    if not entries:
        die(f"no Xi lowering entries parsed from {args[1]}")
    snapshot = capture_xi_lowering_validation_snapshot(source_root)
    discovery_snapshot = _xi_capture_aot_discovery_census(source_root)
    validate_xi_lowering_consumer_sources(
        entries, source_root, args[1], snapshot, discovery_snapshot)
    expected_stamp = _xi_lowering_validation_stamp_content(
        ops_source, lowering_source, snapshot, discovery_snapshot)
    validation_stamp = os.environ.get('XRAY_XI_LOWERING_VALIDATION_STAMP')
    if validation_stamp:
        _xi_require_lowering_validation_stamp(
            Path(validation_stamp), expected_stamp)
    if (_xi_secure_repository_file_bytes(
            source_root, ops_relative,
            'xi-lowering: ops schema') != ops_source or
            _xi_secure_repository_file_bytes(
                source_root, lowering_relative,
                'xi-lowering: lowering schema') != lowering_source or
            capture_xi_lowering_validation_snapshot(source_root) != snapshot or
            _xi_capture_aot_discovery_census(source_root) != discovery_snapshot):
        die("xi-lowering: validated inputs changed before generation")
    outputs = write_xi_lowering_outputs(args[2], entries, ops)
    print(f"xisagen: parsed {len(entries)} Xi lowering entries from {args[1]}", file=sys.stderr)
    for path in outputs:
        print(f"xisagen: generated {path}", file=sys.stderr)


def _xi_run_lowering_check(
        ops_path: Path, lowering_path: Path, output_root: Path, stamp: Path,
        *, after_validation_hook=None, after_first_projection_hook=None
) -> list[str]:
    ops_path = Path(os.path.abspath(ops_path))
    lowering_path = Path(os.path.abspath(lowering_path))
    source_root = ops_path.parents[2]
    ops_relative = _xi_repository_relative_name(
        source_root, ops_path, 'xi-lowering-check: ops schema')
    lowering_relative = _xi_repository_relative_name(
        source_root, lowering_path, 'xi-lowering-check: lowering schema')
    ops_snapshot = _xi_capture_repository_file(
        source_root, ops_relative, 'xi-lowering-check: ops schema')
    lowering_snapshot = _xi_capture_repository_file(
        source_root, lowering_relative, 'xi-lowering-check: lowering schema')
    try:
        ops_text = ops_snapshot.data.decode('utf-8', errors='strict')
        lowering_text = lowering_snapshot.data.decode('utf-8', errors='strict')
    except UnicodeDecodeError as error:
        die(f"xi-lowering-check: schema input is not UTF-8: {error}")
    ops = parse_xi_ops_def(ops_text, os.fspath(ops_path))
    entries = parse_xi_lowering_def(
        lowering_text, ops, os.fspath(lowering_path))
    if not entries:
        die(f"no Xi lowering entries parsed from {lowering_path}")
    closure = capture_xi_lowering_validation_snapshot(source_root)
    discovery = _xi_capture_aot_discovery_census(source_root)
    validate_xi_lowering_consumer_sources(
        entries, source_root, os.fspath(lowering_path), closure, discovery)
    expected_stamp = _xi_lowering_validation_stamp_content(
        ops_snapshot.data, lowering_snapshot.data, closure, discovery)
    stamp_snapshot = _xi_require_lowering_validation_stamp(stamp, expected_stamp)
    if after_validation_hook is not None:
        after_validation_hook()
    expected_outputs = _xi_lowering_output_contents(entries, ops)
    projection_snapshot = _xi_capture_lowering_projection_snapshot(
        output_root, expected_outputs)
    outputs = check_xi_lowering_outputs(
        os.fspath(output_root), entries, ops,
        after_first_compare_hook=after_first_projection_hook)
    unchanged = (
        _xi_capture_repository_file(
            source_root, ops_relative,
            'xi-lowering-check: ops schema') == ops_snapshot and
        _xi_capture_repository_file(
            source_root, lowering_relative,
            'xi-lowering-check: lowering schema') == lowering_snapshot and
        capture_xi_lowering_validation_snapshot(source_root) == closure and
        _xi_capture_aot_discovery_census(source_root) == discovery and
        _xi_require_lowering_validation_stamp(stamp, expected_stamp) ==
            stamp_snapshot and
        _xi_capture_lowering_projection_snapshot(
            output_root, expected_outputs) == projection_snapshot)
    if not unchanged:
        die("xi-lowering-check: schema, proof stamp, compile closure, discovery, "
            "or projection snapshot changed before final success")
    return outputs


def cmd_xi_lowering_check(args: list[str]):
    if len(args) != 4:
        die("usage: xisagen.py xi-lowering-check "
            "<ops.def> <lowering.def> <output-root> <validation-stamp>")
    outputs = _xi_run_lowering_check(
        Path(args[0]), Path(args[1]), Path(os.path.abspath(args[2])),
        Path(args[3]).resolve())
    print(f"xisagen: verified {len(outputs)} checked-in Xi lowering projections",
          file=sys.stderr)


def _xi_depfile_escape(path: str) -> str:
    if any(char in path for char in '\0\r\n'):
        die("xi-lowering: depfile path contains a forbidden control character")
    escaped = []
    for char in path:
        if char == '$':
            escaped.append('$$')
        elif char in {'\\', '#', ' ', '\t', ':'}:
            escaped.append('\\' + char)
        else:
            escaped.append(char)
    return ''.join(escaped)


def _xi_lowering_validation_stamp_content(
        ops_source: bytes, lowering_source: bytes,
        snapshot: XiLoweringValidationSnapshot,
        discovery_snapshot: XiAotDiscoverySnapshot) -> str:
    return (
        "xi-lowering-source-validation/3\n"
        f"ops_sha256={hashlib.sha256(ops_source).hexdigest()}\n"
        f"lowering_sha256={hashlib.sha256(lowering_source).hexdigest()}\n"
        f"aot_sources={len(snapshot.sources)}\n"
        f"include_directories={len(snapshot.include_directories)}\n"
        f"include_directory_snapshots={len(snapshot.directories)}\n"
        f"closure_fingerprint={snapshot.fingerprint()}\n"
        f"closure_identity_fingerprint={snapshot.identity_fingerprint()}\n"
        f"discovery_sources={len(discovery_snapshot.sources)}\n"
        f"discovery_directories={len(discovery_snapshot.directories)}\n"
        f"discovery_fingerprint={discovery_snapshot.fingerprint()}\n"
        f"discovery_identity_fingerprint="
        f"{discovery_snapshot.identity_fingerprint()}\n"
    )


def _xi_capture_regular_file_snapshot(path: Path, context: str) -> XiFileSnapshot:
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes

        kernel32 = _xi_windows_kernel32()
        handle = kernel32.CreateFileW(
            os.fspath(path), _xi_windows_component_access(False), 0x00000001,
            None, 3, 0x00200000, None)
        if handle == ctypes.c_void_p(-1).value:
            die(f"{context}: cannot open {path}: "
                f"{ctypes.WinError(ctypes.get_last_error())}")
        try:
            info = _xi_windows_handle_info(handle)
            if (info.dwFileAttributes & 0x00000410 or
                    kernel32.GetFileType(wintypes.HANDLE(handle)) != 0x0001):
                die(f"{context}: not a regular file: {path}")
            content = _xi_windows_read_open_file(
                handle, info, os.fspath(path), context)
            identity = _xi_windows_stable_file_identity(info)
        finally:
            kernel32.CloseHandle(wintypes.HANDLE(handle))
    else:
        flags = (os.O_RDONLY | getattr(os, 'O_CLOEXEC', 0) |
                 getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0))
        try:
            descriptor = os.open(path, flags)
        except OSError as error:
            die(f"{context}: cannot open {path}: {error}")
        try:
            before = os.fstat(descriptor)
            if not stat.S_ISREG(before.st_mode):
                die(f"{context}: not a regular file: {path}")
            chunks = []
            while True:
                chunk = os.read(descriptor, 65536)
                if not chunk:
                    break
                chunks.append(chunk)
            if _xi_stable_file_identity(before) != \
                    _xi_stable_file_identity(os.fstat(descriptor)):
                die(f"{context}: changed while read: {path}")
            content = b''.join(chunks)
            identity = _xi_stable_file_identity(before)
        finally:
            os.close(descriptor)
    return XiFileSnapshot(os.fspath(path), identity, content)


def _xi_require_lowering_validation_stamp(
        stamp: Path, expected_content: str) -> XiFileSnapshot:
    snapshot = _xi_capture_regular_file_snapshot(
        stamp, 'xi-lowering: validation stamp')
    if snapshot.data != expected_content.encode('utf-8'):
        die("xi-lowering: validation stamp does not match the descriptor-bound "
            "schema, compile closure, and discovery snapshots")
    return snapshot


def _xi_stage_atomic_write(path: Path, content: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(
        prefix=path.name + '.tmp.', dir=path.parent)
    try:
        with os.fdopen(handle, 'w', encoding='utf-8', newline='\n') as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        return Path(temporary)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def _xi_remove_atomic_temps(path: Path) -> None:
    if not path.parent.exists():
        return
    for temporary in path.parent.glob(path.name + '.tmp.*'):
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _xi_sync_parent_directory(path: Path) -> None:
    if os.name == 'nt':
        return
    directory_fd = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def _xi_atomic_write(path: Path, content: str) -> None:
    _xi_remove_atomic_temps(path)
    temporary = _xi_stage_atomic_write(path, content)
    try:
        os.replace(temporary, path)
        _xi_sync_parent_directory(path)
    except BaseException:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def _xi_publish_lowering_validation_artifacts(
        *, ops_path: Path, ops_source: bytes,
        lowering_path: Path, lowering_source: bytes,
        source_root: Path, snapshot: XiLoweringValidationSnapshot,
        discovery_snapshot: XiAotDiscoverySnapshot,
        depfile: Path, depfile_content: str,
        stamp: Path, stamp_content: str,
        before_stamp_hook=None, after_stamp_hook=None) -> None:
    """Publish an exact proof, then fail closed unless its inputs recapture."""
    try:
        stamp.unlink()
    except FileNotFoundError:
        pass
    _xi_remove_atomic_temps(stamp)
    _xi_remove_atomic_temps(depfile)
    try:
        _xi_atomic_write(depfile, depfile_content)
        if before_stamp_hook is not None:
            before_stamp_hook()
        _xi_atomic_write(stamp, stamp_content)
        published_stamp = _xi_require_lowering_validation_stamp(
            stamp, stamp_content)
        if after_stamp_hook is not None:
            after_stamp_hook()
        ops_relative = _xi_repository_relative_name(
            source_root, ops_path, 'xi-lowering: ops schema')
        lowering_relative = _xi_repository_relative_name(
            source_root, lowering_path, 'xi-lowering: lowering schema')
        unchanged = (
            _xi_secure_repository_file_bytes(
                source_root, ops_relative,
                'xi-lowering: ops schema') == ops_source and
            _xi_secure_repository_file_bytes(
                source_root, lowering_relative,
                'xi-lowering: lowering schema') == lowering_source and
            capture_xi_lowering_validation_snapshot(source_root) == snapshot and
            _xi_capture_aot_discovery_census(source_root) == discovery_snapshot and
            _xi_require_lowering_validation_stamp(
                stamp, stamp_content) == published_stamp)
        if not unchanged:
            die("xi-lowering: validation inputs changed after the proof stamp "
                "was published")
    except BaseException:
        try:
            stamp.unlink()
        except FileNotFoundError:
            pass
        try:
            depfile.unlink()
        except FileNotFoundError:
            pass
        raise


def _test_xi_lowering_build_artifacts() -> None:
    print("  test_xi_lowering_build_artifacts...", end='', file=sys.stderr)
    assert _xi_windows_component_access(True) == 0x0080
    assert _xi_windows_component_access(False) & 0x80000000
    assert _xi_windows_component_access(None) & 0x80000000
    if os.name == 'nt':
        kernel32 = _xi_windows_kernel32()
        assert len(kernel32.CreateFileW.argtypes) == 7
        assert len(kernel32.GetFileInformationByHandle.argtypes) == 2
        assert (kernel32.GetFileInformationByHandle.argtypes[1]._type_ is
                _xi_windows_file_info_type())
        assert len(kernel32.ReadFile.argtypes) == 5
        assert len(kernel32.GetFileType.argtypes) == 1
        assert len(kernel32.CloseHandle.argtypes) == 1
    windows_path = r'C:\xray build\owner#1$.c'
    assert _xi_depfile_escape(windows_path) == \
        r'C\:\\xray\ build\\owner\#1$$.c'
    assert _xi_depfile_escape('owner\tfile.c') == 'owner\\\tfile.c'
    for hostile in ('owner\0file.c', 'owner\rfile.c', 'owner\nfile.c'):
        try:
            _xi_depfile_escape(hostile)
            assert False, "depfile control characters must fail closed"
        except SystemExit:
            pass
    assert _xi_local_c_include_spellings(
        '#include "owner.c" /* first */ /* second */ // tracked\n',
        'literal include fixture') == ['owner.c']
    assert _xi_local_c_include_spellings(
        '#include <stddef.h>\n#include "owner.h"\n',
        'header include fixture') == ['owner.h']
    assert _xi_literal_c_includes(
        '??=inc??/\nlude "tri.h"\n#inc\\\nlude <angle.h>\n',
        'translation phase include fixture') == [
            ('quoted', 'tri.h'), ('angle', 'angle.h')]
    assert 'XI_GO' in _xi_c_source_without_literals('XI_??/\nGO')
    assert 'XI_GO' in _xi_c_source_without_literals('XI_\\\nGO')
    assert 'XI_GO' not in _xi_c_source_without_literals(
        '"XI_??/\nGO" /* XI_\\\nGO */')
    try:
        _xi_local_c_include_spellings(
            '#define OWNER "../../outside.c"\n#include OWNER\n',
            'hostile include operand fixture')
        assert False, "macro-expanded includes must fail closed"
    except SystemExit:
        pass
    assert _xi_local_c_include_spellings(
        '#\finclude "../../outside.c"\n',
        'form-feed include fixture') == ['../../outside.c']
    assert _xi_local_c_include_spellings(
        '#\vinclude "../../outside.c"\n',
        'vertical-tab include fixture') == ['../../outside.c']

    with tempfile.TemporaryDirectory(prefix='xisagen-lowering-build-') as directory:
        root = Path(directory)
        (root / 'CMakeLists.txt').write_text(
            'set(XRAY_COMMON_INCLUDES\n'
            '    ${CMAKE_CURRENT_SOURCE_DIR}/include\n'
            '    ${CMAKE_CURRENT_SOURCE_DIR}/src\n'
            '    ${CMAKE_CURRENT_SOURCE_DIR}/src/base\n'
            '    ${CMAKE_CURRENT_SOURCE_DIR}/src/aot\n'
            ')\n', encoding='utf-8')
        aot = root / 'src/aot'
        aot.mkdir(parents=True)
        (aot / 'xi_cgen.c').write_text(
            '#include "owner.c" /* first */ /* second */ // tracked\n',
            encoding='utf-8')
        (aot / 'owner.c').write_text(
            '#include "router.c"\n', encoding='utf-8')
        (aot / 'router.c').write_text(
            'static void route(void) {}\n', encoding='utf-8')
        if os.name == 'nt':
            from ctypes import wintypes

            handles, info = _xi_windows_open_repository_object(
                root, 'src/aot/owner.c', 'xi-lowering: Windows ABI self-test',
                directory=False)
            try:
                assert isinstance(info, _xi_windows_file_info_type())
                assert isinstance(
                    _xi_windows_handle_info(handles[-1]),
                    _xi_windows_file_info_type())
            finally:
                for handle in reversed(handles):
                    _xi_windows_kernel32().CloseHandle(
                        wintypes.HANDLE(handle))
        nested = aot / 'nested'
        nested.mkdir()
        (nested / 'unrelated.c').write_text(
            'static void unrelated(void) {}\n', encoding='utf-8')
        include_directory = root / 'include'
        base_directory = root / 'src/base'
        include_directory.mkdir()
        base_directory.mkdir()
        (include_directory / 'ordered_owner.h').write_text(
            'static void include_order_owner(void) {}\n', encoding='utf-8')
        (base_directory / 'ordered_owner.h').write_text(
            'static void base_order_owner(void) {}\n', encoding='utf-8')

        snapshot = capture_xi_lowering_validation_snapshot(root)
        assert [relative for relative, _ in snapshot.sources] == [
            'src/aot/owner.c',
            'src/aot/router.c',
            'src/aot/xi_cgen.c',
        ]
        assert 'src/aot/nested/unrelated.c' not in snapshot.source_bytes()
        assert {'include', 'src', 'src/base', 'src/aot'} <= {
            relative for relative, _ in snapshot.directories}
        discovery = _xi_capture_aot_discovery_census(root)
        assert 'src/aot/nested/unrelated.c' in dict(discovery.sources)
        assert {relative for relative, _ in discovery.directories} == {
            'src/aot', 'src/aot/nested'}
        old_fingerprint = snapshot.fingerprint()

        (aot / 'xi_cgen.c').write_text(
            '#include "ordered_owner.h"\n', encoding='utf-8')
        ordered = capture_xi_lowering_validation_snapshot(root)
        assert 'include/ordered_owner.h' in ordered.source_bytes()
        assert 'src/base/ordered_owner.h' not in ordered.source_bytes()
        (aot / 'xi_cgen.c').write_text(
            '#include <ordered_owner.h>\n#include <stddef.h>\n',
            encoding='utf-8')
        angle_ordered = capture_xi_lowering_validation_snapshot(root)
        assert 'include/ordered_owner.h' in angle_ordered.source_bytes()
        assert 'src/base/ordered_owner.h' not in angle_ordered.source_bytes()
        angle_mtime = (include_directory / 'ordered_owner.h').stat().st_mtime_ns
        (include_directory / 'ordered_owner.h').write_text(
            'static void include_order_drift(void) {}\n', encoding='utf-8')
        os.utime(include_directory / 'ordered_owner.h',
                 ns=(angle_mtime, angle_mtime))
        assert capture_xi_lowering_validation_snapshot(root) != angle_ordered
        (include_directory / 'ordered_owner.h').write_text(
            'static void include_order_owner(void) {}\n', encoding='utf-8')
        (aot / 'xi_cgen.c').write_text(
            '#include "ordered_owner.h"\n', encoding='utf-8')
        (aot / 'ordered_owner.h').write_text(
            'static void local_order_owner(void) {}\n', encoding='utf-8')
        local_first = capture_xi_lowering_validation_snapshot(root)
        assert 'src/aot/ordered_owner.h' in local_first.source_bytes()
        assert 'include/ordered_owner.h' not in local_first.source_bytes()
        assert local_first.fingerprint() != ordered.fingerprint()
        (aot / 'ordered_owner.h').unlink()
        (root / 'src/parent_owner.h').write_text(
            'static void parent_owner(void) {}\n', encoding='utf-8')
        (aot / 'xi_cgen.c').write_text(
            '#include "../parent_owner.h"\n', encoding='utf-8')
        parent_relative = capture_xi_lowering_validation_snapshot(root)
        assert 'src/parent_owner.h' in parent_relative.source_bytes()
        (root / 'src/parent_owner.h').unlink()
        (aot / 'xi_cgen.c').write_text(
            '#include "owner.c" /* first */ /* second */ // tracked\n',
            encoding='utf-8')

        (aot / 'new_owner.c').write_text(
            'static void new_owner(void) {}\n', encoding='utf-8')
        (aot / 'owner.c').write_text(
            '#include "router.c"\n#include "new_owner.c"\n', encoding='utf-8')
        expanded = capture_xi_lowering_validation_snapshot(root)
        assert 'src/aot/new_owner.c' in expanded.source_bytes()
        assert expanded.fingerprint() != old_fingerprint

        (aot / 'new_owner.c').unlink()
        try:
            capture_xi_lowering_validation_snapshot(root)
            assert False, "a deleted include-closure member must fail closed"
        except SystemExit:
            pass
        (aot / 'owner.c').write_text(
            '#include "router.c"\n', encoding='utf-8')

        preserved_time = (aot / 'router.c').stat().st_mtime_ns
        (aot / 'router.c').write_text(
            'static void routed(void) {}\n', encoding='utf-8')
        os.utime(aot / 'router.c', ns=(preserved_time, preserved_time))
        assert capture_xi_lowering_validation_snapshot(root) != snapshot
        (aot / 'router.c').write_text(
            'static void route(void) {}\n', encoding='utf-8')

        if hasattr(os, 'symlink'):
            symlink = aot / 'owner-link.c'
            directory_symlink = aot / 'outside-link'
            external = root.parent / f'{root.name}-outside'
            symlink_mutations = 0
            try:
                for target in ('owner.c', 'router.c'):
                    try:
                        symlink.unlink()
                    except FileNotFoundError:
                        pass
                    symlink.symlink_to(target)
                    symlink_mutations += 1
                    (aot / 'xi_cgen.c').write_text(
                        '#include "owner-link.c"\n', encoding='utf-8')
                    try:
                        capture_xi_lowering_validation_snapshot(root)
                        assert False, "a lexical include symlink retarget must fail closed"
                    except SystemExit:
                        pass
                external.mkdir()
                (external / 'owner.c').write_text(
                    'static void outside_owner(void) {}\n', encoding='utf-8')
                directory_symlink.symlink_to(external, target_is_directory=True)
                symlink_mutations += 1
                for hostile_include in (
                        'outside-link/owner.c',
                        'outside-link/../owner.c',
                        '../../outside-link/../src/aot/owner.c'):
                    (aot / 'xi_cgen.c').write_text(
                        f'#include "{hostile_include}"\n', encoding='utf-8')
                    try:
                        capture_xi_lowering_validation_snapshot(root)
                        assert False, "symlink/parent traversal include must fail closed"
                    except SystemExit:
                        pass

                for hostile_directive in (
                        '#include "../../outside.c" /*a*/ /*b*/\n',
                        '#include /*prefix*/ "../outside.c"\n',
                        '#inc\\\nlude "../../outside.c"\n',
                        '#inc??/\nlude "../../outside.c"\n',
                        '#/**/include "../../outside.c"\n',
                        '%:include "../../outside.c"\n',
                        '??=include "../../outside.c"\n',
                        '??=inc??/\nlude "../../outside.c"\n',
                        '#\finclude "../../outside.c"\n',
                        '#\vinclude "../../outside.c"\n',
                        '#include "owner.c" trailing\n'):
                    (aot / 'xi_cgen.c').write_text(
                        hostile_directive, encoding='utf-8')
                    try:
                        capture_xi_lowering_validation_snapshot(root)
                        assert False, "noncanonical local C include must fail closed"
                    except SystemExit:
                        pass
            except OSError as error:
                unsupported = {
                    errno.EACCES, errno.EPERM, errno.ENOTSUP,
                    getattr(errno, 'EOPNOTSUPP', errno.ENOTSUP),
                }
                if error.errno not in unsupported:
                    raise
            finally:
                try:
                    symlink.unlink()
                except FileNotFoundError:
                    pass
                try:
                    directory_symlink.unlink()
                except FileNotFoundError:
                    pass
                try:
                    (external / 'owner.c').unlink()
                    external.rmdir()
                except FileNotFoundError:
                    pass
            (aot / 'xi_cgen.c').write_text(
                '#include "owner.c" /* first */ /* second */ // tracked\n',
                encoding='utf-8')
            if os.name != 'nt' and getattr(os, 'O_NOFOLLOW', 0):
                assert symlink_mutations == 3

            owner = aot / 'owner.c'
            owner_saved = aot / 'owner.saved'
            file_swapped = False

            def swap_file_before_open(relative, _index, _component, final) -> None:
                nonlocal file_swapped
                if (not file_swapped and final and
                        relative == 'src/aot/owner.c'):
                    owner.rename(owner_saved)
                    owner.symlink_to('router.c')
                    file_swapped = True

            file_swap_rejected = False
            try:
                capture_xi_lowering_validation_snapshot(
                    root, before_open_hook=swap_file_before_open)
                assert False, "a check-to-open file symlink swap must fail closed"
            except SystemExit:
                file_swap_rejected = True
            finally:
                if owner.is_symlink():
                    owner.unlink()
                if owner_saved.exists():
                    owner_saved.rename(owner)
            assert file_swap_rejected and file_swapped

            closure_header = nested / 'closure-owner.h'
            closure_header.write_text(
                'static void closure_owner(void) {}\n', encoding='utf-8')
            owner.write_text(
                '#include "router.c"\n#include "nested/closure-owner.h"\n',
                encoding='utf-8')
            closure_nested_saved = aot / 'nested.closure-saved'
            closure_external = root / 'closure-external'
            closure_directory_swapped = False

            def swap_closure_directory_before_open(
                    relative, _index, component, final) -> None:
                nonlocal closure_directory_swapped
                if (not closure_directory_swapped and not final and
                        relative == 'src/aot/nested/closure-owner.h' and
                        component == 'nested'):
                    nested.rename(closure_nested_saved)
                    closure_external.mkdir()
                    (closure_external / 'closure-owner.h').write_text(
                        'static void alternate_closure_owner(void) {}\n',
                        encoding='utf-8')
                    nested.symlink_to(closure_external, target_is_directory=True)
                    closure_directory_swapped = True

            closure_directory_swap_rejected = False
            try:
                capture_xi_lowering_validation_snapshot(
                    root, before_open_hook=swap_closure_directory_before_open)
                assert False, "a closure directory symlink swap must fail closed"
            except SystemExit:
                closure_directory_swap_rejected = True
            finally:
                if nested.is_symlink():
                    nested.unlink()
                if closure_nested_saved.exists():
                    closure_nested_saved.rename(nested)
                try:
                    (closure_external / 'closure-owner.h').unlink()
                    closure_external.rmdir()
                except FileNotFoundError:
                    pass
                owner.write_text('#include "router.c"\n', encoding='utf-8')
                try:
                    closure_header.unlink()
                except FileNotFoundError:
                    pass
            assert closure_directory_swap_rejected and closure_directory_swapped

            nested_saved = aot / 'nested.saved'
            discovery_external = root / 'discovery-external'
            directory_swapped = False

            def swap_directory_before_open(
                    relative, _index, _component, final) -> None:
                nonlocal directory_swapped
                if (not directory_swapped and final and
                        relative == 'src/aot/nested'):
                    nested.rename(nested_saved)
                    discovery_external.mkdir()
                    (discovery_external / 'unrelated.c').write_text(
                        'static void alternate(void) {}\n', encoding='utf-8')
                    nested.symlink_to(discovery_external, target_is_directory=True)
                    directory_swapped = True

            directory_swap_rejected = False
            try:
                _xi_capture_aot_discovery_census(
                    root, before_open_hook=swap_directory_before_open)
                assert False, "a check-to-open directory symlink swap must fail closed"
            except SystemExit:
                directory_swap_rejected = True
            finally:
                if nested.is_symlink():
                    nested.unlink()
                if nested_saved.exists():
                    nested_saved.rename(nested)
                try:
                    (discovery_external / 'unrelated.c').unlink()
                    discovery_external.rmdir()
                except FileNotFoundError:
                    pass
            assert directory_swap_rejected and directory_swapped

        fixture_snapshot = capture_xi_lowering_validation_snapshot(root)
        fixture_discovery = _xi_capture_aot_discovery_census(root)
        fixture_dependencies = xi_lowering_validation_dependency_paths(
            [], root, fixture_snapshot, fixture_discovery)
        fixture_relatives = {
            path.relative_to(root).as_posix() for path in fixture_dependencies}
        assert fixture_relatives == {
            'CMakeLists.txt', 'include', 'src', 'src/base', 'src/aot',
            'src/aot/nested',
            'src/aot/nested/unrelated.c', 'src/aot/owner.c',
            'src/aot/router.c', 'src/aot/xi_cgen.c'}
        assert 'src/aot/nested/unrelated.c' not in fixture_snapshot.source_bytes()
        (nested / 'unrelated.c').write_text(
            'static void unrelated(void) { /* discovery content drift */ }\n',
            encoding='utf-8')
        assert capture_xi_lowering_validation_snapshot(root) == fixture_snapshot
        assert _xi_capture_aot_discovery_census(root) != fixture_discovery
        (nested / 'unrelated.c').write_text(
            'static void unrelated(void) {}\n', encoding='utf-8')
        restored_discovery = _xi_capture_aot_discovery_census(root)
        assert restored_discovery.fingerprint() == fixture_discovery.fingerprint()
        assert restored_discovery != fixture_discovery
        added_unrelated = nested / 'added_unrelated.c'
        added_unrelated.write_text(
            'static void added_unrelated(void) {}\n', encoding='utf-8')
        assert _xi_capture_aot_discovery_census(root) != restored_discovery
        added_unrelated.unlink()
        removed_discovery = _xi_capture_aot_discovery_census(root)
        assert removed_discovery.fingerprint() == restored_discovery.fingerprint()
        assert removed_discovery != restored_discovery

        ops_path = root / 'xisa/xi/ops.def'
        lowering_path = root / 'xisa/xi/lowering.def'
        ops_path.parent.mkdir(parents=True)
        ops_path.write_text('ops\n', encoding='utf-8')
        lowering_path.write_text('lowering\n', encoding='utf-8')
        sealed_snapshot = capture_xi_lowering_validation_snapshot(root)
        sealed_discovery = _xi_capture_aot_discovery_census(root)
        stamp = root / 'build/sealed.stamp'
        depfile = root / 'build/sealed.d'

        preserved_owner_time = (aot / 'owner.c').stat().st_mtime_ns

        def mutate_before_stamp() -> None:
            (aot / 'owner.c').write_text(
                '#include "router.c"\n/* pre-publication drift */\n',
                encoding='utf-8')
            os.utime(aot / 'owner.c',
                     ns=(preserved_owner_time, preserved_owner_time))

        try:
            _xi_publish_lowering_validation_artifacts(
                ops_path=ops_path, ops_source=ops_path.read_bytes(),
                lowering_path=lowering_path,
                lowering_source=lowering_path.read_bytes(), source_root=root,
                snapshot=sealed_snapshot, discovery_snapshot=sealed_discovery,
                depfile=depfile, depfile_content='sealed: inputs\n',
                stamp=stamp, stamp_content='sealed\n',
                before_stamp_hook=mutate_before_stamp)
            assert False, "pre-publication fingerprint drift must fail closed"
        except SystemExit:
            pass
        assert not stamp.exists()
        assert not depfile.exists()
        (aot / 'owner.c').write_text(
            '#include "router.c"\n', encoding='utf-8')
        sealed_snapshot = capture_xi_lowering_validation_snapshot(root)
        sealed_discovery = _xi_capture_aot_discovery_census(root)

        def replace_stamp_identity() -> None:
            _xi_atomic_write(stamp, 'sealed\n')

        try:
            _xi_publish_lowering_validation_artifacts(
                ops_path=ops_path, ops_source=ops_path.read_bytes(),
                lowering_path=lowering_path,
                lowering_source=lowering_path.read_bytes(), source_root=root,
                snapshot=sealed_snapshot, discovery_snapshot=sealed_discovery,
                depfile=depfile, depfile_content='sealed: inputs\n',
                stamp=stamp, stamp_content='sealed\n',
                after_stamp_hook=replace_stamp_identity)
            assert False, "validation stamp identity replacement must fail closed"
        except SystemExit:
            pass
        assert not stamp.exists()
        assert not depfile.exists()

        def mutate_after_stamp() -> None:
            (aot / 'owner.c').write_text(
                '#include "router.c"\n/* post-publication mutation */\n',
                encoding='utf-8')

        try:
            _xi_publish_lowering_validation_artifacts(
                ops_path=ops_path, ops_source=ops_path.read_bytes(),
                lowering_path=lowering_path,
                lowering_source=lowering_path.read_bytes(), source_root=root,
                snapshot=sealed_snapshot, discovery_snapshot=sealed_discovery,
                depfile=depfile, depfile_content='sealed: inputs\n',
                stamp=stamp, stamp_content='sealed\n',
                after_stamp_hook=mutate_after_stamp)
            assert False, "post-publication source mutation must fail closed"
        except SystemExit:
            pass
        assert not stamp.exists()
        assert not depfile.exists()

        (aot / 'owner.c').write_text(
            '#include "router.c"\n', encoding='utf-8')
        destructive_snapshot = capture_xi_lowering_validation_snapshot(root)
        destructive_discovery = _xi_capture_aot_discovery_census(root)

        def delete_after_stamp() -> None:
            (aot / 'owner.c').unlink()

        try:
            _xi_publish_lowering_validation_artifacts(
                ops_path=ops_path, ops_source=ops_path.read_bytes(),
                lowering_path=lowering_path,
                lowering_source=lowering_path.read_bytes(), source_root=root,
                snapshot=destructive_snapshot,
                discovery_snapshot=destructive_discovery,
                depfile=depfile, depfile_content='sealed: inputs\n',
                stamp=stamp, stamp_content='sealed\n',
                after_stamp_hook=delete_after_stamp)
            assert False, "destructive post-publication mutation must fail closed"
        except SystemExit:
            pass
        assert not stamp.exists()
        assert not depfile.exists()

        (aot / 'owner.c').write_text(
            '#include "router.c"\n', encoding='utf-8')
        interrupted_snapshot = capture_xi_lowering_validation_snapshot(root)
        interrupted_discovery = _xi_capture_aot_discovery_census(root)

        def interrupt_after_stamp() -> None:
            raise KeyboardInterrupt("simulated interruption after stamp publication")

        try:
            _xi_publish_lowering_validation_artifacts(
                ops_path=ops_path, ops_source=ops_path.read_bytes(),
                lowering_path=lowering_path,
                lowering_source=lowering_path.read_bytes(), source_root=root,
                snapshot=interrupted_snapshot,
                discovery_snapshot=interrupted_discovery,
                depfile=depfile, depfile_content='sealed: inputs\n',
                stamp=stamp, stamp_content='sealed\n',
                after_stamp_hook=interrupt_after_stamp)
            assert False, "interruption after stamp publication must fail closed"
        except KeyboardInterrupt:
            pass
        assert not stamp.exists()
        assert not depfile.exists()
        assert not list(stamp.parent.glob(stamp.name + '.*'))

        orphaned_proof = _xi_lowering_validation_stamp_content(
            ops_path.read_bytes(), lowering_path.read_bytes(),
            interrupted_snapshot, interrupted_discovery)
        _xi_atomic_write(stamp, orphaned_proof)
        (aot / 'owner.c').write_text(
            '#include "router.c"\n/* changed after orphaned proof */\n',
            encoding='utf-8')
        current_snapshot = capture_xi_lowering_validation_snapshot(root)
        current_discovery = _xi_capture_aot_discovery_census(root)
        current_proof = _xi_lowering_validation_stamp_content(
            ops_path.read_bytes(), lowering_path.read_bytes(),
            current_snapshot, current_discovery)
        try:
            _xi_require_lowering_validation_stamp(stamp, current_proof)
            assert False, "generation must reject an orphaned stale proof stamp"
        except SystemExit:
            pass
        stamp.unlink()
        (aot / 'owner.c').write_text(
            '#include "router.c"\n', encoding='utf-8')

        kill_stamp = root / 'build/killed.stamp'
        kill_depfile = root / 'build/killed.d'
        kill_marker = root / 'build/killed.after-publish'
        child_source = r'''
import importlib.util
import pathlib
import sys
import time

module_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
marker = pathlib.Path(sys.argv[3])
stamp = pathlib.Path(sys.argv[4])
depfile = pathlib.Path(sys.argv[5])
spec = importlib.util.spec_from_file_location('xisagen_kill_child', module_path)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
ops_path = root / 'xisa/xi/ops.def'
lowering_path = root / 'xisa/xi/lowering.def'
ops_source = ops_path.read_bytes()
lowering_source = lowering_path.read_bytes()
snapshot = module.capture_xi_lowering_validation_snapshot(root)
discovery = module._xi_capture_aot_discovery_census(root)
proof = module._xi_lowering_validation_stamp_content(
    ops_source, lowering_source, snapshot, discovery)
def after_publish():
    marker.write_text('published\n', encoding='utf-8')
    while True:
        time.sleep(60)
module._xi_publish_lowering_validation_artifacts(
    ops_path=ops_path, ops_source=ops_source,
    lowering_path=lowering_path, lowering_source=lowering_source,
    source_root=root, snapshot=snapshot, discovery_snapshot=discovery,
    depfile=depfile, depfile_content='killed: inputs\n',
    stamp=stamp, stamp_content=proof, after_stamp_hook=after_publish)
'''
        killed = subprocess.Popen(
            [sys.executable, '-B', '-c', child_source,
             str(Path(__file__).resolve()), str(root), str(kill_marker),
             str(kill_stamp), str(kill_depfile)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            deadline = time.monotonic() + 10
            while (not kill_marker.exists() and killed.poll() is None and
                   time.monotonic() < deadline):
                time.sleep(0.01)
            assert kill_marker.exists(), \
                "kill-window child must publish the proof before termination"
            killed.kill()
            killed.communicate(timeout=10)
        finally:
            if killed.poll() is None:
                killed.kill()
                killed.communicate(timeout=10)
        assert killed.returncode != 0
        assert kill_stamp.exists() and kill_depfile.exists()
        killed_unchanged_snapshot = capture_xi_lowering_validation_snapshot(root)
        killed_unchanged_discovery = _xi_capture_aot_discovery_census(root)
        killed_unchanged_proof = _xi_lowering_validation_stamp_content(
            ops_path.read_bytes(), lowering_path.read_bytes(),
            killed_unchanged_snapshot, killed_unchanged_discovery)
        _xi_require_lowering_validation_stamp(
            kill_stamp, killed_unchanged_proof)
        (aot / 'owner.c').write_text(
            '#include "router.c"\n/* drift after killed publisher */\n',
            encoding='utf-8')
        killed_snapshot = capture_xi_lowering_validation_snapshot(root)
        killed_discovery = _xi_capture_aot_discovery_census(root)
        killed_current_proof = _xi_lowering_validation_stamp_content(
            ops_path.read_bytes(), lowering_path.read_bytes(),
            killed_snapshot, killed_discovery)
        try:
            _xi_require_lowering_validation_stamp(
                kill_stamp, killed_current_proof)
            assert False, "generation must reject a killed publisher's stale proof"
        except SystemExit:
            pass
        _xi_publish_lowering_validation_artifacts(
            ops_path=ops_path, ops_source=ops_path.read_bytes(),
            lowering_path=lowering_path,
            lowering_source=lowering_path.read_bytes(), source_root=root,
            snapshot=killed_snapshot, discovery_snapshot=killed_discovery,
            depfile=kill_depfile, depfile_content='killed: inputs\n',
            stamp=kill_stamp, stamp_content=killed_current_proof)
        _xi_require_lowering_validation_stamp(
            kill_stamp, killed_current_proof)
        (aot / 'owner.c').write_text(
            '#include "router.c"\n', encoding='utf-8')

        artifact = root / 'build output' / 'stamp file.txt'
        _xi_atomic_write(artifact, 'first\n')
        _xi_atomic_write(artifact, 'replacement\n')
        assert artifact.read_text(encoding='utf-8') == 'replacement\n'
        assert list(artifact.parent.iterdir()) == [artifact]
    print(" PASS", file=sys.stderr)


def _test_xi_lowering_ninja_failed_edge() -> None:
    print("  test_xi_lowering_ninja_failed_edge...", end='', file=sys.stderr)
    ninja = shutil.which('ninja')
    if ninja is None:
        die("xisagen self-test requires Ninja; install ninja and retry")

    def ninja_command(arguments: list[object]) -> str:
        values = [os.fspath(value) for value in arguments]
        if os.name == 'nt':
            command = subprocess.list2cmdline(values)
        else:
            command = shlex.join(values)
        return command.replace('$', '$$')

    validator_source = r'''
import importlib.util
import os
import pathlib
import signal
import sys

module_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
mode_path = pathlib.Path(sys.argv[3])
stamp = root / 'build/proof.stamp'
depfile = root / 'build/proof.d'
events = root / 'build/events.log'
spec = importlib.util.spec_from_file_location('xisagen_ninja_validator', module_path)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)

def record(event):
    with events.open('a', encoding='utf-8') as stream:
        stream.write(event + '\n')
        stream.flush()
        os.fsync(stream.fileno())

ops_path = root / 'xisa/xi/ops.def'
lowering_path = root / 'xisa/xi/lowering.def'
ops_source = ops_path.read_bytes()
lowering_source = lowering_path.read_bytes()
snapshot = module.capture_xi_lowering_validation_snapshot(root)
discovery = module._xi_capture_aot_discovery_census(root)
dependencies = module.xi_lowering_validation_dependency_paths(
    [], root, snapshot, discovery)
dependencies.extend((ops_path, lowering_path))
dependency_text = ' '.join(
    module._xi_depfile_escape(path.as_posix())
    for path in sorted(set(dependencies)))
depfile_content = 'build/proof.stamp: ' + dependency_text + '\n'
proof = module._xi_lowering_validation_stamp_content(
    ops_source, lowering_source, snapshot, discovery)

def after_publish():
    record('validation-published-' + mode_path.read_text(encoding='utf-8').strip())
    if mode_path.read_text(encoding='utf-8').strip() == 'kill':
        if os.name == 'nt':
            os._exit(91)
        os.kill(os.getpid(), signal.SIGKILL)

module._xi_publish_lowering_validation_artifacts(
    ops_path=ops_path, ops_source=ops_source,
    lowering_path=lowering_path, lowering_source=lowering_source,
    source_root=root, snapshot=snapshot, discovery_snapshot=discovery,
    depfile=depfile, depfile_content=depfile_content,
    stamp=stamp, stamp_content=proof, after_stamp_hook=after_publish)
record('validation-success')
'''
    checker_source = r'''
import os
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
projection = root / 'tracked_projection.txt'
events = root / 'build/events.log'
if projection.read_bytes() != b'canonical\n':
    print('checked-in projection mismatch', file=sys.stderr)
    sys.exit(2)
with events.open('a', encoding='utf-8') as stream:
    stream.write('verification-success\n')
    stream.flush()
    os.fsync(stream.fileno())
'''

    with tempfile.TemporaryDirectory(
            prefix='xisagen-ninja-failed-edge-') as directory:
        root = Path(directory)
        aot = root / 'src/aot'
        schema = root / 'xisa/xi'
        build = root / 'build'
        aot.mkdir(parents=True)
        schema.mkdir(parents=True)
        build.mkdir()
        (root / 'CMakeLists.txt').write_text(
            'set(XRAY_COMMON_INCLUDES\n'
            '    ${CMAKE_CURRENT_SOURCE_DIR}/include\n'
            '    ${CMAKE_CURRENT_SOURCE_DIR}/src\n'
            '    ${CMAKE_CURRENT_SOURCE_DIR}/src/aot\n'
            ')\n', encoding='utf-8')
        (root / 'include').mkdir()
        (aot / 'xi_cgen.c').write_text(
            '#include "owner.c"\n', encoding='utf-8')
        owner = aot / 'owner.c'
        owner.write_text('static void owner(void) {}\n', encoding='utf-8')
        (schema / 'ops.def').write_text('ops\n', encoding='utf-8')
        (schema / 'lowering.def').write_text('lowering\n', encoding='utf-8')
        mode = root / 'mode.txt'
        mode.write_text('pass\n', encoding='utf-8')
        projection = root / 'tracked_projection.txt'
        projection.write_text('canonical\n', encoding='utf-8')
        validator = root / 'validator.py'
        checker = root / 'checker.py'
        validator.write_text(validator_source, encoding='utf-8')
        checker.write_text(checker_source, encoding='utf-8')
        module_path = Path(__file__).resolve()
        build_file = root / 'build.ninja'
        build_file.write_text(
            'ninja_required_version = 1.10\n'
            'rule validate\n'
            f'  command = {ninja_command([sys.executable, "-B", validator, module_path, root, mode])}\n'
            '  depfile = build/proof.d\n'
            '  deps = gcc\n'
            'rule verify\n'
            f'  command = {ninja_command([sys.executable, "-B", checker, root])}\n'
            'build build/proof.stamp: validate xisa/xi/ops.def xisa/xi/lowering.def\n'
            'build force: phony\n'
            'build build/verify: verify build/proof.stamp tracked_projection.txt | force\n'
            'default build/verify\n',
            encoding='utf-8')

        def run_ninja() -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                [ninja, '-f', build_file.name, '-v'], cwd=root,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=30, check=False)

        initial = run_ninja()
        assert initial.returncode == 0, initial.stdout
        first_proof = (build / 'proof.stamp').read_text(encoding='utf-8')
        initial_events = (build / 'events.log').read_text(
            encoding='utf-8').splitlines()
        assert initial_events.count('validation-success') == 1
        assert initial_events.count('verification-success') == 1
        initial_identity = _xi_stable_file_identity(projection.stat())

        unchanged = run_ninja()
        assert unchanged.returncode == 0, unchanged.stdout
        unchanged_events = (build / 'events.log').read_text(
            encoding='utf-8').splitlines()
        assert unchanged_events.count('validation-success') == 1
        assert unchanged_events.count('verification-success') == 2
        assert _xi_stable_file_identity(projection.stat()) == initial_identity

        added = aot / 'added.c'
        added.write_text(
            'static void added(void) {}\n', encoding='utf-8')
        added_result = run_ninja()
        assert added_result.returncode == 0, added_result.stdout
        added_events = (build / 'events.log').read_text(
            encoding='utf-8').splitlines()
        assert added_events.count('validation-success') == 2
        assert added_events.count('verification-success') == 3
        added.unlink()
        deleted_result = run_ninja()
        assert deleted_result.returncode == 0, deleted_result.stdout
        deleted_events = (build / 'events.log').read_text(
            encoding='utf-8').splitlines()
        assert deleted_events.count('validation-success') == 3
        assert deleted_events.count('verification-success') == 4

        projection.write_text('corrupt\n', encoding='utf-8')
        corrupt = run_ninja()
        assert corrupt.returncode != 0, corrupt.stdout
        assert 'checked-in projection mismatch' in corrupt.stdout
        corrupt_events = (build / 'events.log').read_text(
            encoding='utf-8').splitlines()
        assert corrupt_events.count('validation-success') == 3
        assert corrupt_events.count('verification-success') == 4
        projection.write_text('canonical\n', encoding='utf-8')
        repaired = run_ninja()
        assert repaired.returncode == 0, repaired.stdout
        repaired_events = (build / 'events.log').read_text(
            encoding='utf-8').splitlines()
        assert repaired_events.count('validation-success') == 3
        assert repaired_events.count('verification-success') == 5

        owner.write_text(
            'static void owner(void) { /* changed input */ }\n',
            encoding='utf-8')
        mode.write_text('kill\n', encoding='utf-8')
        killed = run_ninja()
        assert killed.returncode != 0, killed.stdout
        killed_events = (build / 'events.log').read_text(
            encoding='utf-8').splitlines()
        assert 'validation-published-kill' in killed_events
        assert killed_events.count('validation-success') == 3
        assert killed_events.count('verification-success') == 5

        failed_retry = run_ninja()
        assert failed_retry.returncode != 0, failed_retry.stdout
        retry_events = (build / 'events.log').read_text(
            encoding='utf-8').splitlines()
        assert retry_events.count('validation-published-kill') == 2
        assert retry_events.count('validation-success') == 3
        assert retry_events.count('verification-success') == 5

        mode.write_text('pass\n', encoding='utf-8')
        recovered = run_ninja()
        assert recovered.returncode == 0, recovered.stdout
        recovered_events = (build / 'events.log').read_text(
            encoding='utf-8').splitlines()
        assert recovered_events.count('validation-success') == 4
        assert recovered_events.count('verification-success') == 6
        assert (build / 'proof.stamp').read_text(encoding='utf-8') != first_proof

        cleaned = subprocess.run(
            [ninja, '-f', build_file.name, '-t', 'clean'], cwd=root,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=30, check=False)
        assert cleaned.returncode == 0, cleaned.stdout
        assert projection.read_text(encoding='utf-8') == 'canonical\n'
        assert not (build / 'proof.stamp').exists()
        rebuilt = run_ninja()
        assert rebuilt.returncode == 0, rebuilt.stdout
        rebuilt_events = (build / 'events.log').read_text(
            encoding='utf-8').splitlines()
        assert rebuilt_events.count('validation-success') == 5
        assert rebuilt_events.count('verification-success') == 7
    print(" PASS", file=sys.stderr)


def _test_xi_lowering_actual_ninja_edge() -> None:
    print("  test_xi_lowering_actual_ninja_edge...", end='', file=sys.stderr)
    ninja = shutil.which('ninja')
    cmake = shutil.which('cmake')
    if ninja is None:
        die("xisagen self-test requires Ninja; install ninja and retry")
    if cmake is None:
        die("xisagen self-test requires CMake; install cmake and retry")
    repository_root = Path(__file__).resolve().parents[2]
    ops_source = (repository_root / 'xisa/xi/ops.def').read_bytes()
    lowering_source = (repository_root / 'xisa/xi/lowering.def').read_bytes()
    ops = parse_xi_ops_def(ops_source.decode('utf-8'), 'actual-edge ops')
    entries = parse_xi_lowering_def(
        lowering_source.decode('utf-8'), ops, 'actual-edge lowering')
    closure = capture_xi_lowering_validation_snapshot(repository_root)
    discovery = _xi_capture_aot_discovery_census(repository_root)
    projections = _xi_lowering_output_contents(entries, ops)
    validator_source = r'''
import importlib.util
import os
import pathlib
import signal
import sys

module_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
mode_path = pathlib.Path(sys.argv[3])
stamp = pathlib.Path(sys.argv[4])
depfile = pathlib.Path(sys.argv[5])
events = root / 'build/events.log'
spec = importlib.util.spec_from_file_location('xisagen_actual_edge', module_path)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
mode = mode_path.read_text(encoding='utf-8').strip()
original_publish = module._xi_publish_lowering_validation_artifacts
if mode == 'kill':
    def killed_publish(**kwargs):
        def kill_after_publication():
            if os.name == 'nt':
                os._exit(91)
            os.kill(os.getpid(), signal.SIGKILL)
        kwargs['after_stamp_hook'] = kill_after_publication
        return original_publish(**kwargs)
    module._xi_publish_lowering_validation_artifacts = killed_publish
module.cmd_xi_lowering_validate([
    str(root / 'xisa/xi/ops.def'), str(root / 'xisa/xi/lowering.def'),
    str(stamp), str(depfile)])
if mode == 'post-validation-mutate':
    owner = root / 'src/aot/xi_cgen_coro.inc.c'
    preserved = owner.stat().st_mtime_ns
    owner.write_bytes(owner.read_bytes() + b'/* post-validation drift */\n')
    os.utime(owner, ns=(preserved, preserved))
with events.open('a', encoding='utf-8') as stream:
    stream.write('validation-success-' + mode + '\n')
    stream.flush()
    os.fsync(stream.fileno())
'''
    checker_source = r'''
import importlib.util
import os
import pathlib
import sys

module_path = pathlib.Path(sys.argv[1])
root = pathlib.Path(sys.argv[2])
stamp = pathlib.Path(sys.argv[3])
events = root / 'build/events.log'
spec = importlib.util.spec_from_file_location('xisagen_actual_checker', module_path)
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
module.cmd_xi_lowering_check([
    str(root / 'xisa/xi/ops.def'), str(root / 'xisa/xi/lowering.def'),
    str(root), str(stamp)])
with events.open('a', encoding='utf-8') as stream:
    stream.write('verification-success\n')
    stream.flush()
    os.fsync(stream.fileno())
'''
    with tempfile.TemporaryDirectory(
            prefix='xisagen-actual-ninja-edge-') as directory:
        root = Path(directory)
        for relative in closure.include_directories:
            (root / relative).mkdir(parents=True, exist_ok=True)
        shutil.copytree(repository_root / 'src', root / 'src', dirs_exist_ok=True)
        shutil.copytree(repository_root / 'include', root / 'include',
                        dirs_exist_ok=True)
        copied = dict(discovery.sources)
        copied.update(closure.sources)
        copied['xisa/xi/ops.def'] = ops_source
        copied['xisa/xi/lowering.def'] = lowering_source
        for relative, content in projections:
            copied[relative] = content.encode('utf-8')
        for relative, data in copied.items():
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        build = root / 'build'
        build.mkdir()
        mode = root / 'mode.txt'
        mode.write_text('pass\n', encoding='utf-8')
        validator = root / 'validator.py'
        checker = root / 'checker.py'
        validator.write_text(validator_source, encoding='utf-8')
        checker.write_text(checker_source, encoding='utf-8')
        module_path = Path(__file__).resolve()
        include_lines = ''.join(
            f'    ${{CMAKE_CURRENT_SOURCE_DIR}}/{relative}\n'
            for relative in closure.include_directories)
        projection_lines = ''.join(
            f'    ${{CMAKE_CURRENT_SOURCE_DIR}}/{relative}\n'
            for relative, _ in projections)
        cmake_lists = (
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(xisagen_actual_edge C)\n'
            'set(CMAKE_C_STANDARD 11)\n'
            'set(XRAY_COMMON_INCLUDES\n' + include_lines + ')\n'
            f'set(XISAGEN "{module_path.as_posix()}")\n'
            f'set(VALIDATOR "{validator.as_posix()}")\n'
            f'set(CHECKER "{checker.as_posix()}")\n'
            f'set(MODE "{mode.as_posix()}")\n'
            'set(STAMP ${CMAKE_CURRENT_BINARY_DIR}/xi.validated)\n'
            'set(DEPFILE ${CMAKE_CURRENT_BINARY_DIR}/xi.d)\n'
            'set(PROJECTIONS\n' + projection_lines + ')\n'
            'add_custom_command(\n'
            '  OUTPUT ${STAMP}\n'
            f'  COMMAND "{sys.executable}" -B ${{VALIDATOR}} ${{XISAGEN}} '
            '${CMAKE_CURRENT_SOURCE_DIR} ${MODE} ${STAMP} ${DEPFILE}\n'
            '  BYPRODUCTS ${DEPFILE}\n'
            '  DEPENDS ${XISAGEN} ${VALIDATOR} ${MODE} '
            '${CMAKE_CURRENT_SOURCE_DIR}/xisa/xi/ops.def '
            '${CMAKE_CURRENT_SOURCE_DIR}/xisa/xi/lowering.def\n'
            '  DEPFILE ${DEPFILE}\n'
            '  VERBATIM)\n'
            'add_custom_target(gen-xi-lowering\n'
            f'  COMMAND "{sys.executable}" -B ${{CHECKER}} ${{XISAGEN}} '
            '${CMAKE_CURRENT_SOURCE_DIR} ${STAMP}\n'
            '  DEPENDS ${STAMP} ${PROJECTIONS}\n'
            '  VERBATIM)\n'
            'add_executable(test_xi_lowering_gen '
            'tests/unit/ir/test_xi_lowering_gen.c)\n'
            'target_include_directories(test_xi_lowering_gen PRIVATE '
            '${XRAY_COMMON_INCLUDES})\n'
            'add_dependencies(test_xi_lowering_gen gen-xi-lowering)\n')
        (root / 'CMakeLists.txt').write_text(cmake_lists, encoding='utf-8')
        configured = subprocess.run(
            [cmake, '-S', root, '-B', build, '-G', 'Ninja',
             '-DCMAKE_BUILD_TYPE=Release'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=60, check=False)
        assert configured.returncode == 0, configured.stdout

        def run_target() -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                [cmake, '--build', build, '--parallel', '16',
                 '--target', 'test_xi_lowering_gen'],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=120, check=False)

        def events() -> list[str]:
            path = build / 'events.log'
            return path.read_text(encoding='utf-8').splitlines() \
                if path.exists() else []

        initial = run_target()
        assert initial.returncode == 0, initial.stdout
        assert events().count('validation-success-pass') == 1
        assert events().count('verification-success') == 1

        hostile = root / 'src/aot/hostile_selector.c'
        hostile.write_text(
            'static void hostile_selector(const XiValue *v, XiCgenCtx *ctx, '
            'FILE *out, const XiFunc *f, const char *prefix) { '
            'if (v->op == XI_GO) { xicgen_go(ctx, out, f, v, prefix); return; } }\n',
            encoding='utf-8')
        added = run_target()
        assert added.returncode != 0, added.stdout
        assert 'direct-consumer router census mismatch' in added.stdout
        assert events().count('verification-success') == 1
        hostile.unlink()

        deleted_path = root / 'include/xray_value_abi.h'
        deleted_bytes = deleted_path.read_bytes()
        deleted_path.unlink()
        deleted = run_target()
        assert deleted.returncode != 0, deleted.stdout
        assert ('cannot open' in deleted.stdout or
                'cannot be resolved in-repository' in deleted.stdout), deleted.stdout
        assert events().count('verification-success') == 1
        deleted_path.write_bytes(deleted_bytes)

        projection = root / projections[0][0]
        projection_bytes = projection.read_bytes()
        projection.write_bytes(projection_bytes + b'/* corrupt projection */\n')
        corrupt = run_target()
        assert corrupt.returncode != 0, corrupt.stdout
        assert 'checked-in projection differs from canonical content' in corrupt.stdout
        assert events().count('verification-success') == 1
        projection.write_bytes(projection_bytes)

        owner = root / 'src/aot/xi_cgen_coro.inc.c'
        owner_bytes = owner.read_bytes()
        mode.write_text('kill\n', encoding='utf-8')
        owner.write_bytes(owner_bytes + b'/* trigger killed validation */\n')
        killed = run_target()
        assert killed.returncode != 0, killed.stdout
        assert events().count('verification-success') == 1
        killed_retry = run_target()
        assert killed_retry.returncode != 0, killed_retry.stdout
        assert events().count('verification-success') == 1
        owner.write_bytes(owner_bytes)
        mode.write_text('post-validation-mutate\n', encoding='utf-8')
        between_edges = run_target()
        assert between_edges.returncode != 0, between_edges.stdout
        assert ('validation stamp does not match' in between_edges.stdout or
                'changed before final success' in between_edges.stdout)
        assert events().count('verification-success') == 1
        owner.write_bytes(owner_bytes)
        mode.write_text('pass\n', encoding='utf-8')

        cleaned = subprocess.run(
            [cmake, '--build', build, '--target', 'clean'],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=60, check=False)
        assert cleaned.returncode == 0, cleaned.stdout
        test_projection = root / 'tests/unit/ir/test_xi_lowering_gen.c'
        test_projection_bytes = test_projection.read_bytes()
        test_projection.write_bytes(
            test_projection_bytes + b'\n#error XI_LOWERING_COMPILE_RACE\n')
        ordering = run_target()
        assert ordering.returncode != 0, ordering.stdout
        assert 'checked-in projection differs from canonical content' in ordering.stdout
        assert 'XI_LOWERING_COMPILE_RACE' not in ordering.stdout
        test_projection.write_bytes(test_projection_bytes)
        ordering_recovered = run_target()
        assert ordering_recovered.returncode == 0, ordering_recovered.stdout
        assert events().count('verification-success') == 2
    print(" PASS", file=sys.stderr)


def cmd_xi_lowering_validate(args: list[str]):
    if len(args) != 4:
        die("usage: xisagen.py xi-lowering-validate "
            "<ops.def> <lowering.def> <stamp> <depfile>")
    ops_path = Path(os.path.abspath(args[0]))
    lowering_path = Path(os.path.abspath(args[1]))
    stamp = Path(args[2]).resolve()
    depfile = Path(args[3]).resolve()
    source_root = ops_path.parents[2]
    ops_relative = _xi_repository_relative_name(
        source_root, ops_path, 'xi-lowering: ops schema')
    lowering_relative = _xi_repository_relative_name(
        source_root, lowering_path, 'xi-lowering: lowering schema')
    ops_source = _xi_secure_repository_file_bytes(
        source_root, ops_relative, 'xi-lowering: ops schema')
    lowering_source = _xi_secure_repository_file_bytes(
        source_root, lowering_relative, 'xi-lowering: lowering schema')
    try:
        ops_text = ops_source.decode('utf-8', errors='strict')
        lowering_text = lowering_source.decode('utf-8', errors='strict')
    except UnicodeDecodeError as error:
        die(f"xi-lowering: schema input is not UTF-8: {error}")
    ops = parse_xi_ops_def(ops_text, str(ops_path))
    entries = parse_xi_lowering_def(
        lowering_text, ops, str(lowering_path))
    if not entries:
        die(f"no Xi lowering entries parsed from {lowering_path}")
    snapshot = capture_xi_lowering_validation_snapshot(source_root)
    discovery_snapshot = _xi_capture_aot_discovery_census(source_root)
    validate_xi_lowering_consumer_sources(
        entries, source_root, str(lowering_path), snapshot, discovery_snapshot)
    if (_xi_secure_repository_file_bytes(
            source_root, ops_relative,
            'xi-lowering: ops schema') != ops_source or
            _xi_secure_repository_file_bytes(
                source_root, lowering_relative,
                'xi-lowering: lowering schema') != lowering_source or
            capture_xi_lowering_validation_snapshot(source_root) != snapshot or
            _xi_capture_aot_discovery_census(source_root) != discovery_snapshot):
        die("xi-lowering: validation inputs changed while the snapshot was being verified")
    dependencies = xi_lowering_validation_dependency_paths(
        entries, source_root, snapshot, discovery_snapshot)
    dependencies.extend({ops_path.parent, lowering_path.parent})
    dependencies = sorted(set(dependencies))
    dependency_text = ' '.join(
        _xi_depfile_escape(path.as_posix()) for path in dependencies)
    depfile_content = f"{_xi_depfile_escape(stamp.as_posix())}: {dependency_text}\n"
    stamp_content = _xi_lowering_validation_stamp_content(
        ops_source, lowering_source, snapshot, discovery_snapshot)
    _xi_publish_lowering_validation_artifacts(
        ops_path=ops_path, ops_source=ops_source,
        lowering_path=lowering_path, lowering_source=lowering_source,
        source_root=source_root, snapshot=snapshot,
        discovery_snapshot=discovery_snapshot, depfile=depfile,
        depfile_content=depfile_content, stamp=stamp,
        stamp_content=stamp_content)
    print(f"xisagen: validated {len(entries)} Xi lowering entries; "
          f"tracked {len(snapshot.sources)} AOT source inputs and "
          f"{len(snapshot.directories)} include directories and "
          f"{len(discovery_snapshot.directories)} discovery directories",
          file=sys.stderr)


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


def cmd_aot_c_emission_rules(args: list[str]):
    import c_emission_rules

    if len(args) != 2:
        die("usage: xisagen.py aot-c-emission-rules <rules.def> <output-root>")
    try:
        outputs = c_emission_rules.generate(read_file(args[0]), args[0])
    except c_emission_rules.RuleError as error:
        die(str(error))
    for relative, content in outputs.items():
        output = os.path.join(args[1], *relative.split('/'))
        write_file(output, content)
        print(f"xisagen: generated {output}", file=sys.stderr)


def cmd_target_vm_ops(args: list[str]):
    if len(args) != 2:
        die("usage: xisagen.py target-vm-ops <vm_ops.def> <output-root>")
    entries = parse_target_instruction_def(read_file(args[0]), args[0])
    if not entries:
        die(f"no target instructions parsed from {args[0]}")
    outputs = write_target_vm_outputs(args[1], entries)
    print(f"xisagen: parsed {len(entries)} target instructions from {args[0]}",
          file=sys.stderr)
    for path in outputs:
        print(f"xisagen: generated {path}", file=sys.stderr)


def cmd_test(args: list[str]):
    import c_emission_rules

    """Run self-tests."""
    print("xisagen self-test:", file=sys.stderr)
    _test_sexpr_parser()
    _test_xi_ops_parser()
    _test_xi_semantic_ops_parser()
    _test_xi_lowering_parser()
    _test_xi_lowering_build_artifacts()
    _test_xi_lowering_actual_ninja_edge()
    _test_xi_verifier_parser()
    _test_aot_rep_parser()
    _test_aot_abi_parser()
    _test_aot_layout_parser()
    c_emission_rules.self_test()
    _test_target_instruction_parser()
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
      :own-use borrow
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
      :own-use borrow
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
      :own-use consume
      :effects (side-effect memory-read memory-write)
      :tbaa-group top
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
      :own-use consume
      :effects (side-effect memory-write allocates)
      :tbaa-group fresh
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.atomic.load
      :class memory-read
      :arity variadic
      :own-use borrow
      :effects (memory-read)
      :tbaa-group top
      :sync acquire
      :requires (atomic-order)
      :observable (atomic-load)
      :targets (vm-bytecode aot-c aot-verify))
    '''
    ops = parse_xi_ops_def(text)
    assert len(ops) == 9
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
    assert ops[7].tbaa_group == 'fresh'
    assert ops[7].sync_order == 'none'
    assert ops[8].tbaa_group == 'top'
    assert ops[8].sync_order == 'acquire'
    assert ops[8].observable_contract == 'contracts/xi-canonical-ops.md'
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
    assert 'case XI_ARRAY_NEW: return XI_GEN_TBAA_FRESH;' in header
    assert 'xi_generated_op_sync_order' in header
    assert 'case XI_ATOMIC_LOAD: return XI_GEN_SYNC_ACQUIRE;' in header
    assert 'case XI_ADD: return XI_GEN_SYNC_NONE;' in header
    assert 'xi_generated_op_backend_rewrite' in header
    assert 'case XI_ITER_NEW: return XI_GEN_BACKEND_REWRITE_BUILTIN;' in header
    assert 'xi_generated_op_backend_rewrite_name' in header
    assert 'case XI_ITER_NEW: return "iter_new";' in header
    assert 'xi_generated_op_backend_legal' not in header
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

    # Task 219 C5 (fail-closed operand ownership): an op that omits :own-use
    # must be a hard compile error, not a silent `consume` default. This is the
    # structural regression for incident 5 (nativeBytes receiver mis-consumed
    # via a default guess). `pass` (identity-alias) must also round-trip.
    missing_own_use = '''
    (define-xi-op xi.needs.decl
      :class pure
      :arity 1
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    '''
    raised = False
    try:
        parse_xi_ops_def(missing_own_use)
    except SystemExit:
        raised = True
    assert raised, "missing :own-use must fail closed (task 219 C5)"

    pass_alias = '''
    (define-xi-op xi.move
      :class pure
      :arity 1
      :own-use pass
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    '''
    pass_ops = parse_xi_ops_def(pass_alias)
    assert pass_ops[0].own_use == 'pass'
    pass_header = generate_xi_ops_header(pass_ops)
    assert 'case XI_MOVE: return XI_GEN_OWN_USE_PASS;' in pass_header

    # Memory-scope rule (fail-closed): an op that touches memory must classify
    # it. `none` means "no memory at all"; using it for unclassified memory
    # makes the op invisible to xi_tbaa_may_alias, so its stores stop killing
    # loads. That is unsound, not conservative — reject it at generation time.
    def _rejects(text: str, why: str) -> None:
        raised = False
        try:
            parse_xi_ops_def(text)
        except SystemExit:
            raised = True
        assert raised, why

    _rejects('''
    (define-xi-op xi.unclassified.store
      :class memory-write
      :arity 2
      :result-kind void
      :result-ownership none
      :own-use borrow
      :effects (side-effect memory-write)
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    ''', "memory effect without :tbaa-group must fail closed")

    _rejects('''
    (define-xi-op xi.pure.but.grouped
      :class pure
      :arity 1
      :own-use borrow
      :effects ()
      :tbaa-group array
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    ''', ":tbaa-group without a memory effect must fail closed")

    _rejects('''
    (define-xi-op xi.fresh.that.reads
      :class allocation
      :arity 1
      :own-use consume
      :effects (side-effect memory-read memory-write allocates)
      :tbaa-group fresh
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    ''', "'fresh' plus memory-read must fail closed")

    _rejects('''
    (define-xi-op xi.sync.without.effect
      :class pure
      :arity 1
      :own-use borrow
      :effects ()
      :sync acquire
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    ''', ":sync on an effect-free op must fail closed")

    _rejects('''
    (define-xi-op xi.bad.sync.name
      :class memory-read
      :arity 1
      :own-use borrow
      :effects (memory-read)
      :tbaa-group top
      :sync consume
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    ''', "unknown :sync order must fail closed")

    _rejects('''
    (define-xi-op xi.speculatable.barrier
      :class memory-read
      :arity 1
      :own-use borrow
      :effects (memory-read)
      :tbaa-group top
      :sync acquire
      :speculation safe
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    ''', "a speculatable synchronisation edge must fail closed")

    _rejects('''
    (define-xi-op xi.numbered.barrier
      :class memory-read
      :arity 1
      :own-use borrow
      :effects (memory-read)
      :tbaa-group top
      :sync acquire
      :vn-kind memory-read
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    ''', "a value-numbered synchronisation edge must fail closed")
    print(" PASS", file=sys.stderr)

def _test_xi_semantic_ops_parser():
    print("  test_xi_semantic_ops_parser...", end='', file=sys.stderr)
    text = '''
    (define-xi-op xi.primitive :class pure :arity 0 :own-use borrow
      :effects () :requires () :observable () :targets (aot-c))
    (define-xi-op xi.kernel :class pure :arity 0 :own-use borrow
      :observable-contract "src/shared/xr_test_kernel.h"
      :effects () :requires () :observable (kernel-result) :targets (aot-c))
    (define-xi-op xi.provider :class side-effect :arity 0 :own-use borrow
      :effects (side-effect) :requires () :observable (host-result) :targets (aot-c))
    (define-xi-op xi.generated :class call :arity 0 :own-use borrow
      :effects () :requires () :observable () :targets (aot-c))
    (define-xi-semantic-owner declarative-primitive :operations (xi.primitive))
    (define-xi-semantic-owner shared-semantic-kernel :operations (xi.kernel))
    (define-xi-semantic-owner capability-provider :operations (xi.provider))
    (define-xi-semantic-owner generated-specialization :operations (xi.generated))
    (define-xi-observable-owner shared.test-kernel
      :category shared-semantic-kernel
      :operations (xi.kernel)
      :consumers (semantic-plan cgen)
      :cgen-adapter xr_test_kernel
      :production-bindings (
        (semantic-plan "src/plan/semantic/xr_semantic_ops.c" "xr_semantic_op_contract")
        (cgen "src/aot/xi_cgen.c" "xr_semantic_owner_cgen_adapter")))
    '''
    ops = parse_xi_ops_def(text)
    owners = parse_xi_semantic_owners(text, ops)
    assert owners['xi.primitive'] == 'declarative-primitive'
    assert owners['xi.kernel'] == 'shared-semantic-kernel'
    explicit, observable = parse_xi_observable_owners(text, ops, owners)
    header = generate_xi_semantic_ops_header(ops, owners, observable)
    assert 'XR_SEM_OWNER_CAPABILITY_PROVIDER' in header
    assert '"kernel-result"' in header
    assert 'XR_SEM_OWNER_ID_SHARED_TEST_KERNEL_HI' in header
    assert explicit[0].name == 'shared.test-kernel'
    assert observable[1].consumer_bits == (
        XI_SEMANTIC_CONSUMERS['semantic-plan'] | XI_SEMANTIC_CONSUMERS['cgen'])
    assert observable[1].observable_contract == 'src/shared/xr_test_kernel.h'
    registry = build_semantic_owner_registry(observable)
    assert len(registry['canonical_fingerprint']) == 64
    assert registry['operations'][1]['owner'] == 'shared.test-kernel'
    assert registry['operations'][1]['observable_contract'] == 'src/shared/xr_test_kernel.h'
    ids_header = generate_semantic_owner_ids_header(explicit, observable)
    assert 'XR_SEM_OWNER_ID_SHARED_TEST_KERNEL_HI' in ids_header
    assert 'xr_semantic_owner_cgen_adapter' in ids_header

    def reject_semantic(source: str):
        candidate_ops = parse_xi_ops_def(source)
        try:
            parse_xi_semantic_owners(source, candidate_ops)
            assert False, "invalid semantic owner registry should be rejected"
        except SystemExit:
            pass

    def reject_observable(source: str, hasher=None):
        candidate_ops = parse_xi_ops_def(source)
        candidate_owners = parse_xi_semantic_owners(source, candidate_ops)
        try:
            parse_xi_observable_owners(source, candidate_ops, candidate_owners,
                                       id_hasher=hasher)
            assert False, "invalid observable owner registry should be rejected"
        except SystemExit:
            pass

    reject_semantic(text.replace('xi.generated))', 'xi.primitive))'))
    reject_semantic(text.replace('(xi.generated))', '())'))
    reject_semantic(text.replace('generated-specialization :operations (xi.generated)',
                                 'unknown-category :operations (xi.generated)'))
    reject_semantic(text + '''
      (define-xi-semantic-owner declarative-primitive :operations ())
    ''')
    reject_observable(text.replace(':operations (xi.kernel)\n      :consumers',
                                   ':operations (xi.unknown)\n      :consumers'))
    reject_observable(text.replace(':category shared-semantic-kernel',
                                   ':category unknown-category'))
    reject_observable(text + '''
      (define-xi-observable-owner shared.second
        :category shared-semantic-kernel
        :operations (xi.kernel)
        :consumers (semantic-plan)
        :production-bindings (
          (semantic-plan "src/plan/semantic/xr_semantic_ops.c" "xr_semantic_op_contract")))
    ''')
    reject_observable(text.replace('(semantic-plan cgen)',
                                   '(semantic-plan unknown-consumer)'))
    reject_observable(text.replace('      :cgen-adapter xr_test_kernel\n', ''))
    reject_observable(text.replace(
        '        (cgen "src/aot/xi_cgen.c" "xr_semantic_owner_cgen_adapter")', ''))
    reject_observable(text.replace(
        '        (cgen "src/aot/xi_cgen.c" "xr_semantic_owner_cgen_adapter")',
        '        (cgen "src/aot/xi_cgen.c" "xr_semantic_owner_cgen_adapter")\n'
        '        (cgen "src/aot/xi_cgen.c" "duplicate")'))
    reject_observable(text.replace(
        '        (cgen "src/aot/xi_cgen.c" "xr_semantic_owner_cgen_adapter")',
        '        (unknown-consumer "src/aot/xi_cgen.c" "unknown")'))
    reject_observable(text, hasher=lambda _: bytes(16))
    try:
        parse_xi_ops_def(text.replace('src/shared/xr_test_kernel.h', '../outside.h'))
        assert False, "non-canonical observable contract path should be rejected"
    except SystemExit:
        pass
    print(" PASS", file=sys.stderr)


def _test_xi_lowering_parser():
    print("  test_xi_lowering_parser...", end='', file=sys.stderr)
    ops_text = '''
    (define-xi-op xi.add
      :class arithmetic
      :arity 2
      :own-use borrow
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.copy
      :class pure
      :arity 1
      :own-use borrow
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.codegen.compiler-fence
      :class memory-write
      :arity 0
      :result-kind void
      :result-ownership none
      :own-use borrow
      :tbaa-group top
      :effects (side-effect memory-read memory-write)
      :requires ()
      :observable (compiler-order)
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.await
      :class coroutine
      :arity 1
      :own-use borrow
      :effects (side-effect may-suspend)
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.phi
      :class pure
      :arity variadic
      :own-use pass
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify)
      :lowering-policy special)
    (define-xi-op xi.extract
      :class call
      :arity 1
      :own-use borrow
      :effects ()
      :requires ()
      :observable ()
      :targets (aot-verify)
      :lowering-policy verifier-only)
    (define-xi-op xi.vec.add
      :class vector
      :arity 2
      :own-use borrow
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
      :vm-bytecode (driver xi_emit_add :fresh-dst yes)
      :aot-c (driver xicgen_add))
    (lower xi.copy
      :match ()
      :required-targets (vm-bytecode aot-c)
      :vm-bytecode (driver xi_emit_copy)
      :aot-c (driver xicgen_copy))
    (lower xi.codegen.compiler-fence
      :match ()
      :required-targets (vm-bytecode aot-c-stmt)
      :vm-bytecode (driver emit_codegen_compiler_fence)
      :aot-c-stmt (driver xicgen_stmt_codegen_compiler_fence))
    (lower xi.await
      :match ()
      :required-targets (vm-bytecode aot-c)
      :vm-bytecode (driver xi_emit_await)
      :aot-c (consumer xi-cgen-direct
        (binding "src/aot/fixture.c" emit_coro_value_stmt
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value))
        (binding "src/aot/fixture.c" emit_value_rhs
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value))))
    (lower xi.phi
      :required-targets (aot-c)
      :aot-c (consumer xi-cgen-direct
        (binding "src/aot/fixture.c" emit_phi_copies
          (structural phi edge-parallel-copy))
        (binding "src/aot/fixture.c" emit_declarations
          (structural phi sync-storage))
        (binding "src/aot/fixture.c" emit_coro_frame_type
          (structural phi coroutine-frame-storage))
        (binding "src/aot/fixture.c" emit_coro_local_declarations
          (structural phi coroutine-local-storage))))
    '''
    ops = parse_xi_ops_def(ops_text)
    entries = parse_xi_lowering_def(lowering_text, ops)
    assert len(entries) == 5
    assert entries[0].ident == 'ADD'
    assert entries[0].targets == ['aot-c', 'vm-bytecode']
    assert entries[0].template == 'value-binary'
    assert entries[0].target_attrs['vm-bytecode']['fresh-dst'] is True
    assert entries[1].target_drivers['aot-c'] == 'xicgen_copy'
    assert 'aot-c-stmt' not in entries[1].target_drivers
    assert entries[1].template == 'custom'
    assert entries[2].target_drivers['aot-c-stmt'] == \
        'xicgen_stmt_codegen_compiler_fence'
    assert [binding.symbol for binding in entries[3].target_consumers['aot-c']] == [
        'emit_coro_value_stmt', 'emit_value_rhs'
    ]
    assert entries[3].target_consumers['aot-c'][0].emitters == (
        XiConsumerEmitterWitness('src/aot/fixture.c', 'emit_real_value'),)
    assert {binding.structural_category
            for binding in entries[4].target_consumers['aot-c']} == \
        XI_CONSUMER_STRUCTURAL_CATEGORIES

    def reject_lowering(candidate: str, reason: str) -> None:
        try:
            parse_xi_lowering_def(candidate, ops)
            assert False, reason
        except SystemExit:
            pass

    reject_lowering(
        lowering_text.replace('(lower xi.add\n', '(lower xi.add\n      :bogus kept\n', 1),
        'unknown lowering field should be rejected')
    reject_lowering(
        lowering_text.replace(':match (value-binary)',
                              ':match (value-binary)\n      :match ()', 1),
        'duplicate lowering field should be rejected')
    reject_lowering(
        lowering_text.replace(':aot-c (driver xicgen_add))',
                              ':aot-c (driver xicgen_add) trailing)', 1),
        'stray lowering field should be rejected')
    reject_lowering(
        lowering_text.replace(':required-targets (vm-bytecode aot-c)',
                              ':required-targets (vm-bytecode aot-c)\n'
                              '      :required-targets (vm-bytecode aot-c)', 1),
        'duplicate required-targets should be rejected')
    reject_lowering(
        lowering_text.replace(
            ':aot-c-stmt (driver xicgen_stmt_codegen_compiler_fence)',
            ':aot-c-stmt (driver xicgen_stmt_try)', 1),
        'statement driver remap should be rejected')
    header = generate_xi_lowering_coverage_header(entries, ops)
    assert 'XI_LOWERING_ENTRY_COUNT = 5' in header
    assert 'XI_LOWERING_PATTERNED_ENTRY_COUNT = 1' in header
    assert 'XI_LOWERING_MAIN_BACKEND_ENTRY_COUNT = 4' in header
    assert 'XI_LOWERING_MAIN_BACKEND_PATTERNED_ENTRY_COUNT = 1' in header
    assert 'XI_LOWERING_CONSUMER_TARGET_COUNT = 2' in header
    assert 'XI_LOWERING_CONSUMER_BINDING_COUNT = 6' in header
    assert 'XI_LOWERING_CONSUMER_ROUTER_WITNESS_COUNT = 0' in header
    assert 'XI_LOWERING_CONSUMER_EMITTER_WITNESS_COUNT = 2' in header
    assert 'XI_LOWERING_CONSUMER_PREDICATE_WITNESS_COUNT = 0' in header
    assert 'XI_LOWERING_CONSUMER_GUARDED_SELECTOR_COUNT = 0' in header
    assert 'XI_LOWERING_CONSUMER_ACTIVATION_EDGE_COUNT = 0' in header
    assert 'XI_LOWERING_CONSUMER_ACTIVATION_CALL_COUNT = 0' in header
    assert 'XI_LOWERING_CONSUMER_OUTPUT_SEQUENCE_COUNT = 0' in header
    assert ('X(AWAIT, "xi.await", "src/aot/fixture.c", emit_coro_value_stmt, '
            'XI_LOWERING_CONSUMER_WITNESS_SELECTOR)' in header)
    assert ('X(AWAIT, "xi.await", "src/aot/fixture.c", emit_coro_value_stmt, '
            '"src/aot/fixture.c", emit_real_value)' in header)
    assert ('X(PHI, "xi.phi", "src/aot/fixture.c", emit_coro_frame_type, '
            'XI_LOWERING_CONSUMER_WITNESS_STRUCTURAL_COROUTINE_FRAME_STORAGE)'
            in header)
    assert 'case XI_ADD: return XI_LOWER_TEMPLATE_VALUE_BINARY;' in header
    assert 'case XI_ADD: return XI_LOWER_TARGET_AOT_C | XI_LOWER_TARGET_VM_BYTECODE;' in header
    assert ('case XI_CODEGEN_COMPILER_FENCE: return '
            'XI_LOWER_TARGET_AOT_C_STMT | XI_LOWER_TARGET_VM_BYTECODE;'
            in header)
    assert 'return aot_implementations != 0 && aot_rejections == 0;' in header
    vm_header = generate_xi_vm_dispatch_header(entries)
    assert 'X(ADD, xi_emit_add)' in vm_header
    assert 'case XI_ADD: return OP_ADD;' in vm_header
    assert 'xi_emit_vm_template_swaps_args' in vm_header
    assert 'case XI_ADD: return true;' in vm_header
    vm_bitwise = generate_xi_vm_template_bitwise_binary_dispatch([
        XiLoweringDef('xi.band', 'BAND', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-binary'),
        XiLoweringDef('xi.bxor', 'BXOR', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-binary'),
    ])
    assert 'XVM_TEMPLATE_BITWISE_BINARY_CASE(OP_BAND, XR_BITWISE_BINARY_AND, true' in vm_bitwise
    assert 'XVM_TEMPLATE_BITWISE_BINARY_CASE(OP_BXOR, XR_BITWISE_BINARY_XOR, false' in vm_bitwise
    assert 'xr_bigint_and' not in vm_bitwise and 'SYMBOL_OP_BAND' not in vm_bitwise
    vm_bitwise_unary = generate_xi_vm_template_bitwise_unary_dispatch([
        XiLoweringDef('xi.bnot', 'BNOT', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_bnot'}, {}, {},
                      template='value-unary'),
    ])
    assert ('XVM_TEMPLATE_BITWISE_UNARY_CASE(OP_BNOT, '
            '"bitwise NOT requires integer type")') in vm_bitwise_unary
    assert '~' not in vm_bitwise_unary and 'SYMBOL_OP_BNOT' not in vm_bitwise_unary
    vm_unary = generate_xi_vm_template_unary_dispatch([
        XiLoweringDef('xi.neg', 'NEG', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-unary'),
        XiLoweringDef('xi.not', 'NOT', ['vm-bytecode'], ['vm-bytecode'],
                      {'vm-bytecode': 'xi_emit_arith'}, {}, {},
                      template='value-unary'),
    ])
    assert 'XVM_TEMPLATE_UNARY_NEG_CASE(OP_UNM)' in vm_unary
    assert '"-"' not in vm_unary
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
    assert 'XVM_TEMPLATE_ARITH_ADD_CASE(OP_ADD, xr_i64_add_wrap, +,' in vm_arith_binary
    assert 'XVM_TEMPLATE_ARITH_NUMERIC_CASE(OP_SUB, xr_i64_sub_wrap, -,' in vm_arith_binary
    assert 'XVM_TEMPLATE_ARITH_MUL_CASE(OP_MUL, xr_i64_mul_wrap, *,' in vm_arith_binary
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
    assert 'XVM_TEMPLATE_SHIFT_CASE(OP_SHL, XR_SHIFT_LEFT)' in vm_shift
    assert 'XVM_TEMPLATE_SHIFT_CASE(OP_SHR, XR_SHIFT_RIGHT_SIGNED)' in vm_shift
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
    lowering_test = generate_xi_lowering_test(entries, ops)
    assert 'xi_emit_vm_requires_fresh_dst(XI_ADD) == true' in lowering_test
    assert 'xi_emit_vm_requires_fresh_dst(XI_COPY) == false' in lowering_test
    assert ('xi_lowering_op_backend_legal(XI_CODEGEN_COMPILER_FENCE) == true'
            in lowering_test)
    assert 'xi_lowering_template_kind(XI_ADD) == XI_LOWER_TEMPLATE_VALUE_BINARY' in lowering_test
    aot_header = generate_xi_to_c_dispatch_header(entries)
    assert 'X(ADD, "xi.add", xicgen_add)' in aot_header
    assert 'emit_coro_value_stmt' not in aot_header
    assert 'XI_TO_C_TEMPLATE_ARITH_DRIVERS' in aot_header
    assert 'case XI_ADD: return "xrt_add";' in aot_header
    assert 'xi_to_c_template_arith_native_op' in aot_header
    assert 'XI_TO_C_TEMPLATE_WIDTH_DRIVERS' in aot_header
    assert 'xi_to_c_template_width_numeric_kernel' in aot_header
    assert 'xi_to_c_template_width_uses_f64_lane' in aot_header
    stmt_header = generate_xi_target_dispatch_header(entries, 'aot-c-stmt', 'TEST_STMT_H',
                                                     'TEST_STMT')
    assert 'xi.copy' not in stmt_header
    assert ('X(CODEGEN_COMPILER_FENCE, "xi.codegen.compiler-fence", '
            'xicgen_stmt_codegen_compiler_fence)' in stmt_header)
    rejected_stmt_text = lowering_text.replace(
        ':aot-c-stmt (driver xicgen_stmt_codegen_compiler_fence)',
        ':aot-c-stmt (reject unsupported_compiler_fence)')
    rejected_stmt_entries = parse_xi_lowering_def(rejected_stmt_text, ops)
    rejected_stmt_test = generate_xi_lowering_test(rejected_stmt_entries, ops)
    assert ('xi_lowering_op_backend_legal(XI_CODEGEN_COMPILER_FENCE) == false'
            in rejected_stmt_test)
    mixed_aot_text = lowering_text.replace(
        '''    (lower xi.codegen.compiler-fence
      :match ()
      :required-targets (vm-bytecode aot-c-stmt)
      :vm-bytecode (driver emit_codegen_compiler_fence)
      :aot-c-stmt (driver xicgen_stmt_codegen_compiler_fence))''',
        '''    (lower xi.codegen.compiler-fence
      :match ()
      :required-targets (vm-bytecode aot-c aot-c-stmt)
      :vm-bytecode (driver emit_codegen_compiler_fence)
      :aot-c (driver xicgen_codegen_compiler_fence)
      :aot-c-stmt (reject unsupported_compiler_fence))''')
    try:
        parse_xi_lowering_def(mixed_aot_text, ops)
        assert False, "normalized AOT driver/reject collision should be rejected"
    except SystemExit:
        pass
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
      :own-use borrow
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
    await_consumer = '''(consumer xi-cgen-direct
        (binding "src/aot/fixture.c" emit_coro_value_stmt
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value))
        (binding "src/aot/fixture.c" emit_value_rhs
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value)))'''
    fixture_selector_binding = '''(binding "src/aot/fixture.c" emit_value_rhs
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value))'''
    multi_router_text = lowering_text.replace(
        fixture_selector_binding,
        '''(binding "src/aot/fixture.c" emit_value_rhs
          (selected-by "src/aot/fixture.c" emit_value_stmt)
          (selected-by "src/aot/fixture.c" emit_block))''')
    multi_router_entries = parse_xi_lowering_def(multi_router_text, ops)
    multi_router_binding = multi_router_entries[3].target_consumers['aot-c'][1]
    assert multi_router_binding.witness_kind == 'selected-by'
    assert [(router.source_path, router.symbol)
            for router in multi_router_binding.routers] == [
        ('src/aot/fixture.c', 'emit_value_stmt'),
        ('src/aot/fixture.c', 'emit_block'),
    ]
    guarded_selector_text = lowering_text.replace(
        fixture_selector_binding,
        '''(binding "src/aot/fixture.c" emit_value_rhs
          (guarded-selector)
          (emitter "src/aot/fixture.c" emit_real_value))''')
    guarded_selector_entries = parse_xi_lowering_def(guarded_selector_text, ops)
    assert (guarded_selector_entries[3].target_consumers['aot-c'][1].witness_kind ==
            'guarded-selector')
    invalid_consumers = (
        (lowering_text.replace(
            await_consumer, '(consumer xi-cgen-direct)'),
         'consumer target without an owner should be rejected'),
        (lowering_text.replace(
            '(driver xi_emit_await)',
            '(consumer xi-cgen-direct\n'
            '        (binding "src/aot/fixture.c" emit_coro_value_stmt\n'
            '          (selector)\n'
            '          (emitter "src/aot/fixture.c" emit_real_value)))'),
         'consumer owner on a VM target should be rejected'),
        (lowering_text.replace(
            await_consumer,
            '''(consumer xi-cgen-direct
        (binding "src/aot/fixture.c" emit_coro_value_stmt
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value))
        (binding "src/aot/fixture.c" emit_coro_value_stmt
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value)))'''),
         'duplicate consumer owner should be rejected'),
        (lowering_text.replace(
            ':aot-c ' + await_consumer,
            ':aot-c ' + await_consumer + '\n'
            '      :aot-c (driver xicgen_await)'),
         'consumer and driver for one target should be rejected'),
        (lowering_text.replace('"src/aot/fixture.c" emit_value_rhs',
                               '"../fixture.c" emit_value_rhs'),
         'non-canonical consumer path should be rejected'),
        (lowering_text.replace('"src/aot/fixture.c" emit_value_rhs',
                               '"src/aot/fixture.c" static'),
         'C keyword consumer symbol should be rejected'),
        (lowering_text.replace(
            fixture_selector_binding,
            '(binding "src/aot/fixture.c" emit_value_rhs (selected-by))'),
         'selected-by without a router should be rejected'),
        (lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (selected-by "src/aot/fixture.c" emit_value_stmt)
          (selected-by "src/aot/fixture.c" emit_value_stmt))'''),
         'duplicate selected-by routers should be rejected'),
        (lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value)
          (selected-by "src/aot/fixture.c" emit_value_stmt))'''),
         'selector and selected-by witnesses should not mix'),
        (lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (guarded-selector extra)
          (emitter "src/aot/fixture.c" emit_real_value))'''),
         'guarded-selector arguments should be rejected'),
        (lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value)
          (guarded-selector))'''),
         'selector and guarded-selector witnesses should not mix'),
        (lowering_text.replace(
            '(structural phi sync-storage)',
            '''(structural phi sync-storage)
          (selector)'''),
         'structural and selector witnesses should not mix'),
        (lowering_text.replace(
            '(structural phi sync-storage)',
            '''(structural phi sync-storage)
          (selected-by "src/aot/fixture.c" emit_value_stmt)'''),
         'structural and selected-by witnesses should not mix'),
        (lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (structural await edge-parallel-copy))'''),
         'non-phi structural ownership should be rejected'),
        (lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (selected-by "src/aot/fixture.c" emit_value_stmt)
          (predicate "src/aot/fixture.c" predicate_owner))'''),
         'predicate witness outside selector ownership should be rejected'),
    )
    for invalid_text, reason in invalid_consumers:
        try:
            parse_xi_lowering_def(invalid_text, ops)
            assert False, reason
        except SystemExit:
            pass

    def reject_consumer_schema_mutation(mutated_text: str,
                                        expected_reason: str) -> None:
        diagnostics = io.StringIO()
        try:
            with contextlib.redirect_stderr(diagnostics):
                parse_xi_lowering_def(mutated_text, ops)
            assert False, expected_reason
        except SystemExit:
            assert expected_reason in diagnostics.getvalue()

    reject_consumer_schema_mutation(
        lowering_text.replace(
            fixture_selector_binding,
            '(binding "src/aot/fixture.c" emit_value_rhs (selector))'),
        'selector witness requires an exact terminal emitter')
    reject_consumer_schema_mutation(
        lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (selector)
          (emitter "src/aot/fixture.c"))'''),
        'emitter must name one path and symbol')
    reject_consumer_schema_mutation(
        lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value)
          (emitter "src/aot/fixture.c" emit_real_value))'''),
        'duplicate emitter witness')
    reject_consumer_schema_mutation(
        lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (selected-by "src/aot/fixture.c" emit_value_stmt)
          (emitter "src/aot/fixture.c" emit_real_value))'''),
        'emitter witness is only valid with selector ownership')
    reject_consumer_schema_mutation(
        lowering_text.replace(
            fixture_selector_binding,
            '(binding "src/aot/fixture.c" emit_value_rhs (selected-by))'),
        'selected-by witness must name one router path and symbol')
    reject_consumer_schema_mutation(
        lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value)
          (predicate "src/aot/fixture.c"))'''),
        'predicate must name one path and symbol')
    reject_consumer_schema_mutation(
        lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value)
          (predicate "src/aot/fixture.c" predicate_owner)
          (predicate "src/aot/fixture.c" predicate_owner))'''),
        'duplicate predicate witness')
    reject_consumer_schema_mutation(
        lowering_text.replace(
            fixture_selector_binding,
            '''(binding "src/aot/fixture.c" emit_value_rhs
          (selector)
          (emitter "src/aot/fixture.c" emit_real_value)
          (predicate "src/aot/fixture.c" predicate_owner
            (domain "src/aot/fixture.c")))'''),
        'predicate domain must name one path and symbol')
    repository_root = Path(__file__).resolve().parents[2]
    real_ops_path = repository_root / 'xisa/xi/ops.def'
    real_lowering_path = repository_root / 'xisa/xi/lowering.def'
    real_ops = parse_xi_ops_def(read_file(str(real_ops_path)), str(real_ops_path))
    real_lowering_text = read_file(str(real_lowering_path))
    real_entries = parse_xi_lowering_def(real_lowering_text, real_ops,
                                         str(real_lowering_path))
    real_snapshot = capture_xi_lowering_validation_snapshot(repository_root)
    real_discovery = _xi_capture_aot_discovery_census(repository_root)
    validated_baseline = validate_xi_lowering_consumer_sources(
        real_entries, repository_root, str(real_lowering_path),
        real_snapshot, real_discovery)
    assert validated_baseline == real_snapshot
    with tempfile.TemporaryDirectory(prefix='xisagen-lowering-no-touch-') as directory:
        first_outputs = write_xi_lowering_outputs(directory, real_entries, real_ops)
        assert len(first_outputs) == 12
        for index, output in enumerate(first_outputs):
            marker = 1_700_000_000_000_000_000 + index
            os.utime(output, ns=(marker, marker))
        before_outputs = {
            output: (os.stat(output).st_mtime_ns, os.stat(output).st_size,
                     os.stat(output).st_ino)
            for output in first_outputs
        }
        second_outputs = write_xi_lowering_outputs(directory, real_entries, real_ops)
        assert second_outputs == first_outputs
        assert {
            output: (os.stat(output).st_mtime_ns, os.stat(output).st_size,
                     os.stat(output).st_ino)
            for output in second_outputs
        } == before_outputs
        assert check_xi_lowering_outputs(
            directory, real_entries, real_ops) == first_outputs
        proof_stamp = Path(directory) / 'validation.stamp'
        proof_content = _xi_lowering_validation_stamp_content(
            real_ops_path.read_bytes(), real_lowering_path.read_bytes(),
            real_snapshot, real_discovery)
        _xi_atomic_write(proof_stamp, proof_content)
        early_projection = Path(first_outputs[0])
        early_content = early_projection.read_bytes()

        def mutate_before_projection_check() -> None:
            early_projection.write_bytes(early_content + b'/* pre-check drift */\n')

        try:
            _xi_run_lowering_check(
                real_ops_path, real_lowering_path, Path(directory), proof_stamp,
                after_validation_hook=mutate_before_projection_check)
            assert False, "post-validation pre-check mutation must fail closed"
        except SystemExit:
            pass
        early_projection.write_bytes(early_content)

        def mutate_early_projection_after_compare() -> None:
            early_projection.write_bytes(early_content + b'/* mid-check drift */\n')

        try:
            _xi_run_lowering_check(
                real_ops_path, real_lowering_path, Path(directory), proof_stamp,
                after_first_projection_hook=mutate_early_projection_after_compare)
            assert False, "an early projection changed after comparison must fail closed"
        except SystemExit:
            pass
        early_projection.write_bytes(early_content)

        def replace_proof_after_validation() -> None:
            _xi_atomic_write(proof_stamp, proof_content)

        try:
            _xi_run_lowering_check(
                real_ops_path, real_lowering_path, Path(directory), proof_stamp,
                after_validation_hook=replace_proof_after_validation)
            assert False, "proof stamp identity drift before checking must fail closed"
        except SystemExit:
            pass
        stale_output = Path(first_outputs[0])
        stale_output.write_text(
            stale_output.read_text(encoding='utf-8') + '/* stale */\n',
            encoding='utf-8')
        diagnostic = io.StringIO()
        try:
            with contextlib.redirect_stderr(diagnostic):
                check_xi_lowering_outputs(directory, real_entries, real_ops)
            assert False, "a stale checked-in projection must fail closed"
        except SystemExit:
            assert 'checked-in projection differs from canonical content' in \
                diagnostic.getvalue()
    assert _xi_terminal_emission_branch(
        'emit_aot_coro_op_stmt(ctx, out, f, v); return;')
    assert not _xi_terminal_emission_branch(
        'fprintf(out, "log only"); return;')
    assert not _xi_guarded_selector_present(
        'if (!v || v->op != XI_THREAD_SPAWN) return false; return false;',
        'XI_THREAD_SPAWN')
    assert not _xi_direct_selector_present(
        'if (false) { if (v->op == XI_AWAIT) { '
        'emit_real_value(out); return; } }',
        'XI_AWAIT')
    assert not _xi_selector_routes_to_owner(
        'if (false) { if (v->op == XI_GO) { xicgen_go(); return; } }',
        'XI_GO', 'xicgen_go')
    assert not _xi_selector_routes_to_owner(
        'if (v->op == XI_PAR_MAP || v->op == XI_PAR_REDUCE) { '
        'if (false) { if (v->op == XI_PAR_MAP) xicgen_par_map(); '
        'else xicgen_par_reduce(); } return; }',
        'XI_PAR_MAP', 'xicgen_par_map')
    assert not _xi_switch_routes_to_owner(
        'switch (v->op) { case XI_GO: if (false) xicgen_go(); return true; }',
        'XI_GO', 'xicgen_go')
    assert not _xi_switch_routes_to_owner(
        'switch (v->op) { case XI_GO: return true; xicgen_go(); return true; }',
        'XI_GO', 'xicgen_go')
    real_consumers = [entry for entry in real_entries if entry.target_consumers]
    assert len(real_consumers) == 33
    assert sum(len(bindings) for entry in real_consumers
            for bindings in entry.target_consumers.values()) == 44
    assert sum(len(binding.predicates) for entry in real_consumers
               for bindings in entry.target_consumers.values()
               for binding in bindings) == 6
    assert sum(1 for entry in real_consumers
               for bindings in entry.target_consumers.values()
               for binding in bindings
               for predicate in binding.predicates
               if predicate.domain_symbol is not None) == 2
    call_consumers = {
        (entry.op_name, target, binding.source_path, binding.symbol,
         binding.witness_kind,
         tuple((emitter.source_path, emitter.symbol)
               for emitter in binding.emitters))
        for entry in real_consumers
        if entry.op_name in {
            'xi.call', 'xi.call.method', 'xi.call.method.direct',
            'xi.call.builtin'}
        for target, bindings in entry.target_consumers.items()
        for binding in bindings
    }
    assert call_consumers == {
        (op_name, 'aot-c', 'src/aot/xi_cgen.c', 'emit_value_stmt',
         'selector', (('src/aot/xi_cgen.c', 'emit_value_rhs'),))
        for op_name in {
            'xi.call', 'xi.call.method', 'xi.call.method.direct',
            'xi.call.builtin'}
    }
    assert {
        (entry.op_name, predicate.source_path, predicate.symbol,
         predicate.domain_source_path, predicate.domain_symbol)
        for entry in real_consumers
        if entry.op_name in {
            'xi.call', 'xi.call.method', 'xi.call.method.direct',
            'xi.call.builtin'}
        for bindings in entry.target_consumers.values()
        for binding in bindings
        for predicate in binding.predicates
    } == {
        (op_name, 'src/aot/xi_cgen.c',
         'cg_unused_call_result_emits_statement', None, None)
        for op_name in {
            'xi.call', 'xi.call.method', 'xi.call.method.direct',
            'xi.call.builtin'}
    }
    assert sum(len(binding.routers) for entry in real_consumers
               for bindings in entry.target_consumers.values()
               for binding in bindings) == 6
    assert sum(len(binding.activations) for entry in real_consumers
               for bindings in entry.target_consumers.values()
               for binding in bindings) == 13
    assert sum(activation.count for entry in real_consumers
               for bindings in entry.target_consumers.values()
               for binding in bindings
               for activation in binding.activations) == 15
    assert sum(len(binding.output_sequences) for entry in real_consumers
               for bindings in entry.target_consumers.values()
               for binding in bindings) == 3
    assert [(entry.op_name, binding.source_path, binding.symbol)
            for entry in real_consumers
            for bindings in entry.target_consumers.values()
            for binding in bindings
            if binding.witness_kind == 'guarded-selector'] == [
        ('xi.thread.spawn', 'src/aot/xi_cgen_coro.inc.c',
         'emit_thread_spawn_value_stmt')
    ]

    def reject_binding_mutation(op_name: str, binding_index: int,
                                binding: XiConsumerBinding) -> None:
        mutated = list(real_entries)
        entry_index = next(index for index, entry in enumerate(mutated)
                           if entry.op_name == op_name)
        entry = mutated[entry_index]
        target_consumers = dict(entry.target_consumers)
        bindings = list(target_consumers['aot-c'])
        bindings[binding_index] = binding
        target_consumers['aot-c'] = bindings
        mutated[entry_index] = replace(entry, target_consumers=target_consumers)
        try:
            validate_xi_lowering_consumer_sources(mutated, repository_root,
                                                  'consumer binding mutation',
                                                  real_snapshot, real_discovery)
            assert False, "invalid source binding mutation should be rejected"
        except SystemExit:
            pass

    await_entry = next(entry for entry in real_entries if entry.op_name == 'xi.await')
    await_binding = await_entry.target_consumers['aot-c'][0]
    reject_binding_mutation('xi.await', 0,
                            replace(await_binding, symbol='missing_consumer_owner'))
    reject_binding_mutation('xi.await', 0,
                            replace(await_binding, symbol='emit_thread_spawn_value_stmt'))
    reject_binding_mutation(
        'xi.await', 0,
        replace(await_binding, symbol='emit_coro_debug_result_source_var_sync'))
    reject_binding_mutation(
        'xi.await', 0,
        replace(await_binding, source_path='src/aot/xi_cgen.c',
                symbol='cg_debug_source_var_storage_value'))
    phi_entry = next(entry for entry in real_entries if entry.op_name == 'xi.phi')
    phi_binding = phi_entry.target_consumers['aot-c'][0]
    reject_binding_mutation('xi.phi', 0,
                            replace(phi_binding, symbol='emit_phi_ref'))
    par_reduce_entry = next(entry for entry in real_entries
                            if entry.op_name == 'xi.par.reduce')
    par_reduce_binding = par_reduce_entry.target_consumers['aot-c'][0]
    reject_binding_mutation('xi.par.reduce', 0,
                            replace(par_reduce_binding, symbol='xicgen_par_map'))
    mul_entry = next(entry for entry in real_entries if entry.op_name == 'xi.mul')
    mul_binding = mul_entry.target_consumers['aot-c'][0]
    assert mul_binding.predicates == (
        XiConsumerPredicateWitness('src/aot/xi_cgen.c',
                                   'cg_u64_mul_wide_pair',
                                   'src/aot/xi_cgen.c',
                                   'cg_u64_mul_wide_value_is_eligible'),)
    reject_binding_mutation(
        'xi.mul', 0,
        replace(mul_binding, predicates=(XiConsumerPredicateWitness(
            'src/aot/xi_cgen.c', 'cg_u64_mul_wide_value_is_eligible'),)))
    reject_binding_mutation(
        'xi.mul', 0,
        replace(mul_binding, predicates=(XiConsumerPredicateWitness(
            'src/aot/xi_cgen.c', 'emit_value_stmt'),)))

    consumer_sources = {relative for relative, _ in real_snapshot.sources}

    with tempfile.TemporaryDirectory(prefix='xisagen-aot-discovery-') as directory:
        discovery_root = Path(directory)
        (discovery_root / 'CMakeLists.txt').write_bytes(
            (repository_root / 'CMakeLists.txt').read_bytes())
        for relative, data in real_snapshot.sources:
            destination = discovery_root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
        for source_path in sorted((repository_root / 'src/aot').rglob('*.c')):
            relative = source_path.relative_to(repository_root)
            destination = discovery_root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(source_path.read_bytes())
        unrelated = discovery_root / 'src/aot/discovery/plain_unrelated.c'
        unrelated.parent.mkdir(parents=True, exist_ok=True)
        unrelated.write_text(
            'static void plain_unrelated(void) {}\n', encoding='utf-8')
        validate_xi_lowering_consumer_sources(
            real_entries, discovery_root, 'plain unrelated discovery source')

        def reject_discovery_source(name: str, source: str,
                                    expected_reason: str) -> None:
            hostile = discovery_root / f'src/aot/discovery/{name}.c'
            hostile.write_text(source, encoding='utf-8')
            diagnostics = io.StringIO()
            try:
                with contextlib.redirect_stderr(diagnostics):
                    validate_xi_lowering_consumer_sources(
                        real_entries, discovery_root,
                        f'undeclared matching AOT discovery source {name}')
                assert False, f"undeclared matching AOT source {name} must fail closed"
            except SystemExit:
                message = diagnostics.getvalue()
                assert expected_reason in message, (expected_reason, message)
            finally:
                hostile.unlink()

        reject_discovery_source(
            'undeclared_await_selector',
            'static void undeclared_await_selector(XiValue *v, CgContext *ctx, '
            'FILE *out, XiFunc *f) {\n'
            '    if (v->op == XI_AWAIT) {\n'
            '        emit_aot_coro_op_stmt(ctx, out, f, v);\n'
            '        return;\n'
            '    }\n'
            '}\n',
            'xi.await:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'undeclared_add_selector',
            'static void undeclared_add_selector(XiValue *v, CgContext *ctx, '
            'FILE *out, XiFunc *f) {\n'
            '    if (v->op == XI_ADD) {\n'
            '        emit_aot_coro_op_stmt(ctx, out, f, v);\n'
            '        return;\n'
            '    }\n'
            '}\n',
            'xi.add:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'undeclared_add_structural_emitter',
            'static void undeclared_add_structural_emitter('
            'XiValue *v, CgContext *ctx, FILE *out) {\n'
            '    if (v->op == XI_ADD) {\n'
            '        emit_phi_ref(ctx, out, 0);\n'
            '        return;\n'
            '    }\n'
            '}\n',
            'xi.add:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'undeclared_par_reduce_router',
            'static void undeclared_par_reduce_router(XiValue *v, CgContext *ctx, '
            'FILE *out, XiFunc *f, const char *prefix) {\n'
            '    if (v->op == XI_PAR_REDUCE) {\n'
            '        xicgen_par_reduce(ctx, out, f, v, prefix, false);\n'
            '        return;\n'
            '    }\n'
            '}\n',
            'xi.par.reduce:aot-c: direct-consumer router census mismatch')
        reject_discovery_source(
            'undeclared_par_reduce_direct_selector',
            'static void undeclared_par_reduce_direct_selector('
            'XiValue *v, CgContext *ctx, FILE *out, XiFunc *f) {\n'
            '    if (v->op == XI_PAR_REDUCE) {\n'
            '        emit_aot_coro_op_stmt(ctx, out, f, v);\n'
            '        return;\n'
            '    }\n'
            '}\n',
            'xi.par.reduce:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'undeclared_phi_activation',
            'static void undeclared_phi_activation(CgContext *ctx, FILE *out, '
            'XiFunc *f, XiBlock *blk, size_t pred_idx) {\n'
            '    emit_phi_copies(ctx, out, f, blk, pred_idx);\n'
            '}\n',
            'activation census for src/aot/xi_cgen.c::emit_phi_copies differs')
        reject_discovery_source(
            'undeclared_thread_spawn_guard',
            'static bool undeclared_thread_spawn_guard(XiValue *v, CgContext *ctx, '
            'FILE *out, XiFunc *f) {\n'
            '    if (!v || v->op != XI_THREAD_SPAWN) return false;\n'
            '    emit_aot_coro_op_stmt(ctx, out, f, v);\n'
            '    return true;\n'
            '}\n',
            'xi.thread.spawn:aot-c: guarded-selector census mismatch')
        reject_discovery_source(
            'undeclared_thread_spawn_direct_selector',
            'static void undeclared_thread_spawn_direct_selector('
            'XiValue *v, CgContext *ctx, FILE *out, XiFunc *f) {\n'
            '    if (v->op == XI_THREAD_SPAWN) {\n'
            '        emit_aot_coro_op_stmt(ctx, out, f, v);\n'
            '        return;\n'
            '    }\n'
            '}\n',
            'xi.thread.spawn:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'macro_go_owner',
            '#define I1_ROUTE_GO(ctx, out, f, v, prefix) '
            'xicgen_go(ctx, out, f, v, prefix)\n'
            'static void macro_go_owner(XiValue *v, CgContext *ctx, FILE *out, '
            'XiFunc *f, const char *prefix) {\n'
            '    if (v->op == XI_GO) { I1_ROUTE_GO(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'governed token appears in preprocessing directive')
        reject_discovery_source(
            'macro_argument_go_owner',
            '#define I1_SELECT_OWNER(op, body) if (v->op == (op)) { body; return; }\n'
            'static void macro_argument_go_owner(XiValue *v, CgContext *ctx, '
            'FILE *out, XiFunc *f, const char *prefix) {\n'
            '    I1_SELECT_OWNER(XI_GO, xicgen_go(ctx, out, f, v, prefix));\n'
            '}\n',
            'governed selector is hidden in a macro argument')
        reject_discovery_source(
            'lowercase_macro_argument_go_owner',
            '#define select_owner(op, body) if (v->op == (op)) { body; return; }\n'
            'static void lowercase_macro_argument_go_owner('
            'XiValue *v, CgContext *ctx, FILE *out, XiFunc *f, '
            'const char *prefix) {\n'
            '    select_owner(XI_GO, xicgen_go(ctx, out, f, v, prefix));\n'
            '}\n',
            'governed selector is hidden in a macro argument')
        reject_discovery_source(
            'initializer_alias_go_owner',
            'static void initializer_alias_go_owner('
            'XiValue *v, CgContext *ctx, FILE *out, XiFunc *f, '
            'const char *prefix) {\n'
            '    XiOp alias = XI_GO;\n'
            '    if (v->op == alias) { xicgen_go(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'governed selector is used through an initializer alias')
        reject_discovery_source(
            'cast_initializer_alias_go_owner',
            'static void cast_initializer_alias_go_owner('
            'XiValue *v, CgContext *ctx, FILE *out, XiFunc *f, '
            'const char *prefix) {\n'
            '    XiOp alias = (XiOp) XI_GO;\n'
            '    if (v->op == alias) { xicgen_go(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'governed selector is used through an initializer alias')
        reject_discovery_source(
            'aggregate_alias_go_owner',
            'static const XiOp aliases[] = { XI_GO };\n'
            'static void aggregate_alias_go_owner('
            'XiValue *v, CgContext *ctx, FILE *out, XiFunc *f, '
            'const char *prefix) {\n'
            '    if (v->op == aliases[0]) { '
            'xicgen_go(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'governed selector is used through an initializer alias')
        reject_discovery_source(
            'identity_wrapper_go_owner',
            'static XiOp identity(XiOp op) { return op; }\n'
            'static void identity_wrapper_go_owner('
            'XiValue *v, CgContext *ctx, FILE *out, XiFunc *f, '
            'const char *prefix) {\n'
            '    if (v->op == identity((XiOp) XI_GO)) { '
            'xicgen_go(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'xi.go:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'stored_identity_alias_go_owner',
            'static XiOp identity_alias(XiOp op) { return op; }\n'
            'static void stored_identity_alias_go_owner('
            'XiValue *v, CgContext *ctx, FILE *out, XiFunc *f, '
            'const char *prefix) {\n'
            '    XiOp alias = identity_alias(XI_GO);\n'
            '    if (v->op == alias) { '
            'xicgen_go(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'governed selector is used through an initializer alias')
        reject_discovery_source(
            'scalar_selector_helper_go_owner',
            'static bool is_go(XiOp op) { return op == XI_GO; }\n'
            'static void scalar_selector_helper_go_owner('
            'XiValue *v, CgContext *ctx, FILE *out, XiFunc *f, '
            'const char *prefix) {\n'
            '    if (is_go(v->op)) { '
            'xicgen_go(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'xi.go:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'value_selector_helper_go_owner',
            'static bool value_is_go(const XiValue *v) { '
            'return v && v->op == XI_GO; }\n'
            'static void value_selector_helper_go_owner('
            'XiValue *v, CgContext *ctx, FILE *out, XiFunc *f, '
            'const char *prefix) {\n'
            '    if (value_is_go(v)) { '
            'xicgen_go(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'xi.go:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'renamed_second_argument_selector_helper_go_owner',
            'static bool second_argument_is_go(bool enabled, XiOp selected) { '
            'return enabled && selected == XI_GO; }\n'
            'static void renamed_second_argument_selector_helper_go_owner('
            'const XiValue *value, XiCgenCtx *ctx, FILE *out, '
            'const XiFunc *f, const char *prefix) {\n'
            '    if (second_argument_is_go(true, value->op)) { '
            'xicgen_go(ctx, out, f, value, prefix); return; }\n'
            '}\n',
            'xi.go:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'selector_factory_go_owner',
            'static XiOp go_op(void) { return XI_GO; }\n'
            'static void selector_factory_go_owner('
            'const XiValue *value, XiCgenCtx *ctx, FILE *out, '
            'const XiFunc *f, const char *prefix) {\n'
            '    if (value->op == go_op()) { '
            'xicgen_go(ctx, out, f, value, prefix); return; }\n'
            '}\n',
            'xi.go:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'two_hop_predicate_go_owner',
            'static bool inner_is_go(const XiValue *value) { '
            'return value->op == XI_GO; }\n'
            'static bool outer_is_go(const XiValue *value) { '
            'return inner_is_go(value); }\n'
            'static void two_hop_predicate_go_owner('
            'const XiValue *value, XiCgenCtx *ctx, FILE *out, '
            'const XiFunc *f, const char *prefix) {\n'
            '    if (outer_is_go(value)) { '
            'xicgen_go(ctx, out, f, value, prefix); return; }\n'
            '}\n',
            'xi.go:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'two_hop_factory_go_owner',
            'static XiOp inner_go_op(void) { return XI_GO; }\n'
            'static XiOp outer_go_op(void) { return inner_go_op(); }\n'
            'static void two_hop_factory_go_owner('
            'const XiValue *value, XiCgenCtx *ctx, FILE *out, '
            'const XiFunc *f, const char *prefix) {\n'
            '    if (value->op == outer_go_op()) { '
            'xicgen_go(ctx, out, f, value, prefix); return; }\n'
            '}\n',
            'xi.go:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'two_hop_emitter_go_owner',
            'static void inner_emit_go(XiCgenCtx *ctx, FILE *out, '
            'const XiFunc *f, const XiValue *value, const char *prefix) { '
            'xicgen_go(ctx, out, f, value, prefix); }\n'
            'static void outer_emit_go(XiCgenCtx *ctx, FILE *out, '
            'const XiFunc *f, const XiValue *value, const char *prefix) { '
            'inner_emit_go(ctx, out, f, value, prefix); }\n'
            'static void two_hop_emitter_go_owner('
            'const XiValue *v, XiCgenCtx *ctx, FILE *out, '
            'const XiFunc *f, const char *prefix) {\n'
            '    if (v->op == XI_GO) { '
            'outer_emit_go(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'xi.go:aot-c: direct-consumer selector census mismatch')
        reject_discovery_source(
            'trigraph_splice_go_owner',
            'static void trigraph_splice_go_owner('
            'const XiValue *v, XiCgenCtx *ctx, FILE *out, '
            'const XiFunc *f, const char *prefix) {\n'
            '    if (v->op == XI_??/\nGO) { '
            'xicgen_??/\ngo(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'xi.go:aot-c: direct-consumer router census mismatch')
        reject_discovery_source(
            'backslash_splice_go_owner',
            'static void backslash_splice_go_owner('
            'const XiValue *v, XiCgenCtx *ctx, FILE *out, '
            'const XiFunc *f, const char *prefix) {\n'
            '    if (v->op == XI_\\\nGO) { '
            'xicgen_\\\ngo(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'xi.go:aot-c: direct-consumer router census mismatch')
        reject_discovery_source(
            'function_pointer_go_owner',
            'static void function_pointer_go_owner(XiValue *v, CgContext *ctx, '
            'FILE *out, XiFunc *f, const char *prefix) {\n'
            '    void (*route)(XiCgenCtx *, FILE *, const XiFunc *, const XiValue *, '
            'const char *) = xicgen_go;\n'
            '    if (v->op == XI_GO) { route(ctx, out, f, v, prefix); return; }\n'
            '}\n',
            'governed function token is used through an alias')

        observation = discovery_root / 'src/aot/discovery/log_observation.c'
        observation.write_text(
            'static void log_observation(void) { '
            'int observed = observe(XI_GO); (void) observed; }\n'
            'static void literal_controls(void) { '
            'const char *text = "XI_??/\nGO"; '
            '/* XI_\\\nGO */ (void) text; }\n',
            encoding='utf-8')
        validate_xi_lowering_consumer_sources(
            real_entries, discovery_root, 'non-routing selector observation')
        observation.unlink()

        xi_cgen_path = discovery_root / 'src/aot/xi_cgen.c'
        xi_cgen_original = xi_cgen_path.read_text(encoding='utf-8')
        header_owner = discovery_root / 'src/aot/discovery/header_go_owner.h'
        header_owner.write_text(
            'static void header_go_owner(XiValue *v, XiCgenCtx *ctx, FILE *out, '
            'const XiFunc *f, const char *prefix) {\n'
            '    if (v->op == XI_GO) { xicgen_go(ctx, out, f, v, prefix); return; }\n'
            '}\n', encoding='utf-8')
        xi_cgen_path.write_text(
            xi_cgen_original + '\n#include "discovery/header_go_owner.h"\n',
            encoding='utf-8')
        diagnostics = io.StringIO()
        try:
            with contextlib.redirect_stderr(diagnostics):
                validate_xi_lowering_consumer_sources(
                    real_entries, discovery_root, 'header-only undeclared owner')
            assert False, "header-only undeclared owner must fail closed"
        except SystemExit:
            message = diagnostics.getvalue()
            assert 'xi.go:aot-c: direct-consumer router census mismatch' in message
            assert 'src/aot/discovery/header_go_owner.h::header_go_owner' in message
        finally:
            xi_cgen_path.write_text(xi_cgen_original, encoding='utf-8')
            header_owner.unlink()

        plain_header = discovery_root / 'src/aot/discovery/plain_unrelated.h'
        plain_header.write_text('static inline int i1_plain(void) { return 1; }\n',
                                encoding='utf-8')
        xi_cgen_path.write_text(
            xi_cgen_original + '\n#include "discovery/plain_unrelated.h"\n',
            encoding='utf-8')
        header_snapshot = capture_xi_lowering_validation_snapshot(discovery_root)
        validate_xi_lowering_consumer_sources(
            real_entries, discovery_root, 'plain included header')
        plain_header.write_text('static inline int i1_plain(void) { return 2; }\n',
                                encoding='utf-8')
        assert capture_xi_lowering_validation_snapshot(discovery_root) != header_snapshot
        xi_cgen_path.write_text(xi_cgen_original, encoding='utf-8')
        plain_header.unlink()

    def reject_source_mutation(relative: str,
                               replacements: tuple[tuple[str, str], ...],
                               expected_reason: str,
                               append: str = '') -> None:
        assert validated_baseline == real_snapshot
        assert relative in consumer_sources
        snapshot_sources = dict(real_snapshot.sources)
        assert set(snapshot_sources) == consumer_sources
        text = snapshot_sources[relative].decode('utf-8', errors='strict')
        for old, new in replacements:
            assert old in text
            text = text.replace(old, new)
        snapshot_sources[relative] = (text + append).encode('utf-8')
        mutated_snapshot = XiLoweringValidationSnapshot(
            tuple(sorted(snapshot_sources.items())),
            real_snapshot.include_directories, real_snapshot.directories)
        discovery_sources = dict(real_discovery.sources)
        if relative in discovery_sources:
            discovery_sources[relative] = snapshot_sources[relative]
        mutated_discovery = XiAotDiscoverySnapshot(
            tuple(sorted(discovery_sources.items())), real_discovery.directories)
        diagnostics = io.StringIO()
        try:
            with contextlib.redirect_stderr(diagnostics):
                validate_xi_lowering_consumer_sources(
                    real_entries, repository_root, 'consumer source mutation',
                    mutated_snapshot, mutated_discovery)
            assert False, "invalid consumer source mutation should be rejected"
        except SystemExit:
            message = diagnostics.getvalue()
            assert message.startswith('xisagen: error: consumer source mutation:')
            assert expected_reason in message, (expected_reason, message)
            assert 'cannot open' not in message
            assert 'outside the validated' not in message

    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('v->op == XI_AWAIT', 'v->op == XI_REMOVED_AWAIT'),),
        'does not select XI_AWAIT',
        append='\n/* v->op == XI_AWAIT */\n'
               'const char *xi_await_witness = "XI_AWAIT";\n')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('v->op == XI_AWAIT', 'v->op != XI_AWAIT'),),
        'does not select XI_AWAIT')
    call_selector_return = (
        'return v->op == XI_CALL || v->op == XI_CALL_METHOD || '
        'v->op == XI_CALL_METHOD_DIRECT ||\n'
        '           v->op == XI_CALL_BUILTIN;')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        ((call_selector_return,
          call_selector_return.replace(
              'v->op == XI_CALL ||', 'v->op == XI_CALL || true ||')),),
        'does not return exactly the declared positive selector set')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        ((call_selector_return,
          call_selector_return.replace(
              'v->op == XI_CALL ||', 'v->op != XI_CALL ||')),),
        'does not return exactly the declared positive selector set')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        ((call_selector_return,
          call_selector_return.replace(
              'v->op == XI_CALL ||', 'f->op == XI_CALL ||')),),
        'does not return exactly the declared positive selector set')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        ((call_selector_return, 'return true;'),),
        'does not return exactly the declared positive selector set')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        ((call_selector_return,
          call_selector_return.replace(
              'v->op == XI_CALL_BUILTIN;',
              'v->op == XI_CALL_BUILTIN || v->op == XI_ADD;')),),
        'does not return exactly the declared positive selector set')
    wide_domain = '(v->op != XI_MUL && v->op != XI_BIT_MUL_HIGH)'
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        ((wide_domain, '(v->op == XI_MUL || v->op == XI_BIT_MUL_HIGH)'),),
        'does not admit exactly the declared selector set')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        ((wide_domain,
          '(v->op != XI_MUL && v->op != XI_BIT_MUL_HIGH && false)'),),
        'does not admit exactly the declared selector set')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('!cg_u64_mul_wide_value_is_eligible(ctx, f, v)',
          'cg_u64_mul_wide_value_is_eligible(ctx, f, v)'),),
        'is not fail-closed through its declared selector domain')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('!cg_u64_mul_wide_value_is_eligible(ctx, f, v)',
          '!cg_u64_mul_wide_value_is_eligible(ctx, f, v->args[0])'),),
        'is not fail-closed through its declared selector domain')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('emit_aot_coro_op_stmt(ctx, out, f, v);',
          'fprintf(out, "log only");'),),
        'does not select XI_CORO_OP')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('emit_aot_coro_op_stmt(ctx, out, f, v);',
          'emit_fake(out); fprintf(out, "log only");'),),
        'does not select XI_CORO_OP',
        append='\nstatic void emit_fake(FILE *out) { '
               'fprintf(out, "log only"); }\n')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('emit_aot_coro_op_stmt(ctx, out, f, v);',
          'abort(); emit_aot_coro_op_stmt(ctx, out, f, v);'),),
        'stale terminal selector(s): '
        'src/aot/xi_cgen_coro.inc.c::emit_coro_value_stmt')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('emit_aot_coro_op_stmt(ctx, out, f, v);',
          'audit_noreturn(); emit_aot_coro_op_stmt(ctx, out, f, v);'),),
        'stale terminal selector(s): '
        'src/aot/xi_cgen_coro.inc.c::emit_coro_value_stmt',
        append='\n_Noreturn static void audit_noreturn(void) { abort(); }\n')
    thread_guard = ('    if (!v || v->op != XI_THREAD_SPAWN)\n'
                    '        return false;')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        ((thread_guard,
          '    if (v->op != XI_THREAD_SPAWN)\n        return false;'),),
        'lacks the leading fail-closed guard for XI_THREAD_SPAWN')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        ((thread_guard,
          '    if (!v || v->op == XI_THREAD_SPAWN)\n        return false;'),),
        'lacks the leading fail-closed guard for XI_THREAD_SPAWN')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        ((thread_guard,
          '    if (!v || v->op != XI_AWAIT)\n        return false;'),),
        'lacks the leading fail-closed guard for XI_THREAD_SPAWN')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        ((thread_guard,
          '    if (!v || v->op != XI_THREAD_SPAWN)\n        return true;'),),
        'lacks the leading fail-closed guard for XI_THREAD_SPAWN')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        ((thread_guard, ''),),
        'lacks the leading fail-closed guard for XI_THREAD_SPAWN')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        ((thread_guard,
          '    emit_value_generated_line_reset(ctx, out, v);\n' + thread_guard),),
        'lacks the leading fail-closed guard for XI_THREAD_SPAWN')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('static bool emit_thread_spawn_value_stmt(',
          '#if 0\nstatic bool emit_thread_spawn_value_stmt('),
         ('static bool cg_sync_go_param_needs_release(',
          '#endif\nstatic bool cg_sync_go_param_needs_release(')),
        'is enclosed by conditional preprocessing')
    for selector_condition in (
            'v->op == XI_CHAN_SEND || xi_value_is_channel_method_call(v, "send", 1)',
            'v->op == XI_CHAN_RECV',
            'v->op == XI_CHAN_TRY_SEND',
            'v->op == XI_CHAN_TRY_RECV',
            'v->op == XI_CHAN_IS_CLOSED',
            'v->op == XI_CHAN_NEW'):
        reject_source_mutation(
            'src/aot/xi_cgen_coro.inc.c',
            ((f'if ({selector_condition}) {{',
              f'if (v->op == XI_REMOVED_CHANNEL_OWNER) {{'),),
            'does not select ' + re.search(
                r'XI_[A-Z0-9_]+', selector_condition).group(0))
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c', (),
        'expected one file-scope definition',
        append='\nstatic void emit_coro_value_stmt(void) {}\n')
    reject_source_mutation(
        'src/aot/xi_cgen_dispatch_helpers.inc.c',
        (('case XI_PAR_REDUCE:', 'case XI_REMOVED_PAR_REDUCE:'),),
        'does not route XI_PAR_REDUCE')
    reject_source_mutation(
        'src/aot/xi_cgen_dispatch_helpers.inc.c',
        (('switch (v->op) {', 'switch (v->aux_int) {'),),
        'does not route XI_PAR_FOR')
    reject_source_mutation(
        'src/aot/xi_cgen_dispatch_helpers.inc.c',
        (('xicgen_par_reduce(ctx, out, f, v, prefix, false);',
          'removed_par_reduce(ctx, out, f, v, prefix, false);'),),
        'does not route XI_PAR_REDUCE')
    reject_source_mutation(
        'src/aot/xi_cgen_dispatch_helpers.inc.c',
        (('xicgen_par_reduce(ctx, out, f, v, prefix, false);',
          'emit_aot_coro_op_stmt(ctx, out, f, v);\n'
          '            xicgen_par_reduce(ctx, out, f, v, prefix, false);'),),
        'does not route XI_PAR_REDUCE')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('if (v->op == XI_GO) {', 'if (v->op == XI_GO && false) {'),),
        'contains a statically false conditional branch')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('if (v->op == XI_PAR_MAP || v->op == XI_PAR_REDUCE) {',
          'if ((v->op == XI_PAR_MAP || v->op == XI_PAR_REDUCE) && false) {'),),
        'contains a statically false conditional branch')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('if (v->op == XI_PAR_MAP)', 'if (v->op != XI_PAR_MAP)'),),
        'does not route XI_PAR_MAP')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('        xicgen_go(ctx, out, f, v, prefix);',
          '        #if 0\n'
          '        xicgen_go(ctx, out, f, v, prefix);\n'
          '        #endif'),),
        'contains conditional preprocessing')
    assert not _xi_switch_routes_to_owner(
        'switch (v->op) { case XI_OTHER: { switch (v->op) { '
        'case XI_GO: xicgen_go(); } } }',
        'XI_GO', 'xicgen_go')
    reject_source_mutation(
        'src/aot/xi_cgen_loop_helpers.inc.c',
        (('emit_phi_copies(ctx, out, f, loop.guard, pre_idx);',
          'removed_phi_copies(ctx, out, f, loop.guard, pre_idx);'),),
        'activation census for src/aot/xi_cgen.c::emit_phi_copies differs')
    reject_source_mutation(
        'src/aot/xi_cgen_loop_helpers.inc.c',
        (('emit_phi_copies(ctx, out, f, loop.guard, pre_idx);',
          '#if 0\n'
          '    emit_phi_copies(ctx, out, f, loop.guard, pre_idx);\n'
          '#endif'),),
        'contains conditional preprocessing')
    reject_source_mutation(
        'src/aot/xi_cgen_loop_helpers.inc.c',
        (('emit_phi_copies(ctx, out, f, loop.guard, body_idx);',
          'removed_phi_copies(ctx, out, f, loop.guard, body_idx);'),),
        'activation census for src/aot/xi_cgen.c::emit_phi_copies differs')
    reject_source_mutation(
        'src/aot/xi_cgen_loop_helpers.inc.c',
        (('emit_phi_copies(ctx, out, f, loop.body, entry_idx);',
          'removed_phi_copies(ctx, out, f, loop.body, entry_idx);'),),
        'activation census for src/aot/xi_cgen.c::emit_phi_copies differs')
    reject_source_mutation(
        'src/aot/xi_cgen_loop_helpers.inc.c',
        (('emit_phi_copies(ctx, out, f, loop.body, body_idx);',
          'removed_phi_copies(ctx, out, f, loop.body, body_idx);'),),
        'activation census for src/aot/xi_cgen.c::emit_phi_copies differs')
    reject_source_mutation(
        'src/aot/xi_cgen_loop_helpers.inc.c',
        (('emit_phi_copies(ctx, out, f, loop.guard, pre_idx);',
          'emit_phi_copies(ctx, out, f, loop.guard, body_idx);'),),
        'activation census for src/aot/xi_cgen.c::emit_phi_copies differs')
    reject_source_mutation(
        'src/aot/xi_cgen_loop_helpers.inc.c',
        (('emit_phi_copies(ctx, out, f, loop.guard, pre_idx);',
          'return true;\n    emit_phi_copies(ctx, out, f, loop.guard, pre_idx);'),),
        'activation census for src/aot/xi_cgen.c::emit_phi_copies differs')
    reject_source_mutation(
        'src/aot/xi_cgen_loop_helpers.inc.c',
        (('emit_phi_copies(ctx, out, f, loop.body, entry_idx);',
          'emit_phi_copies(ctx, out, f, loop.body, body_idx);'),),
        'activation census for src/aot/xi_cgen.c::emit_phi_copies differs')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('if (emit_thread_spawn_value_stmt(ctx, out, f, v, prefix, false))',
          'if (v->op == XI_OP_COUNT && '
          'emit_thread_spawn_value_stmt(ctx, out, f, v, prefix, false))'),),
        'guarded activation must be the complete if predicate')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('if (emit_thread_spawn_value_stmt(ctx, out, f, v, prefix, true))',
          'if (v->op == XI_OP_COUNT && '
          'emit_thread_spawn_value_stmt(ctx, out, f, v, prefix, true))'),),
        'guarded activation must be the complete if predicate')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('    emit_declarations(ctx, out, f);\n', ''),),
        'activation census for src/aot/xi_cgen.c::emit_declarations differs')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('    emit_coro_frame_type(ctx, out, f, prefix);\n', ''),),
        'activation census for src/aot/xi_cgen_coro.inc.c::emit_coro_frame_type differs')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('    emit_coro_local_declarations(ctx, out, f);\n', ''),),
        'activation census for src/aot/xi_cgen_coro.inc.c::emit_coro_local_declarations differs')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('            fprintf(out, "    %s%s ",\n'
          '                    cg_value_is_cleanup_live_local_source',
          '            fprintf(stderr, "    %s%s ",\n'
          '                    cg_value_is_cleanup_live_local_source'),),
        'output sequence expected')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('            fprintf(out, "    %s ", cg_coro_decl_ctype',
          '            fprintf(stderr, "    %s ", cg_coro_decl_ctype'),),
        'output sequence expected')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('            fprintf(out, "    %s ", ctype);\n'
          '            emit_phi_ref(ctx, out, phi);',
          '            fprintf(stderr, "    %s ", ctype);\n'
          '            emit_phi_ref(ctx, out, phi);'),),
        'output sequence expected')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('            fprintf(out, "    %s ", ctype);\n'
          '            emit_phi_ref(ctx, out, phi);',
          '            abort();\n'
          '            fprintf(out, "    %s ", ctype);\n'
          '            emit_phi_ref(ctx, out, phi);'),),
        'output sequence expected')
    reject_source_mutation(
        'src/aot/xi_cgen_coro.inc.c',
        (('            fprintf(out, "    %s ", ctype);\n'
          '            emit_phi_ref(ctx, out, phi);',
          '            return;\n'
          '            fprintf(out, "    %s ", ctype);\n'
          '            emit_phi_ref(ctx, out, phi);'),),
        'output sequence expected')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('if (emit_structured_counted_loop_stmt(ctx, out, f, blk, prefix))',
          'if (removed_structured_counted_loop_stmt(ctx, out, f, blk, prefix))'),),
        'activation census for src/aot/xi_cgen_loop_helpers.inc.c::'
        'emit_structured_counted_loop_stmt differs')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('if (emit_structured_array_fill_loop_stmt(ctx, out, f, blk, prefix))',
          'if (removed_structured_array_fill_loop_stmt(ctx, out, f, blk, prefix))'),),
        'activation census for src/aot/xi_cgen_loop_helpers.inc.c::'
        'emit_structured_array_fill_loop_stmt differs')
    reject_source_mutation(
        'src/aot/xi_cgen.c',
        (('emit_phi_incoming_as_rep(ctx, out, phi, pred_idx);',
          'removed_phi_incoming_as_rep(ctx, out, phi, pred_idx);'),),
        'lacks edge-parallel-copy witness token(s): emit_phi_incoming_as_rep')
    print(" PASS", file=sys.stderr)


def _test_xi_verifier_parser():
    print("  test_xi_verifier_parser...", end='', file=sys.stderr)
    ops_text = '''
    (define-xi-op xi.eq
      :class comparison
      :arity 2
      :own-use borrow
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.select
      :class pure
      :arity 3
      :own-use consume
      :effects ()
      :requires ()
      :observable ()
      :targets (vm-bytecode aot-c aot-verify))
    (define-xi-op xi.extract
      :class call
      :arity 1
      :own-use borrow
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
      :scalar-rep yes
      :typed-boundary yes)
    '''
    entries = parse_aot_abi_def(text, reps)
    assert len(entries) == 1
    assert entries[0].ident == 'INT'
    assert entries[0].scalar_rep
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
          :scalar-rep no
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


def _test_target_instruction_parser():
    print("  test_target_instruction_parser...", end='', file=sys.stderr)
    text = '''
    (define-target-instruction const-i64
      :id 1 :name "const.i64" :arity 0 :terminator no
      :result-rep i64 :operand-reps ()
      :result-ownership trivial :operand-ownership ()
      :effects () :error none :suspend no :immediate i64 :control none
      :semantic xi.const :dispatch const :dispatch-arg none)
    (define-target-instruction return-i64
      :id 2 :name "return.i64" :arity 1 :terminator yes
      :result-rep none :operand-reps (i64)
      :result-ownership none :operand-ownership (borrow)
      :effects (control) :error none :suspend no :immediate none :control return
      :semantic none :dispatch return :dispatch-arg none)
    '''
    entries = parse_target_instruction_def(text)
    assert len(entries) == 2
    assert entries[0].stable_id == 1 and entries[1].terminator
    header = generate_target_instruction_header(entries)
    assert 'XR_TARGET_INSTRUCTION_CONST_I64 = 1' in header
    assert 'XR_TARGET_INSTRUCTION_COUNT = 3' in header
    assert 'X(XI_CONST, CONST_I64)' in header
    dispatch = generate_target_vm_ops(entries)
    assert 'XR_VM_OP(CONST_I64, const, CONST, NONE)' in dispatch
    assert 'XR_VM_OP(RETURN_I64, return, RETURN, NONE)' in dispatch
    specialized = parse_target_instruction_def(
        text.replace(':semantic none :dispatch return',
                     ':semantic xi.const :dispatch return'))
    assert specialized[0].semantic == specialized[1].semantic

    wide_text = '\n'.join(
        f'''(define-target-instruction op-{stable_id}
          :id {stable_id} :name "op.{stable_id}" :arity 0 :terminator no
          :result-rep i64 :operand-reps ()
          :result-ownership trivial :operand-ownership ()
          :effects () :error none :suspend no :immediate none :control none
          :semantic none :dispatch const :dispatch-arg none)'''
        for stable_id in range(1, 257))
    assert parse_target_instruction_def(wide_text)[-1].stable_id == 256

    def rejects(source: str):
        try:
            parse_target_instruction_def(source)
            assert False, "invalid target instruction registry should be rejected"
        except SystemExit:
            pass

    rejects(text.replace(':id 2', ':id 3'))
    rejects(text.replace(':arity 1', ':arity 0'))
    rejects(text.replace(':effects (control)', ':effects ()'))
    rejects(text.replace(':error none :suspend no :immediate none :control return',
                         ':error divide-by-zero :suspend no :immediate none :control return'))
    rejects(text.replace(':dispatch return :dispatch-arg none',
                         ':dispatch return :dispatch-arg i64'))
    side_effect = parse_target_instruction_def('''
    (define-target-instruction array-push-tagged
      :id 1 :name "array.push.tagged" :arity 2 :terminator no
      :result-rep none :operand-reps (dyn-value dyn-value)
      :result-ownership none :operand-ownership (borrow consume)
      :effects (may-error memory-write) :error array-push :suspend no
      :immediate call-record :control none
      :semantic none :dispatch array-push :dispatch-arg none)
    ''')[0]
    assert side_effect.result_rep == 'none'
    assert side_effect.operand_ownership == ('borrow', 'consume')
    rejects(text.replace(':operand-ownership (borrow)',
                         ':operand-ownership (borrow consume)'))
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
        'semantic-ops': cmd_semantic_ops,
        'xi-lowering': cmd_xi_lowering,
        'xi-lowering-check': cmd_xi_lowering_check,
        'xi-lowering-validate': cmd_xi_lowering_validate,
        'xi-verify': cmd_xi_verify,
        'aot-rep': cmd_aot_rep,
        'aot-abi': cmd_aot_abi,
        'aot-layout': cmd_aot_layout,
        'aot-c-emission-rules': cmd_aot_c_emission_rules,
        'target-vm-ops': cmd_target_vm_ops,
        'test': cmd_test,
    }

    if cmd not in commands:
        die(f"unknown command '{cmd}'. Available: {', '.join(commands)}")

    commands[cmd](args)


if __name__ == '__main__':
    main()
