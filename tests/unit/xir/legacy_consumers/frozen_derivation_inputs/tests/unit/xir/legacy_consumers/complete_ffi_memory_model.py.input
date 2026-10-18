"""Derive complete original FFI traces from explicit x64 layout and byte models.

Foreign allocations describe the original successful normal path. These traces
do not qualify malloc failure, native contracts, callback ABI, tail-stack use,
loan reclamation, physical release, or any provider. No Xray or C code runs here.
"""
from pathlib import Path
import hashlib
import tomllib

ROOT = Path(__file__).resolve().parents[4]
BASE = 'tests/diff/cases/semantics/ffi/'
TARGET = 'windows-x86_64'
_NAMES = frozenset({
    '_extern_layout_bytecode_lib', '_native_size_layout_lib', 'cfn_bsearch',
    'cfn_optable', 'cfn_tail_thread', 'cfn_visitor_return_once',
    'extern_decl_large_layout_control_flow', 'extern_decl_registry_matrix',
    'extern_direct_noescape_pointer', 'extern_layout_bytecode_roundtrip',
    'extern_layout_introspection', 'extern_libm_dylib',
    'extern_pointer_float_prototypes', 'extern_scalar_descriptor_matrix',
    'extern_used_dylib_unused_symbol_isolation', 'mem_load_store_native_size',
    'mem_view_extern_layout', 'mem_view_extern_layout_native_size',
    'mem_view_imported_layout_native_size', 'mem_view_packed_nested_native_size',
    'mem_with_slice_mut_exception_loan_end', 'mem_with_slice_mut_extern_layout',
    'ptr_memory', 'rawptr_copy_nonoverlap_owner', 'rawptr_static_null',
    'unsafe_return_block'})
_PROGRAMS = {name: BASE + name + '.xr' for name in _NAMES}
_PORT_NAMES = frozenset({'extern_layout_bytecode_roundtrip', 'mem_view_extern_layout',
    'mem_view_extern_layout_native_size', 'mem_view_imported_layout_native_size',
    'mem_view_packed_nested_native_size', 'mem_with_slice_mut_exception_loan_end',
    'mem_with_slice_mut_extern_layout'})
_PORTS = {name: 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/' + name + '/root.xr'
          for name in _PORT_NAMES}
_AUTHORITIES = (
    'spec/source/sections/003-2-type-system.md',
    'spec/source/sections/004-3-expressions.md',
    'spec/source/sections/005-4-statements.md',
    'spec/source/sections/006-5-declarations.md',
    'spec/source/sections/009-8-error-handling.md',
    'spec/source/sections/015-14-built-in-type-methods.md',
    'spec/source/sections/018-17-compilation-pipeline.md',
    'src/base/xtarget_data_layout.h', 'src/base/xtarget_data_layout.c',
    'stdlib/mem/mem.xr', BASE + 'xray.toml')
# The initial pins are written once from the reviewed delivered inputs.
_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440',
 'src/base/xtarget_data_layout.c': '6bc2336985a1da1336af372011c2b9b85e162e5deb7eaea08b23d5b1d8448c95',
 'src/base/xtarget_data_layout.h': 'c3e557ea5be1e6c4e3927ce579075f05a9b79503df76bfb52410b679837eae0c',
 'stdlib/mem/mem.xr': '9750e120d6b22f5c2caffd2d59d13238393ac6939c17827090a3829c33fa9dab',
 'tests/diff/cases/semantics/ffi/_extern_layout_bytecode_lib.xr': '4bf2dc1f041e4b9debab9b29acaa5cc31a6aaf55f6d884145d24ea5140765c16',
 'tests/diff/cases/semantics/ffi/_native_size_layout_lib.xr': 'c740d4610c26c177c12d2818361315894f26980eaf5a5fe1c7feab2430a5ec00',
 'tests/diff/cases/semantics/ffi/cfn_bsearch.xr': 'edeb5489f7d6a9520dbfcbdd9d98ae7253d49757ce84dfe530a2688b3871c37d',
 'tests/diff/cases/semantics/ffi/cfn_optable.xr': '742e31d96ebb1b929c47acb07946fd7ccc07eaa53f3143e9cb2b801dfc15ce3d',
 'tests/diff/cases/semantics/ffi/cfn_tail_thread.xr': '3b8ec49c2926043ac4daf8ab03e0ea06268c6f8514fbd2995ad51053eee90e33',
 'tests/diff/cases/semantics/ffi/cfn_visitor_return_once.xr': 'b91c706e3178af15a70dfd460a7bbc2c1dbf84b0499055b1727509a57408add6',
 'tests/diff/cases/semantics/ffi/extern_decl_large_layout_control_flow.xr': '94127d2a658f7006df3a39168cadf6e1ae764eb2c74bd2024885b71e8711c25f',
 'tests/diff/cases/semantics/ffi/extern_decl_registry_matrix.xr': 'a850ecc5b8287395cf5bce795d8e06db38e1c54d99e8ab62662594693b2f83cf',
 'tests/diff/cases/semantics/ffi/extern_direct_noescape_pointer.xr': '1621d2122b26c1ee3a66bddf18dcb4a46370feda0c619aa2df3182741922080c',
 'tests/diff/cases/semantics/ffi/extern_layout_bytecode_roundtrip.xr': 'c9952f576205c27c0bf39d80fddbf319bdcc11f034cd9dfb4a6df11eb9d3176e',
 'tests/diff/cases/semantics/ffi/extern_layout_introspection.xr': '5f544f03224b91f4694c72d59725d99113c6a7a34e50f628a648ab18c930b017',
 'tests/diff/cases/semantics/ffi/extern_libm_dylib.xr': '73b7f43d9fd2a865aa18578d2ed676516f22b3cd9da7f15ad3158cd653dd36c9',
 'tests/diff/cases/semantics/ffi/extern_pointer_float_prototypes.xr': 'fc945f3449b63b35f4ef74b466c63f8236bc21e6f6aefaba64c9a5da27258262',
 'tests/diff/cases/semantics/ffi/extern_scalar_descriptor_matrix.xr': '9761eed7879d10017ee2c7ada4878066f00c97a41c7f09ce5a119458fda65ec9',
 'tests/diff/cases/semantics/ffi/extern_used_dylib_unused_symbol_isolation.xr': '08e1f0abca68575c041ae29bfbcff7edbcee8488300b9cb568ca7d2c7903b74d',
 'tests/diff/cases/semantics/ffi/mem_load_store_native_size.xr': '0385c27891c68607f1f722f8dd9676930c8a3c4a776763ebac4bb10feb87ad11',
 'tests/diff/cases/semantics/ffi/mem_view_extern_layout.xr': '06ccd9d1c6e6ab2f3de43b65af008e68370be3928d527baa438fb3545682bfc8',
 'tests/diff/cases/semantics/ffi/mem_view_extern_layout_native_size.xr': '546f77db298649653b17b7d5f2a77f99c47c62d830d25737f78e6845a5c85921',
 'tests/diff/cases/semantics/ffi/mem_view_imported_layout_native_size.xr': 'f54de878f96c1f1c325ac13de3158f83e9bf018341f4fd4e93969f69ed4cd29e',
 'tests/diff/cases/semantics/ffi/mem_view_packed_nested_native_size.xr': 'fa4e17fc57cf3c639a6795d8090b59328f5a5a2f00ceb9d9b35b56e3899a334a',
 'tests/diff/cases/semantics/ffi/mem_with_slice_mut_exception_loan_end.xr': '95af03d141b69c8d98c578efbfee2b1ee2f4af14f7cd7066d131e0117d91f4cc',
 'tests/diff/cases/semantics/ffi/mem_with_slice_mut_extern_layout.xr': '53d140e2de9bd052ef349596bb21e0974377330a0ace43892703b98c5ae2b290',
 'tests/diff/cases/semantics/ffi/ptr_memory.xr': '88bfc85a59cbbbdc0600eb80722b76a2d32463012e0c22287d721adaa205a3b9',
 'tests/diff/cases/semantics/ffi/rawptr_copy_nonoverlap_owner.xr': '042c448f061edcbf5e833d3c4414a3dd85ce3f10c4fba839efb25c029d341f4b',
 'tests/diff/cases/semantics/ffi/rawptr_static_null.xr': '3286a62cbce1c11f4b69c89bccfe764516ca8cbba3d01d97567988da565779d3',
 'tests/diff/cases/semantics/ffi/unsafe_return_block.xr': 'd86139ee7173bba5fabb4ad2ab837e387b63fb99590785c38b4af9d762f2fb98',
 'tests/diff/cases/semantics/ffi/xray.toml': '3076508db88c5906218c3c464b740aeb52b25f9d9bc8e757289201a03a327f94',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/extern_layout_bytecode_roundtrip/_extern_layout_bytecode_lib.xr': '4bf2dc1f041e4b9debab9b29acaa5cc31a6aaf55f6d884145d24ea5140765c16',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/extern_layout_bytecode_roundtrip/root.xr': 'ff9d5480da4686e602b57eb56566783d0f7e90e0b9d4e0c7f3d12301faf21733',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/extern_layout_bytecode_roundtrip/xray.toml': '3076508db88c5906218c3c464b740aeb52b25f9d9bc8e757289201a03a327f94',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_extern_layout/root.xr': '9d3d576ba81537e8c2003cdc4432fdeb1f0f6ba953b7f08ed5b5fe277a346975',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_extern_layout/xray.toml': '3076508db88c5906218c3c464b740aeb52b25f9d9bc8e757289201a03a327f94',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_extern_layout_native_size/root.xr': '44be0fa8c3baa89e098f594d485ac31f8d4806e81c9ec2a338a48aa0abd94438',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_extern_layout_native_size/xray.toml': '3076508db88c5906218c3c464b740aeb52b25f9d9bc8e757289201a03a327f94',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_imported_layout_native_size/_native_size_layout_lib.xr': 'c740d4610c26c177c12d2818361315894f26980eaf5a5fe1c7feab2430a5ec00',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_imported_layout_native_size/root.xr': '526e6354ea16d990ebf16cfbb47713c4353cb4debdea2490de65286114368213',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_imported_layout_native_size/xray.toml': '3076508db88c5906218c3c464b740aeb52b25f9d9bc8e757289201a03a327f94',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_packed_nested_native_size/root.xr': 'd1724f76a65adcdcfa2a363ff1ae7312ad64e008de9c0bdcb120057ec707ef1b',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_view_packed_nested_native_size/xray.toml': '3076508db88c5906218c3c464b740aeb52b25f9d9bc8e757289201a03a327f94',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_with_slice_mut_exception_loan_end/root.xr': '599c95f616194f86b129340d65f186b9c6d9ed5e9033e4b0306b90f0ab752618',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_with_slice_mut_exception_loan_end/xray.toml': '3076508db88c5906218c3c464b740aeb52b25f9d9bc8e757289201a03a327f94',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_with_slice_mut_extern_layout/root.xr': '05a9daffd5ed50f112fa453466f46cae91e79351c6c377ec148a710def5efa84',
 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/mem_with_slice_mut_extern_layout/xray.toml': '3076508db88c5906218c3c464b740aeb52b25f9d9bc8e757289201a03a327f94'}


def aggregate(fields, *, packed=False, union=False):
    """Lay out explicit (size, alignment) facts without querying the host ABI."""
    offset = 0; alignment = 1; offsets = []
    for size, align in fields:
        if size < 0 or align <= 0 or align & (align - 1):
            raise ValueError('Invalid independent layout fact')
        align = 1 if packed else align
        alignment = max(alignment, align)
        start = 0 if union else (offset + align - 1) // align * align
        offsets.append(start)
        offset = max(offset, start + size)
    return ((offset + alignment - 1) // alignment * alignment, alignment), offsets


class Memory:
    def __init__(self, length): self.bytes = bytearray(length)
    def store(self, offset, width, value, *, signed=False, endian='little'):
        if offset < 0 or offset + width > len(self.bytes): raise ValueError('Out of bounds')
        self.bytes[offset:offset + width] = value.to_bytes(width, endian, signed=signed)
    def load(self, offset, width, *, signed=False, endian='little'):
        if offset < 0 or offset + width > len(self.bytes): raise ValueError('Out of bounds')
        return int.from_bytes(self.bytes[offset:offset + width], endian, signed=signed)


class Loan:
    def __init__(self, memory): self.memory, self.active = memory, False
    def use(self, callback):
        if self.active: raise ValueError('Overlapping mutable loan')
        self.active = True
        try: return callback(self.memory)
        finally: self.active = False


class LoanStop(Exception): pass


def copy_nonoverlapping(destination, source, count):
    if count <= 0: return
    if destination is None or source is None or count > min(len(destination), len(source)):
        raise ValueError('Invalid positive copy span')
    destination[:count] = source[:count]


def bsearch_index(key, values, compare):
    first, last = 0, len(values)
    while first < last:
        middle = (first + last) // 2; relation = compare(key, values[middle])
        if relation == 0: return middle
        if relation < 0: last = middle
        else: first = middle + 1
    return None


def copy_checksum(count):
    if not 1 <= count <= 40: raise ValueError('Fixture copy count')
    source = bytes(range(11, 51)); destination = bytearray([205] * 40)
    copy_nonoverlapping(destination, source, count)
    result = destination[0] + destination[count - 1] * 257
    if count < 40: result += destination[count] * 65537
    return result


def layout_observations():
    byte, half, word, wide, pointer = (1, 1), (2, 2), (4, 4), (8, 8), (8, 8)
    scalar, _ = aggregate([byte, word]); ptr, _ = aggregate([pointer])
    array, _ = aggregate([(half[0] * 3, half[1])])
    m3, m3_fields = aggregate([pointer, half, half])
    header, header_fields = aggregate([byte, word, pointer, (half[0] * 3, half[1])])
    union, union_fields = aggregate([word, (byte[0] * 4, byte[1])], union=True)
    envelope, envelope_fields = aggregate([header, union])
    function, _ = aggregate([pointer, wide, (word[0] * 4, word[1]),
                             (wide[0] * 4, wide[1]), pointer])
    values = [scalar[0], ptr[0], array[0], *m3, *m3_fields,
              *header, *header_fields, *union, *union_fields,
              *envelope, *envelope_fields]
    return values, function[0]


def observations():
    out = {'_extern_layout_bytecode_lib': [], '_native_size_layout_lib': []}
    out['cfn_bsearch'] = [bsearch_index(42, [42], lambda left, right: 0) is None]
    immediates, stack = [10, 20, 0, 0], []
    for opcode, pc in zip(['constant', 'constant', 'add'], range(3)):
        if opcode == 'constant': stack.append(immediates[pc])
        else: right = stack.pop(); stack[-1] += right
    separate_stack = [immediates[0]]
    out['cfn_optable'] = ['sum=' + str(stack[-1]), 'f=1 v=' + str(separate_stack[0])]
    remaining, payload = 200000, 42
    while remaining > 0: remaining -= 1
    out['cfn_tail_thread'] = ['result=' + str(payload)]
    visits = 0; events = []
    def visit(left, right):
        nonlocal visits
        visits += 1; return left + right
    result = visit(20, 22); events.append('seen=' + str(result))
    out['cfn_visitor_return_once'] = events + ['result=' + str(result), 'visits=' + str(visits)]
    introspection, function_size = layout_observations()
    out['extern_layout_introspection'] = introspection
    out['extern_decl_large_layout_control_flow'] = [function_size, sum(b'wasm3'), sum(range(5))]
    text = b'registry\0'
    out['extern_decl_registry_matrix'] = [text.index(0), text[text.index(103)], abs(-11) + abs(-31), True, False]
    out['extern_direct_noescape_pointer'] = [(b'AB' > b'AB') - (b'AB' < b'AB')]
    out['extern_libm_dylib'] = ['1.0', str(3 ** 2) + '.0']
    text = b'ABC'; found = text.find(bytes([66]))
    out['extern_pointer_float_prototypes'] = [found < 0, text[found], True]
    left, right = b'ABC\0', b'ABD'
    out['extern_scalar_descriptor_matrix'] = [left.index(0), left[:3] < right, abs(-1234567890123), abs(-23),
                                               min(2.5, -1.5) == -1.5, max(2.5, -1.5) == 2.5]
    out['extern_used_dylib_unused_symbol_isolation'] = [True]
    memory = Memory(64)
    writes = [(1, 37, False), (10, -37, True), (19, 91, False), (28, -91, True),
              (40, (1 << 64) - 1, False), (49, -(1 << 63), True)]
    for offset, value, signed in writes: memory.store(offset, 8, value, signed=signed)
    out['mem_load_store_native_size'] = [memory.load(offset, 8, signed=signed) for offset, _, signed in writes[:4]]
    out['mem_load_store_native_size'] += [memory.load(offset, 8, signed=signed) == value for offset, value, signed in writes[4:]]
    packed, packed_fields = aggregate([(1, 1), (8, 8), (8, 8)], packed=True)
    memory = Memory(packed[0])
    for offset, width, value in zip(packed_fields, [1, 8, 8], [7, 37, -37]): memory.store(offset, width, value, signed=value < 0)
    out['extern_layout_bytecode_roundtrip'] = [memory.load(packed_fields[0], 1), memory.load(packed_fields[1], 8), memory.load(packed_fields[2], 8, signed=True)]
    child, _ = aggregate([(4, 4)]); header, fields = aggregate([(1, 1), (4, 4), (8, 8), child])
    memory = Memory(header[0]); memory.store(fields[0], 1, 7); memory.store(fields[1], 4, 4099)
    memory.store(fields[1], 4, memory.load(fields[1], 4) + 1); memory.store(fields[3], 4, 91)
    out['mem_view_extern_layout'] = [memory.load(fields[0], 1), memory.load(fields[1], 4), memory.load(fields[3], 4), memory.load(fields[3], 4)]
    for name in ['mem_view_extern_layout_native_size', 'mem_view_imported_layout_native_size']:
        memory = Memory(16); events = []
        for value in [37, 91]:
            memory.store(0, 8, value); memory.store(8, 8, -value, signed=True)
            events += [memory.load(0, 8), memory.load(8, 8, signed=True)]
        out[name] = events
    envelope, offsets = aggregate([(1, 1), packed]); memory = Memory(envelope[0]); events = []
    for value in [37, 91]:
        memory.store(offsets[1] + packed_fields[0], 1, 7); memory.store(offsets[1] + packed_fields[1], 8, value)
        memory.store(offsets[1] + packed_fields[2], 8, -value, signed=True)
        if value == 37: events.append(memory.load(offsets[1] + packed_fields[0], 1))
        events += [memory.load(offsets[1] + packed_fields[1], 8), memory.load(offsets[1] + packed_fields[2], 8, signed=True)]
    out['mem_view_packed_nested_native_size'] = events
    loan = Loan(Memory(1)); events = []
    def interrupted(view): view.store(0, 1, 7); raise LoanStop('Stop')
    try: loan.use(interrupted)
    except LoanStop: events.append('caught')
    def write_again(view): view.store(0, 1, 9); return view.load(0, 1)
    events.append(loan.use(write_again)); out['mem_with_slice_mut_exception_loan_end'] = events
    loan = Loan(Memory(16))
    def write_header(view): view.store(0, 8, 73); view.store(8, 8, -73, signed=True); return view.load(8, 8, signed=True)
    result = loan.use(write_header)
    out['mem_with_slice_mut_extern_layout'] = [result, loan.memory.load(0, 8), loan.memory.load(8, 8, signed=True)]
    source = bytes([10, 20, 30, 40]); destination = bytearray(4)
    copy_nonoverlapping(destination, source, 4)
    out['ptr_memory'] = [*destination, destination[2], False, False]
    copy_nonoverlapping(None, None, 0); copy_nonoverlapping(None, None, -1)
    out['rawptr_copy_nonoverlap_owner'] = [1, *(copy_checksum(count) for count in [1, 2, 7, 8, 9, 16, 17, 31])]
    out['rawptr_static_null'] = [True, True, True, 0, 0, 0, 'ok']
    def early(): return 42
    out['unsafe_return_block'] = [early()]
    if set(out) != _NAMES: raise ValueError('FFI trace lost a complete original program')
    return out


def render(value):
    if type(value) is bool: return str(value).lower()
    if type(value) in (int, str): return str(value)
    raise ValueError('Unreviewed FFI observation')


def port_text(name, original):
    if name not in _PORT_NAMES: raise ValueError('No reviewed complete foreign-view migration')
    if name == 'mem_view_extern_layout_native_size':
        binding = '    var headers = unsafe { mem.slice<NativeSizeHeader>(raw, 1, raw) }\n'
        boundary = '    }\n    print(headers[0].length)'
        if original.count(binding) != 1 or original.count(boundary) != 1:
            raise ValueError('Native-size view lost its exact write/read boundary')
        return original.replace(binding, '').replace(boundary, '    }\n' + binding + '    print(headers[0].length)')
    expected = 2 if name == 'mem_with_slice_mut_exception_loan_end' else 1
    if original.count('ref Slice<') != expected:
        raise ValueError('Foreign callback lost its exact writable view parameters')
    return original.replace('ref Slice<', 'ref MutSlice<')


def companion_paths(name):
    directory = Path(_PORTS[name]).parent.as_posix() + '/'
    files = ['xray.toml']
    if name == 'extern_layout_bytecode_roundtrip': files.append('_extern_layout_bytecode_lib.xr')
    if name == 'mem_view_imported_layout_native_size': files.append('_native_size_layout_lib.xr')
    return [directory + item for item in files]


def expected_rows():
    if (set(_PROGRAMS) != _NAMES or len(set(_PROGRAMS.values())) != len(_NAMES)
            or any(path != BASE + name + '.xr' for name, path in _PROGRAMS.items())
            or set(_PORTS) != _PORT_NAMES
            or any(path != 'tests/unit/xir/legacy_consumers/fixtures/ffi_mutable_views/' + name + '/root.xr'
                   for name, path in _PORTS.items())
            or set(_INPUTS) != set(_AUTHORITIES) | set(_PROGRAMS.values()) | set(_PORTS.values()) |
               {path for name in _PORTS for path in companion_paths(name)}):
        raise ValueError('FFI model lost its exact complete programs or required authority')
    for name, digest in _INPUTS.items():
        if hashlib.sha256((ROOT / name).read_bytes().replace(b'\r\n', b'\n')).hexdigest() != digest:
            raise ValueError('FFI source, manifest or layout authority changed: ' + name)
    for name, path in _PORTS.items():
        original = (ROOT / _PROGRAMS[name]).read_bytes().replace(b'\r\n', b'\n').decode('utf-8')
        if (ROOT / path).read_bytes().replace(b'\r\n', b'\n') != port_text(name, original).encode('utf-8'):
            raise ValueError('Foreign view port changed beyond its complete-source permission migration')
        for companion in companion_paths(name):
            if (ROOT / companion).read_bytes().replace(b'\r\n', b'\n') != (ROOT / (BASE + Path(companion).name)).read_bytes().replace(b'\r\n', b'\n'):
                raise ValueError('Foreign view port changed its native plan or imported declaration')
    plan = tomllib.loads((ROOT / (BASE + 'xray.toml')).read_text(encoding='utf-8'))
    symbols = {item['xray']: item['native'] for item in plan['native']['symbol']}
    if symbols['task208LabsA'] != 'labs' or symbols['task208LabsB'] != 'labs':
        raise ValueError('FFI source aliases lost their explicit symbol mappings')
    traces = observations()
    return [{'id': 'complete_ffi_memory_' + name, 'source': _PORTS.get(name, path), 'original_source': path,
             'required_target': TARGET, 'expected': {'exit_code': 0, 'stderr_utf8': '',
             'stdout_utf8': ''.join(render(value) + '\n' for value in traces[name])},
             'model': 'Independent full original callback/state/byte trace and explicit little-endian x64 size/alignment/offset model. Successful foreign allocations only; no ABI, stack bound, provider, failure, lifetime or physical-release qualification is inferred.' +
                      (' Complete-source permission migration: writable callbacks use MutSlice; a shared read view is formed after the preceding writes.' if name in _PORTS else ''),
             'observation_count': len(traces[name])} for name, path in sorted(_PROGRAMS.items())]
