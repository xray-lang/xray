"""CPython references with the same loop and container operations."""

import sys


def fib(n: int) -> int:
    return n if n < 2 else fib(n - 1) + fib(n - 2)


def integer_loop(n: int) -> int:
    total = 0
    for i in range(n):
        total += i % 97
    return total


def push_sum(n: int) -> int:
    values: list[int] = []
    for i in range(n):
        values.append(i)
    total = 0
    for i in range(n):
        total += values[i]
    return total


if __name__ == "__main__":
    sys.stdout.reconfigure(newline="\n")
    cases = {"fib": fib, "integer_loop": integer_loop, "push_sum": push_sum}
    if len(sys.argv) != 3 or sys.argv[1] not in cases:
        raise SystemExit("usage: reference.py <fib|integer_loop|push_sum> <n>")
    print(cases[sys.argv[1]](int(sys.argv[2])))
