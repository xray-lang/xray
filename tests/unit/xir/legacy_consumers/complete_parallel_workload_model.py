"""Complete original parallel workloads with unchanged loop and body duties.

Pure task results and commutative channel reductions are independent of a
scheduler. All original counts, sleeps, buffer sizes and recursive depth remain
runtime obligations. Linked cancellation does not predict every child starts;
zero-output declaration entries do not execute their annotated test bodies.
"""
from pathlib import Path
from collections import Counter,deque
import hashlib,itertools,re
from independent_observation_format import render
ROOT=Path(__file__).resolve().parents[4]
BASE='tests/regression/11_coroutine/'
_NAMES=('1141_concurrent_producer_consumer','1146_multicore_string_concat','1147_preempt_test',
 '1148_scope_race_stress','1151_inject_queue_spill','1174_channel_tuple_buffer_transfer','1175_channel_tuple_scalar_transfer')
_PROGRAMS={n:BASE+n+'.xr' for n in _NAMES}
_NAMED={
 '1141_concurrent_producer_consumer':['test_main'],
 '1146_multicore_string_concat':['test_concurrent_short_concat','test_concurrent_long_concat','test_concurrent_mixed_concat',
    'test_concurrent_loop_concat','test_concurrent_threshold_boundary','test_concurrent_data_isolation','test_stress_concurrent_concat'],
 '1147_preempt_test':['test_preempt_test'],
 '1148_scope_race_stress':['test_scope_loses_no_children','test_scope_all_success','test_linked_first_error_deterministic'],
 '1151_inject_queue_spill':['test_inject_queue_spill_chain']}
_AUTHORITIES=('spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md','spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md','spec/source/sections/011-10-concurrency-and-coroutines.md',
 'spec/source/sections/014-13-built-in-functions.md','spec/source/sections/015-14-built-in-type-methods.md',
 'spec/source/sections/018-17-compilation-pipeline.md','stdlib/types/channel.xr','stdlib/types/coroutine.xr',
 'stdlib/types/string.xr','stdlib/types/array.xr','stdlib/mem/mem.xr','stdlib/time/time.xr',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440',
 'stdlib/mem/mem.xr': '9750e120d6b22f5c2caffd2d59d13238393ac6939c17827090a3829c33fa9dab',
 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72',
 'stdlib/types/array.xr': 'ac31595b3801ee1df3c3f3ab6f1903c5f66ef084f0fa016dc19d24a2fa653223',
 'stdlib/types/channel.xr': 'ff2be02bcc28be44f7927b746d316eec7fef4e815f2693207ef07176bbbf1284',
 'stdlib/types/coroutine.xr': 'f85c868bf12b2b90988ead5cf746fd8e522d5c2d6398a64882b47637c72dc7a6',
 'stdlib/types/string.xr': '95307d2e652f164c46b028ead9c6138f85ea783ee42143e4b6d8c60d89d607d7',
 'tests/regression/11_coroutine/1141_concurrent_producer_consumer.xr': 'f9436e174ca16f94f2181b0ff2b50b99f00a52c2dae86072e3aa2c90f1c0f5d5',
 'tests/regression/11_coroutine/1146_multicore_string_concat.xr': 'f873b61b11a0ff3fc4d3258b1b8f508e895ee8161330d2b6e0852efeaf38806e',
 'tests/regression/11_coroutine/1147_preempt_test.xr': '8214622466163d7f9835b5b1a0154a5b2632ef0cd7faf0944f6523d73fc91a19',
 'tests/regression/11_coroutine/1148_scope_race_stress.xr': '298b0d5d1fdd40431ee384822a77417d29b0f7ec80cdfcf3f241a433fd57378e',
 'tests/regression/11_coroutine/1151_inject_queue_spill.xr': 'ae419ce6243abfe696843fbba0c8b1ac600a59417c442b266e65222b93709182',
 'tests/regression/11_coroutine/1174_channel_tuple_buffer_transfer.xr': 'c6a5da5bba6050f46a7dd4e4223527763f9e13cda7e75467453089d0b7a91ced',
 'tests/regression/11_coroutine/1175_channel_tuple_scalar_transfer.xr': '6b358944b68ba4c6b6672046b06ef2e6b8756c9f04bab24bca5dc63e816a98d1',
 'tests/unit/xir/legacy_consumers/independent_observation_format.py': '09843df7bf351411b39877696ae17e489ee982062827d3a6ab8e8cad7ab1f68b'}


class Facts:
    def __init__(self):self.rows=[]
    def equal(self,label,actual,expected):
        passed=type(actual) is type(expected) and actual==expected
        self.rows.append({'label':label,'actual':actual,'expected':expected,'passed':passed})
        if not passed:raise ValueError('Original workload assertion failed: '+label)
    def truth(self,label,value):self.equal(label,value,True)


def ordered_results(values,completion_order):
    if sorted(completion_order)!=list(range(len(values))):raise ValueError('Task completions lost or duplicated an original identity')
    slots={index:values[index] for index in completion_order}
    return [slots[index] for index in range(len(values))]


def concat_value(kind,index):
    decimal=str(index)
    if kind=='short':return ''.join(['task_',decimal,'_done'])
    if kind=='long':return ''.join(['x'*50,'_id',decimal,'_','y'*50])
    if kind=='mixed':return 'short_'+decimal if index%2==0 else ''.join(['x'*50,'_long_',decimal,'_','y'*50])
    if kind=='loop':return '_'.join(['start',decimal,*map(str,range(5)),'end'])
    if kind=='boundary':return len('a'*(30+index%10)+'b'*(30+index%10))
    if kind=='isolation':
        character=chr(65+index%26);value=character*30+'_'+decimal+'_'+character*30
        return {'id':index,'result':value,'prefix_ok':value.startswith(character*30),
            'suffix_ok':value.endswith(character*30),'contains_id':'_'+decimal+'_' in value}
    if kind=='stress':return ['s'+decimal+'e','x'*40+'_'+decimal+'_'+'y'*40,decimal+'_a_b_c']
    raise ValueError('Unreviewed complete concat body')


def concat_valid(kind,index,value):
    decimal=str(index)
    if kind=='short':return value=='task_'+decimal+'_done'
    if kind=='long':return value.startswith('x'*50) and '_id'+decimal+'_' in value and value.endswith('y'*50)
    if kind=='mixed':return value=='short_'+decimal if index%2==0 else '_long_'+decimal+'_' in value
    if kind=='loop':return value.startswith('start_'+decimal) and value.endswith('_end')
    if kind=='boundary':return value==2*(30+index%10)
    if kind=='isolation':return value['prefix_ok'] and value['suffix_ok'] and value['contains_id']
    if kind=='stress':return value[0]=='s'+decimal+'e' and '_'+decimal+'_' in value[1] and value[2].startswith(decimal+'_')
    raise ValueError('Unreviewed complete concat predicate')


def concat_body(kind,heading):
    count=30 if kind=='loop' else 50;values=[concat_value(kind,i) for i in range(count)]
    results=ordered_results(values,list(reversed(range(count))))
    predicates=[concat_valid(kind,i,value) for i,value in enumerate(results)]
    facts=Facts();facts.equal(kind+' correct',sum(predicates),count)
    return {'stdout_utf8':heading+'\n','assertions':facts.rows,'spawned_tasks':count,'await_all_inputs':count,
        'full_task_results':results,'original_per_index_predicates':predicates,
        'original_inner_loop_iterations':count*5 if kind=='loop' else 0,'runtime_concurrency_and_buffer_isolation':'OPEN'}


def report_counts(messages,total):
    if len(messages)!=total or any(x not in (-1,1) for x in messages):raise ValueError('Complete scope report stream lost/changed a child')
    counts=Counter(messages);return counts[-1],counts[1]


def bodies():
    result={}
    fifo=deque([*range(100,103),*range(200,203)]);values=[fifo.popleft() for _ in range(6)]
    result['test_main']={'stdout_utf8':''.join(str(x)+'\n' for x in values)+'true\ndone\n','assertions':[],
        'channel_capacity':10,'try_send_calls':6,'try_recv_calls':6,'received_values':values,
        'original_producer_loop_iterations':[3,3],'explicit_channel_close_calls':0,'helpers_preserved':['recv_value','recv_ok']}
    kinds=['short','long','mixed','loop','boundary','isolation','stress']
    headings=['\n[测试1] 多协程并发短字符串拼接','\n[测试2] 多协程并发长字符串拼接','\n[测试3] 多协程混合短长字符串拼接',
        '\n[测试4] 高并发循环拼接','\n[测试5] 并发阈值边界拼接','\n[测试6] 并发数据隔离性验证','\n[测试7] 大规模并发压力测试']
    for name,kind,heading in zip(_NAMED['1146_multicore_string_concat'],kinds,headings):result[name]=concat_body(kind,heading)
    facts=Facts();facts.equal('received worker count',4,4)
    result['test_preempt_test']={'stdout_utf8':'启动 4 个协程...\n'+''.join('协程 '+str(i)+' 完成\n' for i in range(4))+'调度测试通过：4/4 协程完成\n',
        'assertions':facts.rows,'spawned_tasks':4,'channel_capacity':4,'original_worker_iterations':3,
        'required_sleep_calls_ms':[5]*12,'all24_completion_order_sorted_outputs':[sorted(x) for x in itertools.permutations(range(4))],
        'explicit_channel_close_calls':0,'actual_sleep_preemption_and_wakeup':'OPEN;no wall-clock predicate invented'}
    facts=Facts();sample=[-1]*200+[1]*50;negative,positive=report_counts(list(reversed(sample)),250)
    for iteration in range(50):
        facts.equal('scope'+str(iteration)+' failures',negative,200);facts.equal('scope'+str(iteration)+' successes',positive,50)
    result['test_scope_loses_no_children']={'stdout_utf8':'','assertions':facts.rows,'original_outer_iterations':50,
        'children_per_scope':250,'failing_children_per_scope':200,'successful_children_per_scope':50,
        'channel_capacity':250,'required_report_sends':12500,'required_report_receives':12500,
        'required_child_arithmetic_iterations':625000,'required_channel_close_calls':50,
        'plain_scope_policy':'Join all;failed child does not cancel siblings;report sent before throw'}
    facts=Facts();negative,positive=report_counts([1]*200,200)
    for iteration in range(5):facts.equal('success scope'+str(iteration)+' failures',negative,0);facts.equal('success scope'+str(iteration)+' successes',positive,200)
    result['test_scope_all_success']={'stdout_utf8':'','assertions':facts.rows,'original_outer_iterations':5,
        'children_per_scope':200,'channel_capacity':200,'required_report_sends':1000,'required_report_receives':1000,
        'required_child_arithmetic_iterations':50000,'required_channel_close_calls':5}
    facts=Facts();possible_messages=['err_'+str(index) for index in range(200)]
    for iteration in range(50):
        facts.truth('linked scope'+str(iteration)+' caught',bool(possible_messages))
        facts.truth('linked scope'+str(iteration)+' original child prefix',all('err_' in message for message in possible_messages))
    result['test_linked_first_error_deterministic']={'stdout_utf8':'','assertions':facts.rows,'original_outer_iterations':50,
        'declared_child_launch_bound_per_scope':200,'declared_arithmetic_loop_iterations_per_started_child':50,
        'possible_original_error_payloads':possible_messages,'exact_first_child_identity':'NOT_PREDICTED',
        'actual_started_and_completed_children':'NOT_PREDICTED;linked cancellation can stop siblings/body',
        'error_display_contains_original_payload_prefix':'REQUIRED_RUNTIME_PREDICATE;not an exact invented error rendering'}
    recurrence=list(itertools.accumulate([1]*2048,initial=0))
    facts=Facts();facts.equal('full spill depth',recurrence[-1],2048)
    result['test_inject_queue_spill_chain']={'stdout_utf8':'','assertions':facts.rows,'original_depth':2048,
        'required_recursive_go_calls':2048,'required_root_go_calls':1,'required_await_calls':2049,
        'recurrence_values':recurrence,'actual_queue_overflow_and_stack_safety':'OPEN;depth alone is not measured overflow'}
    if sum(len(x['assertions']) for x in result.values())!=219:raise ValueError('Original full workload assertions lost')
    return result


def tuple_traces():
    sizes=[(32,256,4096)[sequence%3] for sequence in range(256)]
    checksum=sum(sizes)+256*255//2
    frames=[{'sequence':i,'requested_zeroed_bytes':size,'received_sequence':i,'present_payload':True,
             'asBytes_length':size,'awaited_publisher_result':i} for i,size in enumerate(sizes)]
    buffer={'stdout_utf8':str(checksum)+'\n','required_tuple_frames':frames,'channel_capacity':1,
        'required_zeroed_allocations':256,'requested_size_multiplicities':{'32':86,'256':85,'4096':85},
        'required_send_calls':256,'required_recv_calls':256,'required_await_calls':256,'required_channel_close_calls':1,
        'payload_content_bytes_observed':0,'checksum':checksum,'actual_buffer_ownership_transfer_view_lifetime_and_physical_release':'OPEN'}
    triples=[(i+1,i+2,i+3) for i in range(4)];totals=[sum(sum(value) for value in permutation) for permutation in itertools.permutations(triples)]
    if set(totals)!={42}:raise ValueError('Original scalar tuple completion order changed result')
    scalar={'stdout_utf8':'42\n','full_published_triples':[list(x) for x in triples],
        'all24_completion_totals':totals,'channel_capacity':4,'required_child_tasks':4,
        'required_send_calls':4,'required_recv_calls':4,'required_channel_close_calls':1,
        'scope_join_and_tuple_typed_transfer':'OPEN_REQUIRES_REAL_EXECUTION'}
    return {'1174_channel_tuple_buffer_transfer':buffer,'1175_channel_tuple_scalar_transfer':scalar}


def _validate():
    if set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values()) or tuple(_PROGRAMS)!=_NAMES or len(_NAMED)!=5:
        raise ValueError('Parallel workload whole source/authority identities changed')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Complete parallel workload source or normative input changed: '+path)
    for name,names in _NAMED.items():
        source=(ROOT/_PROGRAMS[name]).read_text(encoding='utf-8')
        if re.findall(r'^@test\s*\nfn\s+([A-Za-z_]\w*)\(',source,re.M)!=names:raise ValueError('Original named body declaration order changed')


def expected_rows():
    _validate();all_bodies=bodies();tuples=tuple_traces();rows=[]
    plan_traces=plan_cleanup_traces()
    for name in _NAMES:
        if name in _PLAN_PROGRAMS:
            trace=plan_traces[name]
            rows.append({'id':'complete_parallel_workload_'+name,'source':_PROGRAMS[name],'original_source':_PROGRAMS[name],'required_target':None,
                'expected':{'exit_code':0,'stdout_utf8':''.join(render({'expected':x})+'\n' for x in trace['observations']),'stderr_utf8':''},
                'observation_count':len(trace['observations']),'required_semantic_trace':trace,
                'model':'Complete unchanged Plan cleanup original,allcallbacks/imports/prints/ranges retained;independent logical active-state unwind for ordinary nested-dispatch errors andE0420panic,thenall4post-failureitems. Reference forward/reverse andlanechoices are proof scenarios,not actual scheduling/startcounts/outputrollback/release/cancel/FI/provider qualification.'})
            continue
        row={'id':'complete_parallel_workload_'+name,'source':_PROGRAMS[name],'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':tuples[name]['stdout_utf8'] if name in tuples else '','stderr_utf8':''},
            'observation_count':1 if name in tuples else 0,
            'model':'Complete unchanged parallel workload;pure task/string values,commutative reports and typed tuple checksum retain all original loops/sleeps/close/depth. Declaration entries do not run annotated bodies. No actual scheduler/wakeup/cancel/buffer transfer/resource/lifetime/provider qualification.'}
        if name in _NAMED:row['separate_named_body_obligation']={'names':_NAMED[name],'executed_by_run':False,
            'qualification':'OPEN_REQUIRES_ALL_ORIGINAL_NAMED_BODIES','bodies':{fn:all_bodies[fn] for fn in _NAMED[name]}}
        else:row['required_semantic_trace']=tuples[name]
        rows.append(row)
    return rows


def named_expected_rows():
    _validate();all_bodies=bodies();rows=[]
    for name,names in _NAMED.items():
        facts=[row for fn in names for row in all_bodies[fn]['assertions']]
        rows.append({'id':'complete_parallel_workload_named_'+name,'source':_PROGRAMS[name],'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':''.join(all_bodies[fn]['stdout_utf8'] for fn in names),
                'stderr_utf8':'','required_test_names':names},'required_original_assertions':facts,
            'required_semantic_trace':{fn:all_bodies[fn] for fn in names},
            'model':'Every complete original annotated body in source order;requires real CLI discovery of all names plus whole-source VM/C/native adapters. All219 assertions across the five files retained. Symbolic values are not real stress/scheduling/physical/resource/cancellation qualification.'})
    return rows


_PLAN_PROGRAMS={'parallel_plan_nested_dispatch_cleanup': 'tests/diff/cases/semantics/stdlib/parallel_plan_nested_dispatch_cleanup.xr', 'parallel_plan_map_cleanup_after_panic': 'tests/diff/cases/semantics/stdlib/parallel_plan_map_cleanup_after_panic.xr'}
_NAMES += tuple(_PLAN_PROGRAMS)
_PROGRAMS.update(_PLAN_PROGRAMS)
_AUTHORITIES += ('stdlib/parallel/parallel.xr', 'spec/source/sections/019-18-error-codes.md', 'src/shared/xr_error_messages.h')
_INPUTS.update({'stdlib/parallel/parallel.xr': '013000e9964fa136e73f47210f4afadf0f27798dea74baff6761ea93fbcc7cad', 'spec/source/sections/019-18-error-codes.md': '2c4416085d5d38f37fb0e434d863cba1ff5d5758d44efb29402fa5158282d9ad', 'src/shared/xr_error_messages.h': '9cb2a61e702eb0f768e4a9e7fb1a1a3d7176cc603dfb955ca1e2415e6e8c615c', 'tests/diff/cases/semantics/stdlib/parallel_plan_nested_dispatch_cleanup.xr': 'cb170a48f9a7cfbb7bb19f6096b90e7d35c2aa6e22263aa273899b39d9113ffc', 'tests/diff/cases/semantics/stdlib/parallel_plan_map_cleanup_after_panic.xr': '3928ffed38bfd8ee2f2637b5b15715e2124cf78d7d1a18c3a70b0ff9ff793733'})


class PlanDispatchFailure(Exception):
    def __init__(self,variant):self.variant=variant


class PlanDivisionPanic(BaseException):
    def __init__(self,code,message):self.code=code;self.message=message


def plan_division_panic():
    codes=(ROOT/'spec/source/sections/019-18-error-codes.md').read_text(encoding='utf-8')
    messages=(ROOT/'src/shared/xr_error_messages.h').read_text(encoding='utf-8')
    if 'E0420' not in codes or 'XR_ERR_DIV_BY_ZERO' not in codes:raise ValueError('Division panic code authority changed')
    match=re.search(r'^#define XR_ERROR_CORE_DIVISION_BY_ZERO_MSG "([^"\n]+)"$',messages,re.M)
    if match is None:raise ValueError('Division panic message authority changed')
    return PlanDivisionPanic(420,match[1])


class LogicalCleanupPlan:
    """Model public dispatch guard/unwind only; no physical scheduler or owner."""
    def __init__(self,lane_count):
        if type(lane_count) is not int or lane_count not in (1,2):raise ValueError('Unreviewed admitted lane-count scenario')
        self.states=list(range(lane_count));self.active=False;self.closed=False;self.completed_ends=0

    def begin(self):
        if self.closed:raise PlanDispatchFailure('ClosedPlan')
        if self.active:raise PlanDispatchFailure('ConcurrentDispatch')
        self.active=True

    def dispatch(self,kind,count,body,reverse=False,output=None):
        if kind not in ('forEach','map','mapInto') or type(count) is not int or count<0 or not callable(body):
            raise ValueError('Unreviewed complete Plan dispatch')
        if kind=='mapInto' and type(output) is not list:raise ValueError('mapInto requires the original output place')
        # A rejected nested begin must not clear the outer dispatch's active bit.
        self.begin()
        try:
            values=[] if output is None else output
            if kind=='mapInto':values.clear()
            indices=reversed(range(count)) if reverse else range(count)
            for index in indices:
                value=body(self.states[index%len(self.states)],index)
                if kind!='forEach':values.append(value)
            return values if kind=='map' else None
        finally:
            self.active=False;self.completed_ends+=1


def nested_plan_cleanup_trace(reverse=False,multilane_count=2):
    outputs=[];scenarios=[]
    for label,lane_count,count,trigger in [('seq',1,4,0),('vm',multilane_count,20000,10000)]:
        plan=LogicalCleanupPlan(lane_count);inner_calls=[];visited=[]
        def outer(state,index):
            visited.append(index)
            if index==trigger:
                plan.dispatch('forEach',1,lambda inner,j:inner_calls.append((inner,j)))
        try:plan.dispatch('forEach',count,outer,reverse=reverse);outputs.append(label+'-miss')
        except PlanDispatchFailure as error:outputs.append(label+'-concurrent' if error.variant=='ConcurrentDispatch' else label+'-other')
        touched=[];plan.dispatch('forEach',4,lambda state,index:touched.append(index),reverse=reverse)
        outputs.append(len(touched)==4)
        scenarios.append({'label':label,'requested_outer_range_count':count,'nested_range_count':1,'trigger_index':trigger,
            'reference_failure_trigger_visited':trigger in visited,'inner_callback_calls':len(inner_calls),
            'required_post_failure_range_count':4,'reference_post_failure_indices':sorted(touched),
            'reference_dispatch_ends':plan.completed_ends,'reference_active_after_reuse':plan.active})
    return {'observations':outputs,'scenarios':scenarios,'original_requested_workers':[1,2],
        'original_print_expressions':10,'original_helpers_and_callback_bodies_preserved':True,
        'model_lane_counts_and_order':'FORWARD_OR_REVERSE_REFERENCE_SCENARIOS_ONLY;actual_lane_count_schedule_and_started_prefix_NOT_PREDICTED',
        'runtime_scheduling_ownership_release_cancellation_FI_provider_qualified':False}


def panic_plan_cleanup_trace(reverse=False,lane_count=2):
    plan=LogicalCleanupPlan(lane_count);outputs=[];failures=[];output=[];post_counts=[]
    for kind in ('map','mapInto'):
        visited=[]
        def body(state,index):
            visited.append(index)
            if index==10000:raise plan_division_panic()
            return state+index
        try:
            result=plan.dispatch(kind,20000,body,reverse=reverse,output=output if kind=='mapInto' else None)
            if kind=='map':outputs.append(len(result));outputs.append('map-miss')
            else:outputs.append('mapinto-miss')
        except PlanDivisionPanic as panic:
            outputs.extend([panic.code==420,'division by zero' in panic.message]);failures.append({'method':kind,
                'requested_range_count':20000,'failing_index':10000,'reference_failure_trigger_visited':10000 in visited,
                'caught_channel':'panic','code':panic.code,'message_contains_division_by_zero':'division by zero' in panic.message})
        touched=[];plan.dispatch('forEach',4,lambda state,index:touched.append(index),reverse=reverse);post_counts.append(len(touched));outputs.append(len(touched)==4)
    return {'observations':outputs,'failures':failures,'post_failure_callback_counts':post_counts,
        'original_requested_workers':2,'original_map_andmapInto_range_count':20000,'original_total_declared_work_items':40008,
        'original_print_expressions':9,'reference_dispatch_ends':plan.completed_ends,'reference_active_after_reuse':plan.active,
        'unobserved_mapInto_partial_output_contents_or_rollback':'NOT_ASSERTED',
        'model_lane_counts_and_order':'FORWARD_OR_REVERSE_REFERENCE_SCENARIOS_ONLY;actual_lane_count_schedule_and_started_prefix_NOT_PREDICTED',
        'runtime_scheduling_ownership_release_cancellation_FI_provider_qualified':False}


def plan_cleanup_traces():
    return {'parallel_plan_nested_dispatch_cleanup':nested_plan_cleanup_trace(),
        'parallel_plan_map_cleanup_after_panic':panic_plan_cleanup_trace()}
