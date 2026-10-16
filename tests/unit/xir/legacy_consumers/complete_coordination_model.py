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
    if set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values())|set(_SYSTEM_INPUTS) or len(_PROGRAMS)!=6 or len(_NAMED)!=5:
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
    return rows+system_expected_rows()


def named_expected_rows():
    _validate();traces=body_traces();rows=[]
    for name,function in _NAMED.items():
        trace=traces[name]
        rows.append({'id':'complete_coordination_named_'+name,'source':_PROGRAMS[name],'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':'','stderr_utf8':'','required_test_names':[function]},
            'required_original_assertion_results':trace['assertions'],'required_semantic_trace':trace,
            'model':'Complete unchanged named body with all original asserts/helpers/sleep calls and separate CLI discovery plus VM/C/native execution. Legacy own-LIFO queue requirement is not current FIFO;finite symbolic value invariance is not actual wakeup/scheduler/resource/provider qualification.'})
    return rows


# These remain ordinary run entries; none replaces the five named queue bodies.
_SYSTEM_PROGRAMS={name:'tests/diff/cases/semantics/stdlib/'+name+'.xr' for name in (
    'sys_threadlocal_recursive_init','sys_condvar_direct',
    'sys_thread_os_primitives_cross_thread','sync_module_import')}
_SYSTEM_NAMES=frozenset({'sys_threadlocal_recursive_init','sys_condvar_direct',
    'sys_thread_os_primitives_cross_thread','sync_module_import'})
_SYSTEM_INPUTS={"stdlib/sys/sys.xr":"8e508319d317801403b633b8f27c7fb6154be5f0743ea2bdb6f1fca52bba3471","tests/diff/cases/semantics/stdlib/sys_threadlocal_recursive_init.xr":"aceada7c058a409dbd37e1af86987a0b7497a68505f6ed55b38af79d27ba3108","tests/diff/cases/semantics/stdlib/sys_condvar_direct.xr":"dc14e1ed7574f59e7478ef7a97748833f0ba1ef5919fc0d3dca515409b41b27a","tests/diff/cases/semantics/stdlib/sys_thread_os_primitives_cross_thread.xr":"858c01c1b18cf9e0d1e212926cd98ec66a909b161a285f993cac5c737ef58239","tests/diff/cases/semantics/stdlib/sync_module_import.xr":"7d33f834499a53b238bae4a04dce65d70ecde1f83af53a48dc2dfaa9caeb5ef4"}
_INPUTS.update(_SYSTEM_INPUTS)


class SerializedCell:
    """Finite critical-section value algebra; never a host lock or scheduler."""
    def __init__(self,value):self.value=value;self.held=False;self.writes=0
    def access(self,body,replace=False,try_only=False):
        if self.held:
            if try_only:return None
            raise ValueError('Uncompleted critical section cannot reenter')
        self.held=True
        try:
            result=body(self.value)
            if replace:self.value=result;self.writes+=1
            return result
        finally:self.held=False
    def replace(self,body):return self.access(body,replace=True)


class OnceCompletion:
    def __init__(self):self.done=False;self.calls=0
    def call(self,body):
        if not self.done:
            body();self.calls+=1;self.done=True


class BarrierCompletion:
    def __init__(self,parties):
        if type(parties) is not int or parties<=0:raise ValueError('Invalid barrier parties')
        self.parties=parties;self.arrivals=set();self.released=False
    def arrive(self,thread):
        if self.released or thread in self.arrivals:raise ValueError('Only one original barrier generation is modeled')
        self.arrivals.add(thread);self.released=len(self.arrivals)==self.parties
    def result(self,thread):
        if not self.released or thread not in self.arrivals:raise ValueError('Barrier result read before all original parties arrive')
        return True


class RecursiveInitialization(Exception):pass


class LocalInitialization:
    """Per-thread empty/initializing/ready transitions, including failed init."""
    def __init__(self,initializer):
        self.initializer=initializer;self.states={};self.values={};self.initializations=0
    def get(self,thread):
        state=self.states.get(thread,'empty')
        if state=='ready':return self.values[thread]
        if state=='initializing':raise RecursiveInitialization('Same-thread recursive initialization')
        self.states[thread]='initializing';self.initializations+=1
        try:
            value=self.initializer()
            if self.states.get(thread)!='ready':self.set(thread,value)
            return self.values[thread]
        except Exception:
            if self.states.get(thread)=='initializing':self.states.pop(thread)
            raise
    def set(self,thread,value):self.values[thread]=value;self.states[thread]='ready'


def threadlocal_trace():
    local=LocalInitialization(lambda:local.get('main'));output=[]
    try:output.append(local.get('main'))
    except RecursiveInitialization:output.append('recursive')
    recovered=LocalInitialization(lambda:0);recovered.set('main',7);output.append(recovered.get('main'))
    if output!=['recursive',7] or local.states or local.values or recovered.initializations:
        raise ValueError('Complete recursive ThreadLocal failure/recovery requirement changed')
    return {'outputs':output,'failed_initializer_calls':local.initializations,
        'failed_initializing_flags':dict(local.states),'failed_cached_values':dict(local.values),
        'second_instance_initializer_calls':recovered.initializations,'second_instance_value':recovered.values['main'],
        'actual_thread_identity_pruning_storage_and_cleanup':'OPEN_REQUIRES_NATIVE_FI_LIFETIME'}


def condition_completion(producer_first,initial=0,produced=7,joined=9):
    # Predicate checks and lock release/reacquire are the only legal observations.
    state=initial;consumer_waits=0;epoch=0;lock_owner='consumer';events=[]
    if not producer_first:
        if lock_owner!='consumer' or state!=initial:raise ValueError('Invalid consumer entry')
        events+=['observe predicate','release monitor','wait for changed predicate'];consumer_waits+=1;lock_owner=None
    else:
        events+=['release monitor before producer'];lock_owner=None
    if lock_owner is not None:raise ValueError('Producer needs the monitor')
    lock_owner='producer';state=produced;epoch+=1
    events+=['producer acquires monitor','release store'+str(produced),'signal','producer releases monitor'];lock_owner=None
    lock_owner='consumer';events+=['consumer acquires monitor','acquire load'+str(state),'consumer releases monitor'];lock_owner=None
    return {'producer_first':producer_first,'consumer_waits':consumer_waits,'events':events,
        'observed_state':state,'producer_join':joined,'epoch':epoch,'monitor_released':lock_owner is None}


def timed_condition_trace():
    # No other thread exists in this complete program. Neither signal nor
    # broadcast occurs before waitFor returns, so its observed epoch cannot change.
    timeout_ns=1000000;clock_samples=[0,1,timeout_ns-1,timeout_ns]
    locked=True;observed_epoch=0;current_epoch=0;deadline=clock_samples[0]+timeout_ns
    locked=False;result=None
    for now in clock_samples[1:]:
        if current_epoch!=observed_epoch:result=True;break
        if now>=deadline:result=False;break
    locked=True
    if result is not False or not locked:raise ValueError('Original timed condition wait must reacquire before timeout result')
    locked=False;current_epoch+=1;current_epoch+=1
    return {'outputs':[result,True],'timeout_ns':timeout_ns,'clock_samples':clock_samples,
        'signals_before_wait_return':0,'signals_after_unlock':2,'final_epoch':current_epoch,
        'deadline_comparison':'now >= start+1000000','lock_reacquired_before_return':True,
        'actual_clock_wait_yield_and_native_spurious_behavior':'OPEN_REQUIRES_PAIRED_SOURCE_AND_PROVIDER_EXECUTION'}


def os_thread_trace():
    variants=[]
    # Critical sections are indivisible in this algebra. Four blocks expose all
    # worker identities; arbitrary transaction interleavings share the induction
    # count'=count+1 and the same 4*200 completed transactions. This does not
    # enumerate native instruction schedules or prove progress/fairness.
    for order in itertools.permutations(range(4)):
        guarded=SerializedCell(0);completed=[0]*4
        for worker in order:
            for _ in range(200):guarded.replace(lambda value:value+1);completed[worker]+=1
        if guarded.value!=800 or guarded.writes!=800 or completed!=[200]*4 or guarded.held:
            raise ValueError('Complete four-worker200-loop mutual exclusion requirement changed')
        variants.append({'worker_block_order':list(order),'iterations':completed,
            'guarded':guarded.value,'completed_critical_sections':guarded.writes,'join_sum':sum(completed)})
    barriers=[]
    for order in itertools.permutations(range(3)):
        barrier=BarrierCompletion(3);arrived=0
        for thread in order:arrived+=1;barrier.arrive(thread)
        results=[barrier.result(thread) for thread in range(3)];reads=[arrived]*3
        barriers.append({'arrival_order':list(order),'all_wait_results':results,'post_barrier_reads':reads,'sum':sum(reads)})
    once_variants=[]
    for order in itertools.permutations(range(3)):
        once=OnceCompletion();count=SerializedCell(0);loads={}
        for thread in order:
            once.call(lambda:count.replace(lambda value:value+1));loads[thread]=count.value
        once_variants.append({'entry_order':list(order),'callback_calls':once.calls,'thread_loads':[loads[i] for i in range(3)],'count':count.value})
    conditions=[condition_completion(first) for first in (False,True)]
    output=[variants[0]['join_sum'],variants[0]['guarded'],conditions[0]['observed_state'],conditions[0]['producer_join'],
        barriers[0]['all_wait_results'][0],barriers[0]['sum'],
        *[v>=1 for v in once_variants[0]['thread_loads']],once_variants[0]['count']]
    if output!=[800,800,7,9,True,9,True,True,True,1]:raise ValueError('Original cross-thread outputs changed')
    return {'outputs':output,'mutual_exclusion_variants':variants,'condition_variants':conditions,
        'barrier_variants':barriers,'once_variants':once_variants,'spawned_threads':10,'joined_threads':10,
        'iterations_per_worker':200,'completed_critical_sections':800,'barrier_parties':3,'once_contenders':3,
        'interleaving_value_induction':{'initial':0,'step_delta':1,'exact_completed_transactions':800,'final':800},
        'actual_native_threads_memory_orders_wakeup_fairness_cancellation_and_storage':'OPEN_REQUIRES_REAL_VM_C_NATIVE_FI_SAFETY_PROVIDER'}


def sync_import_trace():
    m=SerializedCell(10);out=[]
    out.append(m.access(lambda value:value*2));out.append(m.access(lambda value:value+1,try_only=True))
    def nested(value):
        attempted=m.access(lambda nested_value:nested_value,try_only=True)
        return -7 if attempted is None else attempted
    out.append(m.access(nested));out.append(m.replace(lambda value:value+5))
    rw=SerializedCell('a');out.append(rw.access(lambda value:value+'!'))
    out.append(rw.replace(lambda value:value+'b'));out.append(rw.access(lambda value:value+'c'))
    fair_variants=[]
    for order in itertools.permutations([1,10]):
        fair=SerializedCell(0);remaining=2
        for delta in order:fair.replace(lambda value:value+delta);remaining-=1
        if remaining:raise ValueError('Both imported RwLock writers must complete')
        fair_variants.append({'writer_add_order':list(order),'result':fair.value,'latch_remaining':remaining})
    out.append(fair_variants[0]['result']);once=OnceCompletion();counter=SerializedCell(0)
    for _ in range(2):once.call(lambda:counter.replace(lambda value:value+1))
    out.append(counter.value);barrier=BarrierCompletion(1);barrier.arrive('main');out.append(barrier.result('main'))
    shared=SerializedCell(0);shared.replace(lambda value:value+100);out.append(shared.access(lambda value:value))
    conds=[]
    for producer_first in (False,True):
        # The same monitor predicate protocol as sys, with literal42 and latch1.
        trace=condition_completion(producer_first,-1,42,None)
        conds.append({'producer_first':producer_first,'consumer_waits':trace['consumer_waits'],
            'observed':trace['observed_state'],'consumed_latch_remaining':0,'output_order':[trace['observed_state'],'done']})
    out+=conds[0]['output_order']
    if out!=[20,11,-7,15,'a!','ab','abc',11,1,True,100,42,'done']:
        raise ValueError('Complete imported sync value or output-order requirement changed')
    return {'outputs':out,'fair_writer_variants':fair_variants,'condition_variants':conds,
        'rw_value_after_nonmutating_write':rw.value,'nested_try_lock_entered':False,'once_callback_calls':once.calls,
        'spawned_coroutines':4,'latch_initial_counts':[2,1,1],
        'all_imports_preserved':['Semaphore','CountdownLatch','EventCount','WorkQueue','ResultGroup','Mutex','RwLock','Once','Barrier','Condvar'],
        'actual_generic_resolution_locks_defers_wakeups_and_task_lifetime':'OPEN_REQUIRES_PAIRED_FULL_SOURCE_AND_SAFETY_EXECUTION'}


def system_traces():
    return {'sys_threadlocal_recursive_init':threadlocal_trace(),'sys_condvar_direct':timed_condition_trace(),
        'sys_thread_os_primitives_cross_thread':os_thread_trace(),'sync_module_import':sync_import_trace()}


def system_expected_rows():
    if set(_SYSTEM_PROGRAMS)!=_SYSTEM_NAMES or set(_SYSTEM_INPUTS)!=set(_SYSTEM_PROGRAMS.values())|{'stdlib/sys/sys.xr'}:
        raise ValueError('Complete synchronization program or authority identity changed')
    traces=system_traces();rows=[]
    for name,source in _SYSTEM_PROGRAMS.items():
        values=traces[name]['outputs']
        text=''.join(('true' if value is True else 'false' if value is False else str(value))+'\n' for value in values)
        rows.append({'id':'complete_coordination_'+name,'source':source,'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':text,'stderr_utf8':''},'observation_count':len(values),
            'required_semantic_trace':traces[name],
            'model':'Complete unchanged synchronization program with all imports,threads,4x200 iterations,joins,waits and callbacks. Independent finite completion/value algebra and failure-state cleanup;no claim of host scheduling,progress,memory-order,FI,resource,lifetime,sanitizer or provider qualification.'})
    return rows
