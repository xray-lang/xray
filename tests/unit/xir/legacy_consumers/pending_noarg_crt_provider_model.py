from pathlib import Path
import hashlib

ROOT=Path(__file__).resolve().parents[4]
TARGET='windows-x86_64'

_PENDING_NOARG_FFI_SOURCE='tests/diff/cases/semantics/ffi/extern_no_arg_void_and_unused.xr'
_PENDING_NOARG_FFI_FIXTURE='tests/unit/xir/legacy_consumers/fixtures/msvc_ucrt_noarg_provider'
_PENDING_NOARG_FFI_AUTHORITIES=('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/006-5-declarations.md',
 'spec/source/sections/018-17-compilation-pipeline.md',
 'tests/diff/cases/semantics/ffi/xray.toml')
_PENDING_NOARG_FFI_INPUTS={'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/006-5-declarations.md': '17b82ddba9472f20b882826910a089bd13a6b510fb36f2e4d83f08c5f6940b6d',
 'spec/source/sections/018-17-compilation-pipeline.md': '91e9ef4bc0ce560d6621a1dbf8bd6a3f23c58e073e97469b10b7dece5866e440',
 'tests/diff/cases/semantics/ffi/extern_no_arg_void_and_unused.xr': '038419eb18ee48e7ee1d4417241515330379b0f43b4709afc8eda9cf0c6d5aa2',
 'tests/diff/cases/semantics/ffi/xray.toml': '3076508db88c5906218c3c464b740aeb52b25f9d9bc8e757289201a03a327f94',
 'tests/unit/xir/legacy_consumers/fixtures/msvc_ucrt_noarg_provider/CMakeLists.txt': 'cb7d311101c3c76ae16b66737f31104e7e3728b0b36db08ff42c1c2e94324923',
 'tests/unit/xir/legacy_consumers/fixtures/msvc_ucrt_noarg_provider/probe.c': '6d3b2b90a47f87e5866eaf2171882c7b70716b2948e3fa6f6e2d0b47778a7dd4',
 'tests/unit/xir/legacy_consumers/fixtures/msvc_ucrt_noarg_provider/provider-facts.json': '0583c424a22170ea0d813dc28227eadcb998e69ccb835aff388ee07cd4303ca3'}
_UCRT_PROFILE='msvc-x64-ucrt-source-10.0.22621.0'


def ucrt_rand_sequence(seed,count):
    """The reviewed UCRT recurrence, not the unspecified ISO C rand sequence."""
    if type(seed) is not int or not 0<=seed<2**32 or type(count) is not int or not 1<=count<=32:
        raise ValueError('Reviewed provider requires a u32 seed and1..32 complete observations')
    state=seed;values=[]
    for _ in range(count):
        state=(state*214013+2531011)%(2**32)
        values.append((state>>16)&32767)
    return values


def ucrt_host_probe_rows():
    return [str(seed)+':'+str(index)+':'+str(value)
            for seed in (0,7,2**32-1)
            for index,value in enumerate(ucrt_rand_sequence(seed,8),1)]


def validate_pending_noarg_provider_inputs():
    import json
    required={_PENDING_NOARG_FFI_SOURCE,*_PENDING_NOARG_FFI_AUTHORITIES,
        *[_PENDING_NOARG_FFI_FIXTURE+'/'+name for name in ('provider-facts.json','probe.c','CMakeLists.txt')]}
    if set(_PENDING_NOARG_FFI_INPUTS)!=required:
        raise ValueError('Pending foreign provider preparation lost a complete original or authority')
    for name,digest in _PENDING_NOARG_FFI_INPUTS.items():
        if hashlib.sha256((ROOT/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Pending noarg foreign provider input changed: '+name)
    facts=json.loads((ROOT/_PENDING_NOARG_FFI_FIXTURE/'provider-facts.json').read_text(encoding='utf-8'))
    if ((facts['profile'],facts['target'],facts['state_bits'],facts['multiplier'],facts['increment'],facts['output_shift'],facts['output_mask'])!=
            (_UCRT_PROFILE,TARGET,32,214013,2531011,16,32767)):
        raise ValueError('The reviewed CRT algorithm or provider identity changed')


def pending_noarg_crt_expectation(provider_profile):
    validate_pending_noarg_provider_inputs()
    if provider_profile!=_UCRT_PROFILE:
        raise ValueError('No independent rand oracle is granted to an unreviewed C provider')
    value=ucrt_rand_sequence(7,1)[0]
    return {'original_source':_PENDING_NOARG_FFI_SOURCE,'required_provider_profile':_UCRT_PROFILE,
        'conditional_stdout_utf8':str(value)+'\n','independent_value':value,
        'source_calls':[{'name':'srand','argument_values':[7],'arguments':1,'result':'()'},
                        {'name':'rand','argument_values':[],'arguments':0,'result':'i32'}],
        'source_unused_declarations':[{'name':'xrayffi_unused_float','argument_types':['f64'],'result':'f64'}],
        'registered_product_oracle':False,'removed_from_missing_expectations':False,
        'actual_Xray_CRT_module_identity_and_same_thread_state_qualified':False,
        'actual_Xray_noarg_void_descriptors_and_unused_link_manifest_qualified':False,
        'all_other_providers_FI_resources_lifetime_safety_mixed_qualified':False,
        'reason':'Installed provider source derives61 fromseed7. Standalone C can check only that selected provider;the complete Xray program still needs real FFI/CRT binding,void/noarg descriptors and unused extern/link isolation. Do not register61 as a universal Windows or ISO C result.'}
