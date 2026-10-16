"""Complete coordination consumers, preserving legacy requirements and conflicts.

WorkQueue old tests require own-shard LIFO and a unique remaining shard for
stealing. The current source instead projects one FIFO. Both facts are kept;
normal expected bodies are legacy acceptance requirements, not a claim that
current sync implements them. Blocking traces prove finite value invariance,
not actual sleep, wakeup, scheduling, cancellation or resource behavior.
"""
from collections import deque
from pathlib import Path
import hashlib,itertools,re

ROOT=Path(__file__).resolve().parents[4]
BASE='tests/regression/11_coroutine/'
_PROGRAMS={
 'work_queue_native_methods':'tests/diff/cases/semantics/coro/work_queue_native_methods.xr',
 **{n:BASE+n+'.xr' for n in ['1157_work_queue_generic_class','1158_work_queue_native',
 '1159_work_queue_blocking_pop','1160_work_queue_close_fanout','1165_result_group_native']}}
_NAMED={'1157_work_queue_generic_class':'test_work_queue_generic_class','1158_work_queue_native':'test_native_work_queue',
 '1159_work_queue_blocking_pop':'test_work_queue_blocking_pop','1160_work_queue_close_fanout':'test_work_queue_close_fanout',
 '1165_result_group_native':'test_result_group_native'}
_SITES={'work_queue_native_methods':(7,10),'1157_work_queue_generic_class':(4,0),
 '1158_work_queue_native':(17,16),'1159_work_queue_blocking_pop':(3,1),
 '1160_work_queue_close_fanout':(4,0),'1165_result_group_native':(11,7)}
_AUTHORITIES=('spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md','spec/source/sections/006-5-declarations.md',
 'spec/source/sections/011-10-concurrency-and-coroutines.md','spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md','spec/source/sections/016-15-standard-library.md',
 'spec/source/sections/018-17-compilation-pipeline.md','stdlib/sync/sync.xr','stdlib/time/time.xr',
 'stdlib/types/channel.xr','stdlib/types/atomic.xr')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa',
 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440',
 'stdlib/sync/sync.xr': '3ee7610b17475e37077fb701c8f32b31f3908285022e80d1b442e887591c8395',
 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72',
 'stdlib/types/atomic.xr': '4da0e8355fdbb9e22a2cc5552f234d171b389302ce29698a19399ec40b2f67e4',
 'stdlib/types/channel.xr': 'ff2be02bcc28be44f7927b746d316eec7fef4e815f2693207ef07176bbbf1284',
 'tests/diff/cases/semantics/coro/work_queue_native_methods.xr': '30324910f069d981c14580d235d5eb90395b41d64b2e9d1ed3871d8a3e752200',
 'tests/regression/11_coroutine/1157_work_queue_generic_class.xr': '843c4457b8f52f49a629ed278dd0d3021ccb3f35e2f975cfa25e1507ee977aba',
 'tests/regression/11_coroutine/1158_work_queue_native.xr': '229c73c67aab542be8ccc52402536d6da4d1be0334c4d798533a537dbfa54ed5',
 'tests/regression/11_coroutine/1159_work_queue_blocking_pop.xr': 'f9c1df9573a00e306c7a388a0d4d3995836e0fd43d6861f3ebee62d3e065e34a',
 'tests/regression/11_coroutine/1160_work_queue_close_fanout.xr': 'a10afc445848bd4eb65dc76050665629165e79dbe423a196f545a930f009d971',
 'tests/regression/11_coroutine/1165_result_group_native.xr': '5df403fb091f1126c2a79ee44b2ada9062946ceb54999aa9fa2c978c4c3c9b80'}


class Trace:
    def __init__(self):self.facts=[]
    def equal(self,label,actual,expected):
        passed=type(actual) is type(expected) and actual==expected
        self.facts.append({'label':label,'actual':actual,'expected':expected,'passed':passed})
        if not passed:raise ValueError('Original coordination assertion failed: '+label)
    def truth(self,label,value):self.equal(label,value,True)


class LegacyQueue:
    """Required old test policy; do not equate it with current sync FIFO."""
    def __init__(self,shards,capacity):
        if shards<=0 or capacity<=0:raise ValueError('Only original positive capacities modeled')
        self.shards=[[] for _ in range(shards)];self.closed=False;self.capacity=capacity
    def __len__(self):return sum(map(len,self.shards))
    def push(self,value,shard):
        if self.closed:return False
        if not 0<=shard:raise ValueError('Unreviewed implicit shard selection')
        self.shards[shard%len(self.shards)].append(value);return True
    def push_range(self,start,count,shard):
        accepted=0
        for i in range(max(count,0)):
            if not self.push(start+i,shard+i):break
            accepted+=1
        return accepted
    def pop(self,worker):
        own=self.shards[worker%len(self.shards)]
        if own:return own.pop(),True
        occupied=[q for q in self.shards if q]
        if len(occupied)>1:raise ValueError('Original tests do not freeze a multi-victim tie policy')
        return (occupied[0].pop(0),True) if occupied else (None,False)
    def close(self):self.closed=True


def native_trace(extended=False):
    q=LegacyQueue(4,2);t=Trace();eq=t.equal;ok=t.truth
    eq('shardCount',len(q.shards),4);eq('initial length',len(q),0);ok('initial open',not q.closed)
    for value,shard in [(1,0),(2,0),(3,1)]:ok('push'+str(value),q.push(value,shard))
    eq('length3',len(q),3);pops=[]
    for worker,value in [(0,2),(1,3),(2,1)]:
        found,success=q.pop(worker);ok('pop'+str(worker)+' success',success);eq('pop'+str(worker)+' value',found,value);pops.append(found)
    _,success=q.pop(0);ok('empty',not success);eq('empty length',len(q),0)
    ranged=[]
    if extended:
        eq('pushRange accepted',q.push_range(10,6,0),6);eq('range length',len(q),6)
        for i,value in enumerate([14,15,12,13,10,11]):
            got,success=q.pop(i%4);ok('range'+str(i)+' success',success);eq('range'+str(i)+' value',got,value);ranged.append(got)
        eq('range drained',len(q),0)
    q.close();ok('closed',q.closed);ok('closed push refused',not q.push(9,0))
    if extended:eq('closed pushRange refused',q.push_range(20,2,0),0)
    if len(t.facts)!=(33 if extended else 17):raise ValueError('Original queue assertion count lost')
    return {'assertions':t.facts,'pop_values':pops,'pushRange_values':ranged,'closed':q.closed,
        'final_length':len(q),'required_policy':'LEGACY_OWN_SHARD_LIFO_UNIQUE_REMAINING_SHARD_STEAL',
        'current_source_policy_conflict':'OPEN_SINGLE_FIFO_NOT_EQUIVALENT'}


def fifo_source_witness():
    # Algebra projection of the exact source push/shift bodies, not CLI output.
    pending=deque()
    for value,shard in [(1,0),(2,0),(3,1)]:pending.append(value)
    actual=[pending.popleft() for worker in [0,1,2]]
    for i in range(6):pending.append(10+i)
    ranged=[pending.popleft() for worker in [0,1,2,3,0,1]]
    return {'current_source_projected_pops':actual,'required_original_pops':[2,3,1],
        'current_source_projected_range':ranged,'required_original_range':[14,15,12,13,10,11],
        'equivalent':actual==[2,3,1] and ranged==[14,15,12,13,10,11],'classification':'SOURCE_ALGEBRA_CONFLICT_NOT_PRODUCT_RUNTIME_OBSERVATION'}


def generic_trace():
    shards=[deque() for _ in range(4)];t=Trace()
    for value,shard in [(10,0),(20,1),(30,2)]:shards[shard].append(value)
    for worker,expected in [(0,10),(1,20),(2,30),(3,None)]:
        index=next((i for i in [(worker+j)%4 for j in range(4)] if shards[i]),None)
        value=shards[index].popleft() if index is not None else None
        t.equal('tryPop'+str(worker),value,expected)
    return {'assertions':t.facts,'constructed_channels':4,'channel_capacity':8,'closed_channels':4,
        'channel_values_drained':True,'generic_methods_and_all_helper_bodies_preserved':True}


def blocking_trace():
    variants=[]
    for order in [('consumer','producer'),('producer','consumer')]:
        q=deque();waiting=False;value=None
        for event in order:
            if event=='consumer':
                if q:value=q.popleft()
                else:waiting=True
            elif waiting:value=42;waiting=False
            else:q.append(42)
        if value is None:value=q.popleft()
        t=Trace();t.truth('producer push',True);t.equal('await producer',42,42);t.equal('await consumer',value,42)
        t.equal('closed empty pop',None,None)
        variants.append({'event_order':list(order),'assertions':t.facts,'final_length':len(q)})
    return {'assertions':variants[0]['assertions'],'finite_completion_variants':variants,
        'required_original_sleep_calls':[1],'spawned_tasks':2,'closed_queue':True,
        'actual_block_and_wakeup':'OPEN;time.sleep(1) does not prove consumer already blocked'}


def fanout_trace():
    variants=[]
    for events in itertools.permutations([0,1,2,3,'close']):
        closed=False;waiters=set();results={}
        for event in events:
            if event=='close':
                closed=True
                for waiter in waiters:results[waiter]=-1
                waiters.clear()
            elif closed:results[event]=-1
            else:waiters.add(event)
        if waiters or [results[i] for i in range(4)]!=[-1]*4:raise ValueError('Close fanout finite trace lost waiter')
        variants.append({'events':list(events),'await_all_values':[results[i] for i in range(4)]})
    t=Trace()
    for i,value in enumerate(variants[0]['await_all_values']):t.equal('result'+str(i),value,-1)
    return {'assertions':t.facts,'finite_completion_variants':variants,'required_original_sleep_calls':[1],
        'spawned_tasks':4,'all4_await_results_preserved':True,'actual_wake_fanout':'OPEN_REQUIRES_REAL_SCHEDULER'}


def result_group_trace():
    # Independent batch partitioning; only original observed counters are asserted.
    t=Trace();eq=t.equal;ok=t.truth;batch=2;pending=[];ready=[];closed=False
    def add(value):
        if closed:return False
        pending.append(value)
        if len(pending)%batch==0:ready.append(sum(pending[-batch:]))
        return True
    eq('batchSize',batch,2);eq('initial ready',len(ready),0);ok('initial open',not closed)
    ok('add4',add(4));eq('ready after4',len(ready),0);eq('pending after4',len(pending),1)
    ok('add5',add(5));eq('ready after5',len(ready),1);eq('pending after5',len(pending),2)
    eq('recv9',ready.pop(0),9);pending.clear();eq('ready after recv',len(ready),0);eq('pending after recv',len(pending),0)
    ok('tryRecv empty',not ready);ok('add3',add(3));ready.append(sum(pending));closed=True;ok('closed',closed)
    eq('recv partial3',ready.pop(0),3);eq('closed recv empty',None,None);ok('closed add refused',not add(1))
    if len(t.facts)!=18:raise ValueError('Original ResultGroup assertion count lost')
    return {'assertions':t.facts,'accepted_values':[4,5,3],'received_batches':[9,3],'closed':closed,
        'partial_counter_source_arithmetic_projection':1-batch,
        'unobserved_partial_counter_policy':'OPEN;original test has no pendingCount read after partial-batch recv;do not invent a successful invariant'}


def body_traces():
    return {'work_queue_native_methods':native_trace(), '1157_work_queue_generic_class':generic_trace(),
        '1158_work_queue_native':native_trace(True),'1159_work_queue_blocking_pop':blocking_trace(),
        '1160_work_queue_close_fanout':fanout_trace(),'1165_result_group_native':result_group_trace()}


def _validate():
    if set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values()) or len(_PROGRAMS)!=6 or len(_NAMED)!=5:
        raise ValueError('Coordination complete original source or authority identities changed')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Coordination complete source or authority changed: '+path)
    for name,source in _PROGRAMS.items():
        text=(ROOT/source).read_text(encoding='utf-8')
        if (text.count('assertEqual('),len(re.findall(r'\bassert\(',text)))!=_SITES[name]:raise ValueError('Original coordination assertions changed')
        if name in _NAMED and ('fn '+_NAMED[name]+'(' not in text or text.count('@test')!=1):raise ValueError('Complete named body identity changed')
    source=(ROOT/'stdlib/sync/sync.xr').read_text(encoding='utf-8')
    if 'var value = this._items.shift()' not in source or 'this._pending.fetchSub(this._batchSize' not in source:
        raise ValueError('Recorded source-policy conflicts need fresh review')


def expected_rows():
    _validate();traces=body_traces();rows=[]
    for name,source in _PROGRAMS.items():
        row={'id':'complete_coordination_'+name,'source':source,'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':'workqueue native ok\n' if name=='work_queue_native_methods' else '','stderr_utf8':''},
            'observation_count':1 if name=='work_queue_native_methods' else 0,
            'model':'Complete original coordination entry with separate named-body duty;legacy per-shard queue requirements and current FIFO conflict both retained. Finite completion/batch models do not qualify actual source,blocking/wakeup/scheduler/atomic/resource/lifetime/provider behavior.'}
        if name in _NAMED:row['separate_named_body_obligation']={'name':_NAMED[name],'executed_by_run':False,
            'qualification':'OPEN_REQUIRES_NAMED_TEST_EXECUTION','trace':traces[name]}
        else:row['required_semantic_trace']=traces[name]
        if name in {'work_queue_native_methods','1158_work_queue_native'}:row['current_source_policy_witness']=fifo_source_witness()
        rows.append(row)
    return rows


def named_expected_rows():
    _validate();traces=body_traces();rows=[]
    for name,function in _NAMED.items():
        trace=traces[name]
        rows.append({'id':'complete_coordination_named_'+name,'source':_PROGRAMS[name],'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':'','stderr_utf8':'','required_test_names':[function]},
            'required_original_assertion_results':trace['assertions'],'required_semantic_trace':trace,
            'model':'Complete unchanged named body with all original asserts/helpers/sleep calls and separate CLI discovery plus VM/C/native execution. Legacy own-LIFO queue requirement is not current FIFO;finite symbolic value invariance is not actual wakeup/scheduler/resource/provider qualification.'})
    return rows
