"""Derive complete function and closure traces without executing Xray."""
from pathlib import Path
import hashlib
import math

ROOT = Path(__file__).resolve().parent.parents[3]
_UNIT_NAME = 'known_empty_call_result_is_null'
_UNIT_ORIGINAL = 'tests/diff/cases/semantics/optimizer/known_empty_call_result_is_null.xr'
_UNIT_PORT = 'tests/unit/xir/legacy_consumers/fixtures/unit_call_result/root.xr'
_INPUTS = {'tests/diff/cases/semantics/functions/unproven_callee_targets.xr': '559ec30a96887af5d4e77a632fb9ef66c01fa6422d0ca3c4f50b2e6a8a428511',
 'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/008-7-scoping-and-name-resolution.md': 'ec46ac53d1f2886e2ff2a5ebf3d5b7bd7e7d99ccf472b8ad64ca4b75f8370fc7',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'tests/diff/cases/semantics/closure/call_through_function_parameter.xr': 'fd1d6a3ece85dbcf321f07ae7a37fa36574c90cbf7af6d7a99e5b07a6bea8503',
 'tests/diff/cases/semantics/closure/capture_forms.xr': 'a87b6b956ac33c59f4bc2925356d9fd96d879cdbeaa994ad126363914ec12fd2',
 'tests/diff/cases/semantics/closure/nested_closure_transitive_capture.xr': '675041c66eca5eafe298c8baf4162b60b3090e03ef5bb57b7e088e8a36726de3',
 'tests/diff/cases/semantics/functions/bare_lambda_expression_positions.xr': 'f7d6bab8c24719bc38954f9be9666faa6c6538323ff47429f9a5caecca721195',
 'tests/diff/cases/semantics/functions/default_param_as_function_value.xr': '55af015a0add1e824153154b5a3ba475ca2fc435d3ee3d3cfb9af1c8f8da23bc',
 'tests/diff/cases/semantics/functions/function_type_surface.xr': 'b240e18969f3538174716b388711df2374b68ad3a49db8fea893f6c7aa178fd1',
 'tests/diff/cases/semantics/functions/lambda_forms.xr': '93ec3e0ad71e110037f25ec69baf1902f5c39f3f909dfb30c0f235ffb569ed54',
 'tests/diff/cases/semantics/functions/lambda_inference_sources.xr': '0d9cd4536d24f3591f25cbdb56028e4e2b89cd4fce1fb17bff2d4feb01f5a6c2',
 'tests/diff/cases/semantics/functions/named_function_forms.xr': '0d7c70776f99918a422dc448705dacd406e1b143523ada6e7f4ea5f55119cbf8',
 'tests/diff/cases/semantics/functions/parenthesized_function_type.xr': 'de500919254f78d0d95c57e941b79e90b8915a6cfb97c90ad61209f4c962c6f4',
 'tests/diff/cases/semantics/functions/string_result_direct_call.xr': 'ff9fb714fd5a6be8d877a96be93d1b5a2c72ca7e1df16da21711ee931bad77f9',
 'tests/diff/cases/semantics/functions/unit_return_expression.xr': '4b81fbb553915041991acedf1aeeb839106aed96534178019fddb0a0a8a6aace',
 'tests/diff/cases/semantics/functions/unit_value_rendering.xr': '124a25aebe1c994719ac358c459883f16bb65f2e7ca40855d68d9458a22c6a33',
 'tests/unit/xir/legacy_consumers/fixtures/callable_explicit_fill/root.xr': '1a26a78e2aca830df18cde987175eb16ae6d0f31e684976a896ce8f0b3d2cb92'}
_INPUTS[_UNIT_ORIGINAL] = '130cdccab3b462b0b294870f5de4d18dd29675d080d275debd8d0b570e1e8865'
_INPUTS[_UNIT_PORT] = 'dc9fe5d9f7a3c802bab14cdd06f9122bbb48a2527e4b58420d3a453838a4db9b'
_REQUIRED_INPUTS = (_UNIT_ORIGINAL, _UNIT_PORT, 'tests/diff/cases/semantics/functions/unproven_callee_targets.xr',
 'spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/008-7-scoping-and-name-resolution.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'tests/diff/cases/semantics/closure/call_through_function_parameter.xr',
 'tests/diff/cases/semantics/closure/capture_forms.xr',
 'tests/diff/cases/semantics/closure/nested_closure_transitive_capture.xr',
 'tests/diff/cases/semantics/functions/bare_lambda_expression_positions.xr',
 'tests/diff/cases/semantics/functions/default_param_as_function_value.xr',
 'tests/diff/cases/semantics/functions/function_type_surface.xr',
 'tests/diff/cases/semantics/functions/lambda_forms.xr',
 'tests/diff/cases/semantics/functions/lambda_inference_sources.xr',
 'tests/diff/cases/semantics/functions/named_function_forms.xr',
 'tests/diff/cases/semantics/functions/parenthesized_function_type.xr',
 'tests/diff/cases/semantics/functions/string_result_direct_call.xr',
 'tests/diff/cases/semantics/functions/unit_return_expression.xr',
 'tests/diff/cases/semantics/functions/unit_value_rendering.xr',
 'tests/unit/xir/legacy_consumers/fixtures/callable_explicit_fill/root.xr')
_PROGRAMS = {'unproven_callee_targets': 'tests/diff/cases/semantics/functions/unproven_callee_targets.xr',
 'bare_lambda_expression_positions': 'tests/diff/cases/semantics/functions/bare_lambda_expression_positions.xr',
 'call_through_function_parameter': 'tests/diff/cases/semantics/closure/call_through_function_parameter.xr',
 'capture_forms': 'tests/diff/cases/semantics/closure/capture_forms.xr',
 'default_param_as_function_value': 'tests/diff/cases/semantics/functions/default_param_as_function_value.xr',
 'function_type_surface': 'tests/diff/cases/semantics/functions/function_type_surface.xr',
 'lambda_forms': 'tests/diff/cases/semantics/functions/lambda_forms.xr',
 'lambda_inference_sources': 'tests/diff/cases/semantics/functions/lambda_inference_sources.xr',
 'named_function_forms': 'tests/diff/cases/semantics/functions/named_function_forms.xr',
 'nested_closure_transitive_capture': 'tests/diff/cases/semantics/closure/nested_closure_transitive_capture.xr',
 'parenthesized_function_type': 'tests/diff/cases/semantics/functions/parenthesized_function_type.xr',
 'string_result_direct_call': 'tests/diff/cases/semantics/functions/string_result_direct_call.xr',
 'unit_return_expression': 'tests/diff/cases/semantics/functions/unit_return_expression.xr',
 'unit_value_rendering': 'tests/diff/cases/semantics/functions/unit_value_rendering.xr'}
_PROGRAMS[_UNIT_NAME] = _UNIT_ORIGINAL
_NAMES = frozenset({_UNIT_NAME,
    'unproven_callee_targets',
    'call_through_function_parameter', 'capture_forms', 'nested_closure_transitive_capture',
    'bare_lambda_expression_positions', 'default_param_as_function_value', 'function_type_surface',
    'lambda_forms', 'lambda_inference_sources', 'named_function_forms',
    'parenthesized_function_type', 'string_result_direct_call', 'unit_return_expression', 'unit_value_rendering'})


def counter():
    cell = [0]
    def advance():
        cell[0] += 1
        return cell[0]
    return advance


def render(value):
    if type(value) is bool: return 'true' if value else 'false'
    if type(value) in (str, int): return str(value)
    if type(value) is float and math.isfinite(value): return repr(value)
    if type(value) is tuple:
        return '(' + ', '.join(render(item) for item in value) + (',' if len(value) == 1 else '') + ')'
    if type(value) is list: return '[' + ', '.join(render(item) for item in value) + ']'
    raise ValueError('Callable trace requires an explicit printable value; Unit is the empty tuple')


def unit_call_port(text):
    before = '// value the caller may bind, and that value compares equal to null. Inlining the'
    after = '// value the caller may bind. It is Unit (), distinct from null. Inlining the'
    if text.count(before) != 1 or text.count('== "null"') != 2:
        raise ValueError('Complete original empty-call source lost its exact Unit migration sites')
    return text.replace(before, after).replace('== "null"', '== "()"')


def unit_type_name(value):
    if type(value) is not tuple or len(value) != 0:
        raise ValueError('The empty-call reflection obligation requires Unit, not an absent Optional or scalar')
    return '()'


def unit_call_trace():
    calls = []
    values = []
    names = []
    events = []
    def empty():
        calls.append('empty')
        events.append('call:empty')
        return ()
    def observe(binding, value):
        name = unit_type_name(value)
        names.append(name)
        events.append('typeName:' + binding)
        if name != '()':
            raise ValueError('Unit type-name assertion failed')
        events.append('assert:' + binding)
        values.append(value)
        events.append('print:' + binding)
    def run():
        calls.append('run')
        events.append('call:run')
        def local_empty():
            calls.append('local_empty')
            events.append('call:local_empty')
            return ()
        result = empty()
        observe('result', result)
        nested = local_empty()
        observe('nested', nested)
    run()
    return {'calls': calls, 'values': values, 'type_names': names, 'assertions': 2, 'events': events}


def observations():
    traces = {}
    apply = lambda value, function: function(value)
    double = lambda value: value * 2
    negate = lambda value: 0 - value
    traces['call_through_function_parameter'] = [apply(21, double), apply(5, lambda n: n + 1)]

    # Each loop iteration owns its capture. Map lookup returns the callable,
    # and composition applies the inner target before the outer target.
    compose = lambda first, second: lambda value: second(first(value))
    table = {'dbl': double}
    closures = [(lambda captured: lambda: captured)(i) for i in range(3)]
    traces['unproven_callee_targets'] = [compose(double, negate)(5), table['dbl'](7),
                                        closures[0](), closures[2]()]

    greeting = 'hi'
    say = lambda: greeting + '!'
    first = counter(); values = [say(), first(), first(), first()]
    second = counter(); values += [second(), first()]
    make_adder = lambda base: lambda value: value + base
    values += [make_adder(10)(5), make_adder(99)(5)]
    total = [0]
    def accumulate(value): total[0] += value
    accumulate(5); accumulate(7); values.append(total[0])
    traces['capture_forms'] = values

    def transitive():
        captured = 10
        def outer():
            inner = lambda: captured * 2
            return inner() + 1
        return outer
    traces['nested_closure_transitive_capture'] = [transitive()()]

    functions = [None] * 100
    for index in range(1, 100):
        functions[index] = lambda value, captured=index: captured + value
    traces['bare_lambda_expression_positions'] = [double(21), functions[10](300), make_adder(7)(35)]

    def connect(host, port=8080): return host + ':' + str(port)
    indirect = connect
    traces['default_param_as_function_value'] = [connect('a'), indirect('b', 9)]

    square = lambda value: value * value
    values = [apply(41, lambda x: x + 1), apply(21, double)]
    function = double; values.append(function(10))
    function = negate; values.append(function(10))
    operations = [double, negate, square]; values += [operation(6) for operation in operations]
    registry = {'handler': lambda value: value + 1000}
    handler = registry['handler']; values.append(handler(1))
    values.append([apply(x, lambda value: value * 100) for x in [1, 2, 3]])
    traces['function_type_surface'] = values

    values = [double(21), (lambda x: x * 2)(21), (lambda a,b:a+b)(3,4),
              (lambda a,b:a*b)(3,4), (lambda:99)(), (lambda x:(x+1)*10)(4),
              (lambda x:x+1)(41), (lambda x:x)(7)]
    read_cell = [10]
    values += [read_cell[0] * 2, read_cell[0]]
    write_cell = [10]; write_cell[0] *= 3; values.append(write_cell[0])
    values.append((lambda a:a+1)(100))
    traces['lambda_forms'] = values

    bound = lambda x:x+1; transforms = [lambda x:x-1]
    make_point = lambda a:{'x':a, 'y':a}
    values = [bound(2), apply(4, lambda x:x*3), transforms[0](9),
              (lambda x,y:x+y)(4,5), make_point(1.0)['x']]
    cell = [5]; cell[0] *= 2; values += [cell[0], cell[0]*2, cell[0]]
    cell[0] += 1; values += [cell[0], cell[0]]
    traces['lambda_inference_sources'] = values

    def connect_log(host, port=8080, tls=False):
        return host + ':' + str(port) + ' tls=' + render(tls)
    quotient, remainder = divmod(17, 5)
    identity = lambda value:value; triple = lambda value:value*3
    traces['named_function_forms'] = [3+4, 'log '+'explicit-unit', 'pong',
        connect_log('localhost'), connect_log('localhost',443), connect_log('localhost',443,True),
        quotient, remainder, identity(42), identity('xyz'), triple(5)]

    advance = counter(); absent = None
    values = [advance(), apply(1, lambda x:x+41), absent is None]
    present = advance; values.append(present())
    traces['parenthesized_function_type'] = values

    label = lambda value,unused:'pos' if value>0 else 'neg'
    forward = lambda value:label(value,0)
    bound = label(3,0)
    traces['string_result_direct_call'] = [label(1,0),label(-1,0),forward(2),bound]

    events = []
    def inner(): events.append('inner'); return ()
    def named(): return inner()
    named(); (lambda:inner())()
    for value in [1,2]: events.append(value)
    traces['unit_return_expression'] = events
    def nothing(): return ()
    traces['unit_value_rendering'] = [nothing(),(1,2)]
    traces[_UNIT_NAME] = unit_call_trace()['values']
    if set(traces) != _NAMES: raise ValueError('Complete callable trace lost a program')
    return traces


def expected_rows():
    if (set(_INPUTS) != set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS) != len(set(_REQUIRED_INPUTS))
            or set(_PROGRAMS) != _NAMES or len(set(_PROGRAMS.values())) != len(_NAMES)):
        raise ValueError('Complete callable model lost a reviewed program or input')
    for name, expected in _INPUTS.items():
        path = Path(name)
        if path.is_absolute() or path.as_posix() != name or '..' in path.parts or path.parts[0] not in {'spec','tests'}:
            raise ValueError('Callable model inputs require delivered source identities')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest() != expected:
            raise ValueError('Complete callable source or language authority changed: '+name)
    if not set(_PROGRAMS.values()) <= set(_INPUTS): raise ValueError('Callable program is missing its exact source binding')
    original = _PROGRAMS['bare_lambda_expression_positions']
    selected = 'tests/unit/xir/legacy_consumers/fixtures/callable_explicit_fill/root.xr'
    before = 'var fns = Array<IntFn>(100)'
    after = 'var fns = Array<IntFn>(100, value -> value)'
    text = (ROOT/original).read_bytes().replace(b'\r\n',b'\n').decode('utf-8')
    if text.count(before) != 1 or (ROOT/selected).read_bytes().replace(b'\r\n',b'\n') != text.replace(before,after).encode('utf-8'):
        raise ValueError('Callable array port must preserve the entire original program with explicit valid fill')
    original_unit = (ROOT/_UNIT_ORIGINAL).read_bytes().replace(b'\r\n',b'\n').decode('utf-8')
    if (ROOT/_UNIT_PORT).read_bytes().replace(b'\r\n',b'\n') != unit_call_port(original_unit).encode('utf-8'):
        raise ValueError('Unit migration changed more than the two obsolete assertions and comment')
    traces = observations()
    return [{'id':'complete_callable_'+name, 'source':_UNIT_PORT if name==_UNIT_NAME else selected if name=='bare_lambda_expression_positions' else source, 'original_source':source,
             'required_target':None, 'expected':{'exit_code':0,'stderr_utf8':'',
             'stdout_utf8':''.join(render(value)+'\n' for value in traces[name])},
             'model':('Complete original top-level/local empty call-result bindings, two typeName assertions and two prints. Only obsolete null assertions and their comment migrate to the published Unit() identity. No result binding/body/observation removed;physical slots,inlining,FI,lifetime and full providers remain unqualified.' if name==_UNIT_NAME else 'Full unchanged original program: source-bound independent call/capture/default/ref/Unit state trace; output does not prove physical release or complete FI.'),
             'observation_count':len(traces[name])} for name,source in sorted(_PROGRAMS.items())]
