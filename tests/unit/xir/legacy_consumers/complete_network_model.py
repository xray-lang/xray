"""Complete network originals: closed guards, cancellation and every legal read prefix.

The finite transfer outputs assume the original loopback operations succeed.
They do not predict packet boundaries, host descriptors, scheduling, cleanup,
physical allocation, cancellation latency or provider implementation.
"""
from frozen_derivation_inputs import resolve as resolve_derivation
from dataclasses import dataclass
from pathlib import Path
import hashlib

ROOT=Path(__file__).resolve().parents[4]
BASE='tests/diff/cases/semantics/stdlib/'
PROGRAMS={name:BASE+name+'.xr' for name in (
    'net_copy_bidirectional_error_enum','net_copy_bidirectional_cancel',
    'net_copy_bidirectional_transfer')}
AUTHORITIES=(
    'spec/source/sections/003-2-type-system.md',
    'spec/source/sections/005-4-statements.md',
    'spec/source/sections/006-5-declarations.md',
    'spec/source/sections/009-8-error-handling.md',
    'spec/source/sections/011-10-concurrency-and-coroutines.md',
    'spec/source/sections/015-14-built-in-type-methods.md',
    'spec/source/sections/016-15-standard-library.md',
    'stdlib/net/net.xr','stdlib/time/time.xr','stdlib/types/coroutine.xr')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'stdlib/net/net.xr': '3894b2d9b0fb7f167b4f39334f0fdedd7bd70d5d1ceada35ff8b9247e08c8d85',
 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72',
 'stdlib/types/coroutine.xr': 'f85c868bf12b2b90988ead5cf746fd8e522d5c2d6398a64882b47637c72dc7a6',
 'tests/diff/cases/semantics/stdlib/net_copy_bidirectional_cancel.xr': 'ea22dbb3597261b0a23a2b66467d5dffa2800258c108ad23641944ba8a19eeae',
 'tests/diff/cases/semantics/stdlib/net_copy_bidirectional_error_enum.xr': 'd5721a088c492bd1955688222dbda4b6128c33260d2af7533e0a119708b5ed1e',
 'tests/diff/cases/semantics/stdlib/net_copy_bidirectional_transfer.xr': '8b685559aa923b24aaac01682fa0ff09677d5d81b63c504e47820bff357882d6'}
_IMPORT_DIGEST=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
LEFT=tuple(b'left')
RIGHT=tuple(b'right')


@dataclass
class Endpoint:
    identity: str
    closed: bool = False

    def close(self):
        self.closed=True

    def descriptor(self):
        return -1 if self.closed else self.identity


def closed_guard(a,b,identity_first=False):
    if identity_first and a.descriptor()==b.descriptor():
        return 'Invalid'
    if a.closed or b.closed:
        return 'Closed'
    if a.descriptor()==b.descriptor():
        return 'Invalid'
    return 'pump'


def partitions(n):
    if n==0:
        return [()]
    return [(head,*tail) for head in range(1,n+1) for tail in partitions(n-head)]


def relay(payload,chunks,half_close=True):
    if sum(chunks)!=len(payload) or any(type(n) is not int or n<=0 for n in chunks):
        raise ValueError('A pump must consume exactly the complete payload')
    offset=0
    delivered=[]
    for n in chunks:
        delivered.extend(payload[offset:offset+n])
        offset+=n
    return {'bytes':tuple(delivered),'count':offset,'peer_eof':bool(half_close)}


def transfer_values(from_right,from_left,half_close=True,swap_counts=False):
    if type(from_right) is not int or not 1<=from_right<=len(RIGHT):
        raise ValueError('First positive read must be a nonempty right-stream prefix')
    if type(from_left) is not int or not 1<=from_left<=len(LEFT):
        raise ValueError('First positive read must be a nonempty left-stream prefix')
    a=relay(LEFT,(len(LEFT),),half_close)
    b=relay(RIGHT,(len(RIGHT),),half_close)
    if not a['peer_eof'] or not b['peer_eof']:
        raise ValueError('No EOF settlement; a successful await cannot be inferred')
    counts=[a['count'],b['count']]
    if swap_counts:
        counts.reverse()
    return [len(LEFT),len(RIGHT),from_right,b['bytes'][0],
            from_left,a['bytes'][0],*counts]


def cancel_trace(publish_cancelled=True,settles=True):
    # No application bytes or half-close can complete the pending bridge.
    # The sleep completion belongs to the parent, not to the bridge Task.
    return {'values':['cancelled' if publish_cancelled else 'unexpected','closed'],
            'task_result':'Cancelled' if publish_cancelled else 'Completed',
            'parent_close_calls':['left','right','listener'] if settles else [],
            'terminates':settles,
            'accepted_handle_runtime_cleanup_qualified':False}


def text(values):
    return ''.join((str(v).lower() if type(v) is bool else str(v))+'\n' for v in values)


def transfer_outputs():
    return [text(transfer_values(r,l))
            for r in range(1,len(RIGHT)+1) for l in range(1,len(LEFT)+1)]


def validate_inputs():
    if set(_INPUTS)!=set(AUTHORITIES)|set(PROGRAMS.values()):
        raise ValueError('Network model lost a complete original or authority')
    for name,digest in _INPUTS.items():
        if hashlib.sha256(resolve_derivation(ROOT, name, digest).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete network source or authority changed: '+name)
    if hashlib.sha256(Path(__file__).read_bytes()).hexdigest()!=_IMPORT_DIGEST:
        raise ValueError('Network model changed after import')


def expected_rows():
    validate_inputs()
    a,b=Endpoint('udp-a'),Endpoint('udp-b')
    a.close()
    b.close()
    error=closed_guard(a,b)
    values={'net_copy_bidirectional_error_enum':[error,error=='Closed'],
            'net_copy_bidirectional_cancel':cancel_trace()['values'],
            'net_copy_bidirectional_transfer':transfer_values(len(RIGHT),len(LEFT))}
    result=[]
    for name,source in PROGRAMS.items():
        expected={'exit_code':0,'stdout_utf8':text(values[name]),'stderr_utf8':''}
        if name=='net_copy_bidirectional_transfer':
            expected.update(stdout_expectation_kind='finite_complete_network_read_prefixes',
                            stdout_utf8_options=transfer_outputs())
        result.append({'id':'complete_network_'+name,'source':source,'original_source':source,
            'required_target':None,'expected':expected,'observation_count':len(values[name]),
            'inputs':[{'path':p,'role':'preserved_source' if p==source else 'authority',
                       'lf_sha256':digest} for p,digest in sorted(_INPUTS.items())],
            'model':'Complete unchanged network original. Logical closed-before-identity guard, cancelled pending bridge, exact byte conservation and all20 legal first-read prefix pairs. Canonical stdout is one representative; transfer qualification requires membership in the independently derived complete finite output set. No actual socket/scheduler/FI/resource/lifetime/safety/provider/mixed qualification.'})
    return result


def expected_outputs(oracle):
    """Accept finite alternatives only for the exact independently owned row."""
    identity=oracle.get('id','')
    finite='stdout_utf8_options' in oracle.get('expected',{})
    if not identity.startswith('complete_network_'):
        if finite or 'stdout_expectation_kind' in oracle.get('expected',{}):
            raise ValueError('Unowned finite output expectation')
        return (oracle['expected']['stdout_utf8'].encode('utf-8'),)
    owned=next((row for row in expected_rows() if row['id']==identity),None)
    if (owned is None or oracle.get('source')!=owned['source'] or
            oracle.get('original_source',oracle.get('source'))!=owned['original_source'] or
            oracle.get('expected')!=owned['expected']):
        raise ValueError('Network source or complete independent output domain changed')
    return tuple(value.encode('utf-8') for value in
                 owned['expected'].get('stdout_utf8_options',[owned['expected']['stdout_utf8']]))


def matches(oracle,result):
    outputs=expected_outputs(oracle)
    return (result is not None and not result.timed_out and
            result.returncode==oracle['expected']['exit_code'] and
            result.stdout in outputs and result.stderr==oracle['expected'].get('stderr_utf8','').encode('utf-8'))
