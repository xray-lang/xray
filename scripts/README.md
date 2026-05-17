# Xray 诊断脚本

> **配套规约**: `docs/tasks/082-pre-076-foundation.md` §4（任务 D — 诊断脚本固化）
> **范围**: 所有从手工命令固化为可重放的 shell 脚本
> **平台**: 全部 Bash；macOS / Linux 直跑，Windows 通过 Git Bash 或 WSL；不依赖网络与未打包的本地路径

## 总览

| 脚本 | 用途 | 输入 | 退出码语义 | 期望耗时 |
|---|---|---|---|---|
| `scripts/repro_jit_force_burn.sh` | 时序敏感 / heisenbug 类回归的 N 次 burn-in | `<test_file> <N>`；env: `XRAY_BIN`, `XRAY_SUBCMD` (默认 `test`), `TIMEOUT_SEC` | 全过=0；任一失败=1；用法错=2 | N × 单次耗时 |
| `scripts/run_fuzz_30min.sh` | 固定时长 JIT 差分 fuzz（nightly 入口） | `[seed]`；env: `FUZZ_DURATION_SEC`, `FUZZ_CHUNK`, `XRAY_BIN` | 无 diff/crash=0；有=1；基础设施缺失=2 | 由 `FUZZ_DURATION_SEC` 决定，默认 1800s |
| `scripts/run_gc_stress.sh` | GC 重测试 1205/1206/1207 burn-in | `[rounds]`；env: `XRAY_BIN`, `GC_STRESS_ROUNDS` | 全过=0；任一失败=1；参数错=2 | rounds × ~30s |
| `scripts/repro_win11_coro_burn.sh` | Win11 协程 4 用例 burn-in（1115/1109/1127/1128） | `[N]`；env: `XRAY_BIN` | 全过=0；任一失败=1；参数错=2 | N × 4 × ~5s |
| `scripts/check_codegen_invariants.sh` | 反向不变量种子（silent fallback / legacy / generated / known_failures） | 无 | 任一违规=非0 | < 30s |
| `scripts/check_temp_workarounds.sh` | `DEFENSIVE-TEMP[NNN]` 标签 ↔ `tests/known_temp_workarounds.md` 双向对账 | 无 | 任一不一致=非0 | < 10s |

## 详细说明

### `repro_jit_force_burn.sh`

跑同一个 `.xr` 文件 N 次，全部 `--jit-force`，每次输出 `idx | exit | duration_ms`。失败时把每次的 stderr 末 30 行写到 `tests/tmp/burn/<basename>/run_<i>.stderr`，便于事后审查；通过的轮次自动清理 stderr 文件。

适用场景：
- 怀疑某个 case 是 heisenbug，先 N=30 看复现率
- 修复 race 后 N=100 验证 0 复现
- CI 把它接到 nightly 的 burn-in 矩阵里

### `run_fuzz_30min.sh`

把 `scripts/jit_fuzz.sh`（已有的 JIT 差分 fuzzer）包成 wall-clock 预算驱动：每个 chunk 跑 `FUZZ_CHUNK` 次（默认 200）随机程序，比较 `--no-jit` 与 `--jit-force` 输出，循环直到预算耗尽。任一 chunk 报告 crash 或 diff，整体退出 1，repro 输入留在 `tests/tmp/jit_fuzz/`，chunk 级别日志同目录。

不替代 `jit_fuzz.sh`：
- `jit_fuzz.sh` 由 iteration 数驱动，适合本地交互式调用
- `run_fuzz_30min.sh` 由时长驱动，适合 nightly 把固定算力投到 fuzz

### `run_gc_stress.sh`

按 `082` 文档 D 表的 `<mode>` 设计，但 xray CLI **不暴露 `--gc-mode`**（GC 模式由 allocator 状态在运行期决定，不能 CLI 选）。务实落地是把 `<mode>` 折成 `[rounds]`：

- 1206 走 `--jit-force`（GC × JIT 集成路径，May 2026 Bug #10/#11 暴露的 surface）
- 1205 / 1207 走 `--no-jit`（已在 `tests/known_failures.txt` 标 NOJIT 的 GC 压力 baseline）

每轮顺序跑全部 case；失败时把每次失败的尾 30 行写入 `tests/tmp/gc_stress_failures.log`。

放大机制：rounds × ASan/MSan + `MALLOC_PERTURB_=205` + MallocScribble = 当前 CI 暴露 May 2026 Bug #8/#11 的实际配方。继续按 rounds 投资就够了，无需新加 CLI 开关。

### `repro_win11_coro_burn.sh`

四个 May 2026 在 Windows 上暴露 `STATUS_HEAP_CORRUPTION` 的协程场景：1115 cancel / 1109 await_any / 1127 priority / 1128 yield。每场景跑 N 次（默认 5，匹配 `nightly.yml`），在 `tests/tmp/win11_coro/failures.log` 收集失败 tail。

可在非 Windows 平台运行——相关 race 是堆破坏而非真正 Windows-only 行为，Linux / macOS 在 ASan/MSan 下也可能暴露相同根因。

### `check_codegen_invariants.sh` 与 `check_temp_workarounds.sh`

W1 已交付并接入 PR 门禁（参考 `.github/workflows/ci.yml` 的 `reverse-invariants` job）。本表只做完整性收录，不再展开规则。

## 与 nightly.yml 的关系

`nightly.yml` 当前内联 `gc-stress` 与 `windows-msvc-release-jit` 的循环逻辑，注释里也注明"When scripts/run_gc_stress.sh is delivered in W2 this block will be replaced by a single invocation of that script"。本批脚本就位后，`nightly.yml` 的两段内联可逐步替换为：

```yaml
- name: Run GC stress
  run: bash scripts/run_gc_stress.sh 10

- name: Run Win11 coroutine burn-in
  shell: bash
  run: bash scripts/repro_win11_coro_burn.sh 5
```

替换是独立 PR，不在本批 commit 一起做，避免诊断脚本本身有 bug 时同时打破 nightly。

## 修订历史

| 日期 | 改动 | 作者 |
|---|---|---|
| 2026-05-17 | 初稿；与 4 个新脚本同批；明确 `<mode>` → `[rounds]` 的务实落地决定 | Cascade + xingleixu |
