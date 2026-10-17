"""Complete Task status trace with both unstarted and started pending shapes."""
from pathlib import Path
import hashlib

ROOT=Path(__file__).resolve().parents[4]
SOURCE='tests/regression/11_coroutine/1116_task_status.xr'
IDENTITY='complete_task_status_1116'
KIND='finite_complete_task_nonterminal_statuses'
AUTHORITIES=('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/005-4-statements.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/011-10-concurrency-and-coroutines.md',
 'spec/source/sections/014-13-built-in-functions.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'stdlib/types/coroutine.xr',
 'stdlib/types/channel.xr')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea',
 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'stdlib/types/channel.xr': 'ff2be02bcc28be44f7927b746d316eec7fef4e815f2693207ef07176bbbf1284',
 'stdlib/types/coroutine.xr': 'f85c868bf12b2b90988ead5cf746fd8e522d5c2d6398a64882b47637c72dc7a6',
 'tests/regression/11_coroutine/1116_task_status.xr': '29f8a8ef5d78886e03b9f950f4bde200e2c990d233768ab4d56e2ec9dd6eb653'}


def status_trace(before_send):
    if before_send not in ('Pending','Running'):
        raise ValueError('No send or cancellation has occurred;the first Task cannot be terminal')
    # The two empty rendezvous channels cannot fabricate a value or completion.
    sent=21
    first_result=sent
    second_result='Cancelled'
    third_result=21*2
    values=['before send:','  done: false','  status: TaskStatus.'+before_send,
            'after await:','  done: true','  result: '+str(first_result),
            'after cancel:','  done: true','  status: TaskStatus.'+second_result,
            '  cancelled result: '+str(second_result=='Cancelled').lower(),
            'completed:','  done: true','  status: TaskStatus.Success','  result: '+str(third_result)]
    return {'values':values,'before_send_status':before_send,'before_send_done':False,
        'first_send':sent,'first_result':first_result,'cancelled_result':second_result,'third_result':third_result,
        'original_workers':3,'original_helpers':2,'original_plain_awaits':2,'original_awaitResult_calls':1,
        'original_cancel_calls':1,'original_channel_capacities':[0,0],'original_close_calls':2,
        'original_root_calls':['test_main'],'original_named_tests':['test_main'],
        'cancellation_waits_on_no_sender_not_on_a_short_sleep':True,
        'physical_scheduler_start_order_or_latency_measured':False,
        'runtime_channel_cancel_resources_lifetime_FI_safety_provider_mixed_qualified':False}


def validate_inputs():
    if set(_INPUTS)!={SOURCE,*AUTHORITIES}:
        raise ValueError('Task status domain lost a complete original or authority')
    for name,digest in _INPUTS.items():
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete task status input changed: '+name)


def output_options():
    return [''.join(value+'\n' for value in status_trace(state)['values']) for state in ('Pending','Running')]


def expected_rows():
    validate_inputs()
    options=output_options()
    return [{'id':IDENTITY,'source':SOURCE,'original_source':SOURCE,'required_target':None,
        'expected':{'exit_code':0,'stderr_utf8':'','stdout_utf8':options[0],
                    'stdout_expectation_kind':KIND,'stdout_utf8_options':options},
        'observation_count':14,'required_semantic_traces':[status_trace(state) for state in ('Pending','Running')],
        'separate_original_named_collection_obligation':{'names':['test_main'],'qualified_by_root_run':False},
        'inputs':[{'path':name,'role':'preserved_source' if name==SOURCE else 'authority','lf_sha256':digest} for name,digest in sorted(_INPUTS.items())],
        'model':'Whole unchanged1116 with its existing root call andtwohelpers. Before any send,an empty rendezvous task cannot finish:published TaskStatus permits Pending before startup or Running after startup. No start barrier precedes the print. Both complete14line outputs retain all remaining fixed done/status/cancelled/value observations,qualified TaskStatus display andoriginal channel0/0,send21,threego/twoawait/oneawaitResult/onecancel/twoclose. Current enum names align with the existing typed-status formatter model,not old unqualified output comments. No arbitrary regex or dropped lines;actual startup scheduling,cancellation/checkpoints,resource/lifetime/FI/safety/provider/mixed andoriginal CLI named hook remain separate obligations.'}]


def expected_outputs(oracle):
    owned=expected_rows()[0]
    if (oracle.get('id')!=IDENTITY or oracle.get('source')!=SOURCE or oracle.get('original_source')!=SOURCE or
            oracle.get('expected')!=owned['expected'] or oracle.get('observation_count')!=14):
        raise ValueError('Unowned or changed complete Task status output domain')
    return tuple(text.encode('utf-8') for text in owned['expected']['stdout_utf8_options'])


def matches(oracle,result):
    outputs=expected_outputs(oracle)
    return (result is not None and not result.timed_out and result.returncode==0 and not result.stderr and result.stdout in outputs)
