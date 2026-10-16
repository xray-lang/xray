"""Complete ordered traversal and range obligations, independent of Xray execution.

All original sources and bodies remain intact. Logical cursors, typed derived
keys and mathematical range endpoints do not prove current Source admission,
compiler specialization, borrowed mutation, allocation recovery or providers.
"""
from dataclasses import dataclass
from pathlib import Path
import hashlib
from independent_observation_format import render as scalar_render

ROOT = Path(__file__).resolve().parents[4]
_PROGRAMS = {
 'range_inclusive':'tests/diff/cases/semantics/range_inclusive.xr',
 'range_iterator_shared_core':'tests/diff/cases/semantics/collections/range_iterator_shared_core.xr',
 'range_to_array_too_large_shared_core':'tests/diff/cases/semantics/collections/range_to_array_too_large_shared_core.xr',
 'iterator_protocol_exhausted':'tests/diff/cases/semantics/collections/iterator_protocol_exhausted.xr',
 'for_in_interface_iterator':'tests/diff/cases/semantics/collections/for_in_interface_iterator.xr',
 'map_set_order':'tests/diff/cases/semantics/iter_order/map_set_order.xr',
 'map_set_derived_key_fields':'tests/diff/cases/semantics/collections/map_set_derived_key_fields.xr'}
_AUTHORITIES = ('spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md','spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md','spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/018-17-compilation-pipeline.md','stdlib/types/iterator.xr',
 'stdlib/types/map.xr','stdlib/types/set.xr','src/shared/xr_range_core.h',
 'src/shared/xr_map_set_abi.h','src/shared/xr_error_messages.h','src/runtime/xerror_codes.h',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py')
_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677', 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c', 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915', 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d', 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b', 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e', 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440', 'src/runtime/xerror_codes.h': 'c57c353946012402931711b2b736013c8b5208fcb5de22370a230cc75e0f8a56', 'src/shared/xr_error_messages.h': '9cb2a61e702eb0f768e4a9e7fb1a1a3d7176cc603dfb955ca1e2415e6e8c615c', 'src/shared/xr_map_set_abi.h': '227db1d370717cd56b453c3ab85989ad96742eaffe3e73e3688b76e5f781c993', 'src/shared/xr_range_core.h': '8c0d9ec40d4b40f4c78566a2bcecb25c217a921725edc1dda2929590ddec951e', 'stdlib/types/iterator.xr': '251d08d069d868e8487cae10f92e3ead7b601262a1bbc1d3989fb5871c2c7f94', 'stdlib/types/map.xr': '49c5f1ddd6d0addab7608d918bbb627e655219ca66d10154cab7b05773488f31', 'stdlib/types/set.xr': '6c8062693701d60982f550ba8b766b4c20924c9ed36cb0dc146be45f9693db8f', 'tests/diff/cases/semantics/collections/for_in_interface_iterator.xr': 'ca5759543132cb95869fad03207f94401d48366a9f77a6804c3a07c32d7ce9a7', 'tests/diff/cases/semantics/collections/iterator_protocol_exhausted.xr': 'b4caf4fb6dd0c6698a00c30f2409aa53fdc2b9f212f73984ebfe5a1ca8f232d9', 'tests/diff/cases/semantics/collections/map_set_derived_key_fields.xr': 'cf989cf13cb483ce8c3efbb3813ca0d49f26f60dc609d232da6618ae2297064c', 'tests/diff/cases/semantics/collections/range_iterator_shared_core.xr': 'f10b37f81efd47a39defd9720121e2ff6f5d7887d8f62d1ced3b4d32fe641e78', 'tests/diff/cases/semantics/collections/range_to_array_too_large_shared_core.xr': '07167b70c5d8baf9e9a9a834d75ef70457d14b10c6e065d4fdbe1ac545a96024', 'tests/diff/cases/semantics/iter_order/map_set_order.xr': '2867ca8141a253a6a44c2e714fd965fe189cfd12ccc785e4e6219911bb7398ab', 'tests/diff/cases/semantics/range_inclusive.xr': '52aa6c98fa659486f18c7702da58b5104bfe73fc854b24ae24d4628d60b02aa6', 'tests/unit/xir/legacy_consumers/independent_observation_format.py': '09843df7bf351411b39877696ae17e489ee982062827d3a6ab8e8cad7ab1f68b'}
I64_MIN, I64_MAX = -(1 << 63), (1 << 63) - 1
NEXT_MESSAGE = 'Iterator.next() on an exhausted iterator: every next() must follow a hasNext() that returned true'
NTH_MESSAGE = 'Iterator.nth(index) ran past the end: the iterator was exhausted before reaching index'


class Fault(ValueError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def text(value):
    if value is None:
        return 'null'
    if type(value) is list:
        return '[' + ', '.join(text(x) for x in value) + ']'
    return scalar_render({'expected': value})


@dataclass(frozen=True)
class Range:
    start: int
    end: int
    inclusive: bool = False
    step: int = 1

    def __post_init__(self):
        if any(type(x) is not int or not I64_MIN <= x <= I64_MAX
               for x in (self.start, self.end, self.step)):
            raise ValueError('Range endpoints and step require complete i64 values')

    def raw_count(self):
        if self.step == 0:
            return 0
        distance = (self.end - self.start) * (1 if self.step > 0 else -1)
        if distance < 0 or distance == 0 and not self.inclusive:
            return 0
        width = abs(self.step)
        return distance // width + 1 if self.inclusive else (distance + width - 1) // width

    def count(self):
        return min(I64_MAX, self.raw_count())

    def contains(self, value):
        if type(value) is not int or self.step == 0:
            return False
        offset = (value - self.start) * (1 if self.step > 0 else -1)
        return offset >= 0 and offset % abs(self.step) == 0 and offset // abs(self.step) < self.raw_count()

    def at(self, index):
        if type(index) is not int or not 0 <= index < self.count():
            raise Fault(430, 'Range index out of bounds')
        return self.start + index * self.step

    def to_array(self):
        if self.count() > 10000000:
            raise Fault(441, 'Range.toArray range too large')
        return [self.at(index) for index in range(self.count())]

    def iterator(self):
        return Cursor(self.to_array())

    def display(self):
        suffix = '' if self.step == 1 else ':' + str(self.step)
        return str(self.start) + ('..=' if self.inclusive else '..') + str(self.end) + suffix


class Cursor:
    def __init__(self, values):
        self.values = list(values)
        self.position = 0
        self.pulls = 0
        self.faults = 0

    def has_next(self):
        return self.position < len(self.values)

    def next(self):
        self.pulls += 1
        if not self.has_next():
            self.faults += 1
            raise Fault(432, NEXT_MESSAGE)
        value = self.values[self.position]
        self.position += 1
        return value

    def nth(self, index):
        if type(index) is not int or index < 0:
            raise Fault(430, 'Iterator.nth index must be non-negative')
        for ordinal in range(index + 1):
            if not self.has_next():
                self.faults += 1
                raise Fault(432, NTH_MESSAGE)
            value = self.next()
            if ordinal == index:
                return value

    def iterator(self):
        return self


def drain(cursor):
    values = []
    while cursor.has_next():
        values.append(cursor.next())
    return values


@dataclass(frozen=True)
class Key:
    color: tuple
    number: int
    label: str
    declaration: str = 'map_set_derived_key_fields::Key'

    def identity(self):
        return self.declaration, self.color, self.number, self.label


def key_equal(left, right):
    return type(left) is type(right) and (left.identity() == right.identity() if type(left) is Key else left == right)


class Ordered:
    def __init__(self):
        self.entries = []
        self.operations = []

    def find(self, key):
        return next((i for i, (stored, value, alive) in enumerate(self.entries)
                     if alive and key_equal(stored, key)), None)

    def put(self, key, value):
        found = self.find(key)
        if found is None:
            self.entries.append((key, value, True))
        else:
            self.entries[found] = (key, value, True)
        self.operations.append('insert' if found is None else 'update')

    def delete(self, key):
        found = self.find(key)
        self.operations.append('delete')
        if found is None:
            return False
        stored, value, alive = self.entries[found]
        self.entries[found] = (stored, value, False)
        return True

    def items(self):
        return [(key, value) for key, value, alive in self.entries if alive]

    def keys(self):
        return [key for key, value in self.items()]

    def values(self):
        return [value for key, value in self.items()]

    def get(self, key):
        found = self.find(key)
        return None if found is None else self.entries[found][1]

    def contains(self, key):
        return self.find(key) is not None


def inclusive_trace():
    r = Range(1, 3, True)
    outputs = [r.display(), r.count(), r.contains(3), r.contains(4), r.at(0), r.at(2), r.to_array()]
    outputs += [sum(r.to_array()), sum(Range(1, 3).to_array()), Range(I64_MIN, I64_MAX, True).count()]
    boundary = Range(I64_MAX, I64_MAX, True).iterator()
    boundary_count = 0
    while boundary.has_next():
        boundary.next()
        boundary_count += 1
        continue
    outputs += [boundary_count]
    for x in (3, 5, 6):
        outputs.append('low' if r.contains(x) else 'mid' if Range(4, 6).contains(x) else 'other')
    return {'outputs': outputs, 'full_i64_mathematical_count': 1 << 64, 'published_count': I64_MAX,
            'max_endpoint_loop_body_steps': boundary_count, 'whole_original_loop_body_steps': 6}


def range_cursor_trace():
    ranges = [Range(2,9), Range(1,4,True), Range(5,5), Range(9,2), Range(-3,2), Range(-4,-2,True), Range(7,7,True)]
    outputs = [drain(r.iterator()) for r in ranges]
    r = Range(3,8,True)
    outputs += [drain(r.iterator()), r.to_array(), sum(r.to_array()), sum(drain(r.iterator()))]
    a, b = r.iterator(), r.iterator()
    outputs += [a.has_next(), a.next(), a.next(), b.next()]
    c = b.iterator()
    outputs += [c.next()]
    e = Range(0,0).iterator()
    outputs += [e.has_next(), len(drain(e))]
    outputs += [sum(drain(x.iterator())) for x in (Range(2,9), Range(1,4,True), Range(9,2), Range(-3,2))]
    outputs += [sum(drain(Range(1,5).iterator())) + sum(drain(Range(10,12,True).iterator()))]
    return {'outputs': outputs, 'independent_range_cursors': a is not b, 'iterator_self_identity': c is b,
            'a_position': a.position, 'b_position_after_c_next': b.position, 'whole_original_loop_body_steps':61,
            'generic_nested_calls':2, 'generic_erased_fallback_is_current_permission':False}


def oversized_trace():
    r = Range(0, 10000001)
    try:
        r.to_array()
    except Fault as error:
        return {'outputs':[str(error), error.code], 'original_length':r.count(),
                'materialization_limit':10000000, 'materialized_elements':0, 'caught_faults':1}
    raise AssertionError('Original oversized range unexpectedly materialized')


def exhausted_trace():
    outputs = []
    fault_codes = []
    def fault(cursor, prefix, index=None, message=False):
        try:
            cursor.next() if index is None else cursor.nth(index)
        except Fault as error:
            fault_codes.append(error.code)
            outputs.append(prefix + str(error.code) + (' ' + str(error) if message else ''))
        else:
            raise AssertionError('Original exhausted iterator returned an in-band value')
    ai = Cursor([10,20,30])
    outputs += ['arr '+text(x) for x in drain(ai)]
    outputs += ['arr-has '+text(ai.has_next())]
    fault(ai, 'arr-next ', message=True)
    entries = Cursor(list(enumerate([10,20,30])))
    outputs += ['arr-entry '+str(i)+'='+str(x) for i,x in drain(entries)]
    fault(entries, 'arr-entry-next ')
    outputs += ['arr-nth '+text(Cursor([10,20,30]).nth(2))]
    fault(Cursor([10,20,30]), 'arr-nth ', index=3, message=True)
    m = Ordered()
    for key,value in [('k',1),('j',2)]:m.put(key,value)
    mi, mei = Cursor(m.keys()), Cursor(m.items())
    outputs += ['map '+x for x in drain(mi)]
    fault(mi, 'map-next ')
    outputs += ['map-entry '+key+'='+str(value) for key,value in drain(mei)]
    fault(mei, 'map-entry-next ')
    si = Cursor([7,8])
    outputs += ['set '+str(x) for x in drain(si)]
    fault(si, 'set-next ')
    ri = Cursor(list('ab'))
    outputs += ['rune '+x for x in drain(ri)]
    fault(ri, 'rune-next ', message=True)
    outputs += ['rune-nth '+Cursor(list('ab')).nth(1)]
    fault(Cursor(list('ab')), 'rune-nth ', index=2)
    empty = Cursor([])
    outputs += ['empty-has '+text(empty.has_next())]
    fault(empty, 'empty-next ')
    return {'outputs':outputs, 'caught_faults':len(fault_codes), 'fault_codes':fault_codes,
            'unreachable_prints_executed':0, 'empty_stays_exhausted':not empty.has_next()}


def countdown_trace():
    state = 4
    outputs = []
    while state > 0:
        state -= 1
        outputs.append(state + 1)
    return {'outputs':outputs,'next_body_steps':4,'final_state':state,
            'legacy_interface_and_iterator_declarations_require_atomic_migration':True}


def order_trace():
    outputs = []
    m = Ordered()
    for key,value in [('first',1),('second',2),('third',3),('first',10)]:m.put(key,value)
    outputs += ['-- map for k --', *m.keys(), '-- map for k,v --']
    outputs += [key+'='+str(value) for key,value in m.items()]
    outputs += ['-- map keys --', *m.keys(), '-- map values --', *map(str,m.values())]
    outputs += ['-- map delete+reinsert --']
    m.delete('second');m.put('second',20)
    outputs += m.keys()
    outputs += ['-- map tombstone append --']
    m.delete('first');m.delete('third');m.put('fourth',4);m.put('fifth',5)
    outputs += [key+'='+str(value) for key,value in m.items()]
    outputs += list(map(str,m.values()))
    s = Ordered()
    for value in ('x','y','z','y'):s.put(value,value)
    outputs += ['-- set for e --', *s.keys(), '-- set delete+reinsert --']
    s.delete('x');s.put('x','x');outputs += s.keys()
    outputs += ['-- set tombstone append --'];s.delete('y');s.put('w','w');outputs += s.values()
    n = Ordered()
    for value in (30,10,20):n.put(value,value)
    outputs += ['-- int set --', *map(str,n.keys())]
    return {'outputs':outputs,'map_final_keys':m.keys(),'set_final_keys':s.keys(),
            'map_operations':m.operations,'set_operations':s.operations,
            'iteration_body_steps':33,'sorted_order_is_not_insertion_order':True}


def derived_trace():
    def key(color, number, label):return Key(('map_set_derived_key_fields::Color',color),number,label)
    m = Ordered()
    for k,v in [(key(0,1,'a'),10),(key(1,2,'b'),20),(key(0,1,'a'),11)]:m.put(k,v)
    outputs = [len(m.items()),m.get(key(0,1,'a')),m.contains(key(1,2,'b')),m.contains(key(2,2,'b')),
               m.contains(key(0,1,'z')),m.get(key(2,9,'q'))]
    m.delete(key(1,2,'b'));outputs += [m.contains(key(1,2,'b')),len(m.items())]
    s = Ordered()
    for k in (key(2,7,'x'),key(2,7,'x'),key(2,8,'x')):s.put(k,True)
    outputs += [len(s.items()),s.contains(key(2,7,'x')),s.contains(key(2,9,'x'))]
    return {'outputs':outputs,'map_final_entries':len(m.items()),'set_final_entries':len(s.items()),
            'key_fields':['exact enum declaration and ordinal','i64 number','string label'],
            'fresh_enum_boxes_must_not_change_key_equality':True,'actual_derive_or_hash_implementation_qualified':False}


def traces():
    return dict(zip(_PROGRAMS,(inclusive_trace(),range_cursor_trace(),oversized_trace(),exhausted_trace(),
                              countdown_trace(),order_trace(),derived_trace())))


def _validate():
    if set(_INPUTS) != set(_AUTHORITIES) | set(_PROGRAMS.values()) or len(_PROGRAMS) != 7:
        raise ValueError('Complete traversal source and authority identities changed')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest() != digest:
            raise ValueError('Complete traversal source or authority changed: '+path)


def expected_rows():
    _validate()
    return [{'id':'complete_traversal_'+name,'source':_PROGRAMS[name],'required_target':None,
             'expected':{'exit_code':0,'stdout_utf8':''.join(text(value)+'\n' for value in trace['outputs']),'stderr_utf8':''},
             'observation_count':len(trace['outputs']),'required_semantic_trace':trace,
             'model':'Complete original ordered traversal/range/cursor/typed derived key behaviors. All source loops,large endpoints,full failure bodies and insertion/tombstone work retained. Logical outputs do not admit current Source Iterator/Iterable/Range/derive/mutation or erased fallback;complete atomic migration and compiler/runtime FI/resource/lifetime/sanitizer/provider/mixed remain OPEN.'}
            for name,trace in traces().items()]
