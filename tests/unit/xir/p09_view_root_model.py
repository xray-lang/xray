"""Check real compiler fixtures against exhaustive, canonicalized root identities.

This oracle models successful allocation/range checks and nondeterministic branch
choices. Its finite state graph overapproximates every concrete branch schedule;
it issues neither a runtime execution result nor a source borrowing permission.
"""
from collections import deque
import json
import subprocess
import sys


def canonical(block, predecessor, values, locals_, active):
    ids = {0: 0}
    def encode(row):
        result = []
        for root in row:
            if root not in ids:
                ids[root] = len(ids)
            result.append(ids[root])
        return tuple(result)
    return block, predecessor, encode(values), encode(locals_), encode(active)


def explore(case, cell_type, mut_type):
    instructions = case["instructions"]
    parameters = case["parameters"]
    count = len(parameters) + len(instructions)
    values = [0] * count
    for index, type_ in enumerate(parameters):
        if type_ == cell_type:
            values[index] = max(values) + 1
    queue = deque([canonical(0, -1, values, [0] * count, [0] * count)])
    seen = set(queue)
    observations = set()
    conflicts = set()
    def schedule(successor, predecessor, values, locals_, active):
        state = canonical(successor, predecessor, values, locals_, active)
        if state not in seen:
            seen.add(state)
            assert len(seen) <= 100000, "The fixed fixture state graph did not close"
            queue.append(state)
    while queue:
        block, predecessor, raw_values, raw_locals, raw_active = queue.popleft()
        values, locals_, active = list(raw_values), list(raw_locals), list(raw_active)
        first, size, panic = case["blocks"][block]
        if panic:
            schedule(panic, block, values, locals_, active)
        selected = {}
        at = first
        while at < first + size and instructions[at][0] == "PHI":
            _, _, start, length, _, _, _ = instructions[at]
            edges = case["operands"][start:start + length]
            choices = [value for pred, value in zip(edges[::2], edges[1::2]) if pred == predecessor]
            assert len(choices) == 1 and values[choices[0]]
            selected[len(parameters) + at] = values[choices[0]]
            at += 1
        # PHI inputs all observe the predecessor, before any destination changes.
        for destination, root in selected.items():
            values[destination] = root
        next_blocks = []
        for index in range(at, first + size):
            op, type_, left, right, target0, target1, _ = instructions[index]
            destination = len(parameters) + index
            if op in ("CONST_INT", "ARRAY_NEW"):
                values[destination] = 0
            elif op == "CELL_NEW":
                values[destination] = max(values + locals_ + active) + 1
            elif op in ("COPY", "OWNED_RETAIN"):
                assert values[left]
                values[destination] = values[left]
            elif op == "LOCAL_UNINIT":
                locals_[destination] = 0
            elif op in ("LOCAL_NEW", "OWNED_LOCAL_NEW"):
                assert values[left]
                locals_[destination] = values[left]
            elif op in ("LOCAL_READ", "OWNED_LOCAL_READ", "PLACE_READ"):
                assert locals_[left]
                values[destination] = locals_[left]
            elif op in ("LOCAL_WRITE", "OWNED_LOCAL_WRITE", "PLACE_WRITE"):
                assert values[right]
                locals_[left] = values[right]
            elif op == "VIEW_NEW":
                root = values[case["operands"][left]]
                assert root and type_ == mut_type
                assert root not in active, "A fixture acquired a second conflicting view"
                values[destination] = active[destination] = root
            elif op == "VIEW_END":
                assert active[left]
                values[left] = active[left] = 0
            elif op in ("CELL_READ", "CELL_LOCAL_WRITE"):
                root = locals_[left] if op == "CELL_LOCAL_WRITE" else values[left]
                assert root
                collision = root in active
                if index == case["bad"]:
                    observations.add(collision)
                if collision:
                    conflicts.add(index)
                # Reads do not change root identity. Continuing this projection
                # after a collision checks later loop schedules as well; it is
                # an overapproximation, not an execution of fault behavior.
                values[destination] = 0
            elif op == "PANIC_CATCH":
                values[destination] = 0
            elif op == "JUMP":
                next_blocks = [target0]
            elif op == "BRANCH":
                next_blocks = [target0, target1]
            elif op == "RETURN":
                assert not any(active)
            else:
                raise AssertionError((case["kind"], "Unmodeled operation", op))
            if panic:
                schedule(panic, block, values, locals_, active)
        for successor in next_blocks:
            schedule(successor, block, values, locals_, active)
    status = case["status"]
    if status == 0:
        assert not conflicts, (case["kind"], "Compiler accepted a root collision", conflicts)
    elif status == 4:
        assert observations == {True} and conflicts == {case["bad"]}, (case["kind"], observations, conflicts)
    else:
        assert status == 10, "Only explicit unresolved decisions remain outside this proof"
    return {"kind": case["kind"], "compiler_status": status, "states": len(seen),
            "bad_site_collisions": sorted(observations), "conflict_sites": sorted(conflicts)}


def main():
    assert len(sys.argv) == 2
    run = subprocess.run([sys.argv[1], "--root-model"], capture_output=True, text=True,
                         encoding="utf-8", timeout=120, check=True)
    data = json.loads(run.stdout)
    assert {row["kind"] for row in data["cases"]} == {120, 134} | set(range(148, 188))
    rows = [explore(row, data["cell_type"], data["mut_type"]) for row in data["cases"]]
    print(json.dumps({"identity_model": "EXHAUSTIVE_FIXED_FIXTURES_ONLY", "cases": rows,
                      "Source_view_execution": False, "full_FI_qualification": False}))


if __name__ == "__main__":
    main()
