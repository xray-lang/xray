"""Fast reporting and case-inventory checks; no consumer validation or builds."""

from __future__ import annotations

import ast
import contextlib
import functools
import io
import json
import unittest
from collections import Counter
from pathlib import Path
from unittest import mock

import c_emission_rules
import xisagen


PREFIX = 'xisagen-self-test: '
CASE_HELPERS = {
    'reject_binding_mutation': 'binding',
    'reject_discovery_source': 'discovery',
    'reject_source_mutation': 'source',
}
EXPECTED_COUNTS = {'binding': 8, 'discovery': 26, 'source': 58}
SUITES = (
    '_test_generated_file_writes', '_test_sexpr_parser', '_test_xi_ops_parser',
    '_test_xi_semantic_ops_parser', '_test_xi_preprocessor_content_cache',
    '_test_xi_lowering_parser', '_test_xi_lowering_build_artifacts',
    '_test_xi_lowering_ninja_failed_edge', '_test_xi_lowering_actual_ninja_edge',
    '_test_xi_verifier_parser', '_test_aot_rep_parser', '_test_aot_abi_parser',
    '_test_aot_layout_parser', 'self_test', '_test_target_instruction_parser',
    '_test_error_paths',
)


def records(output: str) -> list[dict]:
    return [json.loads(line[len(PREFIX):]) for line in output.splitlines()
            if line.startswith(PREFIX)]


def lowering_test() -> ast.FunctionDef:
    tree = ast.parse(Path(xisagen.__file__).read_text(encoding='utf-8'))
    return next(node for node in tree.body
                if isinstance(node, ast.FunctionDef)
                and node.name == '_test_xi_lowering_parser')


def command_suite_names(command: str) -> tuple[str, ...]:
    tree = ast.parse(Path(xisagen.__file__).read_text(encoding='utf-8'))
    function = next(node for node in tree.body
                    if isinstance(node, ast.FunctionDef) and node.name == command)

    suites = None
    for statement in function.body:
        if (isinstance(statement, ast.Assign)
                and any(isinstance(target, ast.Name) and target.id == 'suites'
                        for target in statement.targets)):
            suites = statement.value
        if (isinstance(statement, ast.For) and isinstance(statement.target, ast.Name)
                and statement.target.id == 'suite' and isinstance(statement.iter, ast.Tuple)):
            suites = statement.iter
    assert isinstance(suites, ast.Tuple), command

    def suite_name(expression: ast.expr) -> str:
        if isinstance(expression, ast.Name):
            return expression.id
        if isinstance(expression, ast.Attribute):
            return expression.attr
        assert isinstance(expression, ast.Lambda), ast.dump(expression)
        calls = [node for node in ast.walk(expression)
                 if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)]
        assert len(calls) == 1
        return calls[0].func.id

    return tuple(suite_name(expression) for expression in suites.elts)


def case_call(node: ast.AST) -> bool:
    return (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
            and node.func.id in CASE_HELPERS)


def case_inventory(function: ast.FunctionDef) -> list[tuple[str, str]]:
    """Expand only literal loop inputs; unknown case control flow fails closed."""
    result = []

    def case_id(expression: ast.AST, bindings: dict[str, str]) -> str:
        if isinstance(expression, ast.Constant):
            assert isinstance(expression.value, str)
            return expression.value
        assert isinstance(expression, ast.JoinedStr), ast.dump(expression)
        parts = []
        for part in expression.values:
            if isinstance(part, ast.Constant):
                parts.append(part.value)
            else:
                assert (isinstance(part, ast.FormattedValue)
                        and isinstance(part.value, ast.Name)
                        and part.conversion == -1 and part.format_spec is None)
                parts.append(bindings[part.value.id])
        return ''.join(parts)

    def visit(statements: list[ast.stmt], bindings: dict[str, str]) -> None:
        for node in statements:
            if isinstance(node, ast.FunctionDef):
                if node.name in CASE_HELPERS:
                    assert len(node.decorator_list) == 1
                    decorator = node.decorator_list[0]
                    assert (isinstance(decorator, ast.Call)
                            and isinstance(decorator.func, ast.Attribute)
                            and isinstance(decorator.func.value, ast.Name)
                            and decorator.func.value.id == 'negative_cases'
                            and decorator.func.attr == 'track'
                            and [ast.literal_eval(arg) for arg in decorator.args]
                            == [CASE_HELPERS[node.name]])
                continue
            if isinstance(node, ast.Expr) and case_call(node.value):
                call = node.value
                result.append((CASE_HELPERS[call.func.id],
                               case_id(call.args[0], bindings)))
            elif isinstance(node, ast.With):
                visit(node.body, bindings)
            elif any(case_call(child) for child in ast.walk(node)):
                assert isinstance(node, ast.For), ast.dump(node)
                assert isinstance(node.target, ast.Name) and not node.orelse
                values = ast.literal_eval(node.iter)
                assert isinstance(values, tuple) and values
                for value in values:
                    assert isinstance(value, str)
                    visit(node.body, {**bindings, node.target.id: value})

    visit(function.body, {})
    return result


def assert_inventory(inventory: list[tuple[str, str]]) -> None:
    assert dict(Counter(category for category, _ in inventory)) == EXPECTED_COUNTS
    assert len(inventory) == len(set(inventory)) == 92


class ReportingTests(unittest.TestCase):
    def test_nested_timing_reports_parent_path_without_double_counting(self):
        output = io.StringIO()
        with contextlib.redirect_stderr(output), \
                mock.patch.object(xisagen.time, 'perf_counter',
                                  side_effect=[10.0, 12.0, 15.0, 20.0]):
            with xisagen._xi_self_test_timer('case', 'source/example'):
                with xisagen._xi_self_test_timer('phase', 'prepare'):
                    pass
        start, phase_start, phase_end, end = records(output.getvalue())
        self.assertEqual(start['parents'], [])
        self.assertEqual(phase_start['parents'],
                         [{'kind': 'case', 'name': 'source/example'}])
        self.assertEqual(phase_end['parents'], phase_start['parents'])
        self.assertEqual((phase_end['elapsed_seconds'], phase_end['self_seconds']),
                         (3.0, 3.0))
        self.assertEqual((end['elapsed_seconds'], end['self_seconds']), (10.0, 7.0))
        self.assertEqual(xisagen._XI_SELF_TEST_TIMERS.get(), ())

    def test_phase_calls_keep_negative_diagnostics_and_do_not_cache_rejections(self):
        output = io.StringIO()
        calls = []
        ledger = xisagen._XiSelfTestCaseTimings({'source': 2})

        def validate_xi_lowering_consumer_sources(value, *, path):
            calls.append((value, path))
            print('xisagen: error: exact rejection', file=xisagen.sys.stderr)
            raise SystemExit(1)

        @ledger.track('source')
        def negative(case_id):
            diagnostic = io.StringIO()
            with contextlib.redirect_stderr(diagnostic):
                with self.assertRaises(SystemExit) as caught:
                    xisagen.validate_xi_lowering_consumer_sources(7, path='fixture')
            self.assertEqual(caught.exception.code, 1)
            self.assertEqual(diagnostic.getvalue(), 'xisagen: error: exact rejection\n')

        with mock.patch.object(xisagen, 'validate_xi_lowering_consumer_sources',
                               validate_xi_lowering_consumer_sources), \
                contextlib.redirect_stderr(output):
            with xisagen._xi_self_test_phase_reporting():
                negative('first')
                negative('second')
                ledger.assert_complete()
            self.assertIs(xisagen.validate_xi_lowering_consumer_sources,
                          validate_xi_lowering_consumer_sources)
        self.assertEqual(calls, [(7, 'fixture'), (7, 'fixture')])
        phases = [row for row in records(output.getvalue()) if row['kind'] == 'phase']
        self.assertEqual([row['event'] for row in phases], ['start', 'end'] * 2)
        self.assertEqual([row['parents'][-1]['name'] for row in phases[::2]],
                         ['source/first', 'source/second'])
        self.assertTrue(all(row['status'] == 'failed' and row['exception'] == 'SystemExit'
                            for row in phases[1::2]))
        self.assertEqual(records(output.getvalue())[-1]['status'], 'passed')

    def test_phase_scope_restores_every_callable_after_failure_or_interrupt(self):
        for error in (RuntimeError('exact failure'), KeyboardInterrupt()):
            with self.subTest(error=type(error).__name__):
                original_namespace = dict(vars(xisagen))
                output = io.StringIO()
                try:
                    with contextlib.redirect_stderr(output), \
                            xisagen._xi_self_test_phase_reporting():
                        self.assertIsNot(xisagen.validate_xi_lowering_consumer_sources,
                                         original_namespace[
                                             'validate_xi_lowering_consumer_sources'])
                        with xisagen._xi_self_test_timer('phase', 'failure'):
                            raise error
                except BaseException as caught:
                    self.assertIs(caught, error)
                else:
                    self.fail('phase instrumentation swallowed the failure')
                for name, value in original_namespace.items():
                    if callable(value):
                        self.assertIs(getattr(xisagen, name), value, name)
                self.assertEqual(xisagen._XI_SELF_TEST_TIMERS.get(), ())
                self.assertEqual(records(output.getvalue())[-1]['status'],
                                 'interrupted' if isinstance(error, KeyboardInterrupt)
                                 else 'failed')

    def test_phase_wrapper_preserves_original_cache_and_return_identity(self):
        calls = []
        result = object()

        @functools.lru_cache(maxsize=2)
        def _xi_aot_discovery_functions(value):
            calls.append(value)
            return result

        output = io.StringIO()
        with mock.patch.object(xisagen, '_xi_aot_discovery_functions',
                               _xi_aot_discovery_functions), \
                contextlib.redirect_stderr(output):
            with xisagen._xi_self_test_phase_reporting():
                for _ in range(2):
                    self.assertIs(xisagen._xi_aot_discovery_functions('same'), result)
                self.assertEqual(xisagen._xi_aot_discovery_functions.cache_info(),
                                 _xi_aot_discovery_functions.cache_info())
                self.assertEqual(xisagen._xi_aot_discovery_functions.cache_parameters(),
                                 {'maxsize': 2, 'typed': False})
            self.assertIs(xisagen._xi_aot_discovery_functions, _xi_aot_discovery_functions)
            self.assertIs(xisagen._xi_aot_discovery_functions('same'), result)
        self.assertEqual(calls, ['same'])
        self.assertEqual(_xi_aot_discovery_functions.cache_info().hits, 2)
        self.assertEqual(len(records(output.getvalue())), 4)

    def test_terminal_selector_cache_reuses_only_exact_function_evidence(self):
        owner = xisagen._xi_terminal_selector_routes_for_function
        owner.cache_clear()
        arguments = (
            'if (v->op == XI_ADD) emit(out);',
            frozenset({'XI_ADD'}), frozenset({'emit'}), frozenset(),
            frozenset(), (('XI_ADD', ()),), (('XI_ADD', ()),),
            frozenset({'v'}),
        )
        try:
            with mock.patch.object(xisagen, '_xi_direct_selector_present',
                                   return_value=True) as direct, \
                    mock.patch.object(xisagen, '_xi_hidden_selector_route_present',
                                      return_value=False) as hidden:
                self.assertEqual(owner(*arguments), frozenset({'XI_ADD'}))
                self.assertEqual(owner(*arguments), frozenset({'XI_ADD'}))
                self.assertEqual(direct.call_count, 1)
                self.assertEqual(hidden.call_count, 0)
                changed = ('if (v->op == XI_ADD) { emit(out); }', *arguments[1:])
                self.assertEqual(owner(*changed), frozenset({'XI_ADD'}))
                self.assertEqual(direct.call_count, 2)
                declared = (*arguments[:4], frozenset({'XI_ADD'}), *arguments[5:])
                self.assertEqual(owner(*declared), frozenset())
                self.assertEqual(direct.call_count, 2)
            self.assertEqual(owner.cache_info().hits, 1)
            self.assertEqual(owner.cache_info().misses, 3)
        finally:
            owner.cache_clear()

    def test_real_parser_suite_enables_phase_reporting_without_reordering_cases(self):
        function = lowering_test()
        self.assertEqual(len(function.decorator_list), 1)
        decorator = function.decorator_list[0]
        self.assertIsInstance(decorator, ast.Call)
        self.assertEqual(decorator.func.id, '_xi_self_test_phase_reporting')
        self.assertEqual(decorator.args, [])
        self.assertEqual(xisagen._test_xi_lowering_parser.__wrapped__.__name__,
                         '_test_xi_lowering_parser')
        assert_inventory(case_inventory(function))

    def test_timer_flushes_start_before_body_and_measures_wall_clock(self):
        output = io.StringIO()
        with contextlib.redirect_stderr(output), \
                mock.patch.object(output, 'flush', wraps=output.flush) as flush, \
                mock.patch.object(xisagen.time, 'perf_counter',
                                  side_effect=[10.0, 10.25]):
            with xisagen._xi_self_test_timer('case', 'source/example'):
                self.assertEqual([row['event'] for row in records(output.getvalue())],
                                 ['start'])
                self.assertEqual(flush.call_count, 1)
                print('original diagnostic', file=output)
            self.assertEqual(flush.call_count, 2)
        start, end = records(output.getvalue())
        self.assertEqual(start['schema'], 'xisagen-self-test/1')
        self.assertEqual(end['elapsed_seconds'], 0.25)
        self.assertEqual(end['status'], 'passed')
        self.assertIn('\noriginal diagnostic\n', output.getvalue())

    def test_failure_and_interrupt_keep_original_exception_and_diagnostic(self):
        for error in (AssertionError('original failure'), SystemExit(7),
                      KeyboardInterrupt()):
            with self.subTest(error=type(error).__name__):
                output = io.StringIO()
                with contextlib.redirect_stderr(output):
                    try:
                        with xisagen._xi_self_test_timer('suite', 'failure'):
                            print('exact original diagnostic', file=output)
                            raise error
                    except BaseException as caught:
                        self.assertIs(caught, error)
                    else:
                        self.fail('the reporting wrapper swallowed the failure')
                start, end = records(output.getvalue())
                self.assertEqual(start['event'], 'start')
                self.assertEqual(end['event'], 'end')
                self.assertEqual(end['exception'], type(error).__name__)
                self.assertEqual(end['status'], 'interrupted'
                                 if isinstance(error, KeyboardInterrupt) else 'failed')
                self.assertIn('\nexact original diagnostic\n', output.getvalue())

    def test_manifest_comes_from_successful_invocations_in_execution_order(self):
        ledger = xisagen._XiSelfTestCaseTimings({'source': 2})
        result = object()
        calls = []

        @ledger.track('source')
        def case(name, value):
            calls.append((name, value))
            return value

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            self.assertIs(case('first', result), result)
            case('second', None)
            ledger.assert_complete()
        self.assertEqual(calls, [('first', result), ('second', None)])
        manifest = records(output.getvalue())[-1]
        self.assertEqual(manifest['cases'], ['source/first', 'source/second'])
        self.assertEqual(manifest['counts'], {'source': 2})

    def test_missing_or_duplicate_case_cannot_publish_complete_manifest(self):
        ledger = xisagen._XiSelfTestCaseTimings({'source': 2})
        calls = []

        @ledger.track('source')
        def case(name):
            calls.append(name)

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            case('first')
            with self.assertRaisesRegex(AssertionError, 'duplicate self-test case'):
                case('first')
            with self.assertRaises(AssertionError):
                ledger.assert_complete()
        self.assertEqual(calls, ['first'])
        self.assertFalse(any(row['kind'] == 'case-manifest'
                             for row in records(output.getvalue())))

    def test_fast_manifest_runs_exact_selection_and_names_partial_profile(self):
        selected = frozenset({'binding/one', 'source/three'})
        ledger = xisagen._XiSelfTestCaseTimings(
            {'binding': 2, 'source': 2}, selected)
        calls = []

        @ledger.track('binding')
        def binding(name):
            calls.append('binding/' + name)

        @ledger.track('source')
        def source(name):
            calls.append('source/' + name)

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            binding('one')
            binding('two')
            source('three')
            source('four')
            ledger.assert_complete()
        self.assertEqual(calls, ['binding/one', 'source/three'])
        manifest = records(output.getvalue())[-1]
        self.assertEqual(manifest['profile'], 'fast')
        self.assertEqual(manifest['counts'], {'binding': 1, 'source': 1})
        self.assertEqual(set(manifest['cases']), selected)

    def test_fast_selection_is_a_bounded_cross_category_subset(self):
        inventory = set('/'.join(row) for row in case_inventory(lowering_test()))
        selected = xisagen.XI_FAST_NEGATIVE_CASES
        self.assertTrue(selected < inventory)
        self.assertEqual(len(selected), 4)
        self.assertEqual({name.split('/', 1)[0] for name in selected},
                         {'binding', 'discovery', 'source'})

    def test_exhaustive_suite_inventory_contains_the_complete_fast_inventory(self):
        exhaustive = command_suite_names('cmd_test')
        fast = command_suite_names('cmd_test_fast')
        exhaustive_only = {
            '_test_xi_lowering_ninja_failed_edge',
            '_test_xi_lowering_actual_ninja_edge',
        }
        self.assertEqual(exhaustive, SUITES)
        self.assertEqual(set(fast), set(exhaustive) - exhaustive_only)
        self.assertEqual(len(exhaustive), len(set(exhaustive)))
        self.assertEqual(len(fast), len(set(fast)))
        self.assertEqual(len(exhaustive), 16)
        self.assertEqual(len(fast), 14)

    def test_expensive_discovery_probes_are_exhaustive_only(self):
        exhaustive_labels = set()
        for node in ast.walk(lowering_test()):
            if not isinstance(node, ast.If) or not isinstance(node.test, ast.Compare):
                continue
            compare = node.test
            if not (isinstance(compare.left, ast.Name)
                    and compare.left.id == 'selected_cases'
                    and len(compare.ops) == 1
                    and isinstance(compare.ops[0], ast.Is)
                    and len(compare.comparators) == 1
                    and isinstance(compare.comparators[0], ast.Constant)
                    and compare.comparators[0].value is None):
                continue
            exhaustive_labels.update(
                child.value for statement in node.body for child in ast.walk(statement)
                if isinstance(child, ast.Constant) and isinstance(child.value, str))
        self.assertTrue({
            'plain unrelated discovery source',
            'non-routing selector observation',
            'header-only undeclared owner',
            'plain included header',
        } <= exhaustive_labels)

    def test_failed_case_is_not_completed(self):
        ledger = xisagen._XiSelfTestCaseTimings({'source': 1})

        @ledger.track('source')
        def case(name):
            raise SystemExit(9)

        output = io.StringIO()
        with contextlib.redirect_stderr(output):
            with self.assertRaises(SystemExit) as caught:
                case('failed')
            self.assertEqual(caught.exception.code, 9)
            with self.assertRaisesRegex(AssertionError, 'incomplete self-test case'):
                ledger.assert_complete()
        self.assertEqual(ledger.completed, {})
        self.assertEqual(records(output.getvalue())[-1]['status'], 'failed')

    def test_actual_inventory_expands_53_source_call_sites_to_58_cases(self):
        function = lowering_test()
        inventory = case_inventory(function)
        assert_inventory(inventory)
        source_sites = [node for node in ast.walk(function)
                        if case_call(node) and node.func.id == 'reject_source_mutation']
        self.assertEqual(len(source_sites), 53)
        loop_cases = [name for category, name in inventory
                      if category == 'source' and name.startswith('channel-owner:')]
        self.assertEqual(len(loop_cases), 6)
        self.assertEqual(len(set(loop_cases)), 6)
        ledger_call = next(node for node in ast.walk(function)
                           if isinstance(node, ast.Call)
                           and isinstance(node.func, ast.Name)
                           and node.func.id == '_XiSelfTestCaseTimings')
        self.assertEqual(ast.literal_eval(ledger_call.args[0]), EXPECTED_COUNTS)

    def test_deleting_case_in_any_category_is_detected(self):
        for helper in CASE_HELPERS:
            with self.subTest(helper=helper):
                function = lowering_test()
                removed = False
                for owner in ast.walk(function):
                    body = getattr(owner, 'body', None)
                    if not isinstance(body, list):
                        continue
                    for index, node in enumerate(body):
                        if (isinstance(node, ast.Expr) and case_call(node.value)
                                and node.value.func.id == helper):
                            del body[index]
                            removed = True
                            break
                    if removed:
                        break
                self.assertTrue(removed)
                with self.assertRaises(AssertionError):
                    assert_inventory(case_inventory(function))

    def test_deleting_one_literal_loop_input_is_detected(self):
        function = lowering_test()
        loop = next(node for node in ast.walk(function)
                    if isinstance(node, ast.For)
                    and isinstance(node.target, ast.Name)
                    and node.target.id == 'selector_condition')
        del loop.iter.elts[-1]
        inventory = case_inventory(function)
        self.assertEqual(Counter(category for category, _ in inventory)['source'], 57)
        with self.assertRaises(AssertionError):
            assert_inventory(inventory)

    def test_default_command_runs_all_suites_once_in_original_order(self):
        for fail_at in (None, SUITES[1]):
            with self.subTest(fail_at=fail_at):
                called = []
                output = io.StringIO()
                failure = AssertionError('suite failure')

                def stub(name):
                    def run():
                        called.append(name)
                        if name == fail_at:
                            raise failure
                    run.__name__ = name
                    return run

                with contextlib.ExitStack() as stack:
                    stack.enter_context(contextlib.redirect_stderr(output))
                    for name in SUITES:
                        owner = c_emission_rules if name == 'self_test' else xisagen
                        stack.enter_context(mock.patch.object(owner, name, stub(name)))
                    if fail_at is None:
                        xisagen.cmd_test([])
                    else:
                        with self.assertRaises(AssertionError) as caught:
                            xisagen.cmd_test([])
                        self.assertIs(caught.exception, failure)
                expected = list(SUITES if fail_at is None else SUITES[:2])
                self.assertEqual(called, expected)
                starts = [row['name'] for row in records(output.getvalue())
                          if row['event'] == 'start']
                self.assertEqual(starts, expected)
                self.assertEqual('All xisagen self-tests passed.' in output.getvalue(),
                                 fail_at is None)


if __name__ == '__main__':
    unittest.main()
