# Task 218 CGen verifier overhead evidence

Date: 2026-07-21  
Host: Apple Silicon macOS, AppleClang debug compiler build  
Workload: xxhash port `src/main.xr`, seven modules, native AOT C-only emission  
Command: `python3 scripts/bench_cgen_verifier.py --xray build/xray --samples 5`

| Sample | End-to-end wall | W1-W4 verifier CPU | Share |
|---:|---:|---:|---:|
| 1 | 3542.60 ms | 13.43 ms | 0.379% |
| 2 | 2543.97 ms | 13.41 ms | 0.527% |
| 3 | 2704.10 ms | 14.39 ms | 0.532% |
| 4 | 2792.48 ms | 15.22 ms | 0.545% |
| 5 | 2498.29 ms | 13.01 ms | 0.521% |

Median share: **0.527%**, below the Task 218 `<1%` budget. The timing environment variable only reports time spent in the always-on verifier; it cannot disable or alter verification.
