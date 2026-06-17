#!/usr/bin/env python3
"""fuzz_backend_diff.py - randomized VM/AOT differential tester.

Generates random, type-correct .xr programs and runs each through the VM
(`xray run`) and AOT (`xray build --native` + execute), asserting their
observable result (stdout + exit code) is byte-identical. This is the
randomized companion to run_backend_diff.sh: it explores the numeric /
formatting / control-flow surface where the two backends are most prone to
silently diverge.

stderr is intentionally NOT compared (it carries backend-specific build and
diagnostic noise), matching run_backend_diff.sh's observable contract.

Usage:
  tests/diff/fuzz_backend_diff.py [--count N] [--seed S] [--xray PATH]
                                  [--keep-dir DIR]

Exit code: 0 if all programs agree, 1 if any divergence (details printed and
saved), 2 on harness error.
"""

import argparse
import os
import random
import subprocess
import sys
import tempfile

# Numeric types and their value ranges (inclusive) for literal generation.
INT_TYPES = {
    "int": (-(2**62), 2**62),
    "int8": (-128, 127),
    "int16": (-32768, 32767),
    "int32": (-(2**31), 2**31 - 1),
    "int64": (-(2**62), 2**62),
    "uint8": (0, 255),
    "uint16": (0, 65535),
    "uint32": (0, 2**32 - 1),
    "uint64": (0, 2**63),
}
FLOAT_TYPES = ("float", "float32", "float64")

# Interesting float literals that exercise formatting / rounding boundaries.
FLOAT_LITERALS = [
    "0.0", "-0.0", "1.0", "-1.0", "0.1", "0.2", "0.5", "1.5", "-1.5",
    "3.141592653589793", "2.718281828459045", "100.0", "0.000125",
    "123456789.0", "1.0e10", "1.0e-10", "0.3333333333333333", "-42.75",
    "9999999.9999", "0.0200000014156103",
]


def rand_int_literal(rng, ty):
    lo, hi = INT_TYPES[ty]
    # Bias toward small magnitudes and the type boundaries.
    pick = rng.random()
    if pick < 0.2:
        return str(lo)
    if pick < 0.4:
        return str(hi)
    if pick < 0.7:
        return str(rng.randint(-20, 20) if lo < 0 else rng.randint(0, 20))
    return str(rng.randint(lo, hi))


def rand_float_literal(rng):
    return rng.choice(FLOAT_LITERALS)


class Gen:
    """Generates one random program plus a pool of typed variables."""

    def __init__(self, rng):
        self.rng = rng
        self.vars = {}  # name -> type
        self.lines = []
        self.counter = 0

    def fresh(self):
        self.counter += 1
        return f"v{self.counter}"

    def vars_of(self, kinds):
        return [n for n, t in self.vars.items() if self._kind(t) in kinds]

    @staticmethod
    def _kind(ty):
        if ty in INT_TYPES:
            return "int"
        if ty in FLOAT_TYPES:
            return "float"
        return ty  # bool / string

    def decl(self, ty, value):
        name = self.fresh()
        self.lines.append(f"let {name}: {ty} = {value}")
        self.vars[name] = ty
        return name

    def numeric_operand(self, ty):
        """A var of exactly `ty`, or a literal of `ty`."""
        same = [n for n, t in self.vars.items() if t == ty]
        if same and self.rng.random() < 0.6:
            return self.rng.choice(same)
        if ty in INT_TYPES:
            return rand_int_literal(self.rng, ty)
        return rand_float_literal(self.rng)

    def gen_decls(self, n):
        for _ in range(n):
            ty = self.rng.choice(
                list(INT_TYPES) + list(FLOAT_TYPES) + ["bool", "string"]
            )
            if ty in INT_TYPES:
                val = rand_int_literal(self.rng, ty)
            elif ty in FLOAT_TYPES:
                val = rand_float_literal(self.rng)
            elif ty == "bool":
                val = self.rng.choice(["true", "false"])
            else:
                val = '"' + "".join(
                    self.rng.choice("abcXYZ0_ ") for _ in range(self.rng.randint(0, 5))
                ) + '"'
            self.decl(ty, val)

    def int_expr(self):
        ty = self.rng.choice(list(INT_TYPES))
        a = self.numeric_operand(ty)
        b = self.numeric_operand(ty)
        op = self.rng.choice(["+", "-", "*", "&", "|", "^"])
        if self.rng.random() < 0.25:
            # division / modulo with a guaranteed non-zero divisor literal
            op = self.rng.choice(["/", "%"])
            b = str(self.rng.choice([1, 2, 3, 7, -1, -3, 13]))
        if self.rng.random() < 0.2:
            op = self.rng.choice(["<<", ">>"])
            b = str(self.rng.randint(0, 63))
        return f"({a} {op} {b})"

    def float_expr(self):
        ty = self.rng.choice(FLOAT_TYPES)
        a = self.numeric_operand(ty)
        b = self.numeric_operand(ty)
        op = self.rng.choice(["+", "-", "*", "/"])
        return f"({a} {op} {b})"

    def bool_expr(self):
        ty = self.rng.choice(list(INT_TYPES) + list(FLOAT_TYPES))
        a = self.numeric_operand(ty)
        b = self.numeric_operand(ty)
        op = self.rng.choice(["<", "<=", ">", ">=", "==", "!="])
        return f"({a} {op} {b})"

    def array_literal(self):
        # Homogeneous array of a random scalar type (>=1 element so the
        # element type is inferable without an annotation).
        ty = self.rng.choice(["int", "float", "bool", "string"])
        n = self.rng.randint(1, 4)
        elems = []
        for _ in range(n):
            if ty == "int":
                elems.append(rand_int_literal(self.rng, "int"))
            elif ty == "float":
                elems.append(rand_float_literal(self.rng))
            elif ty == "bool":
                elems.append(self.rng.choice(["true", "false"]))
            else:
                elems.append('"' + "".join(
                    self.rng.choice("abAB0_ ") for _ in range(self.rng.randint(0, 3))
                ) + '"')
        return "[" + ", ".join(elems) + "]"

    def print_stmt(self):
        kind = self.rng.choice(["int", "float", "bool", "concat", "array"])
        if kind == "int":
            expr = self.int_expr()
        elif kind == "float":
            expr = self.float_expr()
        elif kind == "bool":
            expr = self.bool_expr()
        elif kind == "array":
            expr = self.array_literal()
        else:
            inner = self.rng.choice([self.int_expr(), self.float_expr()])
            expr = f'("" + {inner})'
        self.lines.append(f"print({expr})")

    def build(self):
        self.gen_decls(self.rng.randint(3, 8))
        for _ in range(self.rng.randint(4, 12)):
            self.print_stmt()
        return "\n".join(self.lines) + "\n"


def run_vm(xray, path):
    p = subprocess.run([xray, "run", path], capture_output=True, text=True)
    return p.stdout, p.returncode


def run_aot(xray, path, out_bin):
    b = subprocess.run(
        [xray, "build", "--native", path, "-o", out_bin],
        capture_output=True, text=True,
    )
    if b.returncode != 0:
        return None, 200  # build failure sentinel
    p = subprocess.run([out_bin], capture_output=True, text=True)
    return p.stdout, p.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--xray", default=os.environ.get("XRAY_BIN", "build/xray"))
    ap.add_argument("--keep-dir", default=None,
                    help="directory to save diverging programs (default: temp)")
    args = ap.parse_args()

    xray = args.xray
    if not (os.path.isfile(xray) and os.access(xray, os.X_OK)):
        print(f"error: xray binary not found/executable: {xray}", file=sys.stderr)
        return 2

    base_seed = args.seed if args.seed is not None else random.randrange(1 << 30)
    print(f"=== VM/AOT differential fuzz: {args.count} programs, base seed {base_seed} ===")

    save_dir = args.keep_dir or tempfile.mkdtemp(prefix="xray_fuzz_")
    os.makedirs(save_dir, exist_ok=True)

    diverged = 0
    work = tempfile.mkdtemp(prefix="xray_fuzz_work_")
    for i in range(args.count):
        seed = base_seed + i
        prog = Gen(random.Random(seed)).build()
        src = os.path.join(work, "case.xr")
        with open(src, "w") as f:
            f.write(prog)

        vm_out, vm_rc = run_vm(xray, src)
        aot_out, aot_rc = run_aot(xray, src, os.path.join(work, "case_aot"))

        if aot_out is None:
            # Both should accept the generated program; a build failure is a
            # generator/back-end bug worth surfacing.
            diverged += 1
            dst = os.path.join(save_dir, f"buildfail_seed{seed}.xr")
            with open(dst, "w") as f:
                f.write(prog)
            print(f"\n[BUILD FAIL] seed {seed} -> {dst}")
            continue

        if vm_out != aot_out or vm_rc != aot_rc:
            diverged += 1
            dst = os.path.join(save_dir, f"diverge_seed{seed}.xr")
            with open(dst, "w") as f:
                f.write(prog)
            print(f"\n[DIVERGE] seed {seed} -> {dst}")
            print(f"  rc:  vm={vm_rc} aot={aot_rc}")
            print(f"  vm : {vm_out!r}")
            print(f"  aot: {aot_out!r}")

    print(f"\n=== {args.count - diverged}/{args.count} agree, {diverged} diverged ===")
    if diverged:
        print(f"diverging cases saved under {save_dir}")
    return 1 if diverged else 0


if __name__ == "__main__":
    sys.exit(main())
