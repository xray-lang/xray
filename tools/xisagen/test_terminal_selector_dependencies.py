"""Independent selector-route expectations and exact-content cache boundaries."""

from __future__ import annotations

import unittest

import xisagen


class TerminalSelectorDependencies(unittest.TestCase):
    def setUp(self):
        xisagen._xi_terminal_selector_routes_for_function.cache_clear()

    def census(self, body, *, predicates=None, factories=None, emitters=None,
               terminators=None, roots=None, declared=None, selectors=None,
               functions=None):
        selectors = {'XI_GO', 'XI_AWAIT'} if selectors is None else selectors
        functions = (('one.c', 'owner', body),) if functions is None else functions
        return xisagen._xi_terminal_selector_census(
            functions, selectors, {'emit_out'} if emitters is None else emitters,
            set() if terminators is None else terminators,
            {} if declared is None else declared,
            {selector: set((predicates or {}).get(selector, ())) for selector in selectors},
            {selector: set((factories or {}).get(selector, ())) for selector in selectors},
            {('one.c', 'owner'): {'v'}} if roots is None else roots)

    def expect_go(self, result):
        self.assertEqual(result, {'XI_GO': {('one.c', 'owner')}, 'XI_AWAIT': set()})

    def expect_none(self, result):
        self.assertEqual(result, {'XI_GO': set(), 'XI_AWAIT': set()})

    def test_literal_selector_independent_golden(self):
        self.expect_go(self.census('if (v->op == XI_GO) { emit_out(out); return; }'))

    def test_predicate_selector_independent_golden(self):
        self.expect_go(self.census('if (is_go(v->op)) { emit_out(out); return; }',
                                  predicates={'XI_GO': {'is_go'}}))

        both = {'XI_GO': {('one.c', 'owner')}, 'XI_AWAIT': {('one.c', 'owner')}}
        body = 'if (is_go(v->op) || is_await(v->op)) { emit_out(out); return; }'
        self.assertEqual(self.census(body, predicates={
            'XI_GO': {'is_go'}, 'XI_AWAIT': {'is_await'}}), both)
        # One helper can own several selector rows and can occur in both
        # predicate and factory maps. Preserve the existing lexical evidence.
        body = 'if (is_shared(v->op)) { emit_out(out); return; }'
        self.assertEqual(self.census(body, predicates={
            'XI_GO': {'is_shared'}, 'XI_AWAIT': {'is_shared'}}), both)
        self.assertEqual(self.census(body, predicates={'XI_GO': {'is_shared'}},
                                    factories={'XI_AWAIT': {'is_shared'}}), both)

    def test_factory_selector_independent_golden(self):
        self.expect_go(self.census('if (v->op == go_op()) { emit_out(out); return; }',
                                  factories={'XI_GO': {'go_op'}}))

    def test_unused_global_dependencies_do_not_invalidate_exact_body(self):
        body = 'if (v->op == XI_GO) { emit_out(out); return; }'
        self.expect_go(self.census(body))
        before = xisagen._xi_terminal_selector_routes_for_function.cache_info()
        self.expect_go(self.census(body, predicates={'XI_GO': {'unrelated_predicate'}},
                                  factories={'XI_AWAIT': {'unrelated_factory'}},
                                  emitters={'emit_out', 'unrelated_emitter'},
                                  terminators={'unrelated_stop'}))
        after = xisagen._xi_terminal_selector_routes_for_function.cache_info()
        self.assertEqual(after.misses, before.misses)
        self.assertEqual(after.hits, before.hits + 1)

    def test_referenced_predicate_addition_rechecks_route(self):
        body = 'if (is_go(v->op)) { emit_out(out); return; }'
        self.expect_none(self.census(body))
        before = xisagen._xi_terminal_selector_routes_for_function.cache_info()
        self.expect_go(self.census(body, predicates={'XI_GO': {'is_go'}}))
        self.assertEqual(xisagen._xi_terminal_selector_routes_for_function.cache_info().misses,
                         before.misses + 1)

    def test_referenced_factory_addition_rechecks_route(self):
        body = 'if (v->op == go_op()) { emit_out(out); return; }'
        self.expect_none(self.census(body))
        self.expect_go(self.census(body, factories={'XI_GO': {'go_op'}}))

    def test_emitter_membership_rechecks_route(self):
        body = 'if (v->op == XI_GO) { emit_out(out); return; }'
        self.expect_none(self.census(body, emitters={'foreign_emitter'}))
        self.expect_go(self.census(body))

    def test_referenced_terminator_prevents_emission(self):
        body = 'if (v->op == XI_GO) { stop_now(); emit_out(out); return; }'
        self.expect_go(self.census(body))
        self.expect_none(self.census(body, terminators={'stop_now'}))

    def test_helper_prefix_unknown_and_parenthesized_callee_are_not_aliases(self):
        for body in (
            'if (unknown_is_go(v->op)) { emit_out(out); return; }',
            'if (is_go_more(v->op)) { emit_out(out); return; }',
            'if ((is_go)(v->op)) { emit_out(out); return; }',
        ):
            with self.subTest(body=body):
                self.expect_none(self.census(body, predicates={'XI_GO': {'is_go'}}))

    def test_nested_predicate_argument_still_requires_root_value(self):
        body = 'if (is_go(other(v->op))) { emit_out(out); return; }'
        self.expect_none(self.census(body, predicates={'XI_GO': {'is_go'}}))

    def test_existing_qualified_call_spelling_is_preserved(self):
        # This lexical helper already sees a literal callee after a dot. The
        # full source consumer proof owns definition/census/permission checks.
        body = 'if (object.is_go(v->op)) { emit_out(out); return; }'
        self.expect_go(self.census(body, predicates={'XI_GO': {'is_go'}}))

    def test_root_parameter_evidence_rechecks_predicate(self):
        body = 'if (is_go(v->op)) { emit_out(out); return; }'
        self.expect_none(self.census(body, predicates={'XI_GO': {'is_go'}},
                                    roots={('one.c', 'owner'): {'other_value'}}))
        self.expect_go(self.census(body, predicates={'XI_GO': {'is_go'}}))

    def test_current_declared_route_is_not_added_to_selector_census(self):
        body = 'if (v->op == XI_GO) { emit_out(out); return; }'
        self.expect_go(self.census(body))
        self.expect_none(self.census(body, declared={'XI_GO': {('one.c', 'owner')}}))

    def test_changed_body_and_selector_set_have_independent_results(self):
        self.expect_go(self.census('if (v->op == XI_GO) { emit_out(out); return; }'))
        self.assertEqual(self.census('if (v->op == XI_AWAIT) { emit_out(out); return; }'),
                         {'XI_GO': set(), 'XI_AWAIT': {('one.c', 'owner')}})
        self.assertEqual(self.census('if (v->op == XI_GO) { emit_out(out); return; }',
                                     selectors={'XI_AWAIT'}), {'XI_AWAIT': set()})

    def test_duplicate_symbol_bodies_keep_all_source_routes(self):
        functions = (
            ('one.c', 'owner', 'if (v->op == XI_GO) { emit_out(out); return; }'),
            ('two.c', 'owner', 'if (v->op == XI_AWAIT) { emit_out(out); return; }'),
            ('three.c', 'owner', 'foreign(out);'),
        )
        self.assertEqual(self.census('', functions=functions),
                         {'XI_GO': {('one.c', 'owner')}, 'XI_AWAIT': {('two.c', 'owner')}})

    def test_false_gate_return_before_emission_and_foreign_output_stay_rejected(self):
        for body in (
            'if (false) { if (v->op == XI_GO) { emit_out(out); return; } }',
            'if (v->op == XI_GO) { return; emit_out(out); return; }',
            'if (v->op == XI_GO) { foreign(out); return; }',
        ):
            with self.subTest(body=body):
                self.expect_none(self.census(body))

    def test_parameter_cache_retains_current_census_scale_and_exact_content(self):
        parser = xisagen._xi_c_file_scope_function_value_parameters
        parser.cache_clear()
        sources = [f'static void probe_{i}(const XiValue *value_{i}) {{}}' for i in range(300)]
        for i, source in enumerate(sources):
            self.assertEqual(parser(source), {f'probe_{i}': {f'value_{i}'}})
        first = parser.cache_info()
        for i, source in enumerate(sources):
            self.assertEqual(parser(source), {f'probe_{i}': {f'value_{i}'}})
        second = parser.cache_info()
        self.assertEqual(second.misses, first.misses)
        self.assertEqual(second.hits, first.hits + 300)
        self.assertLessEqual(second.maxsize, 512)
        self.assertEqual(parser('static void probe_0(const XiValue *changed) {}'),
                         {'probe_0': {'changed'}})


class FunctionDefinitionFacts(unittest.TestCase):
    def setUp(self):
        xisagen._xi_function_definition_facts.cache_clear()

    def require(self, text, symbol='owner', path='one.c', context='first'):
        from pathlib import Path
        return xisagen._xi_require_function_body(Path('unused-root'), path,
                                                 symbol, context, {path: text})

    def error(self, text, expected, **kwargs):
        import contextlib
        import io
        error = io.StringIO()
        with contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as stopped:
            self.require(text, **kwargs)
        self.assertEqual(stopped.exception.code, 1)
        self.assertEqual(error.getvalue(), 'xisagen: error: ' + expected + '\n')

    def test_same_bytes_different_path_and_context(self):
        text = 'static int owner(void) { return 7; }'
        self.assertEqual(self.require(text), ' return 7; ')
        self.assertEqual(self.require(text, path='two.c', context='second'), ' return 7; ')
        info = xisagen._xi_function_definition_facts.cache_info()
        self.assertEqual((info.misses, info.hits), (1, 1))

    def test_error_facts_keep_current_context_and_path(self):
        text = 'static int other(void) { return 7; }'
        self.error(text, 'first: expected one file-scope definition of one.c::owner, found 0')
        self.error(text, 'second: expected one file-scope definition of two.c::owner, found 0',
                   path='two.c', context='second')
        self.assertEqual(xisagen._xi_function_definition_facts.cache_info().hits, 1)

    def test_current_capture_membership_is_never_cached(self):
        from pathlib import Path
        import contextlib
        import io
        text = 'static int owner(void) { return 7; }'
        self.require(text)
        error = io.StringIO()
        with contextlib.redirect_stderr(error), self.assertRaises(SystemExit):
            xisagen._xi_require_function_body(Path('unused-root'), 'one.c', 'owner',
                                              'later', {'other.c': text})
        self.assertEqual(error.getvalue(), 'xisagen: error: later: consumer source is outside '
                         'the validated xi_cgen.c translation unit: one.c\n')
        self.assertEqual(xisagen._xi_function_definition_facts.cache_info().hits, 0)

    def test_changed_body_invalidates_exact_text(self):
        self.assertEqual(self.require('int owner(void) { return 7; }'), ' return 7; ')
        self.assertEqual(self.require('int owner(void) { return 9; }'), ' return 9; ')
        self.assertEqual(xisagen._xi_function_definition_facts.cache_info().misses, 2)

    def test_symbol_is_part_of_exact_key(self):
        text = 'int owner(void) { return 7; } int other(void) { return 9; }'
        self.assertEqual(self.require(text), ' return 7; ')
        self.assertEqual(self.require(text, symbol='other'), ' return 9; ')

    def test_duplicate_definitions_reject(self):
        self.error('int owner(void) {} int owner(void) {}',
                   'first: expected one file-scope definition of one.c::owner, found 2')

    def test_missing_definition_reject(self):
        self.error('int owner(void);',
                   'first: expected one file-scope definition of one.c::owner, found 0')

    def test_enclosed_conditional_reject(self):
        self.error('#if FEATURE\nint owner(void) {}\n#endif\n',
                   'first: governed consumer function one.c::owner is enclosed by conditional preprocessing')

    def test_enclosed_if_false_reject(self):
        self.error('#if 0\nint owner(void) {}\n#endif\n',
                   'first: governed consumer function one.c::owner is enclosed by conditional preprocessing')

    def test_internal_conditional_reject(self):
        self.error('int owner(void) {\n#if FEATURE\nreturn 7;\n#endif\n}',
                   'first: governed consumer function one.c::owner contains conditional preprocessing')

    def test_static_false_gate_reject(self):
        for condition in ('false', '0', 'ready && false', '((0)) && ready'):
            with self.subTest(condition=condition):
                self.error('int owner(void) { if (' + condition + ') { return 7; } }',
                           'first: governed consumer function one.c::owner contains a statically false conditional branch')

    def test_literals_and_comments_do_not_fabricate_directives(self):
        body = self.require('int owner(void) { /* #if 0 */ const char *s = "if(false)"; return 7; }')
        self.assertIn('return 7;', body)
        self.assertNotIn('false', body)

    def test_cache_is_bounded_without_skipping_new_inputs(self):
        cache = xisagen._xi_function_definition_facts
        self.assertEqual(cache.cache_info().maxsize, 1024)
        for index in range(1050):
            body = self.require('int owner(void) { return ' + str(index) + '; }')
            self.assertEqual(body, ' return ' + str(index) + '; ')
        self.assertEqual(cache.cache_info().currsize, 1024)
        self.assertEqual(cache.cache_info().misses, 1050)
        self.assertEqual(self.require('int owner(void) { return 0; }'), ' return 0; ')
        self.assertEqual(cache.cache_info().misses, 1051)


class GovernedAliasFacts(unittest.TestCase):
    def setUp(self):
        for cache in (xisagen._xi_governed_alias_file_issue,
                      xisagen._xi_governed_preprocessor_facts,
                      xisagen._xi_behavioral_function_symbols):
            cache.cache_clear()

    def issue(self, text, selectors=('XI_GO',), symbols=('owner',), calls=(),
              ranges=(), passthrough=()):
        return xisagen._xi_governed_alias_file_issue(text, selectors, symbols,
                                                   calls, ranges, passthrough)

    def validate(self, sources, context='first', predicates=None, passthrough=None):
        binding = xisagen.XiConsumerBinding('one.c', 'owner', 'direct')
        entry = xisagen.XiLoweringDef('xi.go', 'GO', [], [], {}, {}, {},
                                      {'aot-c': [binding]})
        return xisagen._xi_validate_governed_token_aliases(
            [entry], sources, context, predicates or {}, passthrough or set())

    def error(self, sources, expected, **kwargs):
        import contextlib
        import io
        error = io.StringIO()
        with contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as stopped:
            self.validate(sources, **kwargs)
        self.assertEqual(stopped.exception.code, 1)
        self.assertEqual(error.getvalue(), 'xisagen: error: ' + expected + '\n')

    def test_selector_policy_is_complete_key(self):
        text = 'XiOp alias = XI_GO;'
        self.assertEqual(self.issue(text), ('initializer-alias', 1, 'XI_GO'))
        self.assertIsNone(self.issue(text, selectors=('XI_AWAIT',)))
        self.assertEqual(self.issue(text), ('initializer-alias', 1, 'XI_GO'))
        self.assertEqual(xisagen._xi_governed_alias_file_issue.cache_info().misses, 2)

    def test_symbol_policy_is_complete_key(self):
        text = 'void *p = &owner;'
        self.assertIsNone(self.issue(text, symbols=('other',)))
        self.assertEqual(self.issue(text), ('function-alias', 0, 'owner'))

    def test_behavioral_call_policy_is_complete_key(self):
        text = 'void f(void) { APPLY(XI_GO); }'
        self.assertIsNone(self.issue(text))
        self.assertEqual(self.issue(text, calls=('APPLY',)), ('macro-argument', 0, 'XI_GO'))

    def test_predicate_ranges_are_complete_key(self):
        text = 'XiOp pred(void) { XiOp alias = XI_GO; return alias; }'
        self.assertEqual(self.issue(text), ('initializer-alias', 1, 'XI_GO'))
        self.assertIsNone(self.issue(text, ranges=(('XI_GO', ((18, 52),)),)))
        self.assertEqual(self.issue(text, ranges=(('XI_GO', ((0, 10),)),)),
                         ('initializer-alias', 1, 'XI_GO'))
        self.validate({'one.c': text}, predicates={'XI_GO': {'pred'}})

    def test_passthrough_helper_changes_assignment_exclusion(self):
        text = 'int value = observe(XI_GO);'
        self.assertIsNone(self.issue(text))
        self.assertEqual(self.issue(text, passthrough=('observe',)),
                         ('initializer-alias', 1, 'XI_GO'))

    def test_pointer_alias_and_designated_field_independent_goldens(self):
        self.assertEqual(self.issue('const XiOp *p = &XI_GO;'),
                         ('initializer-alias', 1, 'XI_GO'))
        self.assertIsNone(self.issue('XiValue v = { .op = XI_GO };'))
        self.assertIsNone(self.issue('if (v->op == XI_GO) owner(v);'))

    def test_generated_projection_role_is_complete_key(self):
        text = '#define OP XI_GO\n'
        self.assertEqual(xisagen._xi_governed_preprocessor_facts(text, ('XI_GO',), False),
                         ((('OP', 'XI_GO'),), ('directive', 'XI_GO')))
        self.assertEqual(xisagen._xi_governed_preprocessor_facts(text, ('XI_GO',), True),
                         ((('OP', 'XI_GO'),), None))
        self.validate({'src/ir/xi_ops_gen.h': text})
        self.error({'ordinary.h': text}, 'first: governed token appears in preprocessing '
                   'directive in ordinary.h: XI_GO')

    def test_preprocessing_error_keeps_current_context_path(self):
        text = '/* unterminated'
        self.error({'one.c': text}, 'first: one.c: unterminated preprocessing comment')
        self.error({'two.c': text}, 'second: two.c: unterminated preprocessing comment',
                   context='second')

    def test_cached_alias_fact_keeps_current_context_path(self):
        text = 'void *p = &owner;'
        self.error({'one.c': text}, 'first: governed function token is used through an alias in one.c: owner')
        self.error({'two.c': text}, 'second: governed function token is used through an alias in two.c: owner',
                   context='second')

    def test_current_source_membership_is_fully_revisited(self):
        self.validate({'one.c': 'int noop;'} )
        self.error({'one.c': 'int noop;', 'new.c': 'XiOp alias = XI_GO;'},
                   'first: governed selector is used through an initializer alias in new.c:1: XI_GO')

    def test_macro_argument_through_global_behavioral_closure(self):
        self.error({'one.c': '#define APPLY(v) ((v)->op)\n'
                             '#define FORWARD(v) APPLY(v)\nvoid f(void) { FORWARD(XI_GO); }'},
                   'first: governed selector is hidden in a macro argument in one.c: XI_GO')

    def test_macro_definitions_keep_sorted_file_last_wins(self):
        sources = {'a.c': '#define APPLY(v) ((v)->op)\n',
                   'b.c': '#define APPLY(v) (v)\n',
                   'c.c': 'void f(void) { APPLY(XI_GO); }'}
        self.validate(sources)
        self.error({'b.c': sources['a.c'], 'a.c': sources['b.c'], 'c.c': sources['c.c']},
                   'first: governed selector is hidden in a macro argument in c.c: XI_GO')

    def test_behavioral_function_policy_and_macro_argument(self):
        text = 'void route(XiValue *v) { owner(v->op); }'
        self.assertEqual(xisagen._xi_behavioral_function_symbols(text, ('owner',)), frozenset({'route'}))
        self.assertEqual(xisagen._xi_behavioral_function_symbols(text, ('other',)), frozenset())
        self.error({'a.c': text, 'b.c': 'void f(void) { route(XI_GO); }'},
                   'first: governed selector is hidden in a macro argument in b.c: XI_GO')

    def test_unmatched_behavioral_call_rejects(self):
        self.assertEqual(self.issue('APPLY(XI_GO', calls=('APPLY',)),
                         ('unmatched-call', 0, ''))

    def test_changed_text_and_line_preserve_error_identity(self):
        self.assertEqual(self.issue('XiOp alias = XI_GO;'), ('initializer-alias', 1, 'XI_GO'))
        self.assertEqual(self.issue('\nXiOp alias = XI_GO;'), ('initializer-alias', 2, 'XI_GO'))

    def test_empty_predicate_exclusion_does_not_hide_other_tokens(self):
        text = 'void *p = &owner; XiOp pred(void) { return XI_GO; }'
        self.assertEqual(self.issue(text, ranges=(('XI_GO', ((0, 100),)),)),
                         ('function-alias', 0, 'owner'))

    def test_all_pure_caches_have_finite_bounds_and_validate_after_eviction(self):
        caches = (xisagen._xi_governed_alias_file_issue,
                  xisagen._xi_governed_preprocessor_facts,
                  xisagen._xi_behavioral_function_symbols)
        for cache in caches:
            self.assertEqual(cache.cache_info().maxsize, 1024)
        for index in range(1050):
            text = 'int noop_' + str(index) + ';'
            self.assertIsNone(self.issue(text))
            self.assertEqual(xisagen._xi_governed_preprocessor_facts(text, ('XI_GO',), False), ((), None))
            self.assertEqual(xisagen._xi_behavioral_function_symbols(text, ('owner',)), frozenset())
        for cache in caches:
            self.assertEqual(cache.cache_info().currsize, 1024)
            self.assertEqual(cache.cache_info().misses, 1050)
        self.assertIsNone(self.issue('int noop_0;'))
        self.assertEqual(xisagen._xi_governed_alias_file_issue.cache_info().misses, 1051)


class IfAndNoreturnFacts(unittest.TestCase):
    def setUp(self):
        xisagen._xi_if_statement_facts.cache_clear()
        xisagen._xi_noreturn_file_symbols.cache_clear()

    def test_if_exact_independent_record(self):
        self.assertEqual(xisagen._xi_if_statement_records('if (x) { a(); } else { b(); }'),
                         [(0, 29, 'x', ' a(); ', ' b(); ')])

    def test_if_top_level_policy_changes_exact_key(self):
        text = 'if(a){if(b){go();}}'
        self.assertEqual(xisagen._xi_if_statement_records(text),
                         [(0, 19, 'a', 'if(b){go();}', ''), (6, 18, 'b', 'go();', '')])
        self.assertEqual(xisagen._xi_if_statement_records(text, top_level_only=True),
                         [(0, 19, 'a', 'if(b){go();}', '')])
        self.assertEqual(xisagen._xi_if_statement_facts.cache_info().misses, 2)

    def test_if_changed_text_revalidates(self):
        self.assertEqual(xisagen._xi_if_statement_records('if(x){a();}'),
                         [(0, 11, 'x', 'a();', '')])
        self.assertEqual(xisagen._xi_if_statement_records('if(y){b();}'),
                         [(0, 11, 'y', 'b();', '')])

    def test_if_fresh_list_cannot_corrupt_cached_facts(self):
        text = 'if(x){a();}'
        first = xisagen._xi_if_statement_records(text)
        first.append((99, 100, 'bad', 'bad', 'bad'))
        first[0] = (0, 0, 'changed', '', '')
        self.assertEqual(xisagen._xi_if_statement_records(text), [(0, 11, 'x', 'a();', '')])
        self.assertIsInstance(xisagen._xi_if_statement_facts(text, top_level_only=False), tuple)

    def test_if_same_bytes_hit_without_path_authority(self):
        text = 'if(x){a();}'
        one = xisagen._xi_if_statement_records(text)
        two = xisagen._xi_if_statement_records(text)
        self.assertEqual(one, two)
        self.assertIsNot(one, two)
        self.assertEqual(xisagen._xi_if_statement_facts.cache_info().hits, 1)

    def test_if_bound_and_evicted_input_revalidation(self):
        for index in range(540):
            text = 'if (v' + str(index) + ') { go(); }'
            self.assertEqual(xisagen._xi_if_statement_records(text)[0][2], 'v' + str(index))
        info = xisagen._xi_if_statement_facts.cache_info()
        self.assertEqual((info.maxsize, info.currsize, info.misses), (512, 512, 540))
        self.assertEqual(xisagen._xi_if_statement_records('if (v0) { go(); }')[0][2], 'v0')
        self.assertEqual(xisagen._xi_if_statement_facts.cache_info().misses, 541)

    def test_if_malformed_delimiters_keep_original_empty_result(self):
        self.assertEqual(xisagen._xi_if_statement_records('if (x { go(); }'), [])
        self.assertEqual(xisagen._xi_if_statement_records('if(x){go();'), [])

    def test_if_single_statement_else_independent_record(self):
        self.assertEqual(xisagen._xi_if_statement_records('if(x) a(); else b();'),
                         [(0, 20, 'x', 'a();', 'b();')])

    def test_if_branch_text_is_immutable_with_cache_reuse(self):
        body = 'if (x) { a(); } else { b(); }'
        self.assertEqual(xisagen._xi_if_statements(body), [('x', ' a(); ', ' b(); ')])
        self.assertEqual(xisagen._xi_if_statements(body, top_level_only=True),
                         [('x', ' a(); ', ' b(); ')])

    def test_noreturn_explicit_independent_symbols(self):
        self.assertEqual(xisagen._xi_noreturn_file_symbols(
            '_Noreturn void halt(void);\nXR_NORETURN int fatal(void);\nvoid plain(void);'),
            frozenset({'halt', 'fatal'}))

    def test_noreturn_outer_retains_all_builtin_seeds(self):
        self.assertEqual(xisagen.xi_lowering_noreturn_symbols({}),
                         {'abort', 'exit', '_Exit', 'quick_exit', '__builtin_trap', '__builtin_unreachable'})

    def test_noreturn_changed_text_revalidates(self):
        self.assertEqual(xisagen._xi_noreturn_file_symbols('XR_NORETURN void old(void);'),
                         frozenset({'old'}))
        self.assertEqual(xisagen._xi_noreturn_file_symbols('XR_NORETURN void current(void);'),
                         frozenset({'current'}))

    def test_noreturn_facts_are_immutable_outer_set_is_fresh(self):
        sources = {'a.c': 'XR_NORETURN void fatal(void);'}
        first = xisagen.xi_lowering_noreturn_symbols(sources)
        first.discard('fatal')
        first.add('fabricated')
        second = xisagen.xi_lowering_noreturn_symbols(sources)
        self.assertIn('fatal', second)
        self.assertNotIn('fabricated', second)
        self.assertIsInstance(xisagen._xi_noreturn_file_symbols(sources['a.c']), frozenset)

    def test_noreturn_bound_and_evicted_input_revalidation(self):
        for index in range(1050):
            self.assertEqual(xisagen._xi_noreturn_file_symbols('XR_NORETURN void stop_' + str(index) + '(void);'),
                             frozenset({'stop_' + str(index)}))
        info = xisagen._xi_noreturn_file_symbols.cache_info()
        self.assertEqual((info.maxsize, info.currsize, info.misses), (1024, 1024, 1050))
        self.assertEqual(xisagen._xi_noreturn_file_symbols('XR_NORETURN void stop_0(void);'), frozenset({'stop_0'}))
        self.assertEqual(xisagen._xi_noreturn_file_symbols.cache_info().misses, 1051)

    def test_noreturn_comments_literals_do_not_invent_declarations(self):
        self.assertEqual(xisagen._xi_noreturn_file_symbols(
            '/* XR_NORETURN void fake(void); */\nconst char *s = "XR_NORETURN void fake(void);";\n'
            'XR_NORETURN void real(void);'), frozenset({'real'}))

    def test_noreturn_outer_current_membership_union_is_rebuilt(self):
        seeds = {'abort', 'exit', '_Exit', 'quick_exit', '__builtin_trap', '__builtin_unreachable'}
        self.assertEqual(xisagen.xi_lowering_noreturn_symbols({'a.c': 'XR_NORETURN void old(void);'}), seeds | {'old'})
        self.assertEqual(xisagen.xi_lowering_noreturn_symbols({'b.c': 'XR_NORETURN void new(void);'}), seeds | {'new'})


class LiteralCallFacts(unittest.TestCase):
    def setUp(self):
        xisagen._xi_c_literal_call_identifiers.cache_clear()
        xisagen._xi_c_call_record_facts.cache_clear()

    def test_independent_offsets_and_nested_normalized_arguments(self):
        self.assertEqual(xisagen._xi_c_call_records('pre(); call(a, (b, c), arr[2]); post();', 'call'),
                         [(7, 30, ('a', '(b,c)', 'arr[2]'))])

    def test_duplicate_calls_keep_independent_offsets(self):
        self.assertEqual(xisagen._xi_c_call_records('call(a); call(b);', 'call'),
                         [(0, 7, ('a',)), (9, 16, ('b',))])

    def test_bad_delimiter_remains_candidate_then_original_empty_fact(self):
        self.assertEqual(xisagen._xi_c_literal_call_identifiers('call(a'), frozenset({'call'}))
        self.assertEqual(xisagen._xi_c_call_records('call(a', 'call'), [])
        self.assertEqual(xisagen._xi_c_call_record_facts.cache_info().misses, 1)

    def test_raw_comments_and_literals_retain_original_lexical_matches(self):
        self.assertEqual(xisagen._xi_c_call_records('/*call(a)*/ "call(b)"', 'call'),
                         [(2, 9, ('a',)), (13, 20, ('b',))])

    def test_word_boundaries_prefixes_and_qualified_calls(self):
        text = 'pi_call(a); call_more(a); πcall(a); callπ(a); call(a); obj.call(b);'
        self.assertEqual(xisagen._xi_c_literal_call_identifiers(text),
                         frozenset({'pi_call', 'call_more', 'call'}))
        records = xisagen._xi_c_call_records(text, 'call')
        self.assertEqual([record[2] for record in records], [('a',), ('b',)])

    def test_exact_symbol_and_body_both_change_facts(self):
        text = 'one(a); two(b);'
        self.assertEqual(xisagen._xi_c_call_records(text, 'one'), [(0, 6, ('a',))])
        self.assertEqual(xisagen._xi_c_call_records(text, 'two'), [(8, 14, ('b',))])
        self.assertEqual(xisagen._xi_c_call_records('one(c);', 'one'), [(0, 6, ('c',))])
        self.assertEqual(xisagen._xi_c_call_record_facts.cache_info().misses, 3)

    def test_absent_symbol_never_populates_record_cache(self):
        self.assertEqual(xisagen._xi_c_call_records('one(a);', 'missing'), [])
        self.assertEqual(xisagen._xi_c_call_record_facts.cache_info().currsize, 0)
        self.assertEqual(xisagen._xi_c_literal_call_identifiers.cache_info().currsize, 1)

    def test_fresh_list_mutation_cannot_corrupt_cached_records(self):
        first = xisagen._xi_c_call_records('call(a);', 'call')
        first.append((9, 10, ('fabricated',)))
        first[0] = (1, 2, ('changed',))
        self.assertEqual(xisagen._xi_c_call_records('call(a);', 'call'), [(0, 7, ('a',))])
        self.assertIsInstance(xisagen._xi_c_call_record_facts('call(a);', 'call'), tuple)

    def test_same_content_hits_exact_cache(self):
        one = xisagen._xi_c_call_records('call(a);', 'call')
        two = xisagen._xi_c_call_records('call(a);', 'call')
        self.assertEqual(one, two)
        self.assertIsNot(one, two)
        self.assertEqual(xisagen._xi_c_call_record_facts.cache_info().hits, 1)

    def test_literal_identifier_cache_bound_and_eviction(self):
        for index in range(8200):
            self.assertEqual(xisagen._xi_c_literal_call_identifiers('body_' + str(index) + '();'),
                             frozenset({'body_' + str(index)}))
        info = xisagen._xi_c_literal_call_identifiers.cache_info()
        self.assertEqual((info.maxsize, info.currsize, info.misses), (8192, 8192, 8200))
        self.assertEqual(xisagen._xi_c_literal_call_identifiers('body_0();'), frozenset({'body_0'}))
        self.assertEqual(xisagen._xi_c_literal_call_identifiers.cache_info().misses, 8201)

    def test_matching_record_cache_bound_and_eviction(self):
        for index in range(4110):
            self.assertEqual(xisagen._xi_c_call_records('call(v' + str(index) + ');', 'call')[0][2],
                             ('v' + str(index),))
        info = xisagen._xi_c_call_record_facts.cache_info()
        self.assertEqual((info.maxsize, info.currsize, info.misses), (4096, 4096, 4110))
        self.assertEqual(xisagen._xi_c_call_records('call(v0);', 'call'), [(0, 8, ('v0',))])
        self.assertEqual(xisagen._xi_c_call_record_facts.cache_info().misses, 4111)

    def test_non_ascii_or_non_identifier_symbols_keep_original_literal_regex(self):
        text = 'føø(a); a.bar(b);'
        self.assertEqual(xisagen._xi_c_call_records(text, 'føø'), [(0, 6, ('a',))])
        self.assertEqual(xisagen._xi_c_call_records(text, '.bar'), [(9, 16, ('b',))])
        self.assertEqual(xisagen._xi_c_call_records('3call(a);', '3call'), [(0, 8, ('a',))])

    def test_empty_arguments_remain_empty_tuple(self):
        self.assertEqual(xisagen._xi_c_call_records('call();', 'call'), [(0, 6, ())])


if __name__ == '__main__':
    unittest.main()
