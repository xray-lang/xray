"""Derive complete original task/defer traces without assuming lazy go execution.

This is a finite completion diagram for reviewed pure actions and channel reads,
not a scheduler or an execution engine. Timing, wakeups, resources, ownership,
provider behavior and real cancellation checkpoints require product evidence.
"""
from pathlib import Path
import copy,hashlib,itertools
from complete_async_events_model import Channel,Scope,TypedError

ROOT=Path(__file__).resolve().parents[4]
BASE='tests/regression/11_coroutine/'
_NAMES=frozenset({'1100_cancelled','1101_channel_basic','1102_go_await','1103_defer',
 '1104_coroutine_combined','1105_defer_args','1106_select','1107_go_deferred',
 '1108_await_all','1110_scope','1111_comprehensive','1112_go_lambda_call',
 '1113_select_send','1114_select_after','1115_cancel','1118_comprehensive',
 '1119_await_all_syntax','1120_scope_basic','1121_defer_block','1122_coro_naming',
 '1125_coro_local','1128_yield','1130_linked_scope','1131_linked_go_expr'})
_PROGRAMS={name:BASE+name+'.xr' for name in _NAMES}
_AUTHORITIES=(
 'spec/source/sections/003-2-type-system.md','spec/source/sections/004-3-expressions.md',
 'spec/source/sections/005-4-statements.md','spec/source/sections/006-5-declarations.md',
 'spec/source/sections/008-7-scoping-and-name-resolution.md','spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/011-10-concurrency-and-coroutines.md','spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md','spec/source/sections/018-17-compilation-pipeline.md',
 'stdlib/types/channel.xr','stdlib/types/coroutine.xr','stdlib/types/coro.xr',
 'stdlib/types/atomic.xr','stdlib/time/time.xr',
 'tests/unit/xir/legacy_consumers/complete_async_events_model.py')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/004-3-expressions.md': '1fdce404c935a4208d94f20fd2701b1dae48ca469a944a45b64237f7609eba9c',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/008-7-scoping-and-name-resolution.md': 'ec46ac53d1f2886e2ff2a5ebf3d5b7bd7e7d99ccf472b8ad64ca4b75f8370fc7',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440',
 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72',
 'stdlib/types/atomic.xr': '4da0e8355fdbb9e22a2cc5552f234d171b389302ce29698a19399ec40b2f67e4',
 'stdlib/types/channel.xr': 'ff2be02bcc28be44f7927b746d316eec7fef4e815f2693207ef07176bbbf1284',
 'stdlib/types/coro.xr': '51a94626288d0d5361a8d10563a7dd00987cdc96d8625fb54beff7912a96d51f',
 'stdlib/types/coroutine.xr': 'f85c868bf12b2b90988ead5cf746fd8e522d5c2d6398a64882b47637c72dc7a6',
 'tests/regression/11_coroutine/1100_cancelled.xr': '702e3fdb6786e6b2ffbdcd25f667b180d2b9fc0f8ef73b1dc1ac32ba2d5aebf4',
 'tests/regression/11_coroutine/1101_channel_basic.xr': 'bb0f4e498e310aab83985accf730ef2a1abc7b9617608b901e97e9c1621fc159',
 'tests/regression/11_coroutine/1102_go_await.xr': '698f7bd99153a34b7276a3868e1bfd2d7b3467c7992b78b533db234aec166833',
 'tests/regression/11_coroutine/1103_defer.xr': 'fab6decda9de845a4647ffeeb003067a96435a86b6b33adb69b534ff3f20f7cc',
 'tests/regression/11_coroutine/1104_coroutine_combined.xr': 'd5f4c5441bae3dc287702d53a71ff8ea6e950d169829b34702d654d4b30bfb76',
 'tests/regression/11_coroutine/1105_defer_args.xr': 'e99b48c3d05b6b619bdd3923c2defab6d03771395f0e50e64f427e5fbc4351a5',
 'tests/regression/11_coroutine/1106_select.xr': '83ec7dc96dce0a477823fdd079ff02e6506c5f9f36f8a5bef2ad57aee81f7d2f',
 'tests/regression/11_coroutine/1107_go_deferred.xr': '68d57ee487372ac250294ee3d080b1c74e15017e19099ae5b7f9b8cde6fa768b',
 'tests/regression/11_coroutine/1108_await_all.xr': 'e4688cb501188ad8a3d40b8b431a670a7cb934d7f3fda9382b7f83146c623143',
 'tests/regression/11_coroutine/1110_scope.xr': '05ee9c31ba3c4638accfa8f93738cd401db03c6e6abf6b276e9bccd105b5a687',
 'tests/regression/11_coroutine/1111_comprehensive.xr': '1e79effca38a65996020a47f98d007317293cda237bfc2d470c1fbda5ec8d1ea',
 'tests/regression/11_coroutine/1112_go_lambda_call.xr': '3d434bd12eafcfee138a3bb8949951fb102f31635faeabafcaff7c3f0248a10a',
 'tests/regression/11_coroutine/1113_select_send.xr': '7fd12f28b59176dbb542586e70fbf967b5b681a5a4b26ca68095d8307693b607',
 'tests/regression/11_coroutine/1114_select_after.xr': '0e60c77bce822e9a33d5f64d6d8e12cd49e5a38880515b25ee69aa30c53de62c',
 'tests/regression/11_coroutine/1115_cancel.xr': 'a67f9c5ee9f21c523b3f80a5ad0c529a75e0c41ba443a69473cbd7dfba0a3dfe',
 'tests/regression/11_coroutine/1118_comprehensive.xr': 'f88d0bf14b31b80aa10590950ca190a825a5e47056b9813a85fc6e89139fc4c9',
 'tests/regression/11_coroutine/1119_await_all_syntax.xr': 'dd0ed9e4275e84e23a73567d47f984b7412da84166c42cb580b3e04fc21db32f',
 'tests/regression/11_coroutine/1120_scope_basic.xr': '65805e62988459676f845577bc6ed424266f8a772a204f5b925f7128bbfe170b',
 'tests/regression/11_coroutine/1121_defer_block.xr': 'e741cca3363be3b5583fe4584621233621d1ccc75b5bed683406a23109b645e5',
 'tests/regression/11_coroutine/1122_coro_naming.xr': '7e871f65cdf7adf8d97e77c3045c8bfb13c28229c954f17398df461f2274f748',
 'tests/regression/11_coroutine/1125_coro_local.xr': 'b292eb8a34554dfc1b1df358bee33f8838ef9880d8dffebd415a1f8f5e2c99f3',
 'tests/regression/11_coroutine/1128_yield.xr': '824e106b9eec6e0bffe13836f0d7e4412c9666eb4e91c64002d611d17ab53c11',
 'tests/regression/11_coroutine/1130_linked_scope.xr': '6b58eb44971587073e71ec549b024c8ab5761856089dfe2a31ad478d24ab78b0',
 'tests/regression/11_coroutine/1131_linked_go_expr.xr': '20918e1587c913e01a2db249d5835871d832d17df52c48e150e33d713a3beec5',
 'tests/unit/xir/legacy_consumers/complete_async_events_model.py': '45a54d201c792ed69189e26e3e3f759a7bca543719324fa72ce4ed81d257cd8f'}
_DECLARATION_ONLY={
 '1122_coro_naming':'test_coro_naming','1125_coro_local':'test_coro_local',
 '1130_linked_scope':'test_linked_scope','1131_linked_go_expr':'test_linked_go_expr'}


class Enum:
    def __init__(self,name,variant):self.name,self.variant=name,variant
    def __str__(self):return self.name+'.'+self.variant


def render(value):
    if type(value) is bool:return str(value).lower()
    if type(value) in (str,int):return str(value)
    if isinstance(value,Enum):return str(value)
    if type(value) is list:return '['+', '.join(render(item) for item in value)+']'
    raise ValueError('Unreviewed task output family')


def line(*values):return ' '.join(render(value) for value in values)


class Task:
    def __init__(self,action,ready=lambda:True):
        self.action,self.ready=action,ready;self.state='Pending';self.value=None;self.cancel_requested=False;self.executions=0
    def finish(self):
        if self.state in {'Success','Cancelled'}:return
        if self.cancel_requested:self.state='Cancelled';return
        if not self.ready():self.state='Running';return
        self.executions+=1;self.value=copy.deepcopy(self.action());self.state='Success'
    def await_value(self):
        self.finish()
        if self.state!='Success':raise ValueError('No completed success value')
        return copy.deepcopy(self.value)
    def await_result(self):self.finish();return self.state,copy.deepcopy(self.value)
    def cancel(self):
        if self.state!='Success':self.cancel_requested=True
    def poll(self):return (self.state,copy.deepcopy(self.value)) if self.state in {'Success','Cancelled'} else ('Pending',None)


def ordered_values(values):
    """All completion permutations preserve await-all input order for pure tasks."""
    outcomes=[]
    for order in itertools.permutations(range(len(values))):
        tasks=[Task(lambda value=value:value) for value in values]
        for index in order:tasks[index].finish()
        outcomes.append([task.await_value() for task in tasks])
    if not outcomes or any(result!=values for result in outcomes):raise ValueError('Completion order changed input-index results')
    return outcomes[0]


def receive_only_ready(channels,default):
    ready=[channel for channel in channels if channel.queue]
    if len(ready)>1:raise ValueError('This model grants no competing-ready selection priority')
    return ready[0].try_recv()[1] if ready else default


def close_with_defer(body,cleanups):
    result=list(body)
    for cleanup in reversed(cleanups):result.extend(cleanup)
    return result


def cancellation_trace():
    channel=Channel(0);blocked=Task(lambda:channel.recv()[1],lambda:bool(channel.queue))
    blocked.cancel();state,_=blocked.await_result()
    values=[line('t1 cancelled:',state=='Cancelled'),line('t1 status:',Enum('TaskStatus',state))]
    channel.close();finished=Task(lambda:21*2);result=finished.await_value();finished.cancel()
    values.extend([line('t2 success:',finished.poll()[0]=='Success'),line('t2 result:',result),'done'])
    return values


def comprehensive_trace():
    values=['=== go/await ===',line('task1 result:',Task(lambda:42).await_value()),'=== status ===']
    task=Task(lambda:100);result=task.await_value()
    values.extend([line('done:',task.state=='Success'),line('status:',Enum('TaskStatus',task.state)),line('result:',result),'=== cancel ==='])
    channel=Channel(0);task=Task(lambda:channel.recv()[1],lambda:bool(channel.queue));task.cancel();state,_=task.await_result()
    values.extend([line('cancelled:',state=='Cancelled'),line('cancelled status:',Enum('TaskStatus',state))]);channel.close()
    channel=Channel(3)
    for value in [1,2,3]:channel.try_send(value)
    values.extend(['=== channel ===',line('recv:',*(channel.try_recv()[1] for _ in range(3))),'=== defer ==='])
    values.extend(close_with_defer(['working...'],[['cleanup!']]))
    values.extend(['=== await all ===',line('all results:',ordered_values([1,2,3])),
                   '=== await any ===',line('first result:',Task(lambda:'winner').await_value()),'=== done ==='])
    return values


def joined_counter_paths():
    results=[]
    for order in itertools.permutations(range(3)):
        counter=0;finished=[]
        for index in order:counter+=2;finished.append(index)
        if len(finished)!=3:raise ValueError('Scope left a child unfinished')
        results.append(counter)
    return results


def named_requirements():
    naming=ordered_values([1*10,sum(range(1,5)),2*10,3*10,4*10,5*10])+['done']
    slots={};name_key,count_key,id_key,value_key=object(),object(),object(),object()
    root_context=object();child_id,child_a,child_b=object(),object(),object()
    put=lambda context,key,value:slots.__setitem__((context,key),copy.deepcopy(value))
    get=lambda context,key:copy.deepcopy(slots.get((context,key)))
    put(root_context,name_key,'main');put(root_context,count_key,42)
    checks=[get(root_context,name_key)=='main',get(root_context,count_key)==42]
    put(child_id,id_key,1);checks.extend([get(child_id,id_key)==1,get(root_context,id_key) is None])
    put(child_a,value_key,'A');put(child_b,value_key,'B')
    checks.extend([get(child_a,value_key)=='A',get(child_b,value_key)=='B',get(root_context,name_key)=='main',
                   get(root_context,count_key)==42,get(root_context,object()) is None])
    def fail():raise TypedError('WorkerErr','Failed',{'code':1})
    scope=Scope(linked=True);scope.add(lambda:42);scope.add(fail);caught=False;code=-1
    try:scope.close()
    except TypedError as error:caught=True;code=error.value[2]['code']
    linked=[caught,code==1]
    for linked_scope,actions in [(False,[lambda:42,fail]),(True,[lambda:42,lambda:42])]:
        scope=Scope(linked=linked_scope)
        for action in actions:scope.add(action)
        caught=False
        try:scope.close()
        except TypedError:caught=True
        linked.append(not caught)
    task=Task(lambda:42);value=task.await_value();task2=Task(lambda:42);state,value2=task2.await_result()
    go_checks=[value==42,task2.state=='Success',state=='Success' and value2==42,Task(lambda:42).await_value()==42]
    return {'1122_coro_naming':{'stdout_utf8':''.join(render(v)+'\n' for v in naming)},
            '1125_coro_local':{'assertion_results':checks},'1130_linked_scope':{'assertion_results':linked},
            '1131_linked_go_expr':{'assertion_results':go_checks}}


def observations():
    out={'1100_cancelled':[False]};channel=Channel(3)
    values=[channel.capacity==3]+[channel.try_send(value)=='Sent' for value in [10,20,30,40]]
    received=[channel.try_recv() for _ in range(3)];values.extend(item[1] for item in received)
    values.extend([all(item[0]=='Value' for item in received),channel.try_recv()[0]=='Value','done']);channel.close();out['1101_channel_basic']=values
    a,b=ordered_values([10+20,5*6]);out['1102_go_await']=[a,b,a+b,'done']
    out['1103_defer']=close_with_defer(['start','work done'],[['cleanup 1'],['cleanup 2'],['cleanup 3']])+['end']
    channel=Channel(5)
    for value in [1,2,3]:channel.try_send(value)
    out['1104_coroutine_combined']=close_with_defer(['=== 协程综合测试 ===','producer done',sum(channel.try_recv()[1] for _ in range(3)),False,'=== 测试完成 ==='],[['cleanup done']]);channel.close()
    out['1105_defer_args']=close_with_defer(['start','work done'],[['cleanup 1'],['cleanup 2'],['result',42]])+['end']
    first,second=Channel(2),Channel(2);first.try_send(100)
    values=[receive_only_ready([first,second],'no data')];second.try_send(200)
    values.extend([receive_only_ready([first,second],'no data'),receive_only_ready([first,second],'no data'),'done']);out['1106_select']=values
    tasks=[Task(lambda value=value:value) for value in [1,2,3]]
    out['1107_go_deferred']=['start',*(task.await_value() for task in tasks),tasks[0].await_value(),'end']
    values=ordered_values([n*n for n in [2,3,4]]);out['1108_await_all']=[*values,sum(values),'done']
    outer=1;inner=10;local=20;out['1110_scope']=[inner,local,outer,'done']
    channel=Channel(10)
    for identity in ordered_values([1,2]):
        for index in range(3):channel.try_send(identity*100+index)
    values=[1,2,sum(channel.try_recv()[0]=='Value' for _ in range(6))]
    values.extend(close_with_defer(['working'],[['resource1'],['resource2']]))
    first,second=Channel(1),Channel(1);first.try_send(100);values.append(receive_only_ready([first,second],'default'));out['1111_comprehensive']=values
    out['1112_go_lambda_call']=[Task(lambda:100).await_value(),*ordered_values([200,300]),'done']
    channel=Channel(2);values=[]
    for value in [100,200,300]:values.append('sent '+str(value) if channel.try_send(value)=='Sent' else 'buffer full')
    received=[channel.try_recv() for _ in range(2)];values.extend(item[1] for item in received);values.extend([all(item[0]=='Value' for item in received),'done']);out['1113_select_send']=values
    channel=Channel(1);values=[receive_only_ready([channel],'timeout')];channel.try_send(42)
    values.extend([receive_only_ready([channel],'timeout'),receive_only_ready([channel],'immediate timeout'),'done']);out['1114_select_after']=values
    out['1115_cancel']=cancellation_trace();out['1118_comprehensive']=comprehensive_trace()
    values=ordered_values([10,20,30]);out['1119_await_all_syntax']=['test1: await all basic',line('  r0:',values[0]),line('  r1:',values[1]),line('  r2:',values[2]),line('  sum:',sum(values)),'test2: await all two tasks',line('  r0:',10),line('  r1:',20)]
    counts=joined_counter_paths();assert all(value==6 for value in counts)
    out['1120_scope_basic']=['=== scope 基础测试 ===','scope 结束',line('任务数量:',counts[0]),'所有任务完成','=== 嵌套 scope 测试 ===',line('嵌套 scope 结果数量:',3+sum([1,1]))]
    out['1121_defer_block']=['=== defer 块语法测试 ===',*close_with_defer(['开始','结束'],[['defer 块']]),'=== 多个 defer 块 ===',*close_with_defer(['执行中','结束'],[['defer 1'],['defer 2']]),'=== defer 混合测试 ===',*close_with_defer(['执行'],[['cleanup 函数'],['defer 块 A']])]
    for name in _DECLARATION_ONLY:out[name]=[]
    out['1128_yield']=['test1: main yield','test1: after yield',line('test2: result =','yielded'),line('test3:',*ordered_values([10,20,30])),line('test4: sum =',sum(range(5))),line('test5: sums =',*ordered_values([sum(range(10))]*3))]
    if set(out)!=_NAMES:raise ValueError('Task model lost a complete original program')
    return out


def expected_rows():
    if set(_PROGRAMS)!=_NAMES or any(path!=BASE+name+'.xr' for name,path in _PROGRAMS.items()) or set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values()):
        raise ValueError('Task model lost an exact program or normative input')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Task source or authority changed: '+path)
    named=named_requirements();rows=[]
    for name,values in sorted(observations().items()):
        row={'id':'complete_task_regression_'+name,'source':_PROGRAMS[name],'original_source':_PROGRAMS[name],'required_target':None,
             'expected':{'exit_code':0,'stdout_utf8':''.join(render(value)+'\n' for value in values),'stderr_utf8':''},'observation_count':len(values),
             'model':'Independent complete original entry trace from FIFO capacity, explicit awaits, permutation-invariant pure completions, scope joining and lexical LIFO cleanup;not a lazy-go or scheduling/provider/lifetime proof.'}
        if name in named:row['separate_named_body_obligation']={'name':_DECLARATION_ONLY[name],**named[name],'executed_by_run':False,'qualification':'OPEN_REQUIRES_NAMED_TEST_EXECUTION'}
        if name=='1114_select_after':row['required_runtime_timing_predicates']=[{'expression':'elapsed >= 20','required':True},{'expression':'elapsed < 500','required':True}]
        if name=='1128_yield':row['required_original_yield_checkpoints']=1+2+3+len([i for i in range(5) if i%2==0])+3*10
        rows.append(row)
    return rows


def named_expected_rows():
    """Require separate discovery and execution of every declaration-only body."""
    expected_rows()  # Validate the same complete sources and normative inputs.
    rows=[]
    for name,body in sorted(named_requirements().items()):
        if not all(body.get('assertion_results',[])):raise ValueError('Named task postcondition failed independently')
        rows.append({'id':'complete_task_named_'+name,'source':_PROGRAMS[name],'original_source':_PROGRAMS[name],
            'required_target':None,'expected':{'exit_code':0,'stdout_utf8':body.get('stdout_utf8',''),'stderr_utf8':'',
            'required_test_names':[_DECLARATION_ONLY[name]]},
            'required_original_assertion_results':body.get('assertion_results',[]),
            'model':'Independent complete named body, all original internal asserts and exact body output;requires real CLI test discovery plus separately invoked VM/native body. No scheduler, resource, lifetime or provider qualification inferred.'})
    return rows
