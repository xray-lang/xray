# xray Fuzzing Tests

本目录包含使用 [libFuzzer](https://llvm.org/docs/LibFuzzer.html) 的模糊测试工具，用于发现 lexer、parser 和纯 Xray stdlib 数据解析器中的潜在 bug。

## 种子语料库

`corpus/` 目录包含精心设计的种子文件，覆盖 xray 语言的所有语法特性：

### Lexer 语料库 (`corpus/lexer/`) - 7 个文件
| 文件 | 覆盖内容 |
|------|----------|
| `01_integers.xr` | 整数字面量（十进制、十六进制、二进制、八进制、BigInt）|
| `02_floats.xr` | 浮点数字面量（基本、科学计数法、下划线分隔）|
| `03_strings.xr` | 字符串字面量（转义序列、Unicode、模板字符串）|
| `04_operators.xr` | 所有运算符（算术、比较、逻辑、位运算、赋值）|
| `05_keywords.xr` | 所有关键字（var、const、fn、class、if、for 等）|
| `06_edge_cases.xr` | 边界情况（空字符串、Unicode 标识符、极值）|
| `07_regex.xr` | 正则表达式字面量（/pattern/flags）|

### Parser 语料库 (`corpus/parser/`) - 20 个文件
| 文件 | 覆盖内容 |
|------|----------|
| `01_variables.xr` | 变量声明、类型注解、解构赋值 |
| `02_control_flow.xr` | if/else、while、for、for-in、match 表达式 |
| `03_functions.xr` | 函数声明、箭头函数、闭包、高阶函数、递归 |
| `04_collections.xr` | Array、Map、Set、对象字面量及其方法 |
| `05_classes.xr` | 类、继承、接口、getter/setter、运算符重载 |
| `06_enum_typeof.xr` | 枚举声明、typeof 运算符 |
| `07_exceptions.xr` | try-catch-finally、throw |
| `08_coroutines.xr` | go、await、Channel、select、defer、shared |
| `09_modules.xr` | import、export 语句 |
| `10_advanced.xr` | 类型别名、可选链、展开运算符、复杂表达式 |
| `11_edge_cases.xr` | 边界情况（空结构、深层嵌套、尾随逗号）|
| `12_stdlib_usage.xr` | 标准库模块使用示例 |
| `13_real_world.xr` | 真实世界示例（HTTP 服务器、并发模式）|
| `14_multi_value.xr` | 多值返回、元组解构 (`var (a, b) = f()`) |
| `15_slice_scope.xr` | 切片表达式 (`arr[1:3]`)、scope 块 |
| `16_attributes.xr` | 函数属性/装饰器 (`@test`) |
| `17_type_check.xr` | is 类型检查表达式 (`x is int`) |
| `18_coroutine_advanced.xr` | yield、cancelled()、`await any`、协程取消 |
| `19_generics.xr` | 泛型函数、泛型类、泛型 Channel |
| `20_abstract.xr` | 抽象类、抽象方法 |

### Stdlib 数据解析器语料库 (`corpus/stdlib_data/`) - 4 个文件

这些语料的首字节选择目标解析器，剩余字节作为输入文本：

| 首字节 | 目标 |
|--------|------|
| `0` | `csv.parseReport` |
| `1` | `toml.parseReport` |
| `2` | `xml.parseDetailed` |
| `3` | `yaml.parseAll` |

## 构建

Fuzzing 需要 Clang 编译器（libFuzzer 是 LLVM 的一部分）：

```bash
# 配置 fuzzing 构建
cmake -S . -B build-fuzz -G Ninja \
  -DENABLE_FUZZING=ON \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++

# 构建 fuzzer
cmake --build build-fuzz --target fuzz_lexer fuzz_parser fuzz_stdlib_data fuzz_xtp_decode fuzz_xr_program_decode
```

## 运行

### Lexer Fuzzer

```bash
# 创建种子语料库（使用现有测试文件作为初始输入）
mkdir -p corpus/lexer
cp ../../tests/regression/**/*.xr corpus/lexer/ 2>/dev/null || true

# 运行 fuzzer
./build-fuzz/tests/fuzz/fuzz_lexer corpus/lexer -max_len=4096

# 限制运行时间（例如 1 小时）
./build-fuzz/tests/fuzz/fuzz_lexer corpus/lexer -max_len=4096 -max_total_time=3600
```

### Parser Fuzzer

```bash
# 创建种子语料库
mkdir -p corpus/parser
cp ../../tests/regression/**/*.xr corpus/parser/ 2>/dev/null || true

# 运行 fuzzer
./build-fuzz/tests/fuzz/fuzz_parser corpus/parser -max_len=4096
```

### Stdlib Data Parser Fuzzer

```bash
# 运行纯 Xray CSV/TOML/XML/YAML parser fuzzer
./build-fuzz/tests/fuzz/fuzz_stdlib_data corpus/stdlib_data -max_len=4096
```

### XTP Decoder and Whole-plan Verifier Fuzzer

`fuzz_xtp_decode` 对每个输入执行两条路径：原始字节进入 bounded decoder；另一路以同一输入选择一项确定性结构化 mutation，作用于当前 schema 生成的有效 XTP。后者逐项断言拒绝发生在 decoder 或 whole-plan materializer 的预期边界。运行时 loader 还必须保持输出 plan 为 `NULL`，因此无效 artifact 无法到达后续 provider、finalizer 或 entry 注册所需的 verified-plan 边界。

当前结构化矩阵覆盖 schema 版本、完整件与 section digest、semantic identity 与 plan fingerprint、section bounds/length/count/order、unknown instruction opcode、unknown runtime tag、非 canonical constant form，以及 total-row/verification-work hard budget。

`corpus/xtp/*.xtpseed` 是透明文本控制种子。命名的矩阵 seed 以稳定 mutation code 开头；文件名说明类别，fuzzer 对其他首字节按矩阵索引取模以保留随机探索。有效 XTP 在运行时由当前 builder 生成，不提交不透明二进制 artifact。

```bash
./build-fuzz/tests/fuzz/fuzz_xtp_decode corpus/xtp \
  -max_len=1048576 -max_total_time=3600
```

默认测试构建中的 `test_xtp_fuzz_entry` 会不依赖 libFuzzer 地逐项执行完整确定性 mutation matrix，适合快速回归；它不替代长时间 sanitizer fuzzing。

### XrProgram Decoder and Semantic Admission Fuzzer

`fuzz_xr_program_decode`串联296的bounded structural decoder与297的bounded semantic admission；任何
structurally accepted输入都必须可byte-identical re-encode并保持同一`ProgramId`，任何semantically
accepted输入还必须保留同一canonical bytes并可由bounded reference evaluator安全处理。fuzzer不会调用
生产VM或AOT，也不把随机输入命中率当作valid-program generator/evaluator law suite的替代品。

```bash
./build-fuzz/tests/fuzz/fuzz_xr_program_decode corpus/xr_program \
  -max_len=1048576 -max_total_time=3600
```

默认测试构建中的`test_xr_program_fuzz_entry`会对0–512 byte确定性hostile输入执行同一入口；
`test_xr_program_verify`另行覆盖全部active operation、typed mutation、metamorphic program与资源阶梯。

## 常用选项

| 选项 | 说明 |
|------|------|
| `-max_len=N` | 限制输入最大长度（字节）|
| `-max_total_time=N` | 限制总运行时间（秒）|
| `-jobs=N` | 并行 fuzzing 作业数 |
| `-workers=N` | 并行 worker 数 |
| `-dict=file` | 使用字典文件 |
| `-seed=N` | 随机种子（用于复现）|

## 重现崩溃

当 fuzzer 发现崩溃时，会在当前目录生成 `crash-<hash>` 文件：

```bash
# 重现崩溃
./build-fuzz/tests/fuzz/fuzz_lexer crash-abc123

# 使用 standalone 版本调试
./build-fuzz/tests/fuzz/fuzz_lexer_standalone crash-abc123
```

## 独立测试模式

不使用 libFuzzer 的独立测试版本，用于调试：

```bash
# 测试单个文件
./build-fuzz/tests/fuzz/fuzz_lexer_standalone test.xr

# 从 stdin 读取
echo "var x = 1" | ./build-fuzz/tests/fuzz/fuzz_lexer_standalone

# 测试一个 stdlib 数据解析语料
./build-fuzz/tests/fuzz/fuzz_stdlib_data_standalone corpus/stdlib_data/00_csv_basic.txt
```

## 语料库维护

Fuzzer 会自动将发现的有趣输入添加到语料库。定期清理和最小化语料库：

```bash
# 合并语料库（去重）
./build-fuzz/tests/fuzz/fuzz_lexer -merge=1 corpus/lexer corpus/lexer_new

# 最小化语料库
./build-fuzz/tests/fuzz/fuzz_lexer -minimize_crash=1 crash-file
```

## CI 集成

在 CI 中运行短时间 fuzzing 作为回归检测：

```bash
# 运行 60 秒 fuzzing
./fuzz_lexer corpus/lexer -max_total_time=60 -max_len=4096
./fuzz_parser corpus/parser -max_total_time=60 -max_len=4096
./fuzz_stdlib_data corpus/stdlib_data -max_total_time=60 -max_len=4096
```

## 相关资源

- [libFuzzer 文档](https://llvm.org/docs/LibFuzzer.html)
- [OSS-Fuzz](https://github.com/google/oss-fuzz) - Google 的持续 fuzzing 服务
- [AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html)
