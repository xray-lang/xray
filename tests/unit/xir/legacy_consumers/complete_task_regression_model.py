"""Derive complete original task/defer traces without assuming lazy go execution.

This is a finite completion diagram for reviewed pure actions and channel reads,
not a scheduler or an execution engine. Timing, wakeups, resources, ownership,
provider behavior and real cancellation checkpoints require product evidence.
"""
from pathlib import Path
import copy,hashlib,itertools,re
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


_EVENT_PROGRAMS = {"await_array":"tests/diff/cases/semantics/coro/await_array.xr","channel_send_timeout_partner_arrives":"tests/diff/cases/semantics/coro/channel_send_timeout_partner_arrives.xr","deterministic_timer_schedule_order":"tests/diff/cases/semantics/coro/deterministic_timer_schedule_order.xr","poll_try_recv_safepoint":"tests/diff/cases/semantics/coro/poll_try_recv_safepoint.xr","poll_try_send_safepoint":"tests/diff/cases/semantics/coro/poll_try_send_safepoint.xr","1109_await_any":"tests/regression/11_coroutine/1109_await_any.xr","1138_get_result":"tests/regression/11_coroutine/1138_get_result.xr","1139_error_isolation":"tests/regression/11_coroutine/1139_error_isolation.xr","1140_task_error":"tests/regression/11_coroutine/1140_task_error.xr"}
_INPUTS.update({"tests/diff/cases/semantics/coro/await_array.xr":"928d3bd06ec8a5a3987ccbecc25a39423b61e24f1f174b1940234f7f215eafd1","tests/diff/cases/semantics/coro/channel_send_timeout_partner_arrives.xr":"d62312e2333c4c428d31bb0ef021cafa73a7e372cc8bae109d6073e02aac9d6d","tests/diff/cases/semantics/coro/deterministic_timer_schedule_order.xr":"81cce152dbed095583200fc3a4ae9c81f33fcd95c60e6522fc980a3906d5050e","tests/diff/cases/semantics/coro/poll_try_recv_safepoint.xr":"a45db6387cc4c6f5a5fbde13026822e9c472912736cf10201f849ca1d16924a4","tests/diff/cases/semantics/coro/poll_try_send_safepoint.xr":"179711fffdcd808cf96ce1a134c90ee132329a7c0942da6728a6471e1b090600","tests/regression/11_coroutine/1109_await_any.xr":"a9056ae38e8450667dfd630a2bfe6654fa8d690e5064086e065a573ddd031160","tests/regression/11_coroutine/1138_get_result.xr":"647aa45870e4e15850d39aa00b739cfc3ed6f445b2b465405bf7c273d348600a","tests/regression/11_coroutine/1139_error_isolation.xr":"32e73b7f3f7528671672176c4b8004f3aa4457de04d174fcf7cb66f87fedc3e7","tests/regression/11_coroutine/1140_task_error.xr":"a2c5b05617be28e65fab3f3bc60adf5fb8bb8cc82bf6597ad5af602ab4d24f9f"})
_PROGRAMS.update(_EVENT_PROGRAMS)
_NAMES = _NAMES | frozenset(_EVENT_PROGRAMS)

_NAMED_CHANNEL_PROGRAMS={name:BASE+name+'.xr' for name in ["1137_await_any_success","1150_select_block_buffer_wake","1154_dynamic_channel_capacity","1156_select_close_wake","1161_multiple_task_awaiters","1162_overlapping_await_any_waiters","1163_channel_send_opcode_preserves_receiver"]}
_PROGRAMS.update(_NAMED_CHANNEL_PROGRAMS)
_NAMES = _NAMES | frozenset(_NAMED_CHANNEL_PROGRAMS)
_DECLARATION_ONLY.update({"1137_await_any_success":"test_await_any_success","1150_select_block_buffer_wake":"test_select_wakes_after_buffer_refill","1154_dynamic_channel_capacity":("test_dynamic_channel_capacity","test_bare_dynamic_channel_capacity",),"1156_select_close_wake":"test_select_wakes_on_channel_close","1161_multiple_task_awaiters":"test_multiple_task_awaiters","1162_overlapping_await_any_waiters":"test_overlapping_await_any_waiters","1163_channel_send_opcode_preserves_receiver":"test_chan_send_preserves_receiver"})
_INPUTS.update({"tests/regression/11_coroutine/1137_await_any_success.xr":"1de5d0186d017722788733a27fb10dad978b0b7ba75e64f2fb429eed17fe1cd4","tests/regression/11_coroutine/1150_select_block_buffer_wake.xr":"b60864a58ecc701663896ac0459176d9beffbfb60402509cf6db88c1523e1a26","tests/regression/11_coroutine/1154_dynamic_channel_capacity.xr":"b77276d0b2dd138dbd98c8b7603fa9f013f8b00fd74dce2583515acb40f149c7","tests/regression/11_coroutine/1156_select_close_wake.xr":"e80fe3a863216a1ddcd279bd50b1180e4624a332261a8faea411cfdc539de5a3","tests/regression/11_coroutine/1161_multiple_task_awaiters.xr":"206e61534f1d4e81446f353e24f024345daa62be78b8b584209312e5e81e8b3e","tests/regression/11_coroutine/1162_overlapping_await_any_waiters.xr":"6d640ddb1fc97917916b234399f8328b29b39664d295e2905f343802d7a1f131","tests/regression/11_coroutine/1163_channel_send_opcode_preserves_receiver.xr":"8a6af93d05b4ad696cfc70afb091fe29be471e292a3e7329eab0bc4c8d49982d"})


_CANCEL_PROGRAMS={'1132_task_monitor': 'tests/regression/11_coroutine/1132_task_monitor.xr', '1133_linked_scope_cancel': 'tests/regression/11_coroutine/1133_linked_scope_cancel.xr', '1157_select_after_scope_cancel': 'tests/regression/11_coroutine/1157_select_after_scope_cancel.xr', '1164_channel_recv_raw_match_nullable_payload': 'tests/regression/11_coroutine/1164_channel_recv_raw_match_nullable_payload.xr', '1172_module_readonly_capture': 'tests/regression/11_coroutine/1172_module_readonly_capture.xr'}
_CANCEL_NAMED={'1132_task_monitor': 'test_task_monitor', '1133_linked_scope_cancel': 'test_linked_scope_cancel'}
_COMPANIONS={'tests/regression/11_coroutine/modules/module_readonly_capture.xr': '74e0d44071edb33728b909b4036d27b1361cf040f8e5eeb3caa70134fabbb418'}
_PROGRAMS.update(_CANCEL_PROGRAMS)
_NAMES = _NAMES | frozenset(_CANCEL_PROGRAMS)
_DECLARATION_ONLY.update(_CANCEL_NAMED)
_AUTHORITIES += ('spec/source/sections/012-11-modules.md',)
_INPUTS.update({'tests/regression/11_coroutine/1132_task_monitor.xr': 'ae8260b568f5e4d6240a6f95cbd8d2325cc0c119b1755d52ccd0f5cc9c4fd458', 'tests/regression/11_coroutine/1133_linked_scope_cancel.xr': '2d81e84733acca2aec5e26a59ffaae8eedb9542b590820a3e112c0010f4085bd', 'tests/regression/11_coroutine/1157_select_after_scope_cancel.xr': 'bd99a770dfb0659e141d5f1d7767d13de10a8a30fb1bde6879dda53c6400d1f2', 'tests/regression/11_coroutine/1164_channel_recv_raw_match_nullable_payload.xr': '24fa703263a109406f89b0846dbb118952fe76c9f0a5c0fbeb54a99d9fb02e1e', 'tests/regression/11_coroutine/1172_module_readonly_capture.xr': '6560afc4671b20039f674fb19148938db01cd54bfa771dafeebaf3a5b0ce0b7c', 'tests/regression/11_coroutine/modules/module_readonly_capture.xr': '74e0d44071edb33728b909b4036d27b1361cf040f8e5eeb3caa70134fabbb418', 'spec/source/sections/012-11-modules.md': '3c32493b1e22d29c459362a9f465a6f5316856b5b375c3d1c1e06025756eecd8'})


def named_functions(name):
    names=_DECLARATION_ONLY[name]
    return list(names) if isinstance(names,tuple) else [names]


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


def buffered_select_refill_proof(count=50,capacity=10):
    """Check the complete finite FIFO state graph without enumerating its paths."""
    if type(count) is not int or type(capacity) is not int or count<0 or capacity<=0:raise ValueError('Invalid original FIFO domain')
    paths={(0,0):1}
    for progress in range(2*count+1):
        for sent in range(count+1):
            received=progress-sent
            if (sent,received) not in paths:continue
            queue=list(range(received,sent))
            if not (0<=received<=sent<=count and len(queue)<=capacity):raise ValueError('FIFO invariant violated')
            if sum(range(received))!=received*(received-1)//2:raise ValueError('Receive prefix sum changed')
            for next_state,enabled in [((sent+1,received),sent<count and len(queue)<capacity),
                                       ((sent,received+1),received<sent)]:
                if enabled:paths[next_state]=paths.get(next_state,0)+paths[sent,received]
    if (count,count) not in paths:raise ValueError('Complete FIFO graph cannot drain')
    return {'received_count':count,'received_sum':count*(count-1)//2,'reachable_states':len(paths),
            'valid_operation_interleavings':paths[count,count],'original_send_count':count,'original_receive_count':count,
            'capacity':capacity,'all_receive_prefixes_are_FIFO':True,'producer_close_then_buffer_drain_preserved':True,
            'real_select_refill_wakeup_fairness_and_resources_qualified':False}


def dynamic_capacity_assertions(capacity,values):
    variable_capacity=capacity;channel=Channel(variable_capacity)
    checks=[channel.capacity==capacity]
    checks.extend(channel.try_send(value)=='Sent' for value in values)
    received=[channel.try_recv() for _ in values]
    checks.extend(state=='Value' for state,value in received)
    checks.append(sum(value for state,value in received)==sum(values));channel.close()
    return checks,{'capacity_from_original_variable':variable_capacity,'send_values':list(values),
                   'receive_values':[value for state,value in received],'closed':channel.closed}


def multiple_waiter_trace(value,aggregate):
    outcomes=[]
    for order in itertools.permutations((1,2)):
        task=Task(lambda:value);reports=[]
        for identity in order:
            observed=task.await_value()
            if aggregate:observed=known_completed_any([task])
            reports.append(identity*100+observed)
        outcomes.append(sum(reports))
        if task.executions!=1:raise ValueError('Waiter consumed or recomputed sticky Task result')
    if len(set(outcomes))!=1:raise ValueError('Report ordering changed sum')
    return {'outputs':[outcomes[0],'done'],'original_delayed_sleep_ms':80,'waiters':2,
            'ready_capacity':2,'output_capacity':2,'completion_value_evaluations':1,
            'ready_and_output_channels_closed':True,'all_waiter_completion_orders':len(outcomes),
            'aggregate_await_input_lengths':[1,1] if aggregate else [],
            'does_not_prove_both_waiters_parked_before_task_completed':True}


def close_select_assertions():
    ready=Rendezvous();ready.offer(1);state,ready_value=ready.try_recv()
    outcomes=[]
    for close_before_select in (False,True):
        data=Rendezvous();done=Channel(1)
        if close_before_select:data.close()
        was_ready=data.try_recv()[0]=='Closed'
        if not close_before_select:data.close()
        if data.try_recv()[0]!='Closed':raise ValueError('Closed zero-capacity select stayed unavailable')
        done.send(1);done_state,value=done.recv();done.close()
        outcomes.append([state=='Value' and ready_value==1,done_state=='Value',value==1])
        if was_ready!=close_before_select:raise ValueError('Wrong pre-close readiness state')
    ready.close()
    return outcomes[0],{'registration_orders_checked':2,'all_order_assertions':outcomes,
        'original_yield_count':16,'original_done_timeout_ms':2000,'channel_capacities':[0,0,1],
        'ready_rendezvous_completed_sends':ready.completed_sends,'all_channels_closed':True,
        'sixteen_yields_do_not_prove_a_physical_park':True,'real_close_wake_latency_and_registration_qualified':False}


def send_twice_assertions():
    data=Rendezvous();received=[];return_task=Task(lambda:21+34,lambda:data.completed_sends==2)
    return_task.finish()
    for value in (21,34):
        data.offer(value);state,actual=data.try_recv()
        if state!='Value':raise ValueError('Rendezvous lost pending sender')
        received.append(actual)
    result=return_task.await_value();data.close()
    return [received[0]==21,received[1]==34,result==55],{
        'capacity':0,'pending_send_operations':2,'completed_rendezvous_sends':data.completed_sends,
        'receive_values':received,'return_value':result,'completion_value_evaluations':return_task.executions,
        'channel_closed':data.closed,'receiver_identity_survives_both_operations_required':True,
        'opcode_register_integrity_and_runtime_lifetime_qualified':False}


def channel_named_requirements():
    fifo=buffered_select_refill_proof();first,first_trace=dynamic_capacity_assertions(3,(1,2,3));second,second_trace=dynamic_capacity_assertions(2,(4,5))
    closed_checks,closed_trace=close_select_assertions();send_checks,send_trace=send_twice_assertions()
    results={
      '1150_select_block_buffer_wake':{'assertion_results':[fifo['received_count']==50,fifo['received_sum']==1225],'semantic_trace':fifo},
      '1154_dynamic_channel_capacity':{'assertion_results':first+second,'semantic_trace':{'functions':[
          {'name':'test_dynamic_channel_capacity','assertion_results':first,**first_trace},
          {'name':'test_bare_dynamic_channel_capacity','assertion_results':second,**second_trace}]}},
      '1156_select_close_wake':{'assertion_results':closed_checks,'semantic_trace':closed_trace},
      '1163_channel_send_opcode_preserves_receiver':{'assertion_results':send_checks,'semantic_trace':send_trace}}
    for name,value,aggregate in [('1161_multiple_task_awaiters',7,False),('1162_overlapping_await_any_waiters',9,True)]:
        trace=multiple_waiter_trace(value,aggregate)
        results[name]={'stdout_utf8':''.join(render(value)+'\n' for value in trace['outputs']),'assertion_results':[],
                       'semantic_trace':{key:value for key,value in trace.items() if key!='outputs'}}
    single=Task(lambda:42);single.finish();completed=[Task(lambda:42),Task(lambda:42)]
    for task in completed:task.await_value()
    scope=Scope(False)
    def fail():raise TypedError('CoroErr','Msg',{'message':'error'})
    failed=scope.add(fail);good=scope.add(lambda:42);failed_tag=failed.observe()[0];good.await_value()
    values=['test1: single success',line('  result:',known_completed_any([single])),
            'test2: pre-completed tasks',line('  result:',known_completed_any(completed)),
            'test3: skip failure',line('  result:',known_completed_any([failed,good])),'done']
    results['1137_await_any_success']={'stdout_utf8':''.join(render(value)+'\n' for value in values),
        'assertion_results':[failed_tag=='Failed'],'semantic_trace':{'completed_success_values':[42,42,42],
        'opaque_business_failure_tag_observed':'Failed','business_failure_payload_not_projected_or_retyped':True,
        'public_TaskResult_failed_payload_shape_not_qualified':True}}
    if set(results)!=set(_NAMED_CHANNEL_PROGRAMS):raise ValueError('Full named channel program set changed')
    return results



def task_monitor_requirements():
    scope=Scope(False);evaluations=[]
    def succeed():evaluations.append('success');return 42
    def fail():
        evaluations.append('business_failure')
        raise TypedError('MonitorErr','Msg',{'message':'monitor test error'})
    successful=scope.add(succeed);tag,value=successful.observe()
    checks=[tag=='Success' and value==42,successful.state in {'Success','Failed','Cancelled'}]
    failed=scope.add(fail);failure_tag,_=failed.observe()
    checks += [failure_tag=='Failed',failed.state in {'Success','Failed','Cancelled'}]
    completed=scope.add(succeed);awaited=completed.await_value();poll_tag,poll_value=completed.observe()
    checks.append(poll_tag=='Success' and poll_value==42)
    if awaited!=42 or evaluations!=['success','business_failure','success']:raise ValueError('Completed monitor actions changed')
    return {'assertion_results':checks,'semantic_trace':{
        'selected_match_branches':['Success','Failed','Success'],'completed_success_values':[value,poll_value],
        'completed_task_terminal_flags':[successful.state,failed.state,completed.state],
        'completed_action_evaluations':len(evaluations),'already_completed_poll_evaluates_no_action':len(evaluations)==3,
        'executed_original_assertions':5,'unselected_original_assert_false_branches':3,
        'business_failure_payload_not_projected_or_retyped':True,'public_TaskResult_failed_payload_shape_not_qualified':True}}


def linked_scope_prefix_proof(iterations=100):
    """Retain every possible local increment prefix, including full completion."""
    if type(iterations) is not int or iterations<0:raise ValueError('Invalid loop extent')
    def fail():raise TypedError('ChildErr','Failed')
    caught_paths=[]
    for failure_first in (False,True):
        scope=Scope(True)
        for action in ([fail,lambda:42] if failure_first else [lambda:42,fail]):scope.add(action)
        caught=False
        try:scope.close()
        except TypedError:caught=True
        caught_paths.append(caught)
    outcomes=[]
    for cut in range(iterations+1):
        value=0
        for _ in range(cut):value+=1
        outcomes.append({'increments':value,'terminal':'Cancelled','failure_caught':all(caught_paths)})
    value=0
    for _ in range(iterations):value+=1
    outcomes.append({'increments':value,'terminal':'Success','failure_caught':all(caught_paths)})
    success_paths=[]
    for order in itertools.permutations((0,1)):
        tasks=[Task(lambda:42),Task(lambda:42)]
        for index in order:tasks[index].finish()
        success_paths.append([task.await_value() for task in tasks])
    checks=[all(caught_paths),all(row['failure_caught'] for row in outcomes),all(values==[42,42] for values in success_paths)]
    return {'assertion_results':checks,'semantic_trace':{'original_slow_loop_iterations':iterations,
        'fast_success_failure_completion_orders':2,'slow_sibling_prefix_and_completion_cases':outcomes,
        'all_success_completion_orders':len(success_paths),'completed_success_values':success_paths,
        'does_not_assume_slow_sibling_runs_zero_or_all_iterations':True,
        'real_cancellation_checkpoints_scheduling_and_cleanup_qualified':False}}


def linked_cancel_select_trace(repetitions=20):
    """Finite wait bookkeeping; the product must prove real cancellation wakeup."""
    if type(repetitions) is not int or repetitions<0:raise ValueError('Invalid original repetition count')
    rounds=[]
    for index in range(repetitions+1):
        alternatives=[]
        for parked_before_failure in (False,True):
            waits=WaitTokens();token=None;data=None
            if parked_before_failure:
                data=Channel(1)
                if data.try_recv()[0]!='Empty':raise ValueError('Original select unexpectedly ready')
                token=waits.begin(0,1000000)
            sibling_state='Cancelled'
            if token is not None and waits.complete(token,'Cancelled')!='Cancelled':raise ValueError('Cancel lost registered wait')
            stale_timer=None if token is None else waits.expire(token,1000000)
            if waits.active or stale_timer is not None:raise ValueError('Cancelled select retained an active timeout')
            alternatives.append({'select_registered_before_failure':parked_before_failure,
                'sibling_terminal':sibling_state,'scope_children_remaining':0,
                'active_wait_registrations':len(waits.active),'stale_deadline_has_no_second_completion':stale_timer is None,
                'scope_rethrows_first_child_error':True})
        rounds.append({'original_round':index,'registration_orders':alternatives})
    caught=all(path['scope_rethrows_first_child_error'] for path in rounds[0]['registration_orders'])
    count=sum(all(path['scope_rethrows_first_child_error'] for path in item['registration_orders']) for item in rounds[1:])
    return {'outputs':['scope-cancel-select-after ok'],'assertion_results':[caught,count==20],
        'semantic_trace':{'first_scope_plus_original_loop_rounds':len(rounds),'original_repetition_count':repetitions,
        'original_select_timeout_ms':1000000,'original_channel_capacity':1,'original_caught_loop_rounds':count,
        'rounds':rounds,'independent_registration_order_combinations':2**len(rounds),
        'original_select_has_no_close_or_sender':True,'no_wall_clock_or_cancel_wakeup_latency_measured':True,
        'real_cancel_wakeup_registration_retraction_lifetime_and_resources_qualified':False}}


def nullable_receive_trace():
    channel=Channel(2)
    for value in (0,None):channel.send(value)
    first=channel.recv();zero=first[1] if first[0]=='Value' else -1
    second=channel.recv();nil=second[1] if second[0]=='Value' else 7
    channel.close();closed=channel.recv();closed_branch=closed[0]!='Value'
    selected=Channel(1);selected.send(None);ready_before_select=bool(selected.queue)
    sentinel=object();selected_value=receive_only_ready([selected],sentinel);saw_null=selected_value is None;selected.close()
    return {'outputs':['ok'],'assertion_results':[zero==0,nil is None,closed_branch,saw_null],
        'semantic_trace':{'original_channel_capacities':[2,1],'receive_statuses':[first[0],second[0],closed[0]],
        'original_receive_payloads':[first[1],second[1]],'null_payload_is_ready':ready_before_select,
        'selected_null_is_payload_not_empty_or_closed':saw_null,'all_original_channels_closed':channel.closed and selected.closed,
        'original_root_invocations':['test_recv_match_keeps_payload_status','test_select_keeps_null_payload_ready'],
        'executed_original_assertions':4,'real_raw_recv_lowering_and_runtime_payload_tags_qualified':False}}


def module_readonly_trace():
    published={'PREFIX':'module'};child=Task(lambda:published['PREFIX']+'!');value=child.await_value()
    coalesced=value if value is not None else 'failed'
    return {'outputs':[coalesced],'assertion_results':[],'semantic_trace':{
        'published_module_const_name':'PREFIX','published_module_const_value':published['PREFIX'],
        'original_import':BASE+'modules/module_readonly_capture.xr','completed_child_value':value,
        'null_coalescing_fallback':'failed','fallback_taken':value is None,
        'completion_value_evaluations':child.executions,'ordinary_mutable_module_capture_not_granted':True,
        'real_module_publication_ownership_and_task_transport_qualified':False}}


def cancel_named_requirements():
    return {'1132_task_monitor':task_monitor_requirements(),'1133_linked_scope_cancel':linked_scope_prefix_proof()}


def cancel_root_traces():
    traces={'1157_select_after_scope_cancel':linked_cancel_select_trace(),
        '1164_channel_recv_raw_match_nullable_payload':nullable_receive_trace(),
        '1172_module_readonly_capture':module_readonly_trace()}
    if any(not all(trace['assertion_results']) for trace in traces.values()):raise ValueError('Original root assertion failed independently')
    return traces


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
    result={'1122_coro_naming':{'stdout_utf8':''.join(render(v)+'\n' for v in naming)},
            '1125_coro_local':{'assertion_results':checks},'1130_linked_scope':{'assertion_results':linked},
            '1131_linked_go_expr':{'assertion_results':go_checks}}
    result.update(channel_named_requirements())
    result.update(cancel_named_requirements())
    return result


class EventTask(Task):
    """Count completed reviewed actions, not coroutine entry or suspend steps."""
    def finish(self):
        if self.state in {'Success','Failed','Cancelled'}:return
        if self.cancel_requested:self.state='Cancelled';return
        if not self.ready():self.state='Running';return
        self.executions+=1
        try:self.value=copy.deepcopy(self.action());self.state='Success'
        except ZeroDivisionError:self.value={'observed_runtime_fault_code':420};self.state='Failed'
    def poll(self):
        return (self.state,copy.deepcopy(self.value)) if self.state in {'Success','Failed','Cancelled'} else ('Pending',None)
    @property
    def done(self):return self.state in {'Success','Failed','Cancelled'}


class Rendezvous:
    """A pending zero-capacity sender is an operation, never a buffered item."""
    def __init__(self):self.pending=None;self.completed_sends=0;self.closed=False
    def offer(self,value):
        if self.closed or self.pending is not None:raise ValueError('Only one reviewed pending sender')
        self.pending=(value,)
    def try_recv(self):
        if self.pending is None:return ('Closed' if self.closed else 'Empty'),None
        value=self.pending[0];self.pending=None;self.completed_sends+=1;return 'Value',value
    def close(self):self.closed=True


class WaitTokens:
    """Operation generations keep cancelled timers from completing later waits."""
    def __init__(self):self.next_id=0;self.active={};self.completed={}
    def begin(self,start,timeout):
        if timeout<=0:raise ValueError('Only positive original wait durations')
        self.next_id+=1;self.active[self.next_id]=start+timeout;return self.next_id
    def complete(self,token,status):
        if token not in self.active:return None
        del self.active[token];self.completed[token]=status;return status
    def expire(self,token,now):
        if token not in self.active or now<self.active[token]:return None
        return self.complete(token,'Timeout')


def known_completed_any(tasks):
    if not tasks:raise ValueError('No reviewed empty await-any input')
    ready=[task for task in tasks if task.state=='Success']
    if len(ready)==1:return ready[0].await_value()
    if len(ready)==len(tasks):return tasks[0].await_value()
    raise ValueError('No oracle for ambiguous competing completion order')


def partner_timeout_trace():
    waits=WaitTokens();rendezvous=Rendezvous();first=waits.begin(0,2000);rendezvous.offer(7)
    receiver_at=5;state,value=rendezvous.try_recv();assert (state,value)==('Value',7)
    sent=waits.complete(first,'Sent');second=waits.begin(receiver_at,10)
    assert waits.expire(first,receiver_at) is None
    timeout=waits.expire(second,receiver_at+10);rendezvous.close()
    stale=waits.expire(first,2000)
    return {'outputs':['sent' if sent=='Sent' else 'first-timeout','second-timeout' if timeout=='Timeout' else 'second-sent'],
            'capacity':0,'original_receiver_sleep_ms':5,'original_send_timeout_ms':[2000,10],
            'transferred_values':[value],'completed_sends':rendezvous.completed_sends,
            'two_distinct_wait_generations':first!=second,'cancelled_first_timer_cannot_complete_second':stale is None,
            'channel_closed':rendezvous.closed,'abstract_event_times_not_measured_host_latency':True}


def receive_poll_trace(empty_prefix=0):
    if type(empty_prefix) is not int or empty_prefix<0:raise ValueError('Invalid finite failure prefix')
    data=Rendezvous()
    for _ in range(empty_prefix):assert data.try_recv()==('Empty',None)
    data.offer(42);state,value=data.try_recv();out=Channel(1);out.send(value);result=out.recv()[1]
    data.close();out.close()
    return {'outputs':[result],'assertion_results':[state=='Value',result==42],
            'data_capacity':0,'out_capacity':1,'original_receive_timeout_ms':1000,
            'failed_poll_prefix':empty_prefix,'source_poll_loop_has_no_new_iteration_cap':True,
            'zero_capacity_buffered_items':0,'completed_rendezvous_sends':data.completed_sends,
            'channels_closed':data.closed and out.closed,'cooperative_progress_is_product_obligation':True}


def send_poll_trace(full_prefix=0):
    if type(full_prefix) is not int or full_prefix<0:raise ValueError('Invalid finite failure prefix')
    data,out=Channel(1),Channel(1);first=data.try_send(7)
    for _ in range(full_prefix):assert data.try_send(42)=='Full'
    drained=data.recv()[1];sent=data.try_send(42);out.send(1)
    done=out.recv()[1];value=data.recv()[1];data.close();out.close()
    return {'outputs':[done,value],'assertion_results':[first=='Sent',done==1,value==42],
            'data_capacity':1,'out_capacity':1,'original_receive_timeout_ms':[1000,1000],
            'failed_poll_prefix':full_prefix,'source_poll_loop_has_no_new_iteration_cap':True,
            'drained_initial_value':drained,'successful_retry':sent,'channels_closed':data.closed and out.closed,
            'cooperative_progress_is_product_obligation':True}


def event_boundary_traces():
    traces={}
    group=[Task(lambda n=n:n*100) for n in (1,2,3)];first=ordered_values([task.await_value() for task in group])
    other=[Task(lambda n=n:n*100) for n in (4,5)];second=ordered_values([task.await_value() for task in other])
    ready=[Task(lambda n=n:n*100) for n in (6,7)]
    for task in ready:task.await_value()
    traces['await_array']={'outputs':[first,second,known_completed_any(ready)],'original_compute_calls':7,
        'homogeneous_task_element_type':'i64','await_all_input_lengths':[3,2],'preawaited_any_input_length':2,
        'all_results_sticky':all(task.executions==1 for task in group+other+ready)}
    traces['channel_send_timeout_partner_arrives']=partner_timeout_trace()
    deadlines=[(50,1),(10,2),(30,3)];outputs=[identity for delay,identity in sorted(deadlines)]
    traces['deterministic_timer_schedule_order']={'outputs':outputs,'original_sleep_delays_ms':[50,10,30],
        'channel_capacity':3,'original_assertion_results':[a==b for a,b in zip(outputs,[2,3,1])],
        'timer_cohort':'Original deterministic deadline-order acceptance;host registration skew and wall-time latency are not established by this value model.'}
    traces['poll_try_recv_safepoint']=receive_poll_trace();traces['poll_try_send_safepoint']=send_poll_trace()
    single=Task(lambda:100);single.finish();tasks=[Task(lambda n=n:n*100) for n in (2,3,4)]
    for task in tasks:task.await_value()
    values=[known_completed_any([single]),known_completed_any(tasks),known_completed_any(tasks)]
    left,right=Channel(1),Channel(1);blocked1=Task(lambda:left.recv()[1],lambda:bool(left.queue));blocked2=Task(lambda:right.recv()[1],lambda:bool(right.queue))
    right.send(999);blocked2.finish();values.append(known_completed_any([blocked1,blocked2]));blocked1.cancel();left.close();right.close();values.append('done')
    traces['1109_await_any']={'outputs':values,'all_already_completed_scan_is_input_order':True,'reawait_execution_counts':[task.executions for task in tasks],
        'only_ready_blocked_task_value':999,'other_blocked_task_cancel_requested':blocked1.cancel_requested,'both_channels_closed':left.closed and right.closed,
        'cancel_completion_not_observed_by_original':True}
    data=Rendezvous();blocked=EventTask(lambda:data.try_recv()[1],lambda:data.pending is not None)
    before=blocked.poll()[0]=='Pending';data.offer(21);value=blocked.await_value();again=blocked.poll()[1];data.close()
    cancelled=EventTask(lambda:99,lambda:False);cancelled.cancel();cancel_state,_=cancelled.await_result()
    completed=EventTask(lambda:21*2);complete_state,complete_value=completed.await_result()
    traces['1138_get_result']={'outputs':[line('before pending:',before),line('after:',value),line('again:',again),line('cancelled:',cancel_state=='Cancelled'),line('completed:',complete_value if complete_state=='Success' else -1)],
        'blocked_completed_receives':blocked.executions,'cancelled_completed_actions':cancelled.executions,'completed_task_value_evaluations':completed.executions,
        'pending_poll_itself_evaluates_no_completion_action':before,'body_may_have_started_before_poll':True,'unbuffered_completed_sends':data.completed_sends}
    for name in ('1139_error_isolation','1140_task_error'):
        fault=EventTask(lambda:1//0);fault_state,_=fault.await_result();good=EventTask(lambda:42);good_state,good_value=good.await_result()
        outputs=([ 'test1: error isolation',line('  done:',fault.done),line('  failed:',fault_state=='Failed'),
                   'test2: error does not affect other coroutines',line('  result:',good_value)] if name=='1139_error_isolation' else
                 [line('t1.done:',fault.done),line('t1.failed:',fault_state=='Failed'),line('t2.done:',good.done),line('t2.result:',good_value if good_state=='Success' else -1)])
        traces[name]={'outputs':outputs,'observed_runtime_fault_code':420,'fault_action_evaluations':fault.executions,'good_action_evaluations':good.executions,
            'failed_task_did_not_cancel_unlinked_good_task':good_state=='Success','Error_or_PanicInfo_payload_shape_not_invented':True}
    if set(traces)!=set(_EVENT_PROGRAMS):raise ValueError('Complete event program set changed')
    return traces


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
    out.update({name:trace['outputs'] for name,trace in event_boundary_traces().items()})
    out.update({name:trace['outputs'] for name,trace in cancel_root_traces().items()})
    if set(out)!=_NAMES:raise ValueError('Task model lost a complete original program')
    return out


def expected_rows():
    if set(_PROGRAMS)!=_NAMES or any(path!=BASE+name+'.xr' for name,path in _PROGRAMS.items() if name not in _EVENT_PROGRAMS) or {name:_PROGRAMS.get(name) for name in _EVENT_PROGRAMS}!=_EVENT_PROGRAMS or set(_INPUTS)!=set(_AUTHORITIES)|set(_PROGRAMS.values())|set(_COMPANIONS):
        raise ValueError('Task model lost an exact program or normative input')
    for path,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:raise ValueError('Task source or authority changed: '+path)
    for name,function in _DECLARATION_ONLY.items():
        declarations=re.findall(r'(?m)^\s*@test\s*\n\s*fn\s+([A-Za-z_][A-Za-z_0-9]*)\s*\(',
                                (ROOT/_PROGRAMS[name]).read_text(encoding='utf-8'))
        if declarations!=named_functions(name):raise ValueError('Separate named task body lost its exact declaration')
    named=named_requirements();rows=[]
    for name,values in sorted(observations().items()):
        row={'id':'complete_task_regression_'+name,'source':_PROGRAMS[name],'original_source':_PROGRAMS[name],'required_target':None,
             'expected':{'exit_code':0,'stdout_utf8':''.join(render(value)+'\n' for value in values),'stderr_utf8':''},'observation_count':len(values),
             'model':('Complete unchanged task/event entry;independent sticky completed results,explicit precompleted input-order any,zero-capacity rendezvous,wait generation invalidation,poll failure-prefix invariance and E0420 isolation. Original loops,deadlines,assertions,cancellation and closes stay in source;no real scheduler,clock skew,arbitrary ties,Task failed payload shape,resource,lifetime or provider qualification.' if name in _EVENT_PROGRAMS else 'Independent complete original entry trace from FIFO capacity, explicit awaits, permutation-invariant pure completions, scope joining and lexical LIFO cleanup;not a lazy-go or scheduling/provider/lifetime proof.')}
        if name in _EVENT_PROGRAMS:row['required_semantic_trace']={key:value for key,value in event_boundary_traces()[name].items() if key!='outputs'}
        if name in named:row['separate_named_body_obligation']={'name':_DECLARATION_ONLY[name],**named[name],'executed_by_run':False,'qualification':'OPEN_REQUIRES_NAMED_TEST_EXECUTION'}
        if name in _NAMED_CHANNEL_PROGRAMS and isinstance(_DECLARATION_ONLY[name],tuple):
            obligation=row['separate_named_body_obligation'];obligation.pop('name');obligation['names']=named_functions(name)
        if name in cancel_root_traces():
            trace=cancel_root_traces()[name];row['required_semantic_trace']=trace['semantic_trace'];row['required_original_assertion_results']=trace['assertion_results']
        if name=='1172_module_readonly_capture':row['required_companions']=[{'path':path,'lf_sha256':digest} for path,digest in _COMPANIONS.items()]
        if name=='1114_select_after':row['required_runtime_timing_predicates']=[{'expression':'elapsed >= 20','required':True},{'expression':'elapsed < 500','required':True}]
        if name=='1128_yield':row['required_original_yield_checkpoints']=1+2+3+len([i for i in range(5) if i%2==0])+3*10
        rows.append(row)
    return rows+[stack_entry_expected_row(),plain_scope_entry_expected_row()]+coro_boundary_entry_expected_rows()+[task_link_entry_expected_row()]+task_status_entry_expected_rows()+await_timeout_entry_expected_rows()


def named_expected_rows():
    """Require separate discovery and execution of every declaration-only body."""
    expected_rows()  # Validate the same complete sources and normative inputs.
    rows=[]
    for name,body in sorted(named_requirements().items()):
        if not all(body.get('assertion_results',[])):raise ValueError('Named task postcondition failed independently')
        rows.append({'id':'complete_task_named_'+name,'source':_PROGRAMS[name],'original_source':_PROGRAMS[name],
            'required_target':None,'expected':{'exit_code':0,'stdout_utf8':body.get('stdout_utf8',''),'stderr_utf8':'',
            'required_test_names':named_functions(name)},
            'required_original_assertion_results':body.get('assertion_results',[]),
            'model':'Independent complete named body, all original internal asserts and exact body output;requires real CLI test discovery plus separately invoked VM/native body. No scheduler, resource, lifetime or provider qualification inferred.'})
        if name in _NAMED_CHANNEL_PROGRAMS or name in _CANCEL_NAMED:rows[-1]['required_semantic_trace']=body['semantic_trace']
    return rows+[stack_named_expected_row(),plain_scope_named_expected_row(),coro_handoff_named_expected_row(),task_link_named_expected_row()]

_STACK_SOURCE = 'tests/regression/11_coroutine/1149_module_call_stack_grow.xr'
_STACK_HELPER = 'tests/regression/11_coroutine/modules/big_stack.xr'
_STACK_FIXTURE = 'tests/unit/xir/legacy_consumers/fixtures/stack_call_obs/root.xr'
_STACK_FIXTURE_HELPER = 'tests/unit/xir/legacy_consumers/fixtures/stack_call_obs/modules/big_stack.xr'
_STACK_ENTRY_ID = 'complete_stack_call_run'
_STACK_NAMED_ID = 'complete_stack_call_named'
_STACK_TESTS = ('test_invoke_grows_combined_slab','test_invoke_main_coro','test_repeated_go_reuses_slab')
_STACK_INPUTS = {'tests/regression/11_coroutine/1149_module_call_stack_grow.xr': 'b1deea660566ad74c64c29594657e775d282ccb77d48cc66812653d84e58533f', 'tests/regression/11_coroutine/modules/big_stack.xr': 'defeaa1c1f02cdca57347ac809a6bf430a492c769f3105553b13dfb2721046c2', 'tests/unit/xir/legacy_consumers/fixtures/stack_call_obs/root.xr': 'c4bee90c801e67b2e881c49e6c1edb83e141872c814d8ad13b862267a9bbcd78', 'tests/unit/xir/legacy_consumers/fixtures/stack_call_obs/modules/big_stack.xr': 'defeaa1c1f02cdca57347ac809a6bf430a492c769f3105553b13dfb2721046c2', 'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677', 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915', 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d', 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea'}
_STACK_TRANSFORMS = [('    assertEqual(d12, d23)\n    assert(r1 > 0)\n    assert(r2 > r1)\n', '    assertEqual(d12, d23)\n    assert(r1 > 0)\n    assert(r2 > r1)\n    print(r1)\n    print(r2)\n    print(r3)\n    print(d12)\n    print(d23)\n'), ('    var r = worker(0)\n    assert(r > 0)\n', '    var r = worker(0)\n    assert(r > 0)\n    print(r)\n'), ('        var r = await t\n        assert(r > 0)\n        i = i + 1\n', '        var r = await t\n        assert(r > 0)\n        print(r)\n        i = i + 1\n')]


def stack_wide(seed):
    values=[seed+index for index in range(30)]
    return sum(values)


def stack_worker(seed,wide=stack_wide):
    if type(seed) is not int or seed not in {*range(32),100,200}:
        raise ValueError('Outside selected complete stack-call inputs')
    worker_locals=[seed+index for index in range(10)]
    middle_seed=worker_locals[-1];middle_locals=[middle_seed+index for index in range(16)]
    wide_seed=middle_locals[-1];result=wide(wide_seed)+sum(middle_locals)+sum(worker_locals)
    return {'seed':seed,'worker_locals':worker_locals,'middle_locals':middle_locals,
            'wide_seed':wide_seed,'wide_locals':[wide_seed+index for index in range(30)],
            'result':result}


def stack_call_trace(wide=stack_wide):
    asynchronous=[stack_worker(seed,wide)['result'] for seed in (0,100,200)]
    d12=asynchronous[1]-asynchronous[0];d23=asynchronous[2]-asynchronous[1]
    direct=stack_worker(0,wide)['result'];reused=[stack_worker(seed,wide)['result'] for seed in range(32)]
    checks=[d12==d23,asynchronous[0]>0,asynchronous[1]>asynchronous[0],direct>0,*[value>0 for value in reused]]
    values={_STACK_TESTS[0]:asynchronous+[d12,d23],_STACK_TESTS[1]:[direct],_STACK_TESTS[2]:reused}
    return {'per_test_values':values,'assertion_results':checks,'original_test_names':list(_STACK_TESTS),
            'original_assertion_executions':36,'worker_call_inputs':[0,100,200,0,*range(32)],
            'source_worker_calls':36,'source_go_calls':35,'source_await_calls':35,
            'source_Chain_constructor_calls':36,'source_middle_calls':36,'source_wide_calls':36,
            'source_local_declarations':{'worker':10,'middle':16,'wide':30},
            'original_reuse_iterations':32,'original_print_sites':0,'added_observation_print_sites':7,
            'added_runtime_observation_lines':38,'host_orXray_queries':0,
            'actual_frame_layout_growth_pool_reuse_FI_resources_lifetime_safety_providers_or_mixed_qualified':False}


def validate_stack_inputs():
    required={_STACK_SOURCE,_STACK_HELPER,_STACK_FIXTURE,_STACK_FIXTURE_HELPER,
              'spec/source/sections/003-2-type-system.md','spec/source/sections/005-4-statements.md',
              'spec/source/sections/006-5-declarations.md','spec/source/sections/011-10-concurrency-and-coroutines.md'}
    if set(_STACK_INPUTS)!=required or len(_STACK_TRANSFORMS)!=3:
        raise ValueError('Complete stack-call source/helper/authority or observation plan lost')
    for name,digest in _STACK_INPUTS.items():
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete stack-call input changed: '+name)
    source=(ROOT/_STACK_SOURCE).read_text(encoding='utf-8');rebuilt=source
    for before,after in _STACK_TRANSFORMS:
        if rebuilt.count(before)!=1:raise ValueError('Stack observation must retain one exact original assertion block')
        rebuilt=rebuilt.replace(before,after,1)
    if (ROOT/_STACK_FIXTURE).read_text(encoding='utf-8')!=rebuilt:
        raise ValueError('Stack fixture changed original bodies beyond added scalar prints')
    if (ROOT/_STACK_HELPER).read_bytes()!=(ROOT/_STACK_FIXTURE_HELPER).read_bytes():
        raise ValueError('Imported wide/middle helper copy changed')
    if re.findall(r'@test\s+fn\s+([A-Za-z_][A-Za-z_0-9]*)\s*\(',source)!=list(_STACK_TESTS):
        raise ValueError('Complete stack-call named body set or order changed')


def stack_entry_expected_row():
    validate_stack_inputs()
    return {'id':_STACK_ENTRY_ID,'source':_STACK_SOURCE,'original_source':_STACK_SOURCE,
            'required_target':None,'expected':{'exit_code':0,'stdout_utf8':'','stderr_utf8':''},
            'observation_count':0,'required_companions':[{'path':_STACK_HELPER,'lf_sha256':_STACK_INPUTS[_STACK_HELPER]}],
            'separate_named_body_obligation':{'names':list(_STACK_TESTS),'executed_by_run':False,
                'required_all36worker_calls_and36assertions':True,'qualification':'OPEN_REQUIRES_ALL_THREE_NAMED_BODIES_AND38EXACT_OBSERVATIONS'},
            'model':'Complete unchanged standalone module imports its original big_stack helper anddeclares allthree named bodies withoutrootcalls;empty stdout is only standalone execution expectation. Itdoesnotexecute orqualifythethreebodies,35go/awaits,32reuseiterations,36worker/middle/widecalls,framegrowth,poolreuse oranyphysical/resource/safety/provider/mixed duty.'}


def stack_named_expected_row():
    validate_stack_inputs();trace=stack_call_trace()
    if len(trace['assertion_results'])!=36 or not all(trace['assertion_results']):
        raise ValueError('Independent complete stack-call original assertions failed')
    values=trace['per_test_values'];count=sum(map(len,values.values()))
    if count!=38:raise ValueError('Stack-call exact observation count changed')
    return {'id':_STACK_NAMED_ID,'source':_STACK_FIXTURE,'original_source':_STACK_SOURCE,
            'required_target':None,'expected':{'exit_code':0,'stderr_utf8':'',
                'stdout_utf8':''.join(str(value)+chr(10) for name in _STACK_TESTS for value in values[name]),
                'required_test_names':list(_STACK_TESTS),
                'per_test_observations':{name:{'values':values[name]} for name in _STACK_TESTS}},
            'observation_count':38,'required_original_assertion_results':trace['assertion_results'],
            'required_semantic_trace':trace,
            'model':'Complete threeoriginalnamed bodies andunchanged importedChain/wide/middle helper:all30/16/10locals remaininSource,36worker/middle/widecalls,35go/awaits,all36originalassertionsand32reuseiterations retained. Independent exact sums giveworker(n)=56n+1464forselectedbounded inputs. Addprints ofresolvedscalarlocalsafteroriginalassertions:5firstbody/1directbody/32loop=38observations;nohelpercallorsideeffectisduplicated. RealCLI discoveryandfullVM/C/nativebodyinvocationsareseparate. Original64stack/4frames comments arehistoricalpurpose,notcurrentframe/slab/physicalreuse/FI/resource/lifetime/safety/provider/mixedqualification.'}

_PLAIN_SCOPE_SOURCE = 'tests/regression/11_coroutine/1135_scope_child_errors.xr'
_PLAIN_SCOPE_FIXTURE = 'tests/unit/xir/legacy_consumers/fixtures/plain_scope_child_errors_observed/root.xr'
_PLAIN_SCOPE_ENTRY_ID = 'complete_plain_scope_child_errors_run'
_PLAIN_SCOPE_NAMED_ID = 'complete_plain_scope_child_errors_named'
_PLAIN_SCOPE_TEST = 'test_scope_child_errors'
_PLAIN_SCOPE_AUTHORITIES = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/011-10-concurrency-and-coroutines.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'stdlib/time/time.xr',
 'stdlib/types/coroutine.xr')
_PLAIN_SCOPE_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72',
 'stdlib/types/coroutine.xr': 'f85c868bf12b2b90988ead5cf746fd8e522d5c2d6398a64882b47637c72dc7a6',
 'tests/regression/11_coroutine/1135_scope_child_errors.xr': 'bfcbcf10ab7238cafe5eabda39d1e04b227e229b641e1af0c0a88834a2af7a95',
 'tests/unit/xir/legacy_consumers/fixtures/plain_scope_child_errors_observed/root.xr': 'a51500915b51daacdde6dee4f45ebc736a14db5b296f4b49deafbdd48cd2846b'}
_PLAIN_SCOPE_TRANSFORMS = [('    assertEqual(recv_sum(ch, 3), 111)\n',
  '    var p19PlainScopeSum = recv_sum(ch, 3)\n'
  '    assertEqual(p19PlainScopeSum, 111)\n'
  '    print(p19PlainScopeSum)\n'),
 ('        match (result) {\n'
  '            TaskResult.Failed { error: _ } -> {}\n'
  '            _ -> { assert(false, "failed task should be observed through TaskResult") }\n'
  '        }\n',
  '        match (result) {\n'
  '            TaskResult.Failed { error: _ } -> {}\n'
  '            _ -> { assert(false, "failed task should be observed through TaskResult") }\n'
  '        }\n'
  '        print(match (result) {\n'
  '            TaskResult.Failed { error: _ } -> true\n'
  '            _ -> false\n'
  '        })\n'),
 ('    assertEqual(recv_sum(ch, 1), 5)\n',
  '    var p19PlainScopeFive = recv_sum(ch, 1)\n'
  '    assertEqual(p19PlainScopeFive, 5)\n'
  '    print(p19PlainScopeFive)\n'),
 ('        match (result) {\n'
  '            TaskResult.Cancelled -> {}\n'
  '            _ -> { assert(false, "cancelled task should report TaskResult.Cancelled") }\n'
  '        }\n',
  '        match (result) {\n'
  '            TaskResult.Cancelled -> {}\n'
  '            _ -> { assert(false, "cancelled task should report TaskResult.Cancelled") }\n'
  '        }\n'
  '        print(match (result) {\n'
  '            TaskResult.Cancelled -> true\n'
  '            _ -> false\n'
  '        })\n')]


def plain_scope_child_error_trace(order=(1,10,100),cancel_siblings=False,
                                 failure_escapes_scope=False,failed_tag='Failed',
                                 cancel_tag='Cancelled',cancel_position='not_started'):
    if any(type(value) is not int for value in order) or tuple(sorted(order))!=(1,10,100) or len(order)!=3:
        raise ValueError('Plain scope requires all three original reports exactly once')
    if cancel_position not in {'not_started','sleeping'}:
        raise ValueError('Outside the two selected pending-child cancellation shapes')
    delivered=[]
    states=[]
    failed=False
    for value in order:
        if cancel_siblings and failed:
            states.append({'value':value,'terminal':'Cancelled','sent':False})
            continue
        delivered.append(value)
        status='Failed' if value==10 else 'Success'
        states.append({'value':value,'terminal':status,'sent':True})
        failed|=status=='Failed'
    settled=len(delivered)==3 and not failure_escapes_scope
    scope_sum=sum(delivered) if settled else None
    explicit_delivered=[5]
    observes_failed=failed_tag=='Failed'
    observes_cancelled=cancel_tag=='Cancelled'
    values=[scope_sum,observes_failed,sum(explicit_delivered),observes_cancelled] if settled else []
    return {'values':values,'root_can_terminate':settled,
        'plain_scope_reports':states,'all_three_sibling_reports_complete':len(delivered)==3,
        'first_scope_sum':scope_sum,'explicit_failure_report_before_throw':explicit_delivered,
        'explicit_awaitResult_tag':failed_tag,'cancelled_awaitResult_tag':cancel_tag,
        'cancel_position':cancel_position,'plain_scope_failure_propagates':failure_escapes_scope,
        'semantic_postconditions':[settled and scope_sum==111,observes_failed,
                                   sum(explicit_delivered)==5,observes_cancelled],
        'original_positive_assertion_executions':2,
        'original_failure_assertion_branches_avoided':2,
        'original_static_assertions':4,
        'source_go_calls':5,'source_awaitResult_calls':2,'source_plain_scopes':3,
        'original_send_values':[1,10,100,5],'original_receive_count':4,
        'original_Channel_capacities':[3,1],'original_close_calls':2,
        'original_slow_sleep_ms':1000,
        'source_error_payloads_before_runtime_projection':['SvErr.Msg(error_10)','SvErr.Msg(error_5)'],
        'TaskResult_Failed_payload_layout_or_Error_PanicInfo_conversion_qualified':False,
        'real_scheduling_cancel_checkpoints_cleanup_FI_resource_lifetime_safety_provider_mixed_qualified':False}


def validate_plain_scope_inputs():
    required=set(_PLAIN_SCOPE_AUTHORITIES)|{_PLAIN_SCOPE_SOURCE,_PLAIN_SCOPE_FIXTURE}
    if set(_PLAIN_SCOPE_INPUTS)!=required or len(_PLAIN_SCOPE_TRANSFORMS)!=4:
        raise ValueError('Plain scope lost a complete source, authority or observation transform')
    for name,digest in _PLAIN_SCOPE_INPUTS.items():
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete plain-scope input changed: '+name)
    original=(ROOT/_PLAIN_SCOPE_SOURCE).read_text(encoding='utf-8')
    observed=original
    for before,after in _PLAIN_SCOPE_TRANSFORMS:
        if observed.count(before)!=1:
            raise ValueError('Plain-scope observation lost its exact original call or match')
        observed=observed.replace(before,after,1)
    if observed!=(ROOT/_PLAIN_SCOPE_FIXTURE).read_text(encoding='utf-8'):
        raise ValueError('Plain-scope fixture changed a body beyond single-evaluation observations')
    if re.findall(r'@test\s+fn\s+([A-Za-z_][A-Za-z_0-9]*)\s*\(',original)!=[_PLAIN_SCOPE_TEST]:
        raise ValueError('Plain-scope original named body or order changed')


def plain_scope_entry_expected_row():
    validate_plain_scope_inputs()
    return {'id':_PLAIN_SCOPE_ENTRY_ID,'source':_PLAIN_SCOPE_SOURCE,
        'original_source':_PLAIN_SCOPE_SOURCE,'required_target':None,
        'expected':{'exit_code':0,'stdout_utf8':'','stderr_utf8':''},'observation_count':0,
        'separate_named_body_obligation':{'names':[_PLAIN_SCOPE_TEST],'executed_by_run':False,
            'qualification':'OPEN_REQUIRES_WHOLE_ORIGINAL_NAMED_BODY_AND4_EXTERNAL_VALUES'},
        'model':'Complete unchanged original only declares the named wrapper and all seven helpers;no root call or output. Empty entry success does not execute or qualify any scope,Task/error/cancel operation or assertion.'}


def plain_scope_named_expected_row():
    validate_plain_scope_inputs()
    trace=plain_scope_child_error_trace()
    if not all(trace['semantic_postconditions']) or not trace['root_can_terminate']:
        raise ValueError('Independent complete plain-scope postcondition failed')
    values=trace['values']
    return {'id':_PLAIN_SCOPE_NAMED_ID,'source':_PLAIN_SCOPE_FIXTURE,
        'original_source':_PLAIN_SCOPE_SOURCE,'required_target':None,
        'expected':{'exit_code':0,'stdout_utf8':''.join(render(value)+'\n' for value in values),
                    'stderr_utf8':'','required_test_names':[_PLAIN_SCOPE_TEST],
                    'per_test_observations':{_PLAIN_SCOPE_TEST:{'values':values}}},
        'observation_count':4,'required_semantic_trace':trace,
        'required_original_assertion_results':trace['semantic_postconditions'],
        'model':'Complete original wrapper and all seven helpers;all fivego/twoawaitResult/three plain scopes,four sends/four receives/two closes and originalslow sleep1000 remain. Plain-scope joining keeps successful siblings after a failure;state observations use existing immutable TaskResult values. Two channel-draining recv_sum expressions are evaluated once before the unchanged assertions and prints. Six complete report permutations yield111/true/5/true. Failed payload layout,Error/PanicInfo representation,actual cancellation/scheduler/cleanup/FI/resources/lifetime/safety/provider/mixed remain unqualified.'}

_CORO_SYNC_SOURCE = 'tests/diff/cases/semantics/stdlib/coro_typed_local_sync.xr'
_CORO_DIAG_SOURCE = 'tests/diff/cases/semantics/stdlib/coro_typed_diagnostics_direct.xr'
_CORO_HANDOFF_SOURCE = 'tests/regression/11_coroutine/1143_syscall_handoff.xr'
_CORO_HANDOFF_TEST = 'test_syscall_handoff'
_CORO_SYNC_ID = 'complete_coro_boundary_local_sync'
_CORO_DIAG_ID = 'complete_coro_boundary_diagnostics'
_CORO_HANDOFF_ENTRY_ID = 'complete_coro_boundary_handoff_run'
_CORO_HANDOFF_NAMED_ID = 'complete_coro_boundary_handoff_named'
_CORO_BOUNDARY_PROGRAMS = (_CORO_SYNC_SOURCE,_CORO_DIAG_SOURCE,_CORO_HANDOFF_SOURCE)
_CORO_BOUNDARY_PENDING_PROGRAMS = (
    'tests/diff/cases/semantics/stdlib/coro_typed_local_pool.xr',
    'tests/diff/cases/semantics/stdlib/coro_typed_local_shell_reuse.xr')
_CORO_BOUNDARY_AUTHORITIES = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/011-10-concurrency-and-coroutines.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'stdlib/types/coro.xr',
 'stdlib/types/coroutine.xr',
 'stdlib/time/time.xr')
_CORO_BOUNDARY_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72',
 'stdlib/types/coro.xr': '51a94626288d0d5361a8d10563a7dd00987cdc96d8625fb54beff7912a96d51f',
 'stdlib/types/coroutine.xr': 'f85c868bf12b2b90988ead5cf746fd8e522d5c2d6398a64882b47637c72dc7a6',
 'tests/diff/cases/semantics/stdlib/coro_typed_diagnostics_direct.xr': '084d26f378129aff1b6964474a19db444ea85153e1ecc834e424ac0d59783ea6',
 'tests/diff/cases/semantics/stdlib/coro_typed_local_sync.xr': '40104117260bdc28ff173bfd8d100911f4fdc5b384de366bd032a8eb62fef043',
 'tests/regression/11_coroutine/1143_syscall_handoff.xr': 'ab421c9f371003ebbfa3a3ab6996d129080f2039aea883a595b7a2a11040cbb0'}
_CORO_BOUNDARY_PENDING_INPUTS = {'tests/diff/cases/semantics/stdlib/coro_typed_local_pool.xr': '5886377f718113108d60909d0819ec4e72d56264866193ffa6b39f78712158f3',
 'tests/diff/cases/semantics/stdlib/coro_typed_local_shell_reuse.xr': '6ea6e7e20f52185ef29d172b11674ab805a80ccef993e6672488058b028f9b0f'}


class LogicalCoroLocals:
    """Logical context/key pairs; no runtime address, arena or shell model."""
    def __init__(self,shared_slots=False):
        self.kinds={}
        self.slots={}
        self.shared_slots=shared_slots

    def key(self,kind):
        if kind not in (str,int):
            raise ValueError('Selected Local keys are closed string or i64')
        key=len(self.kinds)
        self.kinds[key]=kind
        return key

    def _slot(self,context,key):
        if key not in self.kinds:
            raise ValueError('Unknown logical Local key')
        return ('shared' if self.shared_slots else context,key)

    def get(self,context,key):
        return self.slots.get(self._slot(context,key))

    def set(self,context,key,value):
        slot=self._slot(context,key)
        if type(value) is not self.kinds[key]:
            raise ValueError('Logical Local value must retain its exact selected type')
        self.slots[slot]=value


def coro_sync_trace():
    local=LogicalCoroLocals()
    string_key=local.key(str)
    before=local.get('root',string_key)
    local.set('root',string_key,'main')
    after=local.get('root',string_key)
    count_key=local.key(int)
    local.set('root',count_key,7)
    count=local.get('root',count_key)
    return {'values':[before is None,after if after is not None else 'missing',
                      count if count is not None else -1],
            'key_types':['string','i64'],'distinct_keys':string_key!=count_key,
            'current_context':'root','actual_local_opcodes_storage_and_lifetime_qualified':False}


def pending_pool_local_trace(order=('first-set','first-get','second-set','second-get'),shared_slots=False):
    expected={'first-set','first-get','second-set','second-get'}
    if (len(order)!=4 or set(order)!=expected or
            order.index('first-set')>order.index('first-get') or
            order.index('second-set')>order.index('second-get')):
        raise ValueError('Pool logical trace must retain both complete callback bodies')
    local=LogicalCoroLocals(shared_slots)
    key=local.key(str)
    results={}
    for step in order:
        context,operation=step.split('-')
        if operation=='set':
            local.set(context,key,context)
        else:
            got=local.get(context,key)
            results[context]=got if got is not None else 'missing'
    return {'values':[results['first'],results['second'],local.get('root',key) is None],
            'registered_for_current_positive_qualification':False,
            'mutable_module_capture_and_CoroLocal_Sendable_plan_qualified':False}


def pending_shell_local_trace(reuse_logical_context=False):
    local=LogicalCoroLocals()
    key=local.key(str)
    local.set('first-logical-task',key,'one')
    first=local.get('first-logical-task',key)
    context='first-logical-task' if reuse_logical_context else 'second-logical-task'
    before=local.get(context,key)
    local.set(context,key,'two')
    after=local.get(context,key)
    return {'values':[first,('clean' if before is None else 'dirty')+'|'+after,
                      local.get('root',key) is None],
            'registered_for_current_positive_qualification':False,
            'physical_shell_or_arena_reuse_count_inferred':False,
            'mutable_module_capture_and_CoroLocal_Sendable_plan_qualified':False}


def coro_diagnostic_values(total=0,created=0,infos=(),top=(),grouped=()):
    if any(type(value) is not int or not -(1<<63)<=value<(1<<63) for value in (total,created)):
        raise ValueError('Coro counters must keep the declared i64 field type')
    return [total>=0 and created>=0 and len(infos)>=0,len(top)>=0,len(grouped)>=0,'Blocked']


def handoff_counts(order):
    if (len(order)!=9 or any(type(value) is not int for value in order) or
            set(order)!={-1,*range(8)}):
        raise ValueError('Complete handoff must retain the one slow and eight distinct fast reports')
    return sum(value>=0 for value in order),-1 in order


def handoff_trace(order=(-1,0,1,2,3,4,5,6,7)):
    fast,slow=handoff_counts(order)
    passed=fast==8 and slow
    values=['fast completed: '+str(fast),'slow completed: '+render(slow)]
    if passed:
        values.append('handoff test passed')
    return {'values':values,'normal_branch':passed,
            'original_fast_worker_ids':list(range(8)),
            'original_fast_iterations_per_worker':100,
            'unobserved_logical_fast_sum':sum(range(100)),
            'original_slow_sleep_ms':200,'original_Channel_capacity':9,
            'original_go_tasks':9,'original_receives':9,
            'slow_report_position':order.index(-1),
            'required_runtime_progress_predicate':'Fast workers progress while the slow worker sleeps',
            'progress_predicate_proved_by_aggregate_output':False,
            'actual_scheduler_P_handoff_fairness_timer_FI_cleanup_resource_lifetime_provider_mixed_qualified':False}


def validate_coro_boundary_inputs(include_pending=False):
    if set(_CORO_BOUNDARY_INPUTS)!=set(_CORO_BOUNDARY_AUTHORITIES)|set(_CORO_BOUNDARY_PROGRAMS):
        raise ValueError('Coro boundary lost a whole program or authority')
    for name,digest in _CORO_BOUNDARY_INPUTS.items():
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete Coro boundary input changed: '+name)
    if include_pending:
        if set(_CORO_BOUNDARY_PENDING_INPUTS)!=set(_CORO_BOUNDARY_PENDING_PROGRAMS):
            raise ValueError('Pending pool/shell source obligation changed')
        for name,digest in _CORO_BOUNDARY_PENDING_INPUTS.items():
            if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
                raise ValueError('Pending pool/shell original changed: '+name)
    source=(ROOT/_CORO_HANDOFF_SOURCE).read_text(encoding='utf-8')
    if re.findall(r'@test\s+fn\s+([A-Za-z_][A-Za-z_0-9]*)\s*\(',source)!=[_CORO_HANDOFF_TEST]:
        raise ValueError('Handoff original named body changed')
    schema=(ROOT/'stdlib/types/coro.xr').read_text(encoding='utf-8')
    enum=re.search(r'enum CoroState\s*\{([^}]+)\}',schema)
    if not enum or [value.strip().split(',')[0] for value in enum[1].strip().splitlines()]!=['Unknown','Ready','Blocked','Running','Done']:
        raise ValueError('Typed CoroState declaration changed')


def coro_boundary_entry_expected_rows():
    validate_coro_boundary_inputs()
    records=[(_CORO_SYNC_ID,_CORO_SYNC_SOURCE,coro_sync_trace()['values']),
             (_CORO_DIAG_ID,_CORO_DIAG_SOURCE,coro_diagnostic_values()),
             (_CORO_HANDOFF_ENTRY_ID,_CORO_HANDOFF_SOURCE,[])]
    rows=[]
    for identity,source,values in records:
        row={'id':identity,'source':source,'original_source':source,'required_target':None,
             'expected':{'exit_code':0,'stdout_utf8':''.join(render(value)+'\n' for value in values),'stderr_utf8':''},
             'observation_count':len(values),
             'model':'Complete unchanged original. Logical per-context typed Local keys and nonnegative typed snapshot predicates;no guessed counters,IDs,physical storage or product-output oracle. Handoff standalone is declaration-only and doesnotexecute its named body. All runtime/type/capture/lifetime/FI/safety/provider and fairness duties remain separate.'}
        if identity==_CORO_HANDOFF_ENTRY_ID:
            row['separate_named_body_obligation']={'names':[_CORO_HANDOFF_TEST],'executed_by_run':False,
                'qualification':'OPEN_REQUIRES_COMPLETE_NAMED_BODY_ANDTHREE_AGGREGATE_OUTPUTS_PLUS_PROGRESS_PROOF'}
        elif identity==_CORO_SYNC_ID:
            row['required_semantic_trace']=coro_sync_trace()
        else:
            row['required_semantic_trace']={'original_list_limit':8,'original_state_filter':'Running',
                'original_top_limit':2,'original_metric':'Reductions','original_group_key':'State',
                'counter_values_are_not_predicted':True,'actual_snapshot_contents_or_ranking_qualified':False}
        rows.append(row)
    return rows


def coro_handoff_named_expected_row():
    validate_coro_boundary_inputs()
    trace=handoff_trace()
    if not trace['normal_branch']:
        raise ValueError('Independent complete handoff aggregate postcondition failed')
    values=trace['values']
    return {'id':_CORO_HANDOFF_NAMED_ID,'source':_CORO_HANDOFF_SOURCE,
            'original_source':_CORO_HANDOFF_SOURCE,'required_target':None,
            'expected':{'exit_code':0,'stdout_utf8':''.join(render(value)+'\n' for value in values),
                'stderr_utf8':'','required_test_names':[_CORO_HANDOFF_TEST],
                'per_test_observations':{_CORO_HANDOFF_TEST:{'values':values}}},
            'observation_count':3,'required_semantic_trace':trace,
            'model':'Complete unchanged named handoff wrapper andallthreehelpers:slow200ms,eightfastworkers each100iterations,ninetasks/channelcapacity9/ninereceives retained. All9! report permutations produce8/true/passed aggregates;even slow-first gives the same output. These outputs do not prove actual progress of fast workers during slow sleep,physical P handoff,fairness,FI,timer/resource/lifetime/safety/provider/mixed behavior;original progress duty remains OPEN.'}

_TASK_LINK_SOURCE = 'tests/regression/11_coroutine/1136_task_link.xr'
_TASK_LINK_FIXTURE = 'tests/unit/xir/legacy_consumers/fixtures/task_link_results_observed/root.xr'
_TASK_LINK_TEST = 'test_task_link'
_TASK_LINK_ENTRY_ID = 'complete_task_link_results_run'
_TASK_LINK_NAMED_ID = 'complete_task_link_results_named'
_TASK_LINK_AUTHORITIES = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/011-10-concurrency-and-coroutines.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'stdlib/types/coroutine.xr',
 'stdlib/time/time.xr')
_TASK_LINK_TRANSFORMS = [('    match (r) {\n        TaskResult.Cancelled ->',
  '    print(match (r) {\n'
  '        TaskResult.Cancelled -> true\n'
  '        _ -> false\n'
  '    })\n'
  '    match (r) {\n'
  '        TaskResult.Cancelled ->'),
 ('    match (r) {\n        TaskResult.Failed { error: _ } ->',
  '    print(match (r) {\n'
  '        TaskResult.Failed { error: _ } -> true\n'
  '        _ -> false\n'
  '    })\n'
  '    match (r) {\n'
  '        TaskResult.Failed { error: _ } ->'),
 ('    assert(r1 == 42)', '    print(r1)\n    assert(r1 == 42)'),
 ('    assert(r2 == 42)', '    print(r2)\n    assert(r2 == 42)')]
_TASK_LINK_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72',
 'stdlib/types/coroutine.xr': 'f85c868bf12b2b90988ead5cf746fd8e522d5c2d6398a64882b47637c72dc7a6',
 'tests/regression/11_coroutine/1136_task_link.xr': 'b2341bab8f0caf95f7bb8de9fea87b7ab3cd5b1e9de2c385bb6433298a079ab1',
 'tests/unit/xir/legacy_consumers/fixtures/task_link_results_observed/root.xr': '4ad5e3b35fead6bbed57757015f75f7adf8c04a13b95f250196a0ec486291ab5'}


def task_link_result_trace(cancel_position='not_started',cancel_tag='Cancelled',
                           failed_tag='Failed',completion_order=(0,1),success_values=(42,42)):
    """Complete wrapper postconditions; no physical clock or Error layout model."""
    if cancel_position not in ('not_started','sleeping'):
        raise ValueError('The pending cancellation diagram does not prove an arbitrary wall-clock schedule')
    if len(completion_order)!=2 or set(completion_order)!={0,1} or len(success_values)!=2:
        raise ValueError('Both complete success workers and their original awaits are required')
    if cancel_tag not in ('Cancelled','Success','Failed','Pending','Timeout') or failed_tag not in ('Cancelled','Success','Failed','Pending','Timeout'):
        raise ValueError('Unknown TaskResult tag')
    completed={index:success_values[index] for index in completion_order}
    observations=[cancel_tag=='Cancelled',failed_tag=='Failed',completed[0],completed[1]]
    assertions=[observations[0],observations[1],completed[0]==42,completed[1]==42]
    return {'values':observations,'semantic_postconditions':assertions,
        'root_can_terminate':all(assertions),'cancel_position':cancel_position,
        'all_original_workers':4,'original_success_workers':2,
        'original_plain_awaits':2,'original_awaitResult_calls':2,
        'original_cancel_calls':1,'original_slow_sleep_ms':50,
        'original_static_assertions':6,'original_positive_assertion_executions':4,
        'original_failure_assertion_branches_avoided':2,
        'source_error_before_runtime_projection':'LinkErr.Msg(link error)',
        'TaskResult_Failed_payload_Error_PanicInfo_layout_qualified':False,
        'actual_cancel_before_completion_for_arbitrary_parent_delays_qualified':False,
        'real_cleanup_FI_resources_lifetime_safety_provider_mixed_qualified':False,
        'filename_does_not_prove_linked_go_or_Task_link_operation':True}


def validate_task_link_result_inputs():
    required=set(_TASK_LINK_AUTHORITIES)|{_TASK_LINK_SOURCE,_TASK_LINK_FIXTURE}
    if set(_TASK_LINK_INPUTS)!=required or len(_TASK_LINK_TRANSFORMS)!=4:
        raise ValueError('Task result observations lost a complete source,authority or insertion')
    for name,digest in _TASK_LINK_INPUTS.items():
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete task-result input changed: '+name)
    original=(ROOT/_TASK_LINK_SOURCE).read_text(encoding='utf-8')
    observed=original
    for before,after in _TASK_LINK_TRANSFORMS:
        if observed.count(before)!=1:
            raise ValueError('Task-result observer lost its exact original match or assertion')
        observed=observed.replace(before,after,1)
    if observed!=(ROOT/_TASK_LINK_FIXTURE).read_text(encoding='utf-8'):
        raise ValueError('Task-result observer changed original work beyond pure observations')
    if re.findall(r'@test\s+fn\s+([A-Za-z_][A-Za-z_0-9]*)\s*\(',original)!=[_TASK_LINK_TEST]:
        raise ValueError('The full original wrapper or named identity changed')


def task_link_entry_expected_row():
    validate_task_link_result_inputs()
    return {'id':_TASK_LINK_ENTRY_ID,'source':_TASK_LINK_SOURCE,
        'original_source':_TASK_LINK_SOURCE,'required_target':None,
        'expected':{'exit_code':0,'stdout_utf8':'','stderr_utf8':''},'observation_count':0,
        'separate_named_body_obligation':{'names':[_TASK_LINK_TEST],'executed_by_run':False,
            'qualification':'OPEN_REQUIRES_COMPLETE_ORIGINAL_WRAPPER_SIX_HELPERS_AND4_VALUES'},
        'model':'Complete unchanged original declares its named wrapper and all six helpers without a root call. Empty run success cannot qualify cancellation,failure,success,any original assertion or a linked operation.'}


def task_link_named_expected_row():
    validate_task_link_result_inputs()
    trace=task_link_result_trace()
    if not trace['root_can_terminate']:
        raise ValueError('Complete task-result postconditions do not hold')
    return {'id':_TASK_LINK_NAMED_ID,'source':_TASK_LINK_FIXTURE,
        'original_source':_TASK_LINK_SOURCE,'required_target':None,
        'expected':{'exit_code':0,'stdout_utf8':''.join(render(value)+'\n' for value in trace['values']),
            'stderr_utf8':'','required_test_names':[_TASK_LINK_TEST],
            'per_test_observations':{_TASK_LINK_TEST:{'values':trace['values']}}},
        'observation_count':4,'required_semantic_trace':trace,
        'required_original_assertion_results':trace['semantic_postconditions'],
        'model':'Complete original wrapper and six helpers;four go,two plain awaits,two awaitResult calls,one cancel,sleep50 and all six original assertions remain. Four pure observations inspect existing immutable result tags or existing r1/r2 exactly once without repeating stateful calls. Pending cancellation before start or while sleeping,explicit Failed tag and both pure success workers independently yield true/true/42/42 for both completion orders;four positive assertions execute and two assert(false) branches are avoided. Actual timer/cancellation progress under arbitrary parent delays,Failed Error/PanicInfo payload layout,cleanup/FI/resources/lifetime/safety/provider/mixed remain separate open obligations. The filename does not qualify linked go or a Task.link operation.'}


def task_status_entry_expected_rows():
    import complete_task_status_model
    return complete_task_status_model.expected_rows()


def await_timeout_entry_expected_rows():
    import complete_await_timeout_model
    return complete_await_timeout_model.expected_rows()
