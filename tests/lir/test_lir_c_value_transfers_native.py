"""Execute generated ownership transfers after retiring their IR and plans."""
from pathlib import Path
import argparse
import json
import os
import subprocess
import tempfile


def run(argv, work):
    print('$ ' + json.dumps([str(item) for item in argv]), flush=True)
    result = subprocess.run([str(item) for item in argv], cwd=work, capture_output=True, timeout=30)
    for data in (result.stdout, result.stderr):
        if data:
            print(data.decode('utf-8', errors='replace'), end='', flush=True)
    if result.returncode:
        raise RuntimeError(f'command exited {result.returncode}')


MANAGED = r'''
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); abort(); } } while (0)
#define OK(c) CHECK((c) == XRT_MANAGED_OK)
typedef XrlValue_TYPE Value;
typedef XrlValue_PODTYPE PodValue;
static PodValue pod_input = {5}, pod_loop = {0}, pod_exit = {37};
static int64_t pod_expected;
typedef struct Node { XrtManagedHeader header; int identifier; } Node;
static Value *watched, *moved;
static XrtManagedHeader *old[2], *expected[2];
static XrtManagedFrame *watched_frame;
static void *watched_payload;
static int64_t expected_score;
static size_t observed;
static int active;
static Value *edge_left, *edge_right, *edge_source_left, *edge_source_right;
static XrtManagedHeader *edge_old[4], *edge_expected[4];
static int64_t phi_left, phi_right, edge_score[2];
static uint8_t edge_tag[2];
static size_t scalar_publications, edge_observed;
static int edge_active;
typedef struct Scalars { int64_t left, right; } Scalars;
static void publish_scalars(void *context) {
    Scalars *values = context;
    phi_left = values->right; phi_right = values->left; ++scalar_publications;
}
static void retired(XrtManagedHeader *node, void *context) {
    (void)context;
    for (unsigned i = 0; edge_active && i < 4; ++i) if (node == edge_old[i]) {
        CHECK(phi_left == 20 && phi_right == 10 && scalar_publications == 1);
        CHECK(pod_exit.f0 == pod_expected && pod_loop.f0 == pod_expected);
        CHECK(edge_left->f1 == edge_score[0] && edge_right->f1 == edge_score[1]);
        CHECK(edge_left->f0.payload == edge_expected[0] && edge_left->f2.payload == edge_expected[1]);
        CHECK(edge_right->f0.payload == edge_expected[2] && edge_right->f2.payload == edge_expected[3]);
        CHECK(edge_left->f2.tag == edge_tag[0] && edge_right->f2.tag == edge_tag[1]);
        CHECK(!edge_source_left->f0.tag && !edge_source_left->f1 && !edge_source_right->f0.tag && !edge_source_right->f1);
        CHECK(!edge_source_left->f0.payload && !edge_source_left->f2.tag && !edge_source_left->f2.payload &&
            !edge_source_right->f0.payload && !edge_source_right->f2.tag && !edge_source_right->f2.payload);
        ++edge_observed;
    }
    if (active && (node == old[0] || node == old[1])) {
        CHECK(watched->f0.tag == 1 && watched->f2.tag == 1 && watched->f1 == expected_score);
        CHECK(watched->f0.payload == expected[0] && watched->f2.payload == expected[1]);
        CHECK(!moved->f0.tag && !moved->f0.payload && !moved->f1 && !moved->f2.tag && !moved->f2.payload);
        CHECK(xrl_edge_1_2_1(watched_frame, watched_payload, NULL, &pod_loop, &pod_loop) == XRT_MANAGED_ARGUMENT);
        ++observed;
    }
}
static XrtManagedHeader *make_node(XrtManagedFrame *frame, size_t slot, XrtManagedOps *ops, int id) {
    XrtManagedHeader *node = NULL;
    if (REAL_STRINGS) {
        OK(lir_c_transfer_string_make(frame, slot, id, &node));
        return node;
    }
    OK(xrt_managed_slot_prepare(frame, slot, ops, sizeof(Node), &node));
    ((Node *)node)->identifier = id;
    OK(xrt_managed_slot_commit(frame, slot));
    return node;
}
static Value *at(void *payload, size_t offset) { return (void *)((unsigned char *)payload + offset); }
static void fill(XrtManagedFrame *frame, Value *value, size_t first, XrtManagedOps *ops, int id) {
    (void)make_node(frame, first, ops, id);
    (void)make_node(frame, first + 1, ops, id + 1);
    value->f0.tag = value->f2.tag = 1; value->f1 = id;
}
int main(void) {
    XrtBillingOwner *billing = NULL; XrtRuntimeTypePlan *plan = NULL; XrtManagedOps *ops = NULL;
    XrtManagedRoot *root = NULL; XrtManagedFrame *caller = NULL; void *caller_payload = NULL;
    OK(xrt_billing_owner_new(NULL, SIZE_MAX, &billing));
    XrtBillingStats baseline = xrt_billing_owner_stats(billing);
    if (REAL_STRINGS) OK(lir_c_transfer_strings_init(billing));
    XrtRuntimeTypeSpec spec = {.fixed_size=sizeof(Node), .alignment=_Alignof(Node),
        .capacity_offset=SIZE_MAX, .drop_payload=retired};
    OK(xrt_runtime_type_plan_new(billing, &spec, &plan)); OK(xrt_managed_ops_bind(billing, plan, &ops));
    OK(xrt_managed_root_new(billing, &root));
    OK(xrt_managed_frame_enter_payload(root, &xrl_frame_1, &caller, &caller_payload));
    Value *landing_value = at(caller_payload, COPY0_OFFSET);
    fill(caller, landing_value, COPY0_FIRST, ops, 100);
    XrlLanding_1 landing = {caller, COPY0_FIRST, COPY0_TEMP_FIRST, landing_value, at(caller_payload, COPY0_TEMP)};
    for (unsigned iteration = 0; iteration < 2; ++iteration) {
        pod_input.f0 = pod_expected = 5 + iteration; pod_loop.f0 = 0; pod_exit.f0 = 37;
        XrtManagedFrame *callee = NULL; void *payload = NULL;
        OK(xrt_managed_frame_enter_payload(root, &xrl_frame_1, &callee, &payload));
        fill(callee, at(payload, COPY0_OFFSET), COPY0_FIRST, ops, 1 + (int)iteration * 10);
        fill(callee, at(payload, COPY1_OFFSET), COPY1_FIRST, ops, 3 + (int)iteration * 10);
        OK(xrl_edge_1_1_1(callee, payload, NULL, &pod_loop, &pod_input)); CHECK(pod_loop.f0 == pod_expected);
        Value *left = at(payload, LOOP0_OFFSET), *right = at(payload, LOOP1_OFFSET);
        int64_t left_score = left->f1, right_score = right->f1;
        OK(xrl_edge_1_2_1(callee, payload, NULL, &pod_loop, &pod_loop)); CHECK(left->f1 == right_score && right->f1 == left_score);
        OK(xrl_edge_1_2_1(callee, payload, NULL, &pod_loop, &pod_loop)); CHECK(left->f1 == left_score && right->f1 == right_score);
        if (!iteration) {
            OK(xrt_place_copy_replace((XrtPlace){callee, LOOP1_FIRST, NULL, XRT_PLACE_NULLABLE},
                (XrtPlace){callee, LOOP0_FIRST, NULL, XRT_PLACE_NULLABLE}, NULL));
            CHECK(left->f0.payload == right->f0.payload && atomic_load(&left->f0.payload->references) == 2);
            OK(xrt_managed_slot_drop(callee, LOOP1_FIRST + 1)); right->f2.tag = 0;
            OK(xrl_edge_1_2_1(callee, payload, NULL, &pod_loop, &pod_loop)); CHECK(!left->f2.tag && !left->f2.payload);
            OK(xrl_edge_1_2_1(callee, payload, NULL, &pod_loop, &pod_loop)); CHECK(!right->f2.tag && !right->f2.payload);
        }
        unsigned char before[4096]; CHECK(xrl_frame_1.payload_size <= sizeof(before));
        phi_left = 10; phi_right = 20; scalar_publications = 0;
        Scalars scalars = {phi_left, phi_right}; XrtPlacePublication scalar_hook = {publish_scalars, &scalars};
        memcpy(before, payload, xrl_frame_1.payload_size);
        XrtManagedSlotLocation location; OK(xrt_managed_slot_location(callee, LOOP0_FIRST, &location));
        *location.busy = true;
        CHECK(xrl_edge_1_2_1(callee, payload, &scalar_hook, &pod_loop, &pod_loop) == XRT_MANAGED_ARGUMENT);
        CHECK(!memcmp(before, payload, xrl_frame_1.payload_size) && phi_left == 10 && phi_right == 20 && !scalar_publications);
        *location.busy = false;
        edge_left = at(payload, EXIT0_OFFSET); edge_right = at(payload, EXIT1_OFFSET);
        fill(callee, edge_left, EXIT0_FIRST, ops, 300); fill(callee, edge_right, EXIT1_FIRST, ops, 400);
        edge_old[0] = edge_left->f0.payload; edge_old[1] = edge_left->f2.payload;
        edge_old[2] = edge_right->f0.payload; edge_old[3] = edge_right->f2.payload;
        edge_expected[0] = left->f0.payload; edge_expected[1] = left->f2.payload;
        edge_expected[2] = right->f0.payload; edge_expected[3] = right->f2.payload;
        edge_score[0] = left->f1; edge_score[1] = right->f1;
        edge_tag[0] = left->f2.tag; edge_tag[1] = right->f2.tag;
        edge_source_left = left; edge_source_right = right; edge_observed = 0; edge_active = 1;
        OK(xrl_edge_1_2_2(callee, payload, &scalar_hook, &pod_exit, &pod_loop)); edge_active = 0;
        CHECK((REAL_STRINGS || edge_observed == 4) && phi_left == 20 && phi_right == 10 && scalar_publications == 1);
        Value *result = at(payload, EXIT0_OFFSET);
        OK(xrt_managed_slot_drop(callee, EXIT1_FIRST)); OK(xrt_managed_slot_drop(callee, EXIT1_FIRST + 1));
        XrtManagedHeader *pending = NULL;
        OK(xrt_managed_slot_prepare(callee, EXIT0_FIRST, ops, sizeof(Node), &pending));
        XrtManagedHeader *prior = landing_value->f0.payload;
        memcpy(before, payload, xrl_frame_1.payload_size);
        CHECK(xrl_return_1_3_3(root, callee, payload, landing) == XRT_MANAGED_ARGUMENT);
        CHECK(landing_value->f0.payload == prior && !memcmp(before, payload, xrl_frame_1.payload_size));
        OK(xrt_managed_slot_abort(callee, EXIT0_FIRST));
        watched = landing_value; moved = result; old[0] = watched->f0.payload; old[1] = watched->f2.payload;
        expected[0] = result->f0.payload; expected[1] = result->f2.payload; expected_score = result->f1;
        watched_frame = callee; watched_payload = payload; size_t was = observed; (void)was; active = 1;
        xrt_billing_owner_fail_after(billing, 0);
        OK(xrl_return_1_3_3(root, callee, payload, landing));
        xrt_billing_owner_fail_after(billing, SIZE_MAX); active = 0;
        CHECK((REAL_STRINGS || observed == was + 2) && landing_value->f1 == expected_score);
        if (REAL_STRINGS) {
            CHECK(lir_c_transfer_string_matches(landing_value->f0.payload, (int)landing_value->f1));
            CHECK(lir_c_transfer_string_matches(landing_value->f2.payload, (int)landing_value->f1 + 1));
        }
    }
    if (REAL_STRINGS) {
        lir_c_transfer_strings_drop();
        CHECK(lir_c_transfer_string_matches(landing_value->f0.payload, (int)landing_value->f1));
    }
    OK(xrt_managed_frame_leave(root, caller)); OK(xrt_managed_root_destroy(&root));
    xrt_managed_ops_drop(ops); xrt_runtime_type_plan_drop(plan);
    XrtBillingStats after = xrt_billing_owner_stats(billing);
    CHECK(after.bytes == baseline.bytes && after.blocks == baseline.blocks); xrt_billing_owner_drop(billing);
    if (REAL_STRINGS) puts("generated C transports real typed String leaves after setup retirement with exact refunds");
    else puts("generated C whole-edge scalar/tag/owner publication and reused caller landing precede all old callbacks");
    return 0;
}
'''

POD = r'''
typedef XrlValue_TYPE Value;
int main(void) {
    Value left = {11}, right = {22};
    xrl_edge_1_1_1(&left, &left, &right, &right);
    xrl_edge_1_2_1(&left, &right, &right, &left);
    if (left.f0 != 22 || right.f0 != 11) return 1;
    xrl_edge_1_2_2(&left, &left, &right, &right);
    return left.f0 != 22 || right.f0 != 11;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--driver', required=True, type=Path)
    parser.add_argument('--compiler', required=True, type=Path)
    parser.add_argument('--kind', required=True, choices=['msvc', 'gnu'])
    parser.add_argument('--include', required=True, type=Path)
    parser.add_argument('--library', required=True, nargs='+')
    parser.add_argument('--msvc-runtime', default='MultiThreadedDLL')
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--asan', action='store_true')
    parser.add_argument('--ubsan', action='store_true')
    parser.add_argument('--ubsan-runtime', type=Path)
    parser.add_argument('--lld-no-tail-merge', action='store_true')
    args = parser.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='value-transfers-', dir=args.work)).resolve()
    runtime = '/MDd' if args.msvc_runtime == 'MultiThreadedDebugDLL' else '/MD' if args.msvc_runtime == 'MultiThreadedDLL' else '/MTd' if args.msvc_runtime == 'MultiThreadedDebug' else '/MT'
    flags = ['/nologo', '/std:c11', '/W4', '/WX', '/utf-8', runtime]
    if args.compiler.stem.lower() != 'clang-cl':
        flags.append('/experimental:c11atomics')
    gnu_flags = []
    link_flags = []
    if args.kind == 'msvc':
        if args.asan:
            flags += ['/fsanitize=address', '/Zi']
            link_flags.append('/INCREMENTAL:NO')
        if args.ubsan:
            if args.compiler.stem.lower() != 'clang-cl':
                raise RuntimeError('UndefinedBehaviorSanitizer requires the Clang provider')
            flags += ['/clang:-fsanitize=undefined', '/clang:-fno-sanitize=function']
        if args.lld_no_tail_merge:
            link_flags.append('/OPT:NOLLDTAILMERGE')
        if args.ubsan_runtime:
            link_flags.append(str(args.ubsan_runtime))
    else:
        enabled = [name for name, on in [('address', args.asan), ('undefined', args.ubsan)] if on]
        if enabled:
            gnu_flags += ['-fsanitize=' + ','.join(enabled), '-fno-omit-frame-pointer', '-g']
        if args.ubsan:
            gnu_flags.append('-fno-sanitize=function')
    fixture = args.include.resolve().parent / 'tests/lir/lir_c_transfer_strings.c'
    adapter = root / ('typed_strings_fixture.obj' if args.kind == 'msvc' else 'typed_strings_fixture.o')
    if args.kind == 'msvc':
        command = [args.compiler, *flags, '/D_CRT_SECURE_NO_WARNINGS', '/D_CRT_NONSTDC_NO_WARNINGS',
                   '/I' + str(args.include), '/I' + str(args.include.parent / 'src'), '/c', fixture, '/Fo' + str(adapter)]
    else:
        command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-D_POSIX_C_SOURCE=200809L', '-I' + str(args.include), '-I' + str(args.include.parent / 'src'),
                   *gnu_flags, '-c', fixture, '-o', adapter]
    run(command, root)
    for name, template in [('managed', MANAGED), ('strings', MANAGED), ('pod', POD)]:
        generated = root / (name + '-generated.c')
        source = root / (name + '.c')
        product = root / (name + ('.exe' if os.name == 'nt' else ''))
        run([args.driver.resolve(), 'pod' if name == 'pod' else 'managed', generated], root)
        facts = json.loads(generated.with_suffix('.c.json').read_text())
        definitions = ''
        for group in ('copies', 'loop', 'exit'):
            for i, home in enumerate(facts[group]):
                prefix = ('COPY' if group == 'copies' else group.upper()) + str(i)
                for key, suffix in [('offset', 'OFFSET'), ('first', 'FIRST'),
                                    ('temporary', 'TEMP'), ('temporary_first', 'TEMP_FIRST')]:
                    definitions += f'#define {prefix}_{suffix} ((size_t)UINT64_C({home[key]}))\n'
        source.write_text('#include ' + json.dumps(generated.as_posix()) + '\n' + definitions +
                          '#include ' + json.dumps(fixture.with_suffix('.h').as_posix()) + '\n' +
                          '#define REAL_STRINGS ' + str(int(name == 'strings')) + '\n' +
                          template.replace('XrlValue_TYPE', 'XrlValue_' + str(facts['type'])).replace(
                              'XrlValue_PODTYPE', 'XrlValue_' + str(facts['pod_type'])), encoding='ascii')
        if args.kind == 'msvc':
            command = [args.compiler, *flags, '/I' + str(args.include), source,
                       '/Fe' + str(product), '/Fo' + str(root / (name + '.obj')), '/link', adapter, *args.library, *link_flags]
        else:
            command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                       *gnu_flags, '-I' + str(args.include), source, adapter, *args.library, '-o', product]
        run(command, root)
        run([product], root)
    print('generated typed transfers execute after final IR, ownership, homes and plans retire', flush=True)


if __name__ == '__main__':
    main()
