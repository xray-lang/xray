# Object and Json convergence Windows baseline

Date: 2026-08-05

## Identity

- Upstream base: `c0707a8a9` (`origin/main`).
- Compiler commit: `0915ef89c78f2959a27e536d3d9ff188cb330eac`.
- Compiler tree: `bd7d31eb8f373ddbe2ada946d47c823ea8d0c798`.
- Compiler identity: Xray 0.9.2, `windows-x86_64`, Release, VM+AOT,
  `dirty=false`.
- `build/xray.exe` SHA-256:
  `7427e47017b847fcab9de54b256e4776f58f9cd78f3a2c823c0f3da453f8665d`.
- Host: Windows 11 Home China 10.0.26200 build 26200, 64-bit.
- CPU: Intel Core i7-14700HX, 20 cores / 28 logical processors.
- Configure: `cmake --preset default`; generator Ninja; build type Release.
- C compiler: MSVC 19.44.35219 from Build Tools 14.44.35207.
- Native provider: MSVC 19.44.35219, target ABI
  `x86_64-windows-msvc`, no fallback, hosted runtime artifact
  `xray-rt-coro-x86_64-windows-msvc-v1`.
- Provider capabilities: C compile, SDK compile, runtime link, native run,
  LTO, force-inline, preserve-call, value-opaque, and compiler-fence all pass.

The compiler commit contains only baseline infrastructure and independent
tooling/test corrections relative to the upstream base; object and Json source
semantics are unchanged.

## Inventory

`python scripts/check_object_json_domain.py --root . --json` records:

| Class | Matches |
|---|---:|
| `PUBLIC_RECORD_SURFACE` | 225 |
| `INTERNAL_RECORD_IDENTITY` | 63 |
| `LEGACY_RECORD_TYPE_API` | 34 |
| `LEGACY_FIXED_FIELD_OP` | 142 |
| `LEGACY_RECORD_EVIDENCE` | 335 |

The inventory deliberately excludes ordinary English `record_*` actions and
the inventory/contract files themselves.

## Behavioral and code-shape baseline

The structural-object static-bracket differential case is a written known
failure: analyzer and AOT accept it, while the VM raises E0402 because lowering
uses generic index operations. The known-failure entry must be deleted when the
two spellings lower to one fixed-field operation.

Generated with `build/xray.exe build --native -O 2 -c`:

| Workload | Generated C bytes | Hot access | Native image bytes |
|---|---:|---|---:|
| exact dot | 6,795 | `xrt_json_get_field(v, ordinal)` | 163,840 |
| exact static bracket | 7,350 | `xrt_index_get(v, string)` | 182,272 |
| construct/destroy | 6,779 | ordinal get + named construction | 163,840 |

Named structural-object construction performs one embedded object allocation
and one per-instance `field_names` allocation. The fixed object header before
the flexible fields is 48 bytes on this target. The candidate may not increase
the header or exact-object allocation count and must remove the field-name
allocation.

## Runtime samples

Native executables were built once with `build/xray.exe build --native -O 2`.
Each workload received three warmups. Ten measurements then ran serially;
dot/static-bracket order alternated on every pair. Times include Windows
process startup and therefore gate only paired candidate runs on this same
host. Output and exit status were checked on every sample.

| Case | Output | Samples, milliseconds | Median | p95 nearest-rank |
|---|---:|---|---:|---:|
| exact dot | 6000000 | 17.7955, 26.9563, 16.2868, 15.4433, 15.9979, 14.9531, 14.5285, 15.5576, 29.3975, 15.6228 | 15.8104 | 29.3975 |
| exact static bracket | 6000000 | 54.9471, 55.9599, 56.8309, 60.8317, 51.2867, 60.2917, 58.5734, 58.0176, 53.3582, 53.2361 | 56.3954 | 60.8317 |
| construct/destroy | 200000 | 32.7472, 33.1011, 33.9586, 33.5763, 33.0795, 35.0215, 31.6412, 34.0537, 43.8894, 34.4686 | 33.7675 | 43.8894 |

Native image SHA-256 values are respectively
`d32783052b402db9f4fef4dcd762eb1294b379dca17b82a9e213e48d83a6a779`,
`119e8ddeeb38aca976001f2723a0eabd7893669483823460df7cb4f594f9cef9`,
and `2fbb143c64bffe6a85e6405ac15851bdacf9e29598c47ec2f05e266f041717e8`.

## Commands

```powershell
cmake --preset default
cmake --build build --target xray
build/xray.exe --version --json
build/xray.exe toolchain doctor --target native --json
python scripts/check_object_json_domain.py --root . --json
build/xray.exe build --native -O 2 -c -o <case>.c <case>.xr
build/xray.exe build --native -O 2 -o <case>.exe <case>.xr
```

Final qualification must rerun the same workloads as paired samples and add
allocation/live-byte instrumentation for the new typed-parse and dynamic-Json
workloads. These Windows measurements do not qualify Linux, macOS, Clang, GCC,
or Zig execution.
