# Xray vs Lua 性能基准测试

**作者：xingleixu@gmail.com**

本目录包含 10 个经典的语言性能基准测试，用于对比 Xray 和 Lua 的性能。

---

## 快速开始

```bash
# 运行所有测试（标准参数）
./run_benchmarks.sh

# 快速模式（较小参数，用于快速验证）
./run_benchmarks.sh --quick

# 运行单个测试
./run_benchmarks.sh binary_trees
```

---

## 10 个基准测试

| # | 测试名称 | 标准参数 | 快速参数 | 测试重点 |
|---|----------|----------|----------|----------|
| 1 | **binary_trees** | 16 | 12 | GC 压力、递归、对象创建 |
| 2 | **fannkuch_redux** | 10 | 9 | 数组操作、排列生成 |
| 3 | **fasta** | 2500000 | 25000 | 字符串生成、随机数 |
| 4 | **knucleotide** | - | - | 哈希表、字符串处理 |
| 5 | **mandelbrot** | 1000 | 200 | 浮点运算、位操作 |
| 6 | **nbody** | 5000000 | 50000 | 浮点运算、数组访问 |
| 7 | **pidigits** | 2000 | 500 | 大整数运算 |
| 8 | **regexredux** | - | - | 正则表达式性能 |
| 9 | **revcomp** | - | - | 字符串反转、I/O |
| 10 | **spectralnorm** | 500 | 100 | 矩阵运算、浮点精度 |

> 注：knucleotide、regexredux、revcomp 需要 fasta 生成的输入文件

---

## 测试详情

### 1. binary_trees - 二叉树
- **测试重点**：GC 压力测试、递归调用、对象创建与销毁
- **算法**：创建完全二叉树，计算节点校验和
- **参数**：深度（depth），标准 16，最大约 131072 个节点

### 2. fannkuch_redux - 排列翻转
- **测试重点**：数组操作、循环效率
- **算法**：生成 n! 个排列，计算最大翻转次数
- **参数**：n，标准 10（10! = 3628800 个排列）

### 3. fasta - DNA 序列生成
- **测试重点**：字符串生成、伪随机数
- **算法**：生成 FASTA 格式 DNA 序列
- **参数**：序列长度，标准 2500000

### 4. knucleotide - K-核苷酸频率
- **测试重点**：哈希表性能、字符串切片
- **算法**：统计 DNA 序列中各长度子串的出现频率
- **输入**：fasta 输出

### 5. mandelbrot - Mandelbrot 集
- **测试重点**：浮点运算、复数计算
- **算法**：计算 Mandelbrot 集，输出 PBM 图像
- **参数**：图像尺寸，标准 1000×1000

### 6. nbody - N-体问题
- **测试重点**：浮点运算、数组访问、数学函数
- **算法**：模拟太阳系 5 个天体运动
- **参数**：模拟步数，标准 5000000

### 7. pidigits - Pi 数字
- **测试重点**：大整数运算
- **算法**：GMP 风格流式算法计算 Pi 小数位
- **参数**：位数，标准 2000

### 8. regexredux - 正则替换
- **测试重点**：正则表达式引擎性能
- **算法**：DNA 序列的正则匹配与替换
- **输入**：fasta 输出

### 9. revcomp - 反向补码
- **测试重点**：字符串处理、I/O 性能
- **算法**：计算 DNA 序列的反向补码
- **输入**：fasta 输出

### 10. spectralnorm - 光谱范数
- **测试重点**：矩阵运算、浮点精度
- **算法**：计算矩阵的光谱范数（最大奇异值）
- **参数**：矩阵大小，标准 500×500

---

## 性能记录

测试结果会自动追加到 `benchmark_history.log` 文件，格式：

```
=== 2026-01-01 13:00:00 ===
binary_trees|16|0m1.234s|0m0.567s
fannkuch_redux|10|0m2.345s|0m1.234s
...
```

### 基准参考值（MacBook Pro M1, 2024-01）

| 测试项 | 参数 | Xray | Lua | 比率 |
|--------|------|------|-----|------|
| binary_trees | 16 | 1.2s | 0.8s | 1.5x |
| fannkuch_redux | 10 | 2.3s | 1.8s | 1.3x |
| fasta | 2500000 | 0.9s | 0.4s | 2.3x |
| mandelbrot | 1000 | 3.5s | 2.1s | 1.7x |
| nbody | 5000000 | 4.2s | 2.8s | 1.5x |
| pidigits | 2000 | 8.5s | 6.2s | 1.4x |
| spectralnorm | 500 | 2.8s | 1.9s | 1.5x |

> 注：以上为参考值，实际性能取决于硬件和 Xray 版本

---

## 性能回归检测

如果发现某项测试性能明显下降（>20%），请检查：

1. **GC 相关**：binary_trees 性能下降可能是 GC 策略变更
2. **字符串相关**：fasta/revcomp 性能下降可能是字符串操作优化问题
3. **数值计算**：nbody/spectralnorm 性能下降可能是浮点运算优化问题
4. **正则引擎**：regexredux 性能下降可能是正则库问题

---

## 文件列表

```
tests/benchmarks/
├── README.md              # 本文档
├── run_benchmarks.sh      # 自动化测试脚本
├── benchmark_history.log  # 历史性能记录
├── binary_trees.xr/.lua   # 二叉树测试
├── fannkuch_redux.xr/.lua # 排列翻转测试
├── fasta.xr/.lua          # DNA 序列生成
├── knucleotide.xr/.lua    # K-核苷酸频率
├── mandelbrot.xr/.lua     # Mandelbrot 集
├── nbody.xr/.lua          # N-体问题
├── pidigits.xr/.lua       # Pi 数字
├── regexredux.xr/.lua     # 正则替换
├── revcomp.xr/.lua        # 反向补码
└── spectralnorm.xr/.lua   # 光谱范数
```
