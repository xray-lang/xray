"""Choose one reviewed source and one exact independent oracle per original input."""
from pathlib import Path
import hashlib
import complete_network_model
import complete_await_timeout_model
import complete_task_status_model
import complete_parallel_api_model


def digest(data):
    return hashlib.sha256(data).hexdigest()


def owned_path(root, relative):
    path = Path(relative)
    if path.is_absolute() or '..' in path.parts:
        raise ValueError('consumer input must keep its explicit repository-relative identity')
    return root / path


def checked_source(root, relative, expected):
    path = owned_path(root, relative)
    if digest(path.read_bytes().replace(b'\r\n', b'\n')) != expected:
        raise ValueError('reviewed consumer source changed: ' + relative)
    return path


def context(root, source, declared, runner):
    arguments = runner.read_args(source)
    stdin = runner.read_stdin(source)
    if declared['arguments'] != arguments or declared['stdin_hex'] != stdin.hex():
        raise ValueError('reviewed consumer arguments or stdin changed')
    for key, suffix in [('arguments_sidecar', '.args'), ('stdin_sidecar', '.stdin')]:
        item = declared.get(key)
        if item is None:
            continue
        path = source.with_suffix(suffix)
        if item['path'] != path.relative_to(root).as_posix():
            raise ValueError('consumer sidecar identity changed')
        if item['state'] == 'MISSING':
            if path.exists():
                raise ValueError('a previously missing consumer sidecar appeared')
        elif item['state'] == 'PRESENT':
            raw = path.read_bytes()
            if (len(raw) != item['bytes'] or digest(raw) != item['sha256']
                    or digest(raw.replace(b'\r\n', b'\n')) != item['lf_sha256']):
                raise ValueError('reviewed consumer sidecar bytes changed')
        else:
            raise ValueError('consumer sidecar has no reviewed presence state')
    return list(arguments), stdin.hex()


def select(row, root, runner):
    identity = row['case_id']
    original = checked_source(root, identity, row['source_lf_sha256'])
    declared = row.get('original_differential_obligation')
    original_arguments, original_stdin = context(root, original, declared, runner) if declared else ([], '')
    port = row.get('target_source_port')
    disposition = row.get('target_disposition', {}).get('kind', '')
    if disposition.endswith('_PORT') and not port:
        raise ValueError('frozen consumer migration lost its required source port')
    selected = identity
    selected_digest = row['source_lf_sha256']
    arguments, stdin = original_arguments, original_stdin
    companions = []
    if port:
        if (port['qualification'] != 'NOT_RUN' or port['owner_package'] not in row['producer_packages']
                or not port.get('frozen_basis')):
            raise ValueError('canonical consumer port has no reviewed authority')
        selected = port['fixture']
        selected_digest = port['lf_sha256']
        checked_source(root, selected, selected_digest)
        for item in port.get('companion_inputs', []):
            checked_source(root, item['path'], item['lf_sha256'])
            companions.append(dict(item))
        if 'differential_input_obligation' in port:
            arguments, stdin = context(root, owned_path(root, selected), port['differential_input_obligation'], runner)
    return {'case_id': identity, 'original_source': identity,
            'original_source_lf_sha256': row['source_lf_sha256'],
            'selected_source': selected, 'selected_source_lf_sha256': selected_digest,
            'canonical_port_selected': bool(port), 'target_disposition': disposition,
            'original_arguments': original_arguments, 'original_stdin_hex': original_stdin,
            'arguments': arguments, 'stdin_hex': stdin, 'companion_inputs': companions,
            'rejection_oracle_required': disposition.startswith(('REJECT_', 'REJECTION_'))}


def expected_for(selection, preserved, models):
    """Require canonical oracles to name the selected source explicitly."""
    if selection['rejection_oracle_required']:
        return None
    selected = selection['selected_source']
    identity = selection['case_id']
    candidates = []
    for row in preserved:
        expected = row.get('expected', {})
        if not {'exit_code', 'stdout_utf8'} <= set(expected):
            continue
        explicit = row.get('execution_source')
        if selection['canonical_port_selected']:
            applies = explicit == selected
        else:
            applies = explicit == selected if explicit else selected in {item['path'] for item in row['inputs']}
        if applies:
            bound = [item for item in row['inputs'] if item['path'] == selected]
            if (len(bound) != 1 or bound[0]['lf_sha256'] != selection['selected_source_lf_sha256']):
                raise ValueError('independent consumer oracle lost its selected-source binding')
            candidates.append(row)
    for model in models:
        if (model.get('source') == selected
                and model.get('original_source', model['source']) == identity):
            candidates.append(model)
    if not candidates:
        return None
    first = candidates[0]
    keys = ('exit_code', 'stdout_utf8', 'stderr_utf8', 'diagnostic_code',
            'stdout_expectation_kind', 'stdout_utf8_options')
    for row in candidates[1:]:
        if any(first['expected'].get(key, '') != row['expected'].get(key, '') for key in keys):
            raise ValueError('independent selected-source consumer oracles disagree: ' + identity)
    return candidates[-1]


def projection_valid(result, generated):
    if result.returncode or result.timed_out or result.stderr or not generated.is_file():
        return False
    data = generated.read_bytes()
    return (b'const XrXirProgramSpec xray_app_program' in data and b'XrXirCallEntry' in data
            and b'xrt_vm_run' not in data and b'XrProto' not in data)


def observation_status(selection, oracle, vm, native, projection, original_sidecar=None):
    if selection['rejection_oracle_required']:
        return 'EXECUTED_FROZEN_TARGET_REJECTION_ORACLE_PENDING'
    if oracle:
        if (oracle.get('id')==complete_await_timeout_model.IDENTITY or
                oracle.get('expected',{}).get('stdout_expectation_kind')==complete_await_timeout_model.KIND):
            return ('PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE'
                    if projection and complete_await_timeout_model.matches(oracle,vm) and complete_await_timeout_model.matches(oracle,native)
                    else 'FAIL_INDEPENDENT_SELECTED_CONSUMER')
        if (oracle.get('id') in complete_parallel_api_model.IDENTITIES or
                oracle.get('expected',{}).get('stdout_expectation_kind')==complete_parallel_api_model.KIND):
            return ('PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE'
                    if projection and complete_parallel_api_model.matches(oracle,vm) and complete_parallel_api_model.matches(oracle,native)
                    else 'FAIL_INDEPENDENT_SELECTED_CONSUMER')
        if (oracle.get('id')==complete_task_status_model.IDENTITY or
                oracle.get('expected',{}).get('stdout_expectation_kind')==complete_task_status_model.KIND):
            return ('PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE'
                    if projection and complete_task_status_model.matches(oracle,vm) and complete_task_status_model.matches(oracle,native)
                    else 'FAIL_INDEPENDENT_SELECTED_CONSUMER')
        if (oracle.get('id','').startswith('complete_network_') or
                any(key in oracle.get('expected',{}) for key in ('stdout_expectation_kind','stdout_utf8_options'))):
            return ('PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE'
                    if projection and complete_network_model.matches(oracle,vm) and complete_network_model.matches(oracle,native)
                    else 'FAIL_INDEPENDENT_SELECTED_CONSUMER')
        expected = oracle['expected']
        def match(result):
            if (result is None or result.timed_out or result.returncode != expected['exit_code']
                    or result.stdout != expected['stdout_utf8'].encode('utf-8')):
                return False
            if 'stderr_utf8' in expected:
                return result.stderr == expected['stderr_utf8'].encode('utf-8')
            code = expected.get('diagnostic_code')
            return code.encode('utf-8') in result.stderr if code else not result.stderr
        return ('PASS_INDEPENDENT_SELECTED_RUN_BYTE_SCOPE' if projection and match(vm) and match(native)
                else 'FAIL_INDEPENDENT_SELECTED_CONSUMER')
    if selection['canonical_port_selected']:
        return 'EXECUTED_CANONICAL_TARGET_INDEPENDENT_ORACLE_PENDING'
    if original_sidecar is not None:
        matched = (native is not None and not vm.timed_out and not native.timed_out
                   and vm.stdout == original_sidecar and native.stdout == original_sidecar)
        return 'OBSERVED_ORIGINAL_STDOUT_MATCH_OTHER_DUTIES_OPEN' if matched else 'FAIL_ORIGINAL_STDOUT_SCOPE'
    return 'EXECUTED_INDEPENDENT_ORACLE_PENDING'
