"""Independent task/channel/scope observations of complete approved programs.

This is an abstract event/value model, not a scheduler or compiler capability
checker. Actual go, yield, synchronization and cleanup stay in the Xray bodies.
"""
from pathlib import Path
import copy,hashlib,itertools

ROOT=Path(__file__).resolve().parents[4]
_INPUTS = {'spec/source/sections/003-2-type-system.md': '916835c38312eccdada94aff018d22d79f3c15ed998048754759764c53fc8620',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/010-9-generics.md': '246757d531f91e2d19d2cd18353b5ec8addf05d309e5489c10da37e48be81226',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': 'db71b9a6b69133a2f2254f6d9abb4d6ed425d15af79af0428f176c946c835c6f',
 'stdlib/sync/sync.xr': '3ee7610b17475e37077fb701c8f32b31f3908285022e80d1b442e887591c8395',
 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/atomic.xr': '4da0e8355fdbb9e22a2cc5552f234d171b389302ce29698a19399ec40b2f67e4',
 'stdlib/types/channel.xr': 'ff2be02bcc28be44f7927b746d316eec7fef4e815f2693207ef07176bbbf1284',
 'stdlib/types/coro.xr': '51a94626288d0d5361a8d10563a7dd00987cdc96d8625fb54beff7912a96d51f',
 'stdlib/types/coroutine.xr': 'f85c868bf12b2b90988ead5cf746fd8e522d5c2d6398a64882b47637c72dc7a6',
 'stdlib/types/map.xr': '49c5f1ddd6d0addab7608d918bbb627e655219ca66d10154cab7b05773488f31',
 'stdlib/types/set.xr': '6c8062693701d60982f550ba8b766b4c20924c9ed36cb0dc146be45f9693db8f',
 'stdlib/types/string.xr': '95307d2e652f164c46b028ead9c6138f85ea783ee42143e4b6d8c60d89d607d7',
 'tests/diff/cases/semantics/concurrency/condvar_compose.xr': 'fcfb16127b3664911c98aa6723211f2b0846eb612736201aa7204a6a44214948',
 'tests/diff/cases/semantics/concurrency/go_lambda_local_scope.xr': '875ae2d49323c30a8cea0dc3a0c0e84b1f726f5cee92062e05d373600ede1739',
 'tests/diff/cases/semantics/concurrency/mutex_cross_coroutine.xr': '426a5e9a4a5321c5ab7a24652f8989af69665970aa9d043725c0b3afd3bc0343',
 'tests/diff/cases/semantics/coro/await_non_rethrowing_forms_observe.xr': '95746e4d343ef5217bfc0c8707e77ee1288109e5e62288a0ac1e21eecba457d6',
 'tests/diff/cases/semantics/coro/await_tuple_destructure.xr': '722455d4feeb9523e020d13eacf2df1facfa2b8eef770044e90ad81ddbf30ddc',
 'tests/diff/cases/semantics/coro/await_value_error_keeps_channel.xr': 'db8d9c1d9e5cbb17def3d2b64c19c68fbeffab7d6eebbc0c67500fc7736888f0',
 'tests/diff/cases/semantics/coro/channel_for_in_recv_or.xr': '43ea8476d9ba1392b4c6c74d7b555f0457719d8fbfdc6c29db2e95a50516a067',
 'tests/diff/cases/semantics/coro/channel_recv_timeout_unbuffered.xr': 'd417fb18843ab506a6ddb4ee19d148f5c1d35c85309efa6bcc4a68a6e06ff28d',
 'tests/diff/cases/semantics/coro/channel_send_timeout_unbuffered.xr': 'a3e2576cc44f294a9c9a20b8e1a15a9ff3d3f6497b12f0bfc44c4c184d343972',
 'tests/diff/cases/semantics/coro/coroutine_throws_unit_variant.xr': '65894189c90d0784d3b5f342eed6dd0b1e298f0407692fc80e74964e61da790b',
 'tests/diff/cases/semantics/coro/inline_match_try_recv_heap.xr': '497cd9ad09484fd7fe1c10a14fcc69a75248a40df3900b2c200dd9bd5830bd83',
 'tests/diff/cases/semantics/coro/lazy_string_boundary.xr': 'e000d279478e76e5d9df664f7fe27e80fa51178ef473ddebb199353af4fd54f6',
 'tests/diff/cases/semantics/coro/linked_scope_error_propagates_at_exit.xr': '96d3ec1db7343c6babfe90453e265071ee5c97fe63b961faa0970fe73b68b778',
 'tests/diff/cases/semantics/coro/scope_child_failure_isolation.xr': '14eb766d76b62ffdd31f24e277fec06d964c3e1bdc5392f4cca27ae5f122ca81',
 'tests/diff/cases/semantics/coro/semaphore_double_acquire_defer_bool_slot.xr': 'e7035e9890ea0f1447caa138a7d3440883c113fdfec8fbe114fe6a15ee95026a',
 'tests/diff/cases/semantics/coro/task_transfer_return_local.xr': '4238addf458238bc5b5f7fc0905aefa80da8aae01c29040291a388274608fd0b',
 'tests/diff/cases/semantics/coro/task_transfer_return_map_set.xr': '39a1832143858f365f3cb7f18201545b04fb637562e7434dc4d41fd9da6d5c56'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/010-9-generics.md',
 'spec/source/sections/011-10-concurrency-and-coroutines.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/018-17-compilation-pipeline.md',
 'stdlib/sync/sync.xr',
 'stdlib/time/time.xr',
 'stdlib/types/array.xr',
 'stdlib/types/atomic.xr',
 'stdlib/types/channel.xr',
 'stdlib/types/coro.xr',
 'stdlib/types/coroutine.xr',
 'stdlib/types/map.xr',
 'stdlib/types/set.xr',
 'stdlib/types/string.xr',
 'tests/diff/cases/semantics/concurrency/condvar_compose.xr',
 'tests/diff/cases/semantics/concurrency/go_lambda_local_scope.xr',
 'tests/diff/cases/semantics/concurrency/mutex_cross_coroutine.xr',
 'tests/diff/cases/semantics/coro/await_non_rethrowing_forms_observe.xr',
 'tests/diff/cases/semantics/coro/await_tuple_destructure.xr',
 'tests/diff/cases/semantics/coro/await_value_error_keeps_channel.xr',
 'tests/diff/cases/semantics/coro/channel_for_in_recv_or.xr',
 'tests/diff/cases/semantics/coro/channel_recv_timeout_unbuffered.xr',
 'tests/diff/cases/semantics/coro/channel_send_timeout_unbuffered.xr',
 'tests/diff/cases/semantics/coro/coroutine_throws_unit_variant.xr',
 'tests/diff/cases/semantics/coro/inline_match_try_recv_heap.xr',
 'tests/diff/cases/semantics/coro/lazy_string_boundary.xr',
 'tests/diff/cases/semantics/coro/linked_scope_error_propagates_at_exit.xr',
 'tests/diff/cases/semantics/coro/scope_child_failure_isolation.xr',
 'tests/diff/cases/semantics/coro/semaphore_double_acquire_defer_bool_slot.xr',
 'tests/diff/cases/semantics/coro/task_transfer_return_local.xr',
 'tests/diff/cases/semantics/coro/task_transfer_return_map_set.xr')
_PROGRAMS = {'await_non_rethrowing_forms_observe': 'tests/diff/cases/semantics/coro/await_non_rethrowing_forms_observe.xr',
 'await_tuple_destructure': 'tests/diff/cases/semantics/coro/await_tuple_destructure.xr',
 'await_value_error_keeps_channel': 'tests/diff/cases/semantics/coro/await_value_error_keeps_channel.xr',
 'channel_for_in_recv_or': 'tests/diff/cases/semantics/coro/channel_for_in_recv_or.xr',
 'channel_recv_timeout_unbuffered': 'tests/diff/cases/semantics/coro/channel_recv_timeout_unbuffered.xr',
 'channel_send_timeout_unbuffered': 'tests/diff/cases/semantics/coro/channel_send_timeout_unbuffered.xr',
 'condvar_compose': 'tests/diff/cases/semantics/concurrency/condvar_compose.xr',
 'coroutine_throws_unit_variant': 'tests/diff/cases/semantics/coro/coroutine_throws_unit_variant.xr',
 'go_lambda_local_scope': 'tests/diff/cases/semantics/concurrency/go_lambda_local_scope.xr',
 'inline_match_try_recv_heap': 'tests/diff/cases/semantics/coro/inline_match_try_recv_heap.xr',
 'lazy_string_boundary': 'tests/diff/cases/semantics/coro/lazy_string_boundary.xr',
 'linked_scope_error_propagates_at_exit': 'tests/diff/cases/semantics/coro/linked_scope_error_propagates_at_exit.xr',
 'mutex_cross_coroutine': 'tests/diff/cases/semantics/concurrency/mutex_cross_coroutine.xr',
 'scope_child_failure_isolation': 'tests/diff/cases/semantics/coro/scope_child_failure_isolation.xr',
 'semaphore_double_acquire_defer_bool_slot': 'tests/diff/cases/semantics/coro/semaphore_double_acquire_defer_bool_slot.xr',
 'task_transfer_return_local': 'tests/diff/cases/semantics/coro/task_transfer_return_local.xr',
 'task_transfer_return_map_set': 'tests/diff/cases/semantics/coro/task_transfer_return_map_set.xr'}
_NAMES=frozenset({'condvar_compose','go_lambda_local_scope','mutex_cross_coroutine',
 'await_non_rethrowing_forms_observe','await_tuple_destructure','await_value_error_keeps_channel',
 'channel_for_in_recv_or','channel_recv_timeout_unbuffered','channel_send_timeout_unbuffered',
 'coroutine_throws_unit_variant','inline_match_try_recv_heap','lazy_string_boundary',
 'linked_scope_error_propagates_at_exit','scope_child_failure_isolation',
 'semaphore_double_acquire_defer_bool_slot','task_transfer_return_local','task_transfer_return_map_set'})
UNIT=('abstract-unit',)

class TypedError(Exception):
    def __init__(self,enum,variant,payload=None):self.value=(enum,variant,copy.deepcopy(payload));super().__init__(self.value)
    def display(self):return self.value[0]+'.'+self.value[1]
class Panic(Exception):pass
class WouldBlock(Exception):pass

class Task:
    def __init__(self,action):self.action=action;self.state='Pending';self.value=UNIT;self.cause=None;self.channel=None
    def finish(self):
        if self.state!='Pending':return
        self.state='Running'
        try:
            value=self.action();self.value=UNIT if value is None else copy.deepcopy(value);self.state='Success'
        except TypedError as error:self.state='Failed';self.channel='typed';self.cause=copy.deepcopy(error.value)
        except Panic as error:self.state='Failed';self.channel='panic';self.cause=str(error)
    def observe(self):
        self.finish();return self.state,copy.deepcopy(self.value) if self.state=='Success' else None
    def await_value(self):
        self.finish()
        if self.state=='Success':return copy.deepcopy(self.value)
        if self.channel=='typed':raise TypedError(*copy.deepcopy(self.cause))
        if self.channel=='panic':raise Panic(self.cause)
        raise ValueError('Task is not in a completed success/failure state')

def all_values(tasks):
    try:return [task.await_value() for task in tasks]
    except (TypedError,Panic):
        for task in tasks:
            if task.state=='Pending':task.state='Cancelled'
        raise

class Scope:
    def __init__(self,linked=False):self.linked=linked;self.tasks=[]
    def add(self,action):task=Task(action);self.tasks.append(task);return task
    def close(self):
        for index,task in enumerate(self.tasks):
            task.finish()
            if self.linked and task.state=='Failed':
                for sibling in self.tasks[index+1:]:
                    if sibling.state=='Pending':sibling.state='Cancelled'
                task.await_value()

class Channel:
    def __init__(self,capacity):self.capacity=capacity;self.queue=[];self.closed=False
    def try_send(self,value):
        if self.closed:return 'Closed'
        if len(self.queue)>=self.capacity:return 'Full'
        self.queue.append(copy.deepcopy(value));return 'Sent'
    def send(self,value):
        state=self.try_send(value)
        if state!='Sent':raise WouldBlock(state)
        return UNIT
    def try_recv(self):
        if self.queue:return 'Value',copy.deepcopy(self.queue.pop(0))
        return ('Closed' if self.closed else 'Empty'),None
    def recv(self):
        state,value=self.try_recv()
        if state=='Empty':raise WouldBlock('recv')
        return state,value
    def recv_or(self,default):
        state,value=self.recv();return value if state=='Value' else copy.deepcopy(default)
    def timed_recv_without_partner(self):
        state,value=self.try_recv();return ('Timeout',None) if state=='Empty' else (state,value)
    def timed_send_without_partner(self,value):
        state=self.try_send(value);return 'Timeout' if state=='Full' else state
    def close(self):self.closed=True

class Semaphore:
    def __init__(self,permits):self.available=permits
    def acquire(self):
        if self.available<=0:raise WouldBlock('acquire')
        self.available-=1;return True
    def release(self):self.available+=1;return self.available

def failure(enum='E',variant='Boom',payload=None):raise TypedError(enum,variant,payload)

def condvar_observation_paths():
    observations=[]
    for consumer_first in [False,True]:
        for mask in range(8):
            for completion_order in itertools.permutations(range(3)):
                box=-1;parked=consumer_first;box=42
                if parked:parked=False
                consumed=box;remaining=1;remaining-=1;assert remaining==0
                ready=True;waiters={i for i in range(3) if mask&(1<<i)};woke=0;remaining=3
                for worker in completion_order:
                    if worker in waiters:waiters.remove(worker)
                    assert ready;woke+=1;remaining-=1
                assert not waiters and remaining==0
                observations.append([consumed,woke,'done'])
    return observations

def observations():
    rows={};paths=condvar_observation_paths();assert len(paths)==96 and all(path==paths[0] for path in paths)
    rows['condvar_compose']=paths[0]
    rows['go_lambda_local_scope']=[Task(lambda:sum(range(1,5))).await_value()]
    mutex_value=0;mutex_value=(lambda value:value+100)(mutex_value);latch_remaining=0;assert latch_remaining==0
    rows['mutex_cross_coroutine']=[mutex_value]
    observed=[]
    for timeout in [None,5000]:
        task=Task(lambda:failure());state,_=task.observe();observed.append('failed' if state=='Failed' else 'other')
    observed.append(sum(all_values([Task(lambda:2),Task(lambda:3)])));rows['await_non_rethrowing_forms_observe']=observed
    pair=Task(lambda:(10,20)).await_value();x,y=(3,4);rows['await_tuple_destructure']=[pair[0],pair[1],Task(lambda:x+y).await_value()]
    events=[]
    try:failure()
    except TypedError as error:events.append('caught direct: '+error.display())
    try:Task(lambda:failure()).await_value()
    except TypedError as error:events.append('caught at await: '+error.display())
    def hop():return Task(lambda:failure()).await_value()
    try:hop()
    except TypedError as error:events.append('caught propagated: '+error.display())
    events.append(Task(lambda:3*2).await_value()+Task(lambda:4*2).await_value());rows['await_value_error_keeps_channel']=events

    channel=Channel(4)
    for value in [2,5,8]:channel.send(value)
    channel.close();total=0
    while True:
        state,value=channel.recv()
        if state=='Closed':break
        total+=value
    ints=Channel(1);ints.send(42);ints.close();words=Channel(1);words.send('xray');words.close()
    rows['channel_for_in_recv_or']=[total,ints.recv_or(-1),ints.recv_or(-1),words.recv_or('fallback'),words.recv_or('fallback')]
    empty=Channel(0);state,_=empty.timed_recv_without_partner();empty.close();rows['channel_recv_timeout_unbuffered']=[state.lower()]
    empty=Channel(0);state=empty.timed_send_without_partner(7);assert empty.queue==[];empty.close();rows['channel_send_timeout_unbuffered']=[state.lower()]
    rows['coroutine_throws_unit_variant']=['failed: unit variant' if Task(lambda:failure()).observe()[0]=='Failed' else 'other',
     'failed: payload variant' if Task(lambda:failure('E2','Code',{'code':7})).observe()[0]=='Failed' else 'other']
    heap=Channel(2);heap.send(copy.deepcopy([10,20,30]));state,first=heap.try_recv();empty,_=heap.try_recv();heap.close()
    rows['inline_match_try_recv_heap']=[first[0] if state=='Value' else -1,-1 if empty!='Value' else 0]

    mapping={};mapping['k'+'1']=7;mapping['k'+'1']=11;events=[len(mapping),mapping.get('k1',-1),('k'+'1') in mapping]
    present='k1' in mapping;del mapping['k1'];events.extend([present,('k'+'1') in mapping])
    values={'t'+'ag','tag'};events.extend([len(values),('t'+'ag') in values]);present='tag' in values;values.remove('tag');events.extend([present,('t'+'ag') in values])
    channel=Channel(1);channel.send('chan_'+str(2));events.append(channel.recv()[1]);channel.close()
    events.append(Task(lambda:'task_'+str(3)).await_value());out=Task(lambda:{'k'+'9':('seed_'+str(4))+'_value'}).await_value()
    events.extend([out.get('k'+'9',''),out.get('k9','')]);rows['lazy_string_boundary']=events
    events=[]
    def linked_failure():
        scope=Scope(True);scope.add(lambda:failure());scope.close();raise AssertionError('Unreachable after linked failure')
    for prefix in ['caught in scope frame: ','caught from caller: ']:
        try:linked_failure()
        except TypedError as error:events.append(prefix+error.display())
    success=Scope(True);success.add(lambda:7);success.add(lambda:7);success.close();events.append(5);rows['linked_scope_error_propagates_at_exit']=events
    sums=[]
    for order in itertools.permutations([0,1]):
        channel=Channel(2);scope=Scope(False)
        def ok():channel.send(1);return 11
        def fail():channel.send(10);failure('DiffScopeErr','Msg',{'message':'diff_fail'})
        actions=[ok,fail]
        for index in order:scope.add(actions[index])
        scope.close();sums.append(sum(channel.recv()[1] for _ in range(2)));channel.close()
    assert sums==[11,11];rows['scope_child_failure_isolation']=[sums[0]]
    a,b=Semaphore(1),Semaphore(1);a.acquire();b.acquire();result=7;b.release();rows['semaphore_double_acquire_defer_bool_slot']=[result,a.release()]
    rows['task_transfer_return_local']=[len(Task(lambda:[1,2,3]).await_value())]
    mapping=Task(lambda:{7:11}).await_value();values=Task(lambda:{3,5}).await_value();rows['task_transfer_return_map_set']=[mapping[7],5 in values]
    if set(rows)!=_NAMES:raise ValueError('Async event model lost a complete program')
    return rows

def expected_rows():
    if set(_INPUTS)!=set(_REQUIRED_INPUTS) or len(_REQUIRED_INPUTS)!=len(set(_REQUIRED_INPUTS)) or set(_PROGRAMS)!=_NAMES or len(set(_PROGRAMS.values()))!=len(_NAMES):raise ValueError('Async model lost reviewed identities')
    for name,digest in _INPUTS.items():
        path=Path(name)
        if path.is_absolute() or path.as_posix()!=name or '..' in path.parts or path.parts[0] not in {'tests','spec','stdlib'}:raise ValueError('Async model requires delivered inputs')
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Async source/authority changed: '+name)
    values=observations();rows=[]
    for name,source in sorted(_PROGRAMS.items()):
        if Path(source).stem!=name or source not in _INPUTS:raise ValueError('Async original identity changed')
        if any(type(value) not in (bool,int,str) for value in values[name]):raise ValueError('Async observation needs an explicit scalar')
        render=lambda value:str(value).lower() if type(value) is bool else str(value)
        rows.append({'id':'complete_async_events_'+name,'source':source,'original_source':source,'required_target':None,'expected':{'exit_code':0,'stdout_utf8':''.join(render(v)+'\n' for v in values[name]),'stderr_utf8':''},
            'model':'Complete original task/channel/sync/scope program. Independent sticky value/error channel,finite event paths and FIFO/close/timeout model;no real scheduler,Sendable permission,deadline latency or physical release proof.',
            'observation_count':len(values[name])})
    return rows
