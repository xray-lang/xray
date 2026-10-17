"""Whole await-timeout program across legal completion/deadline observations."""
from pathlib import Path
import hashlib,itertools
ROOT=Path(__file__).resolve().parents[4]
SOURCE='tests/regression/11_coroutine/1117_await_timeout.xr'
IDENTITY='complete_await_timeout_1117'
KIND='finite_complete_await_timeout_observations'
AUTHORITIES=('spec/source/sections/003-2-type-system.md','spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md','spec/source/sections/009-8-error-handling.md',
 'spec/source/sections/011-10-concurrency-and-coroutines.md','spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md','stdlib/types/coroutine.xr','stdlib/time/time.xr')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677', 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915', 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d', 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b', 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea', 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380', 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e', 'stdlib/types/coroutine.xr': 'f85c868bf12b2b90988ead5cf746fd8e522d5c2d6398a64882b47637c72dc7a6', 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72', 'tests/regression/11_coroutine/1117_await_timeout.xr': '33374094c1db7c1bd382ab3e466805ee2d143e28a31148a80e56e11838ac6890'}


class LogicalTask:
    def __init__(self,value):
        self.value=value;self.completed=False;self.cancelled=False;self.timeout_observations=0

    def await_timeout(self,milliseconds,complete_before_observation):
        if type(milliseconds) is not int or milliseconds<=0 or type(complete_before_observation) is not bool:
            raise ValueError('Selected original has positive waits and a closed completion/deadline choice')
        if complete_before_observation:self.completed=True
        if self.completed:return ('Success',self.value)
        self.timeout_observations+=1
        return ('Timeout',None)

    def await_value(self):
        self.completed=True
        return self.value


def timeout_trace(first_completed,second_completed,slow_completed):
    if any(type(value) is not bool for value in (first_completed,second_completed,slow_completed)):
        raise ValueError('Unreviewed outcome shape')
    tasks=[LogicalTask(value) for value in (42,42,42,7)]
    first=tasks[0].await_timeout(1000,first_completed)
    second=tasks[1].await_timeout(100,second_completed)
    already=tasks[2].await_value()
    third=tasks[2].await_timeout(100,False)
    fourth=tasks[3].await_timeout(1,slow_completed)
    final_slow=tasks[3].await_value()
    extracted=lambda result:result[1] if result[0]=='Success' else -1
    values=['test1: fast task with timeout','  result: '+str(extracted(first)),
            'test2: immediate completion','  result: '+str(extracted(second)),
            'test3: already done task','  result: '+str(extracted(third)),
            'test4: timeout returns TaskResult.Timeout','  timeout: '+str(fourth[0]=='Timeout').lower()]
    return {'values':values,'outcome_tags':[result[0] for result in (first,second,third,fourth)],
        'original_timeout_milliseconds':[1000,100,100,1],'original_slow_sleep_milliseconds':50,
        'original_go_calls':4,'original_plain_awaits':2,'original_named_tests':['test_await_timeout'],
        'original_helpers':['fast_task','slow_task'],'root_calls_original_named_wrapper':True,
        'already_awaited_fast_value':already,'post_timeout_plain_await_slow_value':final_slow,
        'timeout_observation_did_not_cancel_slow':not tasks[3].cancelled,
        'already_completed_task_can_timeout':False,
        'actual_timer_firing_latency_fairness_or_deadline_winner_measured':False,
        'runtime_FI_resources_lifetime_cancellation_safety_provider_mixed_qualified':False}


def validate_inputs():
    if set(_INPUTS)!={SOURCE,*AUTHORITIES}:raise ValueError('Complete timeout source or authority dropped')
    for name,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete await-timeout input changed: '+name)


def output_options():
    return sorted({''.join(value+'\n' for value in timeout_trace(*shape)['values'])
                   for shape in itertools.product((False,True),repeat=3)})


def expected_rows():
    validate_inputs();options=output_options()
    representative=''.join(value+'\n' for value in timeout_trace(True,True,False)['values'])
    return [{'id':IDENTITY,'source':SOURCE,'original_source':SOURCE,'required_target':None,
        'expected':{'exit_code':0,'stderr_utf8':'','stdout_utf8':representative,
                    'stdout_expectation_kind':KIND,'stdout_utf8_options':options},'observation_count':8,
        'required_semantic_traces':[timeout_trace(*shape) for shape in itertools.product((False,True),repeat=3)],
        'separate_original_named_collection_obligation':{'names':['test_await_timeout'],'qualified_by_root_run':False},
        'model':'Complete unchanged1117 root/wrapper/twohelpers,all8prints,fourgo/fourawaitTimeout/twoplainawait,1000/100/100/1ms waits andslow sleep50. No start barrier or host scheduling latency bound guarantees fresh fast-task completion before timeout or slow-task incompletion at call entry. Firsttwo extractedvalues each42/-1;third after plainawait must42;last Timeoutboolean true/false. All8whole8line outputs,not arbitrary regex or dropped lines. WaitTimeout doesnotcancel slowtask,andfinalplainawait mustcomplete7 without printingit. Doesnot qualify exact timer behavior,actualtimeout occurrence,fairness/latency,cancellation,payload Error/PanicInfo,resources/lifetime/FI/safety/provider/mixed ororiginal CLI named hook.'}]


def expected_outputs(oracle):
    owned=expected_rows()[0]
    if (oracle.get('id')!=IDENTITY or oracle.get('source')!=SOURCE or oracle.get('original_source')!=SOURCE or
            oracle.get('expected')!=owned['expected'] or oracle.get('observation_count')!=8):
        raise ValueError('Unowned or changed complete await-timeout output domain')
    return tuple(text.encode('utf-8') for text in owned['expected']['stdout_utf8_options'])


def matches(oracle,result):
    outputs=expected_outputs(oracle)
    return result is not None and not result.timed_out and result.returncode==0 and not result.stderr and result.stdout in outputs
