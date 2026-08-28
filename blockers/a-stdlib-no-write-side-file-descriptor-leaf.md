# Blocker：io 模块没有写侧文件描述符叶子

- **Lane**：A（标准库自举）
- **状态**：`RESOLVED`（由 lane 6-9 在分支 `work/6-9-io-write-leaf-34be0379c` 上解决）
- **原请求归属**：H（编译器 / 运行时）
- **原严重度**：卡住 io 剩下的全部迁移。每一个都是 drain loop，而 drain loop
  没有可写的句柄就写不出来。

## 精确来源

| 项 | 值 |
|---|---|
| 提出时基线 | `f78ca940aeecd8d2512520a46d5e3391ec75b117` |
| 提出时分支 | `work/a-stdlib-selfhost-r2-f78ca940a` |
| 解决时基线 | `34be0379c` |
| 解决时分支 | `work/6-9-io-write-leaf-34be0379c` |

---

## 原文记录：缺什么

io 模块只声明了读侧的句柄三件套：

```
__fileOpen(path: Path): i64
__fileRead(handle: i64, maxBytes: i64): Array<u8>?
__fileClose(handle: i64): bool
```

没有写侧对应物——没有 `__fileOpenWrite`、没有 `__fileWrite`、没有按句柄追加的路径。

被卡住的四个公开符号：`io.appendFile`、`io.writeFile`、`io.writeFileBytes`、
`io.writeStdout` / `writeStderr`。每一个都是同一个形状：打开、写到缓冲耗尽或出错、
关闭并报告关闭是否也成功。循环和「关失败即写失败」是策略，属于模块体；
只有单次写才是 host 调用。

---

## 怎么解的

### 1. 句柄模型从 `FILE*` 改成文件描述符

原来的读侧三件套把 `fopen(3)` 返回的 `FILE*` 强转成 `intptr_t` 递给 Xray，
后果是 `.xr` 源里写 `__fileRead(12345, 100)` 会**解引用地址 12345**。
`native_leaf_allowlist.toml` 的 `io.__fileOpen` 记录自己就把这条列为删除条件。

改成 fd 之后：坏值得到 `EBADF`，不是崩溃。而且 fd 是 io_uring **唯一**接受的句柄形式
——`FILE*` 没有可提交的形态，这是保住写侧 yieldable 的前提。

保留编号：`0` = stdin，`1` = stdout，`2` = stderr。真实句柄恒 `> 2`
（`open` 拿到 `<= 2` 时用 `F_DUPFD_CLOEXEC` 提上去，提不动就拒绝这次 open）。
读侧本来就用 `handle == 0` 表示 stdin，这只是把同一个约定补全到写侧。

**1 和 2 走 C 运行时的 `stdout` / `stderr` 流，不是裸 `write(2)`**：
`print` / `println` 是 core intrinsic，VM 走 `OP_PRINT`，AOT 走 `xrt_print` →
`fwrite(..., stdout)`。裸描述符写会越过同一个 stdio 缓冲，
`tests/aot/basic/system_io_streaming.xr:24` 锚定的 `raw-stdout` 交错顺序就会变。

`__fileClose` 相应地从「拒绝 `<= 0`」改成「拒绝 `<= 2`」。真实 `FILE*` 从来落不到
1/2，所以这不改变任何既有行为，只是把保留区补全。

### 2. 写侧四件套（core.def 的 io 块）

```
__fileOpenWrite(path: Path, append: bool): i64
__fileWrite(handle: i64, data: Array<u8>, offset: i64): i64      vm_binding: yieldable
__fileWriteStr(handle: i64, data: string, offset: i64): i64      vm_binding: yieldable
__fileFlush(handle: i64): bool
```

`__fileClose` 按原请求共享，没有第二个 close。

比原请求多出来的两个，各有理由：

- **`__fileWriteStr`**：`arg_spec` 的 `s` 把 string 降解为借用的 `(data, len)` 对，
  所以字符串写零拷贝。把字符串路由到 `Array<u8>` 那个叶子会让**每一次写文件**
  多一次全量拷贝。
- **`__fileFlush`**：只有标准流带 C 运行时缓冲。原来的 `io_write_stream` 是
  「write_all 之后 flush 一次」，这条规则同样是策略，现在在 `io.xr` 里。

`offset` 是**数据缓冲内的下标**，不是文件偏移——文件位置永远是描述符自己的，
所以追加句柄会一直追加。io_uring 提交用 `req.offset = (uint64_t) -1`
（`sqe->off == -1` 表示用当前文件位置），这正是 `FILE*` 模型做不到的。

### 3. 原请求的 yieldable 硬约束：保住了

原文的约束是：

> 写侧叶子如果不能挂起，就把这些调用推回阻塞 worker。

`__fileWrite` / `__fileWriteStr` 都标了 `vm_binding: "yieldable"`，
在 Linux + `XR_HAS_IO_URING` 且当前在协程里时提交一次 `XR_URING_OP_FILE_WRITE`
并挂起。拆成 open + N×write + close 之后**每个 chunk 都能挂起**，比原来
「整文件一次提交」的粒度更细。

代价是撞上了另一个既有缺陷，见下面「留下的一个 blocker」。

### 4. drain loop 上移到 `stdlib/io/io.xr`

三条策略现在都是 Xray：

```
fn _drainBytes(file: File, data: Array<u8>) -> bool {
    var off = 0
    while (off < len(data)) {
        var n = file.write(data, off)
        if (n <= 0) { return false }        // 零进展是失败，不是重试
        off = off + n
    }
    return true
}

fn _publishBytes(path: Path, data: Array<u8>, append: bool) -> bool {
    var file = File.openWrite(path, append)
    if (file == null) { return false }
    var ok = _drainBytes(file!, data)
    return file!.close() && ok              // 关失败即写失败
}
```

**一个曾经写错、被测出来的地方**：`_drainStr` 的循环上界必须是
`len(data.bytes())` 而不是 `len(data)`——`len(string)` 是**码点数**，而 offset 是
**字节**，用 `len(data)` 会在任何非 ASCII 内容上提前退出、静默截断。
实测 `héllo 世界 🎉` 往返可以复现。

`File` 类同时长出了写侧的一半（`openWrite` / `write` / `writeString` / `flush` /
`stdout()` / `stderr()`），和读侧对称。

### 5. 删掉的 C

六个叶子连同它们的 VM 和 AOT 实现：

| 叶子 | VM | AOT |
|---|---|---|
| `__appendFile` | `io_appendFile` | `xrt_io_append_file` |
| `__copyFile` | `io_copyFile` | `xrt_io_copy_file` |
| `__writeFile` | `io_writeFile` | `xrt_io_write_file` |
| `__writeFileBytes` | `io_writeFileBytes` | `xrt_io_write_file_bytes` |
| `__writeStdout` | `io_writeStdout` | `xrt_io_write_stdout` |
| `__writeStderr` | `io_writeStderr` | `xrt_io_write_stderr` |

连带的 helper：`io_write_stream`、`io_copy_file_*`、`IoCopyFileCtx`、`io_file_write`、
`xrt_io_write_buffer`、`xrt_io_write_stream`、`xrt_io_copy_file_*`、`XrtIoCopyFileCtx`。
uring 状态机的 `FILE_IO_WRITE` 分支也删了——写侧现在有自己的单次提交路径。

**没有留新旧两条写路径。**

### 6. 有意丢弃的东西：copyFile 的平台零拷贝

macOS `fcopyfile`、Linux `sendfile`、Win32 `CopyFileA` 全部丢弃，改成 io.xr 里的
读写泵。理由：

- allowlist 的 deletion_trigger 明写「re-offered as their own leaf **or dropped**」；
- AOT 侧**从来就没有**这些快路径，两个后端对「一次 copy 到底发生了什么」
  一直不一致——这正是 allowlist 记录的
  「The two halves do not reach the same syscalls」；
- 泵本身是策略（读多少、写多少、失败怎么办）。

实测 200 KB 文件跨多个 chunk 的复制字节保真。这是用一点吞吐换两个后端第一次语义一致。

---

## 留下的一个 blocker（新提出，不是本 blocker 的残留）

`BufWriter` 的 publish 步骤从 `w.flush()` 方法改成了 `io.flushWriter(w)` 模块级函数。

原因**不是**设计选择，是一个既有编译器缺陷：stdlib 类方法只要经 Xray 中间函数
到达可挂起叶子，coroutine lowering 就 fail-closed。同样的形状在
`sync` 的 `Mutex` / `RwLock` 上已经先炸了，与 io 无关：

```bash
./build-nofp/xray run tests/diff/cases/semantics/concurrency/mutex_generic_compose.xr
```

报**逐字相同**的错。详见
`blockers/a-stdlib-class-method-cannot-reach-suspending-leaf-indirectly.md`——
根因是编译器有四套「可挂起性」oracle，其中两套的种子硬编码只认 `net`
（`src/stdlib/xstdlib_metadata.h:188-196`）。

这个形状和 `stdlib/net/net.xr` 已有的设计一致（它的每个 I/O 入口都是模块级函数
操作不透明句柄，一个带 I/O 的类方法都没有），所以不算倒退；
但它是被缺陷逼出来的，那个 blocker 解掉之后可以变回方法。

`BufWriter` 在仓库里没有任何调用方，所以这次形状改变的代价只是 API 形式本身。

---

## 原文里那条成本记录，现在怎么看

原文写：

> 8 MB 流上，C 循环 0.01s，Xray 逐元素追加 0.17s，拼接构造 2.75s（二次方）。

那条测的是**读侧**——把读到的字节**累积**成一个数组。写侧不一样：drain loop 不累积
任何东西，常规文件上通常一次迭代就写完（`write(2)` 对常规文件不短写，除非信号或磁盘满）。
所以那 17× 不适用于本次迁移。

读侧的整文件读（`__readFile` / `__readFileBytes` / `__readStdin` /
`__readStdinBytes`）**仍然是叶子**，deletion_trigger 不变：`Array<T>` 现在只有逐元素
`push` 和会重新分配的 `concat`（`stdlib/types/array.xr:6,28`），没有摊还的批量追加，
那条二次方约束依然成立。

---

## 有意未改的文件

```
src/coro/**
src/runtime/**
src/ir/**
src/frontend/**
```

原文说「加一个 yieldable 叶子会触及协程和运行时边界，那是编译器归属」。
实际上没有触及：`vm_binding: "yieldable"` 是 `.def` 的声明属性，
`tools/stdlibgen/stdlibgen.py:1598` 只是把 `XRS_EXPORT` 换成 `XRS_EXPORT_YIELDABLE`，
对哪些叶子可以标它没有白名单。写侧的 uring 提交路径复用了读侧已有的
`xr_yield_for_uring_io`，全部代码都在 `stdlib/io/io.c` 里。
