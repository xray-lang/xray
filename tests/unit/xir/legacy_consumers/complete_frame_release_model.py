"""Whole frame-drop original; arithmetic observations and its required meter bound."""
from frozen_derivation_inputs import resolve as resolve_derivation
from pathlib import Path
import hashlib
ROOT=Path(__file__).resolve().parents[4]
SOURCE='tests/diff/cases/semantics/coro/suspended_frame_release.xr'
IDENTITY='complete_suspended_frame_release'
KIND='complete_frame_drop_original_with_required_meter_bound'
AUTHORITIES=('spec/source/sections/003-2-type-system.md', 'spec/source/sections/005-4-statements.md', 'spec/source/sections/006-5-declarations.md', 'spec/source/sections/009-8-error-handling.md', 'spec/source/sections/011-10-concurrency-and-coroutines.md', 'spec/source/sections/014-13-built-in-functions.md', 'spec/source/sections/015-14-built-in-type-methods.md', 'spec/source/sections/016-15-standard-library.md', 'stdlib/types/channel.xr', 'stdlib/types/coroutine.xr', 'stdlib/time/time.xr', 'stdlib/runtime/runtime.xr', 'stdlib/runtime/runtime.c', 'stdlib/native_leaf_allowlist.toml', 'src/coro/xaot_runtime.c')
_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677', 'spec/source/sections/005-4-statements.md': 'c5849dfeea58b583f6d7685727fde751995b598e734118fce4436d8f2d066915', 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d', 'spec/source/sections/009-8-error-handling.md': '3679ded015ad0222df39070b598fe26c8099247f9e410305a3f1527b9e63161b', 'spec/source/sections/011-10-concurrency-and-coroutines.md': '27c232a9575d0d40e800ea5730e5d2b96e8f2e62e646731a3d9de2e0eea35aea', 'spec/source/sections/014-13-built-in-functions.md': '2a4af98d2d429babb3cdcd875279f0d798efcabb852afd2c366e69a12ca50380', 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e', 'spec/source/sections/016-15-standard-library.md': 'c776ecb898f3dfbbac76e6e9eb08da0697041af5b118eb55bfd294a9630ba2fa', 'stdlib/types/channel.xr': 'ff2be02bcc28be44f7927b746d316eec7fef4e815f2693207ef07176bbbf1284', 'stdlib/types/coroutine.xr': 'f85c868bf12b2b90988ead5cf746fd8e522d5c2d6398a64882b47637c72dc7a6', 'stdlib/time/time.xr': 'd2ab56ef604ba773bf9625efd67450d343e2da2b0a9ec2f179d01ab07cc2bc72', 'stdlib/runtime/runtime.xr': '9f4d87664a727e7b29ceec161efe07d67d4e416c47030b06c682062f8f2fe57c', 'stdlib/runtime/runtime.c': 'e969dd60094f379950636359499ad2951eca95062c89dcae1d29857dc0c08e4f', 'stdlib/native_leaf_allowlist.toml': 'a4cbecc5707c7a731493ef81a44aa9d091f4202d627a699545d8f8fdc90c3023', 'src/coro/xaot_runtime.c': 'eb8d6d4438abcfea4e0f3b7aa73f92983d64c782f3325ed7b88e0279136f7d9b', 'tests/diff/cases/semantics/coro/suspended_frame_release.xr': 'ee330504e157f30db15eb9db896fcc90f0885b0410fa4d206e9be26a170fa9fd'}
CANCEL_POSITIONS=('not_started','before_receive','suspended_receive')


def generator_drop_trace(rounds=200,n=64,take=2):
    if type(rounds) is not int or type(n) is not int or type(take) is not int or not 0<=rounds<=200 or not 1<=take<=n<=64:
        raise ValueError('Unreviewed generator shape')
    payload=[n,n+1,n+2];values=[payload[index%3]+index for index in range(take)]
    return {'per_round_values':values,'sum':rounds*sum(values),'rounds':rounds,'payload_values':payload,
        'logical_frames_created':rounds,'logical_abandons':rounds,'yield_observations':rounds*take,
        'suspended_i_at_abandon':take-1,'post_break_generator_body_resumes':0,
        'physical_byte_or_release_count_measured':False}


def cancelled_drop_trace(positions=('suspended_receive',)*8):
    if len(positions)!=8 or any(position not in CANCEL_POSITIONS for position in positions):
        raise ValueError('All eight original cancel/awaitResult rounds need a reviewed checkpoint')
    results=['Cancelled' for _ in positions]
    return {'count':sum(result=='Cancelled' for result in results),'results':results,'positions':list(positions),
        'original_channel_capacity':1,'channel_has_no_sender_until_after_result':True,
        'worker_payload_length':8,'sleep_before_cancel_milliseconds':1,'cancel_calls':8,'awaitResult_calls':8,'close_calls':8,
        'suspended_frames_guaranteed_by_sleep':False,'physical_byte_or_release_count_measured':False}


def dropped_task_trace(awaited_lengths=(3,)*50,forgotten_finished=(False,)*50):
    if len(awaited_lengths)!=50 or any(type(value) is not int for value in awaited_lengths):
        raise ValueError('All fifty original plain awaits remain')
    if len(forgotten_finished)!=50 or any(type(value) is not bool for value in forgotten_finished):
        raise ValueError('All fifty independent dropped handles remain')
    return {'return':50 if all(value==3 for value in awaited_lengths) else -1,
        'rounds':50,'spawned_workers':100,'plain_awaits':50,'dropped_unawaited_handles':50,
        'forgotten_finished':list(forgotten_finished),'data_length_per_worker':3,
        'forgotten_completion_required_by_return':False,'sleep_after_work_milliseconds':5,
        'physical_byte_or_release_count_measured':False}


def meter_verdict(base,after):
    if any(type(value) is not int or value<0 or value>(1<<63)-1 for value in (base,after)) or base>(1<<63)-1-65536:
        raise ValueError('Unreviewed counter reading outside finite safe normal domain')
    return after<base+65536


def expected_values():
    # The final True is the original required acceptance predicate, not a fake byte measurement.
    return [generator_drop_trace()['sum'],cancelled_drop_trace()['count'],dropped_task_trace()['return'],True]


def render(values):return ''.join((str(value).lower() if type(value) is bool else str(value))+'\n' for value in values)


def validate_inputs():
    if set(_INPUTS)!={SOURCE,*AUTHORITIES}:raise ValueError('Complete frame-drop original or authority dropped')
    for name,digest in _INPUTS.items():
        if hashlib.sha256(resolve_derivation(ROOT, name, digest).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Complete frame-release input changed: '+name)


def expected_rows():
    validate_inputs();text=render(expected_values())
    return [{'id':IDENTITY,'source':SOURCE,'original_source':SOURCE,'required_target':None,
        'expected':{'exit_code':0,'stderr_utf8':'','stdout_utf8':text,
                    'stdout_expectation_kind':KIND,'stdout_utf8_options':[text]},'observation_count':4,
        'independently_derived_algorithmic_output_slots':3,'required_actual_meter_predicate_output_slots':1,
        'required_semantic_trace':{'generator':generator_drop_trace(),'cancelled':cancelled_drop_trace(),'dropped_tasks':dropped_task_trace(),
            'resource_predicate':'after < base + 65536','meter_calls_in_original':2,
            'expected_true_is_required_acceptance_condition_not_predicted_byte_count':True,
            'runtime_counter_scope':'Current execution-local reclamation domain;not all process/child/SDK domains',
            'AOT_counter_not_assumed_constant_zero':True,
            'small_leak_can_still_satisfy_original_bound':True,
            'child_domain_leak_can_be_invisible_to_parent_counter':True,
            'all_original_three_drop_edges_present':True,
            'actual_suspended_startup_timer_progress_or_final_detached_drain_qualified':False,
            'full_physical_release_runtime_FI_safety_provider_mixed_qualified':False},
        'model':'Whole unchanged suspended_frame_release withsixhelpers,counts64/payload3/taketwo200rounds,8cancelrounds/channel1/held8/sleep1/cancel/awaitResult/close,50rounds/twogoeach/tripledata/oneawait/sleep5,two originalruntime.liveBytes reads/all4prints. Generator terms64and66 yield26000,cancel withno sender yields8 across3^8 possible checkpoints,50awaitedlen3 yields50 independentlyof50droppedhandle completion. Fourthtrue is the required originalafter<base+65536acceptancecondition;no fabricatedbase/afterbytes orphysical release inference. Currentdomain counters maymisschildleaks andboundedleak maypass;AOTmeter notassumedzero. Shortsleep doesnotensureactualsuspendedframes ordetachedcompletion. Real meter readings/allthreephysicaldropedges/runtimeFI/resources/lifetime/safety/providers/mixed remain open.'}]


def expected_outputs(oracle):
    owned=expected_rows()[0]
    if (oracle.get('id')!=IDENTITY or oracle.get('source')!=SOURCE or oracle.get('original_source')!=SOURCE or
            oracle.get('expected')!=owned['expected'] or oracle.get('observation_count')!=4 or
            oracle.get('independently_derived_algorithmic_output_slots')!=3 or
            oracle.get('required_actual_meter_predicate_output_slots')!=1 or
            oracle.get('required_semantic_trace')!=owned['required_semantic_trace']):
        raise ValueError('Unowned or changed whole frame-drop oracle')
    return (owned['expected']['stdout_utf8'].encode('utf-8'),)


def matches(oracle,result):
    outputs=expected_outputs(oracle)
    return result is not None and not result.timed_out and result.returncode==0 and not result.stderr and result.stdout in outputs
