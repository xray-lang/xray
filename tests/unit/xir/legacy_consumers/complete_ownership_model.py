"""Model every remaining complete original ownership input without Xray output."""
from copy import deepcopy

PREFIX = 'tests/diff/cases/semantics/ownership/'

def expected():
    models = {}
    box = {'value': 7}
    alias = box
    alias['value'] = 9
    models['auto_borrow_class_alias_return_downgrade'] = [box['value'], alias['value']]
    box = {'value': 7}
    before = box['value']
    box['value'] = 9
    models['auto_borrow_class_readonly_param'] = [before, box['value']]
    xs = [1, 2, 3]
    length_before = len(xs)
    xs.append(4)
    table = {'x': 7}
    models['auto_borrow_heap_readonly_param'] = [length_before, len(xs), table['x']]
    models['converged_capability_positive'] = [len([1, 2, 3]), sum([4, 5]), 7, len([8, 9])]
    nums = [1, 2, 3]
    duplicate = list(nums)
    nums.append(4)
    deep = [len(nums), len(duplicate), duplicate[0]]
    table = {'a': 1}
    table_copy = dict(table)
    table['b'] = 2
    deep += [len(table), len(table_copy)]
    readonly = list(nums)
    nums.append(5)
    deep += [len(nums), len(readonly)]
    grid = [[1, 2], [3, 4]]
    grid_copy = deepcopy(grid)
    grid[0].append(99)
    deep += [len(grid[0]), len(grid_copy[0])]
    window = deepcopy(grid[0:1])
    grid[0].append(100)
    deep += [len(grid[0]), len(window[0])]
    models['copy_deep_independent'] = deep
    models['copy_preserves_type'] = [42, 42, sum([1, 2, 3])]
    values = [1, 2, 3, 4, 5, 6]
    stored = [list(values)]
    values.append(7)
    models['in_alias_reassigned_copy_escape_allowed'] = [len(stored[0])]
    values = [1, 2, 3, 4]
    snapshot = list(values)
    closure = lambda: len(snapshot)
    values.append(5)
    models['in_closure_copy_capture_allowed'] = [closure(), len(values)]
    for name, initial in [
        ('in_forward_function_param_copy_escape_allowed', [1, 2, 3]),
        ('in_function_param_copy_escape_allowed', [1, 2, 3, 4]),
        ('in_function_param_method_copy_escape_allowed', [1, 2, 3]),
        ('in_method_wrapper_param_copy_escape_allowed', [1, 2, 3]),
    ]:
        original = list(initial)
        stored = [list(original)]
        original.append(len(original) + 1)
        models[name] = [len(stored[0]), len(original)]
    values = [1, 2, 3, 4]
    original_length = len(values)
    child = list(values)
    child.append(5)
    models['in_go_copy_argument_allowed'] = [len(child), original_length]
    counter = 10
    push = lambda: counter + 1
    call_push = lambda: push()
    models['in_readonly_user_method_names'] = [push() + call_push()]
    values = [1, 2, 3]
    summed = sum(values)
    values.append(4)
    table = {'x': 7}
    box = {'value': 3}
    before = box['value']
    box['value'] = 8
    models['in_ref_heap_borrows'] = [summed, sum(values), 'x' in table, table['x'], before, box['value']]
    models['move_into_go'] = [len([1, 2, 3])]
    moved = [1, 2, 3]
    moved.append(4)
    models['move_local_heap'] = [len(moved), moved[0] + moved[3]]
    taken = [10, 20, 30]
    taken.append(40)
    models['move_local_in_fn'] = [len([1, 2, 3]), len(taken), taken[0] + taken[3]]
    models['shared_class_copy_go'] = [8080]
    table = {1: 10, 2: 20}
    readonly = dict(table)
    models['shared_freeze_go'] = [len(readonly), len(readonly)]
    return [
        {'source': PREFIX + name + '.xr', 'id': name, 'expected_exit_code': 0,
         'stdout_utf8': ''.join((str(v).lower() if type(v) is bool else str(v)) + '\n' for v in values),
         'stderr_utf8': '', 'observations': values}
        for name, values in sorted(models.items())
    ]
