#!/usr/bin/env python3
"""Independent exact-wrapper and seeded-graph expectations for Xi consumers."""

from __future__ import annotations

import hashlib
from pathlib import Path
import unittest

import xisagen


VECTORS = (
    ('bare', (('a', 'wrap', 'emit(x);'),), {'emit'}, {'emit', 'wrap'}),
    ('returned', (('a', 'wrap', 'return emit(x);'),), {'emit'}, {'emit', 'wrap'}),
    ('bare_then_return', (('a', 'wrap', 'emit(x); return;'),), {'emit'}, {'emit', 'wrap'}),
    ('returned_then_return', (('a', 'wrap', 'return emit(x); return;'),), {'emit'}, {'emit', 'wrap'}),
    ('nested_argument', (('a', 'wrap', 'emit(foo(x));'),), {'emit'}, {'emit', 'wrap'}),
    ('stop_in_argument', (('a', 'wrap', 'emit(abort());'),), {'emit'}, {'emit'}),
    ('extra_statement', (('a', 'wrap', 'emit(x); x=1;'),), {'emit'}, {'emit'}),
    ('conditional', (('a', 'wrap', 'if(x) { emit(x); }'),), {'emit'}, {'emit'}),
    ('valued_return', (('a', 'wrap', 'emit(x); return true;'),), {'emit'}, {'emit'}),
    ('unknown_call', (('a', 'wrap', 'foreign(x);'),), {'emit'}, {'emit'}),
    ('no_seed_cycle', (('a', 'a', 'b(x);'), ('a', 'b', 'a(x);')), {'emit'}, {'emit'}),
    ('seed_cycle', (('a', 'a', 'b(x);'), ('a', 'b', 'a(x);')), {'a'}, {'a', 'b'}),
    ('two_definitions', (('a', 'wrap', 'foreign(x);'), ('b', 'wrap', 'emit(x);')), {'emit'}, {'emit', 'wrap'}),
    ('comment_prefix', (('a', 'wrap', '/* note */ emit(x);'),), {'emit'}, {'emit'}),
    ('callee_prefix', (('a', 'wrap', 'emit_more(x);'),), {'emit'}, {'emit'}),
    ('inside_semicolon', (('a', 'wrap', 'emit(x;y);'),), {'emit'}, {'emit'}),
    # This helper classifies wrapper spelling, not C grammar. Preserve its
    # existing acceptance; the full parser/consumer verifier owns structure.
    ('unbalanced_existing_spelling', (('a', 'wrap', 'emit((( );'),), {'emit'}, {'emit', 'wrap'}),
    ('empty_seeds', (('a', 'wrap', 'emit(x);'),), set(), set()),
    ('unseeded_self_loop', (('a', 'wrap', 'wrap(x);'),), {'emit'}, {'emit'}),
    ('seeded_self_loop', (('a', 'wrap', 'wrap(x);'),), {'wrap'}, {'wrap'}),
    ('duplicate_stopped_and_live_body', (('a', 'wrap', 'emit(abort());'), ('b', 'wrap', 'emit(x);')), {'emit'}, {'emit', 'wrap'}),
    ('duplicate_stopped_and_unknown_body', (('a', 'wrap', 'emit(abort());'), ('b', 'wrap', 'foreign(x);')), {'emit'}, {'emit'}),
)


class TerminalEmitterClosureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        source = Path(xisagen.__file__).resolve()
        print(f'Xi terminal closure actual generator: {source}')
        print(f'Xi terminal closure generator SHA256: {hashlib.sha256(source.read_bytes()).hexdigest()}')

    def test_declared_96_function_graph(self) -> None:
        # Expected reachability is declared independently: all chain nodes reach
        # emit, the seeded ring is reachable, the unseeded ring is not.
        functions = []
        for index in range(64):
            callee = f'chain{index + 1}' if index < 63 else 'emit'
            functions.append(('chain.c', f'chain{index}', f'return {callee}(x);'))
        for index in range(16):
            functions.append(('isolated.c', f'isolated{index}', f'isolated{(index + 1) % 16}(x);'))
            functions.append(('seeded.c', f'seeded{index}', f'seeded{(index + 1) % 16}(x);'))
        self.assertEqual(96, len(functions))
        expected = {'emit'} | {f'chain{i}' for i in range(64)} | {f'seeded{i}' for i in range(16)}
        actual = xisagen._xi_transitive_terminal_emitters(tuple(functions), {'emit', 'seeded0'}, {'abort'})
        self.assertEqual(expected, actual)


def vector_test(functions, seeds, expected):
    def test(self):
        self.assertEqual(expected, xisagen._xi_transitive_terminal_emitters(functions, seeds, {'abort'}))
    return test


for name, functions, seeds, expected in VECTORS:
    setattr(TerminalEmitterClosureTest, 'test_' + name, vector_test(functions, seeds, expected))


if __name__ == '__main__':
    unittest.main()
