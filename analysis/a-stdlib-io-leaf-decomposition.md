# `io` 的 19 个 `not_a_leaf` — 可执行拆解方案

调研树：`/Users/xuxinglei/workspace/xray-lang/worktrees/a-stdlib-selfhost-w0-inventory-bb6eac777369`
只读；未构建、未改任何仓库文件。所有"实测"用 `build/xray run` 跑 scratchpad 脚本（脚本名在每条里给出）。

范围：`analysis/a-stdlib-native-leaf-dossier.md` 判定的 20 个 `io` `not_a_leaf`，减去已删的 `__readLines`，共 **19 个**。

> **行号说明**：dossier 里的行号是 `io_readLines` 删除**之前**的。本文所有行号是**当前树**实测（`stdlib/io/io.c` 1301 行、`src/aot/xrt_io.h` 933 行、`src/shared/xr_io_core.h` 706 行）。

---

## 0. 分档结论

| 档 | 数量 | 条目 |
|---|---:|---|
| **A 立刻可做**（零新 leaf，只改 `.xr` + 删 `.def` 条目） | 4 | `__mkdirp`, `__readDirRecursive`, `__readStdin`, `__readStdinBytes` |
| **B 需要先加/改薄 leaf** | 10 | `__touch`, `__writeStdout`, `__writeStderr`, `__mkdir`, `__tempDir`, `__removeAll`, `__FileStat`, `__tempFile`, `__readDir`, `__appendFile` |
| **C 需要新能力**（可挂起 fd leaf；否则丢 io_uring 异步 + 明显性能损失） | 4 | `__readFile`, `__readFileBytes`, `__writeFile`, `__writeFileBytes` |
| **D 建议保留**（重分类为真 leaf） | 1 | `__copyFile` |

每一档内部按代价从小到大排。

**每个迁移都要删两处 C**：VM 侧 `stdlib/io/io.c` + AOT 侧 `src/aot/xrt_io.h`（dossier 只提了 VM 侧）。第三处是 `src/shared/xr_io_core.h` 的共享 helper——它只有三个使用者（`stdlib/io/io.c`、`src/aot/xrt_io.h`、`tests/unit/stdlib/test_io_core.c`），所以每次迁移都能连带删掉对应的 helper。

---

## A 档 — 立刻可做（零新 leaf）

### A1. `__readDirRecursive` ★ 我推荐的下一个

**C 实现位置与策略行**

| 文件 | 行 | 内容 |
|---|---|---|
| `stdlib/io/io.c` | 1249-1275 (27) | `io_readDirRecursive`：建数组 + 填 `XrIoCoreReadDirOps` + 调 shared core |
| `src/aot/xrt_io.h` | 790-821 (32) | `xrt_io_read_dir_recursive`：同上 |
| `src/shared/xr_io_core.h` | 500-589 (90) | `XrIoCoreReadDirRecursiveCtx` + `_visit` + `_impl` + 入口：**递归下降、深度上限 `XR_IO_CORE_READ_DIR_MAX_DEPTH=64`(72行)、dot 过滤(518)、逐子路径 alloc+join(521-533)、相对路径推导(535)、目录判定后递归(542-548)、上下文结构体逐层复制(543/560)** |
| `src/shared/xr_io_core.h` | 615-623 (9) | `xr_io_core_relative_path_from_base` |

策略全部在 `xr_io_core.h:516-570`：这是**唯一**一个"复制上下文结构体来传递深度"的写法，也是 io core 里最难读的一段。

**`.xr` 目标代码**（实测通过，见 `t20_rdr.xr`）

```xray
// readDirRecursive — 深度上限 64 与 XR_IO_CORE_READ_DIR_MAX_DEPTH 一致；
// 结果是相对 base 的路径，分隔符恒为 '/'。符号链接目录只列出、不下降。
fn _walkDir(full: string, rel: string, depth: i64, out: ref Array<string>) {
    if (depth >= 64) { return }
    var names = __readDir(Path(full))
    for (var i = 0; i < len(names); i++) {
        var name = names[i]
        var childFull = full + "/" + name
        var childRel = name
        if (len(rel) > 0) { childRel = rel + "/" + name }
        out.push(childRel)
        var child = Path(childFull)
        if (__isDir(child) && !__isSymlink(child)) {
            _walkDir(childFull, childRel, depth + 1, ref out)
        }
    }
}

export fn readDirRecursive(path: Path) -> Array<Path> {
    var raw: Array<string> = []
    if (__isDir(path)) { _walkDir(path.toString(), "", 0, ref raw) }
    var out: Array<Path> = []
    for (var i = 0; i < len(raw); i++) { out.push(Path(raw[i])) }
    return out
}
```

**需要的 leaf 签名**：**无新增**。复用现有三个 `host_abi_leaf`
- `__readDir(path: Path): Array<string>`（暂留，B9 再迁）
- `__isDir(path: Path): bool`
- `__isSymlink(path: Path): bool`

**依赖判断**
- `ref Array<T>` 参数（`LANGUAGE_SPEC.md` §5.2.4）——**必需**。普通参数是只读能力，`out.push(...)` 会被拒：`error: Cannot call mutating method 'push' on read parameter 'out' (readonly capability)`（实测 `t20_rdr.xr` 第一版）。调用点也要写 `ref out`。
- 递归函数：实测可用（`t02_caps.xr` 的 `fact`）。
- `io.xr` 已 `import { Path } from path`，不需要新 import。
- 不需要 `os`。

**实测对比**（`t20_rdr.xr`，目录含 `top`/`a/f1`/`a/b/f2`/`a/b/c`/`linkdir`→`a`）
```
X: top | a | a/f1 | a/b | a/b/f2 | a/b/c | linkdir |
C: top | a | a/f1 | a/b | a/b/f2 | a/b/c | linkdir |
X missing: (空)   C missing: (空)
X file:    (空)   C file:    (空)
```
**顺序、内容、符号链接处理、错误路径全部逐字相同。**

**风险**
1. **每个条目多一次 lstat**：C 每条目一次 `kind()`（lstat）；Xray 版是 `__isDir` + `__isSymlink` 两次。可接受，但大目录树上是 2× stat 调用。
2. **修掉一个 VM/AOT 分歧（好事）**：`stdlib/io/io.c:1267` 写死 `.sep = '/'`，而 `src/aot/xrt_io.h:806-811` 在 Windows 上写 `.sep = '\\'`。**同一次调用 Windows VM 返回 `a/b`、AOT 返回 `a\b`。** Xray 版只有一个拼写。
3. `__readDir` 在非目录路径上返回 `xr_null()`（见「发现的缺陷 #2」），所以 `_walkDir` 前必须有 `__isDir(path)` 守卫——上面的代码已经有。
4. 深度上限 64 从 C 常量变成 `.xr` 字面量；要么加注释锚定，要么在 `.def` 里留一个常量。

**删掉多少 C**：io.c 27 + xrt_io.h 32 + xr_io_core.h 99 = **158 行**，外加 `core.def` 一个 `fn __readDirRecursive` 块和 5 个生成文件里的对应行。

---

### A2. `__mkdirp`

**C 实现位置与策略行**

| 文件 | 行 | 内容 |
|---|---|---|
| `stdlib/io/io.c` | 993-1012 (20) | `io_mkdirp`：`XR_PATH_MAX` 截断检查(999)、拷进栈缓冲(1004-1009) |
| `stdlib/io/io.c` | 980-988 (8) | `io_mkdirp_mkdir` / `io_mkdirp_is_dir` 两个 vtable 适配器 |
| `src/aot/xrt_io.h` | 326-350 (23) | 同上三件 |
| `src/shared/xr_io_core.h` | 670-704 (35) | `xr_io_core_mkdirp`：**尾分隔符裁剪(680-681)、根长度检测(679)、段遍历(690-701)、原地改写 `path[i]='\0'` 再还原(693-696)、连续分隔符跳过(699-700)** |
| `src/shared/xr_io_core.h` | 661-668 (8) | `xr_io_core_ensure_dir`：`mkdir==0 或 is_dir` |
| `src/shared/xr_io_core.h` | 629-659 (31) | `xr_io_core_root_len`：**Windows 盘符 + UNC 前缀识别** |
| `src/shared/xr_io_core.h` | 625-627 (3) | `xr_io_core_is_alpha_ascii` |

**`.xr` 目标代码**（POSIX 版实测通过，见 `t06/t07/t08/t14`；`_rootLen` 补上盘符分支是**未实测**的部分）

```xray
fn _isPathSep(c: string) -> bool { return c == "/" || c == "\\" }

// root 长度：POSIX 是前导 '/' 的 1；Windows 盘符 "C:" / "C:/" 是 2 / 3。
// 与 xr_io_core_root_len 对齐（该函数的盘符分支不带 #ifdef，POSIX 上也生效）。
fn _rootLen(p: string) -> i64 {
    if (len(p) == 0) { return 0 }
    if (len(p) >= 2 && p.slice(1, 2) == ":") {
        var c = p.slice(0, 1)
        if ((c >= "A" && c <= "Z") || (c >= "a" && c <= "z")) {
            if (len(p) >= 3 && _isPathSep(p.slice(2, 3))) { return 3 }
            return 2
        }
    }
    if (_isPathSep(p.slice(0, 1))) { return 1 }
    return 0
}

fn _ensureDir(p: Path) -> bool {
    if (__mkdir(p)) { return true }
    return __isDir(p)
}

export fn mkdirp(path: Path) -> bool {
    var raw = path.toString()
    if (len(raw) == 0) { return false }

    var rootLen = _rootLen(raw)
    var end = len(raw)
    while (end > rootLen + 1 && _isPathSep(raw.slice(end - 1, end))) { end = end - 1 }
    var p = raw.slice(0, end)
    if (end <= rootLen) { return __isDir(Path(p)) }

    var i = rootLen
    while (i < len(p) && _isPathSep(p.slice(i, i + 1))) { i = i + 1 }
    while (i < len(p)) {
        if (_isPathSep(p.slice(i, i + 1))) {
            if (i > rootLen && !_ensureDir(Path(p.slice(0, i)))) { return false }
            while (i + 1 < len(p) && _isPathSep(p.slice(i + 1, i + 2))) { i = i + 1 }
        }
        i = i + 1
    }
    return _ensureDir(Path(p))
}
```

**需要的 leaf 签名**：**无新增**。复用 `__mkdir(path: Path): bool`、`__isDir(path: Path): bool`。

**依赖判断**
- `string.slice(a, b)` 是 **rune 下标**不是字节下标（实测 `t13_strlen.xr`：`len("héllo")==5`、`copyBytes()` 长度 6）。这对分隔符扫描是安全的——`/`、`\`、`:` 都是单字节 ASCII 标量，绝不出现在多字节 UTF-8 单元内部（`path.xr` 顶部已经把这条理由写下来了）。UTF-8 路径实测无差异（`t14_utf8path.xr`：`中文/深い/ある` 两版都创建成功）。
- 不需要 `os`、不需要新语法。

**实测对比**（`t06_mkdirp.xr` 6 例 + `t07_mkdirp_adv.xr` 9 例，X=Xray / C=现 C）
```
'' X=false C=false | '.' true/true | '..' true/true | 'rel_a/rel_b' true/true
'…/blocker/child'(父是普通文件) X=false C=false
'…/back\slash/deep'  X=true C=true
'C:…/drive'          X=true C=true
'…/./dot/./seg'      X=true C=true
'…/end/..'           X=true C=true
6 个基础例（含 'x//y///z/'、'trail/'、'/'、根本身）全部一致
```

**风险**
1. **Windows 反斜杠**是唯一实质分歧点。`xr_io_core_is_sep` 在**所有平台**都接受 `\`；`path.xr` 的 `_isSep` 只认 `/`。若照抄 `path.xr`，POSIX 上 `mkdirp("a\\b/c")` 会少建一层。实测证明（`t08_mkdirp_diff.xr`）：
   ```
   X tree: back\slash | back\slash/deep |
   C tree: back | back\slash | back\slash/deep |     ← C 在 POSIX 上多建了一个叫 "back" 的目录
   ```
   现 C 的行为在 POSIX 上**是错的**（反斜杠是合法文件名字节）。上面给的 `_isPathSep` 保留了 C 的双分隔符语义以求零行为变更；如果决定按 POSIX 正确性走，就用 `path.xr` 的 `_isSep`，并把它当成一次**有意的行为修正**写进变更说明。
2. **`_rootLen` 的盘符分支未实测**（我只在 macOS 上跑）。不实现它 → Windows `mkdirp("C:\\a\\b")` 会退化成一次 `mkdir("C:\\a\\b")` 直接失败。**这是唯一的真回归风险，必须实现。**
3. **修掉一个限制（好事）**：现 C 在 `io.c:999` 用 `strnlen(path, XR_PATH_MAX) >= XR_PATH_MAX` 直接拒绝长路径。Xray 版无此上限。
4. **修掉一个 VM/AOT 分歧（好事）**：VM `xr_fs_mkdir`（`src/os/unix/fs_unix.c:96-114`）只在 `errno==EEXIST` 时才去 `stat` 确认；AOT `xrt_io_mkdir`（`src/aot/xrt_io.h:308-324`）对**任何** mkdir 失败都去 `stat` 兜底。Xray 版只有一份 `_ensureDir`。

**删掉多少 C**：io.c 28 + xrt_io.h 23 + xr_io_core.h 77 = **128 行**，外加 `.def` 一块 + 生成文件。

---

### A3. `__readStdinBytes`

**C 实现位置与策略行**

| 文件 | 行 | 内容 |
|---|---|---|
| `stdlib/io/io.c` | 196-215 (20) | `io_readStdinBytes`：`io_prepare_binary_stdin()` + 全读 + memcpy 进 byte array |
| `stdlib/io/io.c` | 133-141 (9) | `xr_io_read_stdin_all`（`clearerr(stdin)` + 调 shared core） |
| `src/aot/xrt_io.h` | 854-875 (22) | `xrt_io_read_stdin_bytes` |
| `src/shared/xr_io_core.h` | **251-306 (56)** | `xr_io_core_read_all_stream_alloc`：**`for(;;)` 读循环 + 容量翻倍 realloc(288) + `max_cap` 上限(283) + 短读判 EOF(275) + 错误分支(276)**。策略全在这里。 |

**`.xr` 目标代码**（实测通过，见 `t09_stdin_pure.xr` / `t11_perf_xr.xr`）

```xray
export fn readStdinBytes(chunkSize: i64 = 65536) -> Array<u8>? {
    var f = File.stdin()
    var out = Array<u8>()
    out.reserve(chunkSize)
    var total = 0
    while (true) {
        var chunk = f.read(chunkSize)
        if (chunk == null) { return null }
        var n = len(chunk!)
        if (n == 0) { break }
        out.resize(total + n, 0)
        for (var i = 0; i < n; i++) { out[total + i] = chunk![i] }
        total = total + n
    }
    return out
}
```

**需要的 leaf 签名**：**无新增**。`File.stdin()` 已存在于 `io.xr:55`，`File.read` 走 `__fileRead(handle: i64, maxBytes: i64): Array<u8>?`——`io_fileRead`（`io.c:235-243`）对 `handle==0` 已经映射到 `stdin` 并调用 `io_prepare_binary_stdin()`。

**依赖判断**
- `Array<u8>.resize(len, fill)` + `arr[i] = v` 索引赋值：可用。
- **`Array<u8>.set(i, v)` 不能用**：`error: Array<u8>.set() must be inside an unsafe block`（实测 `t11`）。必须写 `out[i] = v`。
- **不要用 `concat`**：`out = out.concat(chunk!)` 是 O(n²)，实测 8 MB 要 2.75 s。

**实测**（`t09_stdin_pure.xr`）
```
'hello\nworld'    → len=11
'\xff\xfe\x41'    → len=3, head=255,254,65   （二进制安全，不崩）
空输入            → len=0
300 KB            → len=300000
```

**性能实测**（`t10_*`/`t11_*`，8 MB stdin，各 3 次 `real`）
| 实现 | 耗时 |
|---|---|
| C `__readStdinBytes` | 0.01 s |
| Xray `push` 循环 | 0.17 s（**17×**） |
| Xray `resize` + 索引赋值 | 0.11 s（**11×**） |
| Xray `concat` | 2.75 s（**275×**，二次复杂度，别用） |

**风险**
- 大输入 ~11× 慢（VM 档；AOT 档未测，无法在不构建的前提下验证）。stdin 典型是几 KB，可接受。
- 每次 `f.read(n)` 分配一个 n 字节数组再截断（`io_stream_read_bytes`，`io.c:179-194` 先按 `max_bytes` 建数组再改 `length`），所以 chunk 大小直接等于每轮分配量。C 版是 4096 起步翻倍。
- C 版有 `IO_MAX_READ_BYTES = INT32_MAX` 硬上限，超过就返回 NULL；Xray 版没有显式上限（会撞到 `Array` 自身的 int32 长度上限）。

**删掉多少 C**：io.c 29 + xrt_io.h 22 + xr_io_core.h 56 = **107 行**（`read_all_stream_alloc` 要等 A4 一起走）。

---

### A4. `__readStdin`

**C 实现位置**：`stdlib/io/io.c:165-177 (13)`、`src/aot/xrt_io.h:823-834 (12)`。同样依赖 `xr_io_core_read_all_stream_alloc`。

**`.xr` 目标代码**
```xray
export fn readStdin() -> string? {
    var raw = readStdinBytes()
    if (raw == null) { return null }
    return string.fromUtf8Lossy(raw![:])
}
```

**需要的 leaf 签名**：无新增（复用 A3）。

**依赖判断**：`string.fromUtf8Lossy(bytes: Slice<u8>) -> string`（`src/frontend/analyzer/xnative_type_defs.inc.c` 的 `xr_native_def_string`）。`arr[:]` 全切片走的是与已知 ICE 不同的路径，`io.xr:112/118` 里已经这么用了，安全。

**风险 / 必须先拍板的一件事**
- **现 C 在非 UTF-8 stdin 上 SIGSEGV**（见「发现的缺陷 #1」）。迁移**必然**改变行为，只能三选一：
  - `fromUtf8Lossy` → 非法字节变 U+FFFD（上面给的写法）；
  - `fromUtf8` → 抛 `Utf8Error.InvalidUtf8`（实测确实是抛异常不是返回 null，见 `t03_bytes.xr`）；
  - 先在 C 里补 NULL 检查改成返回 `null`，再迁移时用 `fromUtf8` 并 catch 成 `null` 保持等价。
- 三者都比现状（进程崩溃）好，但要显式选一个并写进变更说明。

**删掉多少 C**：io.c 13 + xrt_io.h 12 = **25 行**；与 A3 合并后 `xr_io_core_read_all_stream_alloc`(56) 彻底无人使用可一并删除。**A3+A4 合计 ≈ 132 行。**

---

## B 档 — 需要先加/改一个薄 leaf

### B1. `__touch` —（新增 1 个 leaf，最小）

**C 位置与策略行**
- `stdlib/io/io.c:1105-1113 (9)` `io_touch` → `xr_io_core_touch`
- `stdlib/io/io.c:1014-1021 (8)` `io_touch_update` = `utime(path, NULL)` / `_utime`
- `stdlib/io/io.c:1023-1029 (7)` `io_touch_create` = `fopen(path,"a")` + `fclose`
- `src/aot/xrt_io.h:487-507 (19)` 同三件
- `src/shared/xr_io_core.h:338-345 (8)` `xr_io_core_touch` —— **策略就是这三行**：`if (update_fn(path)) return true; return create_fn(path);`

**`.xr` 目标代码**
```xray
// touch — 先更新时间戳；路径不存在时退化为"以追加模式创建空文件"。
export fn touch(path: Path) -> bool {
    if (__utimeNow(path)) { return true }
    return __appendFile(path, "")
}
```

**需要的 leaf 签名**
```
fn __utimeNow {
  signature: "(path: Path): bool"
  doc: "Set the file's access and modification times to now"
  vm: "io_utimeNow"          // 就是现在的 io_touch_update 提到边界层
  aot: "xrt_io_utime_now"    // 就是现在的 xrt_io_touch_update
  argc: 1  arg_spec: "p"  aot_direct: true  link_object: false  layer: "system"
}
```
`io_touch` / `io_touch_create` / `xr_io_core_touch` 三者全删。

**依赖判断**：`__appendFile(path: Path, data: string): bool` 已存在。**实测等价**（`t12_touch.xr`）：
```
appendFile('') 在不存在时创建            → exists=true, size=0
appendFile('') 在已有 "hello" 上不截断    → size 5 → 5
io.touch(目录)      = true    io.appendFile(目录,"") = false   ← 所以顺序必须是 utime 优先
io.touch(缺父目录)  = false   io.appendFile(缺父目录,"") = false
```
`io_appendFile` 用 `"ab"`、`io_touch_create` 用 `"a"`——零字节写入下两者无差别。

**风险**
- `__appendFile` 自己也在迁移清单里（B10）。这一步是**分阶段**的：先落 `touch`，等 fd leaf 家族到位后 `appendFile` 再换底。
- `io.touch(目录)` 现在返回 true（utime 对目录成功）。上面的写法保留这个行为。
- `src/os/unix/fs_unix.c:237-248` 里另有一个 `xr_fs_touch`（`futimens` + `O_NOFOLLOW` + `S_ISREG` 检查），**io.touch 不用它**——新 leaf 要沿用 `utime()` 语义（跟随符号链接、对目录有效），不要顺手换成 `xr_fs_touch`，否则是静默行为变更。

**删掉多少 C**：io.c 24 - 新 leaf 8 = 净 16；xrt_io.h 19 - 4 = 净 15；xr_io_core.h 8。**≈ 39 行净删除。**

---

### B2. `__writeStdout` + `__writeStderr` —（新增 2 个 leaf，两条一起走）

**C 位置与策略行**
- `stdlib/io/io.c:255-264 (10)` `io_write_stream`：`xr_io_core_write_all(...)` **&&** `fflush(stream) == 0`
- `stdlib/io/io.c:266-269 / 271-274` 两个 4 行壳
- `src/aot/xrt_io.h:917-931 (13)` 同结构
- `src/shared/xr_io_core.h:323-336 (14)` `xr_io_core_write_all`：**`while (off < len)` 排空循环(329-334)**

策略共两条：排空循环 + "每次写完都 flush"。

**`.xr` 目标代码**
```xray
// 1=stdout, 2=stderr。排空循环与 flush 策略在这里，不在 C 里。
fn _writeAllToStream(stream: i64, data: string) -> bool {
    var view: Slice<u8> = data.bytes()
    var total = len(view)
    var off = 0
    while (off < total) {
        var n = __streamWrite(stream, data, off)
        if (n <= 0) { return false }
        off = off + n
    }
    return __streamFlush(stream)
}

export fn writeStdout(data: string) -> bool { return _writeAllToStream(1, data) }
export fn writeStderr(data: string) -> bool { return _writeAllToStream(2, data) }
```

**需要的 leaf 签名**
```
fn __streamWrite {
  signature: "(stream: i64, data: string, byteOffset: i64): i64"
  doc: "One fwrite from byteOffset to the end; returns bytes written"
  vm: "io_streamWrite"  aot: "xrt_io_stream_write"
  argc: 3  arg_spec: "sii"   // 注意实参顺序：现有 arg_spec 字母只有 ""/i/s/p/v
  ...
}
fn __streamFlush {
  signature: "(stream: i64): bool"
  doc: "One fflush on the given standard stream"
  vm: "io_streamFlush"  aot: "xrt_io_stream_flush"
  argc: 1  arg_spec: "i"
}
```
> `arg_spec` 的合法字母（全树统计）只有 `""`/`i`/`s`/`p`/`v`——**没有 slice 规格**。所以**不要**把 leaf 设计成收 `Slice<u8>`：全树 `core.def` 里没有任何一个条目以 `Slice<T>` 为形参，`net.__readInto` 用的是 `(conn, buffer: Array<u8>, maxlen: i64)`。传 `(string, byteOffset)` 复用已验证的 `s` 编组，是最保险的形状。

**依赖判断（实测 `t19c.xr`）**
- `data.bytes()` 返回零拷贝 `Slice<u8>`，`len()` 给的是**字节长度**（`len("héllo".bytes())==6`，而 `len("héllo")==5`）。
- `s.bytes()` **必须有显式目标类型**：`var view: Slice<u8> = s.bytes()`，否则 `E0365`。
- Slice **不能存在模块级绑定**（`E0383`），只能在函数体内。上面的写法满足。
- `print` 不受影响：它是 core intrinsic（`src/shared/xr_core_intrinsic.def:27` `XR_CORE_INTRINSIC(PRINT, ...)`），不走 `io.writeStdout`。

**风险**
- `xr_io_core_write_all` 对 `fwrite` 的排空循环在实践中**基本是死代码**（stdio 短写即错误），所以迁移几乎不改变可观察行为，但每次 `writeStdout` 多一次 `data.bytes()` + 一次 `len()`——常数开销。
- 现 C 只在**全部写完之后**才 flush 一次；上面的写法一致。如果改成循环内 flush 会改变交错行为，不要。
- `stream` 用魔数 1/2 表示 stdout/stderr。C 侧 leaf 要做白名单映射（`1→stdout, 2→stderr`，其它一律 false），不要让它变成任意 FILE* 逃逸口。

**删掉多少 C**：io.c 18 - 新 leaf ~14 = 净 4；xrt_io.h 13 - ~10 = 净 3；`xr_io_core_write_all`(14) 仍被 `appendFile`/`writeFile*` 用着，要等 C 档一起删。**净删除小（≈ 7 行）——这一条的价值是"策略移位"，不是"删代码"。**

---

### B3. `__mkdir` —（改 1 个 leaf 的返回类型）

**C 位置与策略行**
- `stdlib/io/io.c:672-681 (10)` → `xr_fs_mkdir(path, 0755)`
- `src/os/unix/fs_unix.c:96-114`：`mkdir` 成功 → 0；**`errno == EEXIST` 时再发一次 `stat` 并判 `S_ISDIR` → 也算成功**（103-112）。这是 errno 分类 + 二次系统调用的策略。
- `src/aot/xrt_io.h:308-324 (17)`：**语义不同**——对**任何** mkdir 失败都 `stat` 兜底，不检查 errno。

**`.xr` 目标代码**
```xray
// mkdir — "路径已经是目录"算成功的判定在这里，不在 C 里。
export fn mkdir(path: Path) -> bool {
    if (__mkdirRaw(path, 493) == 0) { return true }   // 493 == 0o755
    return __isDir(path)
}
```

**需要的 leaf 签名**
```
fn __mkdirRaw {
  signature: "(path: Path, mode: i64): i64"
  doc: "One mkdir(2); 0 on success, -errno on failure"
  vm: "io_mkdirRaw"  aot: "xrt_io_mkdir_raw"
  argc: 2  arg_spec: "pv"   // 与 __chmod 同形
}
```
`io_mkdir` + `xr_fs_mkdir` 的 EEXIST 分支 + `xrt_io_mkdir` 的 stat 兜底全删。

**风险**：`xr_fs_mkdir` 被 io 之外的代码用吗？要先查（本轮未查）。若有，就只删 io 侧的调用，不动 `fs_unix.c`。

**为什么它值得单独做**：`__mkdirRaw` 一个新 leaf**同时解锁 B4 `__tempDir`**（见下），也修掉上面的 VM/AOT errno 分歧。

---

### B4. `__tempDir` —（**零额外 leaf**，只要 B3 的 `__mkdirRaw` 先落）

> ⚠️ 任务简报说 `__tempDir` 是"纯环境变量优先级链、零 syscall"——那是 **`os.__tmpdir`**（`stdlib/os/os.c:282-286` → `xr_os_core_tmpdir`，确实零 syscall）。**`io.__tempDir` 不是**：它是 env 链 **+ 模板拼接 + `mkdtemp(3)`**（`stdlib/io/io.c:1220-1245`）。原子独占创建不能纯 Xray 实现。

**C 位置与策略行**
- `stdlib/io/io.c:1220-1245 (26)`：POSIX 分支 `xr_os_core_tmpdir(...)`(1238) + `xr_io_core_temp_template(root,'/', "xray_XXXXXX", tpl, ...)`(1239) + `mkdtemp(tpl)`(1241)；Windows 分支 `GetTempPathA` + `GetTempFileNameA` + `DeleteFileA` + `CreateDirectoryA`（1225-1236，先建文件再删再建目录）
- `stdlib/io/io.c:1188-1191 (4)` `io_core_getenv` 适配器
- `src/shared/xr_os_core.h:94-113` `xr_os_core_tmpdir`：**TMPDIR → TMP → TEMP → "/tmp"** 四级链
- `src/shared/xr_io_core.h:591-613 (23)` `xr_io_core_temp_template`
- `src/aot/xrt_io.h:707-710 / 734-755 (26)` 同结构

**`.xr` 目标代码**
```xray
import os          // io.xr 目前没有这一行；见下方"依赖判断"

fn _tempToken() -> string {
    var digits = "0123456789abcdefghijklmnopqrstuvwxyz"
    var out = ""
    for (var i = 0; i < 12; i++) {
        var k = math.randomInt(0, 35)
        out = out + digits.slice(k, k + 1)
    }
    return out
}

// tempDir — 独占创建的重试循环与命名策略在这里；C 只剩一次 mkdir(2)。
export fn tempDir() -> Path? {
    var root = os.tmpdir()
    for (var attempt = 0; attempt < 32; attempt++) {
        var candidate = path.join(Path(root), Path("xray_" + _tempToken()))
        if (__mkdirRaw(candidate, 448) == 0) { return candidate }   // 448 == 0o700
    }
    return null
}
```

**需要的 leaf 签名**：只有 B3 的 `__mkdirRaw`。`io_tempDir` / `xrt_io_temp_dir` / `io_core_getenv` / `xr_io_core_temp_template` 全删。

**依赖判断**
- **`io.xr` 能不能 `import os`？** 先例充分：`stdlib/log/log.xr:8-9` 同时 `import io` + `import os`；`stdlib/path/path.xr:15` `import os`，而 `io.xr:6` 已经 `import { Path } from path` —— 也就是说 `os` **已经在 io 的传递模块图里**。`os.xr` 不 import 任何东西，所以 `io → os` 不成环。`.xr` 源在 `CMakeLists.txt:1176` 是 `file(GLOB "stdlib/*/*.xr")`，没有硬编码顺序表，由模块图解析器定序。**（代码推断，未构建验证。）**
  - 层号提示：manifest 里 `io`=L2、`os`=L2、`path`=L4（`stdlib/stdlib_boundary.toml:307/319/122`）。`io`(L2) 已经在 import `path`(L4)——层号在这里不是严格 DAG 约束。
- `os.tmpdir()` 已是 Xray 表面（`stdlib/os/os.xr:42`）。实测 env 优先级链在纯 Xray 里可写（`t01_env.xr`）。
- `math.randomInt(min, max)` 的熵源是**系统 CSPRNG**：`stdlib/math/math.c:430-452` → `xr_random_bytes` → `src/os/os_random.h:11-13` 记载 arc4random_buf / getrandom / BCryptGenRandom。所以纯 Xray 生成的临时名与 `mkdtemp` 同一熵等级。**实测可用**（`t02_caps.xr`）。
- `path.join` 已是 Xray（`stdlib/path/path.xr:301`）。

**风险**
1. **修掉一个已存在的路径拼接 bug（好事）**：`xr_io_core_temp_template` 无条件补一个分隔符，而 macOS 的 `TMPDIR` 以 `/` 结尾，所以现在 `io.tempDir()` 返回的是 `…/T//xray_S556LM`（**实测 `t02_caps.xr` 输出里的双斜杠**）。`path.join` 不会产生双斜杠。任何对返回路径做字符串比较的调用方都会看到这个差异。
2. `mkdtemp` 建目录用 0700；上面用 448(0o700) 保持一致。**别用 `__mkdir`（0755）**。
3. 重试上限 32 次与 `src/os/unix/temp_unix.c:39` 的既有实现一致。
4. Windows：现 C 走 `GetTempPathA`/`GetTempFileNameA`；纯 Xray 版会改成 `os.tmpdir()`（env 链 + `C:\Windows\Temp` 兜底）+ `mkdir`。这是**实质行为变更**——`GetTempPathA` 的解析顺序是 TMP→TEMP→USERPROFILE→Windows 目录，与 `xr_os_core_tmpdir` 的 TMPDIR→TMP→TEMP 不同。要么接受，要么给 Windows 留一个 `__winTempRoot()` leaf。
5. 树里**已经有第二份**同算法实现：`src/os/os_temp.h` + `src/os/unix/temp_unix.c:31-58`（CSPRNG token + 独占 `mkdir(0700)` + 32 次重试），只被 toolchain/单测用。迁移后应把它也指向同一套语义（或至少在文档里承认这份重复）。

---

### B5. `__removeAll` —（新增 1-2 个 leaf）

**C 位置与策略行**
- `stdlib/io/io.c:1058-1081 (24)`：装配 `XrIoCoreRemoveAllOps` vtable
- `stdlib/io/io.c:1032-1054 (23)`：`io_remove_all_leaf` / `io_remove_all_dir` 的两套平台实现。**Windows 版 `SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL)` 再 `DeleteFileA`（1037-1038）——清只读属性是策略**
- `src/shared/xr_io_core.h:419-478 (60)`：`_visit` + `_impl`。**递归(452)、逐子路径 alloc+join(440-450)、dot 过滤(431)、`remove_dir` 与 `visit_ctx.ok` 的先后(469)**
- `src/aot/xrt_io.h:569-645 (~50)` 同结构

**`.xr` 目标代码**（POSIX 语义实测通过，见 `t17_removeall.xr`）
```xray
export fn removeAll(path: Path) -> bool {
    if (!__exists(path)) { return false }
    if (!__isDir(path) || __isSymlink(path)) { return __remove(path) }
    var names = __readDir(path)
    var ok = true
    for (var i = 0; i < len(names); i++) {
        if (!removeAll(Path(path.toString() + "/" + names[i]))) { ok = false }
    }
    return __rmdir(path) && ok
}
```

**需要的 leaf 签名**
```
fn __rmdir { signature: "(path: Path): bool"  vm: "io_rmdir"  aot: "xrt_io_rmdir"  argc: 1  arg_spec: "p" }
// Windows 还需要（POSIX 上恒 true）：
fn __clearReadonly { signature: "(path: Path): bool" ... }
```

**依赖判断 / 为什么不是 A 档**
- 在 **POSIX 上零新 leaf 就能跑**：`remove(3)` 对空目录会调 `rmdir`。实测（`t17_removeall.xr`）四组用例（深树+符号链接 / 缺失目标 / 单文件）与 C **完全一致**。
- **但 Windows 的 MSVCRT `remove()` 删不了目录**，且不清只读属性——所以纯 `io.remove` 版会在 Windows 上直接坏掉。`__rmdir` 是必须的。

**风险**
- `__isSymlink` 守卫**必须写**：VM `__isDir` 走 lstat（不跟随），AOT `xrt_io_is_dir`（`src/aot/xrt_io.h:156-171`）走 `stat`（**跟随**）。不加守卫，AOT 会顺着符号链接删到目标目录里去。`__isSymlink` 两端都是 lstat（`xrt_io.h:428`），所以这个守卫是跨后端一致的。
- 递归深度：C 版 `remove_all` **没有**深度上限（不像 readDirRecursive 的 64）。Xray 递归同样没有——深目录树会撞栈。建议顺手加一个上限。
- 现 C 的 `return ops->remove_dir(...) && visit_ctx.ok`（`xr_io_core.h:469`）短路顺序意味着：即使子项删除失败，`rmdir` 仍会被尝试。上面的写法保留这个顺序。

---

### B6. `__FileStat` —（改 1 个 leaf 的返回形状）

**C 位置**
- `stdlib/defs/core.def:2200-2202`：`handle __FileStat { fields: "…10 个字段…" }`
- `stdlib/io/io.c:909-921 (13)` `io_get_stat_class`（+ 每 isolate 的 `cache->io_stat_class` 槽）
- `stdlib/io/io.c:925-978 (54)` `io_stat`：`stat` + `lstat`(942) + `xr_io_core_stat_fields` + **10 次 `xr_instance_set_dynamic_field`（966-975）**
- `src/aot/xrt_io.h:434-472 (39)`
- `src/shared/xr_io_core.h:135-151` `xr_io_core_stat_fields`、`122-124` `xr_io_core_stat_perm_mode`（`mode & 0777` 是策略）、`118-120` 字段名表

**`.xr` 目标代码**
```xray
export fn stat(path: Path) -> FileStat? {
    var r = __statFields(path)     // Array<i64>?，10 槽，bool 用 0/1
    if (r == null) { return null }
    var v = r!
    return FileStat(v[0], v[1] & 511, v[2], v[3], v[4], v[5], v[6],   // 511 == 0o777
                    v[7] != 0, v[8] != 0, v[9] != 0)
}
```
`io.xr:8-33` 的 `FileStat` class 已经存在且是 `check_l2_thinning` 的必需 marker（`scripts/check_stdlib_boundary.py:265`），不能删。

**需要的 leaf 签名**
```
fn __statFields {
  signature: "(path: Path): Array<i64>?"
  doc: "stat(2)+lstat(2) packed as [size,mode,mtime,atime,ctime,uid,gid,isFile,isDir,isSymlink]"
  argc: 1  arg_spec: "p"  return_ownership: "fresh"
}
```
删掉 `handle __FileStat` 声明、`io_get_stat_class`、`XrStdlibCache::io_stat_class` 槽、10 次 `set_dynamic_field`、`xr_io_core_stat_fields`、`xr_io_core_stat_perm_mode`、字段名表。

**风险**：`FileStat` 从"native record class"变成"Xray class from 10 个 i64"——`0777` 掩码从 C 移到 `.xr`。要确认 `xr_stdlib_record_class_get(X,"io","__FileStat")` 没有别的调用方。返回 `Array<i64>` 比返回 object 多一次数组分配（stat 是低频调用，可接受）。

---

### B7. `__tempFile` —（新增 1 个 leaf）

**C 位置**：`stdlib/io/io.c:1194-1217 (24)`、`src/aot/xrt_io.h:712-732 (21)`。与 B4 同一套 env 链 + 模板，末尾 `mkstemp(tpl)` 然后**立刻 `close(fd)` 把 fd 丢掉**（1214）——返回的路径因此是 TOCTOU 竞态的。

**`.xr` 目标代码**
```xray
export fn tempFile() -> Path? {
    var root = os.tmpdir()
    for (var attempt = 0; attempt < 32; attempt++) {
        var candidate = path.join(Path(root), Path("xray_" + _tempToken()))
        if (__createExclusive(candidate, 384) == 0) { return candidate }   // 384 == 0o600
    }
    return null
}
```

**需要的 leaf 签名**
```
fn __createExclusive {
  signature: "(path: Path, mode: i64): i64"
  doc: "One open(2) with O_CREAT|O_EXCL then close; 0 on success, -errno otherwise"
  argc: 2  arg_spec: "pv"
}
```
（更好的终态是 `(path, mode): i64` 返回 **fd**，让调用方保住独占句柄——但那要先有 fd leaf 家族，属于 C 档的配套。）

**风险**：与 B4 同（双斜杠消失、Windows 根目录来源变更）。另外 mkstemp 建文件是 0600，别用 `__writeFile` 的 0644。

---

### B8. `__readDir` —（新增 3 个 leaf）

**C 位置与策略行**
- `stdlib/io/io.c:739-757 (19)` `io_readDir`
- `stdlib/io/io.c:683-703 (21)` `io_dir_for_each_entry`：**`while (xr_dir_next(it,&e))` 循环 + `xr_dir_close`**
- `stdlib/io/io.c:727-731 / 733-736` emit + release 辅助
- `src/shared/xr_io_core.h:480-498 (19)`：`read_dir_visit` 的 **dot 过滤(487)**
- `src/aot/xrt_io.h:540-567 + 761-788 (~50)`

**`.xr` 目标代码**
```xray
export fn readDir(path: Path) -> Array<Path> {
    var out: Array<Path> = []
    var d = __dirOpen(path)
    if (d < 0) { return out }
    defer { __dirClose(d) }
    while (true) {
        var name = __dirNext(d)
        if (name == null) { break }
        if (name! == "." || name! == "..") { continue }
        out.push(Path(name!))
    }
    return out
}
```

**需要的 leaf 签名**
```
fn __dirOpen  { signature: "(path: Path): i64"    doc: "opendir(3); -1 on failure" }
fn __dirNext  { signature: "(handle: i64): string?" doc: "One readdir(3) entry name; null at end" }
fn __dirClose { signature: "(handle: i64): bool"  doc: "closedir(3)" }
```

**依赖判断**：`defer { }` 对 native handle 实测可用（`t18_defer.xr`）。这三个 leaf 与现有 `__fileOpen/__fileRead/__fileClose` 是同一套 handle-as-i64 模式，`.def` 里已有 `visibility: "internal"` 先例。

**风险**
- **句柄泄漏**：`defer` 生效前的每条 early return 都要覆盖。`__fileOpen` 家族已有同样的暴露面，不是新风险类别。
- 迁移后 dot 过滤在 `.xr`，行为不变。
- **顺带修一个类型谎言**（见「缺陷 #2」）：`__readDir` 现在声明 `Array<string>` 却会返回 `xr_null()`；重写后 `readDir` 永远返回数组。

---

### B9. `__appendFile` —（fd leaf 家族的第一步）

**C 位置**：`stdlib/io/io.c:570-587 (18)` = `fopen(path,"ab")` + `xr_io_core_write_all`（排空循环）+ `fclose`。`src/aot/xrt_io.h:257-266 (10)`。

**`.xr` 目标代码**
```xray
export fn appendFile(path: Path, data: string) -> bool {
    var fd = __fdOpenAppend(path, 420)   // 420 == 0o644
    if (fd < 0) { return false }
    var view: Slice<u8> = data.bytes()
    var total = len(view)
    var off = 0
    var ok = true
    while (off < total && ok) {
        var n = __fdWriteString(fd, data, off)
        if (n <= 0) { ok = false } else { off = off + n }
    }
    return __fdClose(fd) && ok
}
```

**需要的 leaf 签名**
```
fn __fdOpenAppend  { signature: "(path: Path, mode: i64): i64" }
fn __fdWriteString { signature: "(fd: i64, data: string, byteOffset: i64): i64" }
fn __fdClose       { signature: "(fd: i64): bool" }
```
（`__fdWriteString` 与 B2 的 `__streamWrite` 只差 stream/fd 一个概念——**应该合并成一个 `__fdWrite(fd, data, off)`，标准流用 fd 1/2**。这样 B2 + B9 只引入 3 个 leaf 而不是 5 个。）

**风险**：`.xr` 里的排空循环 + 每次调用一次 `data.bytes()`。日志路径（`stdlib/log/log.xr:167` 每行一次 `io.appendFile`）会感受到常数开销。

---

### B10.（已并入 B2/B9）

---

## C 档 — 需要新能力

`__readFile` / `__readFileBytes` / `__writeFile` / `__writeFileBytes`

**C 位置**
- `stdlib/io/io.c:414-450 (37)` / `453-497 (45)` / `538-567 (30)` / `500-535 (36)`
- **`stdlib/io/io.c:285-411 (127)`** —— 整块 `#if defined(XR_OS_LINUX) && defined(XR_HAS_IO_URING)`：`FileIoState`、`file_io_step`、`file_io_complete`、`file_io_sync_rest`、`file_io_try_uring`。这四个 leaf 是它唯一的使用者。
- `stdlib/io/io.c:869-883 (15)` `io_read_file_buffer_sync`
- `src/shared/xr_io_core.h:184-249 (66)` `prepare_sized_read` + `read_into` + `read_sized_stream_alloc`
- `src/aot/xrt_io.h:185-281 (~97)`

**为什么现在不能做**
1. 这四个是 `.def` 里 `vm_binding: "yieldable"` 的（`core.def:2422/2436` 等）。它们在 Linux+io_uring 下**真的会挂起协程等 CQE**。现有的 `__fileOpen/__fileRead/__fileClose` 是**非**可挂起的 `fopen/fread`。用它们在 `.xr` 里重写 = **删掉 io_uring 异步文件 I/O**，每次读写都阻塞 worker 线程。
2. 需要的新能力是**可挂起的 fd leaf**：`__fdReadInto(fd, buf: Array<u8>, off, len): i64` 带 `vm_binding: "yieldable"` + `caps: "coro,netpoll"`，语义对标 `net.__readInto`（`core.def:3391-3403`，已经是这个形状）。这不是语言缺口，是**运行时/绑定层工作量**。
3. 性能：实测纯 Xray 的字节搬运在 8 MB 上比 C 慢 **11×**（`t11_perf_xr.xr`，VM 档）。`readFile` 是热路径，这个倍数不可接受，除非 leaf 直接返回整块 `Array<u8>` 而循环只在错误路径上跑。

**建议**：先把 A/B 档做完；等 fd leaf 家族（`__fdOpen/__fdRead/__fdWrite/__fdClose`，其中读写两个可挂起）设计定案后，这四个 + `__appendFile` 一起换底，一次性删掉 io_uring 状态机的**驱动**部分（状态机本身要下沉进 `__fdReadInto` 的实现）。

**顺带**：`stdlib/io/io.c:447` / `:312` / `:176` 是「缺陷 #1」的三个崩溃点，都在这一档里。迁移后 `readFile = fromUtf8Lossy(readFileBytes())`，崩溃自然消失。

---

## D 档 — 建议保留（重分类为真 leaf）

### `__copyFile`

**C 位置**：`stdlib/io/io.c:786-867 (82)`、`src/aot/xrt_io.h:388-416 (29)`。

三条平台快路径：Windows `CopyFileA`(797)、macOS `open+open+fcopyfile+close`(800-811)、Linux `open+fstat+open` + `while (remaining>0) sendfile(...)`(834-843)；其余平台走 `xr_io_core_copy_stream` 缓冲循环(858-861)。

**为什么建议保留**
- `CopyFileA` / `fcopyfile` 是**整文件一次调用**的宿主 ABI，没有可拆的循环。
- 唯一的策略是 Linux 分支里的 sendfile 短传重试（834-843）。把它提到 `.xr` 需要一个 `__copyFileRange(srcFd,dstFd,off,len): i64` leaf——但那个 leaf 在 Windows/macOS 上**没有对应物**，只能做成"一次拷完"的假循环。拆解会**制造**一个跨平台不一致的抽象，而不是消除一个。
- 建议：把 dossier 里的 `not_a_leaf` 改判为 `host_abi_leaf`，并单独修下面两个实打实的缺陷（不是迁移，是 bugfix）：
  1. macOS/Linux 分支**忽略 `close(dst_fd)` 的返回值**（`io.c:809-810` / `844-845`）——写回失败会被当成成功上报。
  2. macOS/Linux 恒用 0644 建目标文件、不保留源权限；Windows `CopyFileA` **会**保留属性。跨平台语义不一致。

---

## 建议落地顺序

| # | 条目 | 档 | 新 leaf | 删 C（io.c / xrt_io.h / xr_io_core.h） | 说明 |
|---:|---|:--:|:--:|---|---|
| 0 | 删 `xr_io_core_read_lines_each` + `trim_line_end` | — | 0 | 0 / 0 / **29** | `readLines` 迁移留下的死代码，**生产侧零调用者**，只有 `tests/unit/stdlib/test_io_core.c` 还在测它。纯清扫，零风险。 |
| 1 | **`__readDirRecursive`** | A | 0 | 27 / 32 / **99** = **158** | 见下节"为什么是它" |
| 2 | `__mkdirp` | A | 0 | 28 / 23 / **77** = **128** | 必须先实现 `_rootLen` 的盘符分支，否则 Windows 回归 |
| 3 | `__readStdinBytes` + `__readStdin` | A | 0 | 42 / 34 / **56** = **132** | 同时消掉一个 SIGSEGV；先拍板非 UTF-8 策略 |
| 4 | `__touch` | B | 1 (`__utimeNow`) | ≈ 39 净 | 最小的 B；`__appendFile(p,"")` 等价性已实测 |
| 5 | `__mkdir` → `__mkdirRaw` | B | 1 | ≈ 20 净 | **解锁第 6 步**；顺带统一 VM/AOT 的 errno 判定 |
| 6 | `__tempDir` | B | 0（复用第 5 步） | 30 / 26 / **23** = **79** | 顺带修双斜杠；Windows 根目录来源要拍板 |
| 7 | `__tempFile` | B | 1 (`__createExclusive`) | ≈ 45 | 与第 6 步共用 `_tempToken` |
| 8 | `__writeStdout` + `__writeStderr` | B | 2（或与第 10 步合并成 1） | ≈ 7 净 | 删得少，但把 flush 策略搬出 C |
| 9 | `__removeAll` | B | 1-2 (`__rmdir`, Win `__clearReadonly`) | 47 / 50 / **60** = **157** | POSIX 版已实测等价 |
| 10 | `__readDir` | B | 3 (`__dirOpen/Next/Close`) | 45 / 50 / **19** = **114** | 做完后第 1、9 步不再依赖旧 `__readDir` |
| 11 | `__appendFile` | B | 复用第 8/10 步的 fd 家族 | ≈ 28 | |
| 12 | `__FileStat` → `__statFields` | B | 1 | 67 / 39 / **21** = **127** | 同时删掉 `XrStdlibCache::io_stat_class` |
| 13 | `__readFile`/`__readFileBytes`/`__writeFile`/`__writeFileBytes` | C | 可挂起 fd leaf 家族 | 148 + **io_uring 块 127** / 97 / 66 = **438** | 最大一块，但要先设计可挂起 leaf |
| — | `__copyFile` | D | — | — | 保留；单独修 close 返回值与权限保留两个 bug |

**A 档三步（第 1-3 步）合计删除 ≈ 418 行 C，零新 leaf，零 `.def` 新增**（只删条目 + 重跑 `tools/stdlibgen/stdlibgen.py` 的 7 个生成文件）。

---

## 附 0：调研期间树上出现的同类先例

调研中途（本地 01:28-01:29）`stdlib/defs/core.def` 与 `stdlib/os/os.xr` 被另一个 session 改了，改的正是同一类拆解：

- 新增 `fn __environBlock { signature: "(): Array<string>"  doc: "Host environment block as raw NAME=VALUE entries"  vm: "os_environ_block"  aot: "xrt_os_environ_block" }`（`core.def:1926-1937`）
- `os.xr` 的 `environ()` 变成纯 Xray：遍历原始条目、`indexOf("=")`、`eq <= 0` 丢弃、`slice` 切分建 Map（`os.xr:37-48`）

这正好是本文 **B6 `__statFields`** 和 **B8 `__dirNext`** 采用的形状：**leaf 只交付原始数据，切分/过滤/组装留在 `.xr`**。可以直接照抄它的 `.def` 字段集（`return_ownership: "fresh"`、`aot_direct: true`、`layer: "system"`）。

（`io` 块本身未被改动：`module io` 仍是 1 个 `handle` + 35 个 `fn __`。本文引用的 `stdlib/io/io.c`、`src/aot/xrt_io.h`、`src/shared/xr_io_core.h`、`stdlib/io/io.xr` 行号在写作时均为当前树。）

## 附：每一步都要动的文件清单

删/改一个 `.def` 条目会波及（由 `tools/stdlibgen/stdlibgen.py` 生成，**不可手改**，见 CMakeLists.txt:362-370）：
- `src/stdlib/xstdlib_defs_generated.h`
- `src/stdlib/xstdlib_vm_bindings_generated.inc.c`
- `src/stdlib/xstdlib_class_bindings_generated.inc.c`
- `src/aot/xstdlib_aot_methods_generated.inc.c`
- `src/aot/xaot_stdlib_generated.inc.c`
- `src/frontend/analyzer/xanalyzer_builtins_generated.h`
- `src/app/lsp/xlsp_stdlib_generated.inc`

必须仍然通过的门（`scripts/check_stdlib_boundary.py:220-305`）：
- `io.public_native` 必须是**空集**（所有 native 都是 `__*`）— 新增薄 leaf 满足。
- `io.xr` 必须仍含 `class BufReader` / `class BufWriter` / `class LineIterator` / `class FileStat`。
- io/os/net 三模块公开符号的 Xray 拥有率 ≥ 85%。
- **没有 leaf 数量上限**——加薄 leaf 不触门。

回归网（这些用例直接盯着本文的目标）：
`tests/diff/cases/semantics/stdlib/io_read_dir_shared_core.xr`、`io_write_shared_core.xr`、`io_touch_shared_core.xr`、`io_remove_all_shared_core.xr`、`io_read_stdin_shared_core.xr`、`io_path_result_shared_core.xr`、`io_chmod_shared_core.xr`、`io_system_direct.xr`；
`tests/regression/10_stdlib/1190_io_basic.xr`（24 处）、`1191_io_edge.xr`（16）、`1192_io_extended.xr`（13）；
`tests/unit/stdlib/test_io_core.c`（23 处引用 shared core，随 helper 删除同步收缩）。
