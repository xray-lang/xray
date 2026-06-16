# Xray 诊断脚本

> **配套规约**: `docs/tasks/082-pre-076-foundation.md` §4（任务 D — 诊断脚本固化）
> **范围**: 所有从手工命令固化为可重放的 shell 脚本
> **平台**: 全部 Bash；macOS / Linux 直跑，Windows 通过 Git Bash 或 WSL；不依赖网络与未打包的本地路径

## 总览

| 脚本 | 用途 | 输入 | 退出码语义 | 期望耗时 |
|---|---|---|---|---|
| `scripts/run_gc_stress.sh` | GC 重测试 1205/1206/1207 burn-in | `[rounds]`；env: `XRAY_BIN`, `GC_STRESS_ROUNDS` | 全过=0；任一失败=1；参数错=2 | rounds × ~30s |
| `scripts/repro_win11_coro_burn.sh` | Win11 协程 4 用例 burn-in（1115/1109/1127/1128） | `[N]`；env: `XRAY_BIN` | 全过=0；任一失败=1；参数错=2 | N × 4 × ~5s |
| `scripts/check_temp_workarounds.sh` | `DEFENSIVE-TEMP[NNN]` 标签 ↔ `tests/known_temp_workarounds.md` 双向对账 | 无 | 任一不一致=非0 | < 10s |

## 详细说明

### `run_gc_stress.sh`

按 `082` 文档 D 表的 `<mode>` 设计，但 xray CLI **不暴露 `--gc-mode`**（GC 模式由 allocator 状态在运行期决定，不能 CLI 选）。务实落地是把 `<mode>` 折成 `[rounds]`：每轮顺序跑全部 GC 重测试（1205/1206/1207），失败时把每次失败的尾 30 行写入 `tests/tmp/gc_stress_failures.log`。

放大机制：rounds × ASan/MSan + `MALLOC_PERTURB_=205` + MallocScribble = 当前 CI 暴露 May 2026 Bug #8/#11 的实际配方。继续按 rounds 投资就够了，无需新加 CLI 开关。

### `repro_win11_coro_burn.sh`

May 2026 在 Windows 上暴露 `STATUS_HEAP_CORRUPTION` 的协程场景：1115 cancel / 1109 await_any / 1128 yield。每场景跑 N 次（默认 5，匹配 `nightly.yml`），在 `tests/tmp/win11_coro/failures.log` 收集失败 tail。

可在非 Windows 平台运行——相关 race 是堆破坏而非真正 Windows-only 行为，Linux / macOS 在 ASan/MSan 下也可能暴露相同根因。

### `check_temp_workarounds.sh`

接入 PR 门禁（参考 `.github/workflows/ci.yml` 的 `reverse-invariants` job）。本表只做完整性收录，不再展开规则。

## 与 nightly.yml 的关系

`nightly.yml` 的 `gc-stress` job 调用 `scripts/run_gc_stress.sh`，`windows-msvc-release` job 调用 `scripts/repro_win11_coro_burn.sh`：

```yaml
- name: Run GC stress
  run: bash scripts/run_gc_stress.sh 10

- name: Run Win11 coroutine burn-in
  shell: bash
  run: bash scripts/repro_win11_coro_burn.sh 5
```

## 修订历史

| 日期 | 改动 | 作者 |
|---|---|---|
| 2026-05-17 | 初稿；与 4 个新脚本同批；明确 `<mode>` → `[rounds]` 的务实落地决定 | Cascade + xingleixu |
| 2026-06-16 | 移除 JIT 相关脚本（repro_jit_force_burn / run_fuzz_30min / check_codegen_* / jit_fuzz / run_jit_*）；run_gc_stress 去 jit-force/no-jit 模式 | — |
