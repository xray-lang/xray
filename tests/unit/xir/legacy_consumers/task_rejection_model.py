"""Bind the controlled Task rejection to exact sources and owned type proofs."""
from pathlib import Path
import hashlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
_INPUTS = {'spec/source/sections/003-2-type-system.md': 'b53fe65114ebc6cfe5d480e620fb9ed0d809c08e2392f0287061646b9fe1f677',
 'spec/source/sections/015-14-built-in-type-methods.md': '0aa7a88b717533bd6bed274b5c7b7136ac355d1581db7122890fe5beb787295e',
 'src/api/xsource_compile.h': '502cfdddebfb278ab2b3deaa3e27371aa4ecdb9ccba43b472189aa2036ffaf7e',
 'src/base/xcompile_resources.c': 'fcf26222e273a27348bddbe01381f5e64d3b1cfc2282d65472976bd5c5fa405f',
 'src/base/xcompile_resources.h': 'f837d59bd49fdb1b8630017c1d112af280bcd3d3ba6a34aa15c88a876b50b6b0',
 'src/base/xmalloc.h': 'bc5038d87f8c37ff556fbd1ecc1b086c3f35c21d3cd3571eaae957ec8f4b4557',
 'src/program/xr_xir_source_product.h': 'e77fbc930a2a1ef5b327146bc94d808d9dfb54feb8c319ce41aed51e6994c240',
 'src/xir/xxir.h': '3753e53bceab8162ee3c566f3d6fed27e950c443dae8f0bcb974e4ba53961041',
 'src/xir/xxir_checked.h': '88bca2535d0eb76a1b95533a65b0f842bfeaf74554364939d7f5d210e9c38218',
 'src/xir/xxir_constraint_proof.h': 'c0f066ec2dd991fa02bdd6f7735b4c85255145e75f821b8a46f33fe7a45536d5',
 'src/xir/xxir_nominal.h': '8a1d9e940206d3cf35f78d9ac0719ae9bb7b8afed1a00d66baa0770485bd1e38',
 'src/xir/xxir_source_task_type.inc.c': 'b891bd32ab468133012098c50c74f6235da6f3a3e5a5d1d346bcb836ed0421e1',
 'src/xir/xxir_task_verify.inc.c': '1f29133b74148988ccd453f334cdee0847cb67a803e34d5eb0e926f91949990a',
 'src/xir/xxir_types.h': '9cb358b9604f9298105c845e8dbd6012de147d6517a10cad3a3020042edef7d3',
 'src/xir/xxir_value.h': '08ea52eeffffd623eaa2dfd66cfc74ebe8dc772a4dc82b170d2ed4551cda4adc',
 'tests/diff/cases/semantics/coro/task_transfer_return_native_class.xr': '68952e6c3b9d277b1e6fcb47c5f98e0657638f89fdcc00bdb0843d1c7b2549f7',
 'tests/lib/xraytest/__init__.py': '39d234cb4da7097df911c0a99fdfabe7fcc829beeefc536ab1108503217b640d',
 'tests/lib/xraytest/platform.py': 'c03df3dc8b3381586b2d8832edde82cc5bcdaa6c745bf291e6d7826f338bce53',
 'tests/lib/xraytest/proc.py': '9418f1b80e3c14f85bea25d8a25753423333914da7435a0c3a66d963581dd1f5',
 'tests/unit/xir/legacy_consumers/build_source_rejection_owner.py': 'd256154dd986b1811435360ea39db6dd429ef823027ac37964cee03bfdc77505',
 'tests/unit/xir/legacy_consumers/fixtures/source_rejection_controls/direct_class.xr': '36a881ed7694d823946f159e01bdb539571bb5f9d34a261716e53d3dd161a12e',
 'tests/unit/xir/legacy_consumers/fixtures/source_rejection_controls/go_i64.xr': 'a2aab9569362f06c064851cf5390a243c9ded84855fa7b2547addf761bcced9b',
 'tests/unit/xir/legacy_consumers/fixtures/task_nonsendable_class_result/root.xr': '68952e6c3b9d277b1e6fcb47c5f98e0657638f89fdcc00bdb0843d1c7b2549f7',
 'tests/unit/xir/legacy_consumers/run_full_diff_driver.py': 'bbaf3de9b634eb9ce53c4c9e8dae231eadb1fbfbfade1b5a45de148036359046',
 'tests/unit/xir/legacy_consumers/run_product_driver.py': 'c3376d7844770e2b124d390f696099b04107b4ca1ee388f37b76ec4c43e8aee5',
 'tests/unit/xir/legacy_consumers/run_source_rejection_driver.py': '48c15ece6c20f6cdceb29ccbc04e49e32a74eb30e0f12d7edd11d236308eb1d3',
 'tests/unit/xir/legacy_consumers/run_source_rejection_faults.py': '671b105dff7584067d799b9df5955766d81580bd87efe996dae03a4e45abf00d',
 'tests/unit/xir/legacy_consumers/source_rejection_owner/CMakeLists.txt': '64042a2db0abc88fc8a678bc0bec777d8c86fe9aaca65d999cf86dcbfe50f025',
 'tests/unit/xir/legacy_consumers/source_rejection_owner/source_rejection_faults.c': '24a43b0852c8a461d71cb754f51950d23b8fbacd2845fbcf6341b7c3285bbaf4',
 'tests/unit/xir/legacy_consumers/source_rejection_owner/source_rejection_owner.c': '6ee80b8f6ec209c35dace65abf01eb65810bb3e9e875356f979dc30ae18ee669',
 'tests/unit/xir/legacy_consumers/test_owned_process_tree.py': '65ac2e9ed33bc8542ac84dfd466b94e8c37425e04834b8ad5e691d6055145272',
 'tests/unit/xir/legacy_consumers/test_source_rejection_fault_review.py': '63f63ce2294f9088e33b845e2f9f19d2e801baec81464465bddcdaafbec4d977',
 'tests/unit/xir/legacy_consumers/test_source_rejection_model.py': '440722be5661c4f394092032715a7c4008dcc40a1c4accaa193cdcfe3faa62b3',
 'tests/unit/xir/xir_instance_compile_observer.h': 'd1c48e7330b30d7d9ea6044e5fea086c7d5181b92a77915aa6b90903a68b695d'}
_REQUIRED_INPUTS = ('spec/source/sections/003-2-type-system.md',
 'spec/source/sections/015-14-built-in-type-methods.md',
 'src/api/xsource_compile.h',
 'src/base/xcompile_resources.c',
 'src/base/xcompile_resources.h',
 'src/base/xmalloc.h',
 'src/program/xr_xir_source_product.h',
 'src/xir/xxir.h',
 'src/xir/xxir_checked.h',
 'src/xir/xxir_constraint_proof.h',
 'src/xir/xxir_nominal.h',
 'src/xir/xxir_source_task_type.inc.c',
 'src/xir/xxir_task_verify.inc.c',
 'src/xir/xxir_types.h',
 'src/xir/xxir_value.h',
 'tests/diff/cases/semantics/coro/task_transfer_return_native_class.xr',
 'tests/lib/xraytest/__init__.py',
 'tests/lib/xraytest/platform.py',
 'tests/lib/xraytest/proc.py',
 'tests/unit/xir/legacy_consumers/build_source_rejection_owner.py',
 'tests/unit/xir/legacy_consumers/fixtures/source_rejection_controls/direct_class.xr',
 'tests/unit/xir/legacy_consumers/fixtures/source_rejection_controls/go_i64.xr',
 'tests/unit/xir/legacy_consumers/fixtures/task_nonsendable_class_result/root.xr',
 'tests/unit/xir/legacy_consumers/run_full_diff_driver.py',
 'tests/unit/xir/legacy_consumers/run_product_driver.py',
 'tests/unit/xir/legacy_consumers/run_source_rejection_driver.py',
 'tests/unit/xir/legacy_consumers/run_source_rejection_faults.py',
 'tests/unit/xir/legacy_consumers/source_rejection_owner/CMakeLists.txt',
 'tests/unit/xir/legacy_consumers/source_rejection_owner/source_rejection_faults.c',
 'tests/unit/xir/legacy_consumers/source_rejection_owner/source_rejection_owner.c',
 'tests/unit/xir/legacy_consumers/test_owned_process_tree.py',
 'tests/unit/xir/legacy_consumers/test_source_rejection_fault_review.py',
 'tests/unit/xir/legacy_consumers/test_source_rejection_model.py',
 'tests/unit/xir/xir_instance_compile_observer.h')
ORIGINAL = 'tests/diff/cases/semantics/coro/task_transfer_return_native_class.xr'
SELECTED = 'tests/unit/xir/legacy_consumers/fixtures/task_nonsendable_class_result/root.xr'
CONTROLS = ['tests/unit/xir/legacy_consumers/fixtures/source_rejection_controls/direct_class.xr',
            'tests/unit/xir/legacy_consumers/fixtures/source_rejection_controls/go_i64.xr']
IDENTITY = 'task_nonsendable_checked_rejection'


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def expected():
    if not _INPUTS or set(_INPUTS) != set(_REQUIRED_INPUTS) or len(_INPUTS) != len(_REQUIRED_INPUTS):
        raise ValueError('Task rejection lost a reviewed source, control or authority')
    for name, value in _INPUTS.items():
        if digest((ROOT / name).read_bytes().replace(b'\r\n', b'\n')) != value:
            raise ValueError('Task rejection input changed: ' + name)
    source = (ROOT / SELECTED).read_text(encoding='utf-8')
    direct = 'var task = go buildBox()\nvar box = await task'
    if source.count(direct) != 1:
        raise ValueError('Task rejection lost its exact worker/await boundary')
    controls = [source.replace(direct, 'var box = buildBox()'),
                source.replace('fn buildBox() -> Box', 'fn buildBox() -> i64')
                      .replace('return Box(41)', 'return 41').replace('print(box.value + 1)', 'print(box + 1)')]
    if any((ROOT / name).read_text(encoding='utf-8') != control
           for name, control in zip(CONTROLS, controls)):
        raise ValueError('Task controls changed the reviewed complete source transformation')
    return {'id': IDENTITY, 'source': SELECTED, 'original_source': ORIGINAL,
            'source_lf_sha256': _INPUTS[SELECTED], 'kind': 'source_rejection',
            'controls': [{'source': name, 'lf_sha256': _INPUTS[name],
                          'expected': {'exit_code': 0, 'stdout_utf8': str(41 + 1) + '\n', 'stderr_utf8': ''}}
                         for name in CONTROLS],
            'expected': {'stage': 'CHECK', 'status': 'BAD_TYPE', 'product_published': False,
                         'class_storage': True, 'class_sendable': False, 'scalar_sendable': True},
            'qualification_scope': 'CONTROLLED_CHECKED_TASK_RESULT_REJECTION',
            'physical_release_and_complete_FI': 'OPEN'}


def for_selection(selection):
    row = expected()
    if selection['case_id'] != ORIGINAL:
        return None
    if (not selection['rejection_oracle_required'] or selection['selected_source'] != SELECTED
            or selection['selected_source_lf_sha256'] != row['source_lf_sha256']
            or selection.get('target_disposition') != 'REJECT_NONSENDABLE_TASK_RESULT_AFTER_FROZEN_TASK313_PORT'
            or selection.get('arguments', []) != [] or selection.get('stdin_hex', '') != ''):
        raise ValueError('Controlled Task rejection lost its exact selected identity')
    return row


def validate_report(report, identities):
    if (type(report.get('schema')) is not int or report.get('schema') != 1 or report.get('scope') != 'CONTROLLED_CHECKED_TASK_RESULT'
            or report.get('qualified') is not True or report.get('physical_zero_verified') is not False):
        raise ValueError('SDK rejection owner did not qualify the controlled group')
    if (set(identities) != {'checked_schema', 'semantic_identity', 'value_abi'}
            or any(type(report.get(name)) is not int or type(value) is not int
                   or report.get(name) != value for name, value in identities.items())):
        raise ValueError('SDK rejection owner compiler identities changed')
    cases = report.get('cases', [])
    if len(cases) != 3 or [row.get('ordinal') for row in cases] != [0, 1, 2]:
        raise ValueError('SDK rejection owner omitted or reordered a required control')
    numeric = ['ordinal', 'status', 'public_stage', 'check_stage', 'check_status',
               'source_status', 'function_count', 'storage', 'sendable']
    flags = ['product_published', 'packet_owned', 'producer_dropped', 'result_i64', 'result_class', 'qualified']
    if any(type(row.get(name)) is not int for row in cases for name in numeric) or any(
            type(row.get(name)) is not bool for row in cases for name in flags):
        raise ValueError('SDK rejection owner changed a typed protocol field')
    if any(row.get('qualified') is not True for row in cases):
        raise ValueError('SDK rejection owner has an unqualified source')
    negative, direct, scalar = cases
    required = {'status': 9, 'public_stage': 4, 'check_stage': 1, 'check_status': 3,
                'source_status': 3, 'product_published': False, 'packet_owned': False}
    if any(negative.get(key) != value for key, value in required.items()):
        raise ValueError('Task must reject at CHECK/BAD_TYPE without publishing a product')
    for row in [direct, scalar]:
        if (row.get('status') != 0 or row.get('product_published') is not True
                or row.get('packet_owned') is not True or row.get('producer_dropped') is not True
                or row.get('function_count') != 1 or row.get('storage') != 0):
            raise ValueError('Task control lacks its authentic owned SOURCE proof')
    if (direct.get('sendable') != 3 or direct.get('result_class') is not True
            or direct.get('result_i64') is not False or scalar.get('sendable') != 0
            or scalar.get('result_i64') is not True or scalar.get('result_class') is not False):
        raise ValueError('Task controls do not distinguish ordinary class from Sendable i64')
    baseline = report.get('ledger_baseline')
    if type(baseline) is not int or baseline < 0 or report.get('ledger_final') != baseline:
        raise ValueError('SDK rejection owner did not restore its actual ledger baseline')

# Current consumer changes bind separately from the preserved compiler authorities.
_INPUTS["tests/unit/xir/legacy_consumers/fixed_source_rejection_owner.py"]="3a0f08834e64d315e4211abccc63e5c442b36c123fe1a276bacee164cb412764"
_INPUTS["tests/unit/xir/legacy_consumers/test_fixed_source_rejection_owner.py"]="4f16e1771462696ac416d8afe2a856e149d3c0deff162e0b8a60519d5a315471"
_REQUIRED_INPUTS=_REQUIRED_INPUTS+("tests/unit/xir/legacy_consumers/fixed_source_rejection_owner.py","tests/unit/xir/legacy_consumers/test_fixed_source_rejection_owner.py",)

_INPUTS["tests/unit/xir/legacy_consumers/fixed_product_inputs.py"]="21111beaceed24e020bbf78f945911b3a71c5012bdf587b4e5330477af410197"
_REQUIRED_INPUTS=_REQUIRED_INPUTS+("tests/unit/xir/legacy_consumers/fixed_product_inputs.py",)
