# Tensor RISC-V 性能基准

本目录用于逐项验证 tensor 优化是否在相同环境、相同输入上带来运行时间收益。测试源码全部以 `tensor` 运算为主要负载，当前全展开实现可以作为基线；后续每加入一个 pass，都应重新运行整套基准并与上一份通过正确性检查的 JSON 比较。

## 用例覆盖

| 用例 | 主要路径 | 静态规模 |
|---|---|---:|
| `01_elementwise_int_n64` | tensor-tensor 逐元素、右标量广播、整体赋值 | 64 |
| `02_broadcast_float_n64` | 浮点 tensor，标量在左右两侧广播 | 64 |
| `03_copy_return_int_n64` | tensor 形参、返回、sret 和复制 | 64 |
| `04_dot_int_k64` | 一维点积 | K=64 |
| `05_gemv_int_8x16` | 矩阵乘向量 | 8x16 |
| `06_vecmat_float_16x8` | 浮点向量乘矩阵 | 16x8 |
| `07_gemm_int_6x6x6` | 通用矩阵乘法 | 6x6 @ 6x6 |
| `08_batch_gemm_int_2x3x6x3` | equal-or-one batch 广播矩阵乘法 | 2x3x6 @ 1x6x3 |

规模参考了现有 testcases 中常见的数组与矩阵程序，但第一版静态 shape 有意控制在当前全展开基线可以编译的范围内；运行轮数负责把动态工作量放大到百万级标量操作。没有把任何测试名称、输入内容或特定结果用于编译器决策。运行轮数来自 `.in`，避免为扩大运行时间而复制源码或进一步放大全展开 IR。

每个用例在计时区间内改变至少一个运行期输入，并在计时区间后输出校验和。runner 对每次预热和正式运行都同时检查 stdout 与 `main` 返回码，任何错误结果都不会进入性能统计。

## 运行方法

默认使用已有的 `build-riscv/compiler`，如果不存在则使用 `build-riscv-release/compiler`：

```bash
python3 tensor-benchmark/run.py --label baseline
```

指定编译器、预热一次并正式运行七次：

```bash
python3 tensor-benchmark/run.py \
  --compiler /path/to/compiler \
  --warmup 1 \
  --repeat 7 \
  --label tensor-elementwise
```

只运行部分用例：

```bash
python3 tensor-benchmark/run.py --case elementwise --case broadcast
python3 tensor-benchmark/run.py --case gemm
```

显式保存报告并与基线比较：

```bash
python3 tensor-benchmark/run.py \
  --compiler /path/to/new/compiler \
  --json results/new.json \
  --csv results/new.csv \
  --compare-json results/baseline.json
```

加速比定义为 `baseline median / current median`，大于 1 表示新版本更快。跨用例汇总使用几何平均，不把编译时间或代码体积混入运行时间指标。

## 计时口径

- 程序只用 `starttime()`/`stoptime()` 包围待测负载。
- runner 只解析运行库输出的 `Timer@...`，不会用整个编译、链接、QEMU 启动的墙钟时间冒充目标程序时间。
- 先执行 `--warmup` 次，再对 `--repeat` 次正式结果取中位数，同时保存全部原始样本、最小值、最大值和相对标准差。
- 编译、汇编、链接时间以及 IR、汇编和目标文件大小只作为诊断数据，不参与 pass 是否过关的主要结论。
- QEMU user-mode 适合功能验证和同一宿主机上的指令执行相对比较，但不能模拟 BOOM v3 的缓存、乱序、分支预测、50MHz 主频和内存系统。候选 pass 最终仍需在指定目标硬件复验。

## Pass 过关规则

建议每个 pass 单独构建编译器并执行：

1. 当前全展开版本保存为不可变基线。
2. 新 pass 必须让所有用例通过输出与返回码检查。
3. 至少进行一次预热和五次正式运行，以中位数作为主指标。
4. 先看与该 pass 直接相关的用例，再检查整套测试的几何平均和最坏回退。
5. 如果在通用成本模型的合理参数范围内始终没有稳定正收益，就回退该 pass，而不是增加用例名称、输入值或固定尺寸特判。
6. QEMU 上接近计时噪声的变化不算过关；在目标板复验后再决定是否默认启用。

所有阈值只能依赖 shape、元素类型、操作成本、布局、别名、寄存器压力和缓存工作集等通用静态性质。`cases/` 和 `manifest.json` 只属于离线测试基础设施，不能被生产编译器读取。

## 已记录的全展开基线

基线使用提交 `ef3fb21c4f56d1b1cb51180fd8a6ac4cc3ef0b7c` 期间构建的 Release 全展开编译器，二进制 SHA-256 为 `0869ee219287150968b5c9fe8998bb2c88e1fecd7c984416e76c766f27da6f6b`。环境是 QEMU 6.2.0、`-cpu rv64`、RISC-V GCC 11.4.0；每项预热 2 次并正式运行 9 次。

| 用例 | 编译时间 | 目标计时中位数 | 最小值 | 最大值 |
|---|---:|---:|---:|---:|
| elementwise int | 5.699 s | 45.743 ms | 40.475 ms | 56.889 ms |
| broadcast float | 3.857 s | 74.855 ms | 68.317 ms | 91.956 ms |
| copy/return int | 3.781 s | 22.830 ms | 21.904 ms | 26.484 ms |
| dot int | 2.688 s | 17.973 ms | 17.878 ms | 20.154 ms |
| GEMV int | 2.566 s | 35.345 ms | 32.348 ms | 39.498 ms |
| vecmat float | 3.048 s | 97.501 ms | 89.254 ms | 118.830 ms |
| GEMM int | 2.625 s | 30.750 ms | 29.648 ms | 40.393 ms |
| batch GEMM int | 1.570 s | 34.825 ms | 32.568 ms | 43.167 ms |

完整原始样本、编译器身份和环境信息保存在 `results/baseline-release-full-expand-ef3fb21.json`，相同数据的扁平表格保存在同名 CSV。浮点用例在 QEMU 上的离散程度高于整型用例，比较相关 pass 时应增加 `--repeat` 并在目标硬件复验。

## 规范标量循环基线过关结果

规范循环候选编译器的 SHA-256 为 `f704f72572968d5a0e32052230cf22bb926b7293ac75fc8856280b871170b1ae`。普通 runner 以 2 次预热、11 次正式运行测得几何平均加速 `1.1718x`，原始样本保存在 `results/tensor-loop-final.json` 和同名 CSV。

为降低短 QEMU 进程的时间漂移，又将全展开与规范循环二进制固定到 CPU8，逐用例执行 21 组交错的 `ABBA`/`BAAB` 配对测试。总计 704 次执行均通过固定输出、返回码和 `Timer@` 校验，配对中位数结果如下：

| 用例 | 全展开 / 规范循环 |
|---|---:|
| elementwise int | 1.4716x |
| broadcast float | 1.0427x |
| copy/return int | 1.5879x |
| dot int | 1.1787x |
| GEMV int | 0.9845x |
| vecmat float | 1.0556x |
| GEMM int | 1.0140x |
| batch GEMM int | 1.1583x |
| 几何平均 | 1.1697x |

GEMV 在这组短区间 QEMU 测试中回退约 1.55%，其余七项为正收益。GEMV 和仅提升约 1.40% 的小型 GEMM 都需要在后续 tensor 矩阵优化中继续处理，并在真实 RV64 目标机上复验；当前结果只证明规范标量循环基线整体优于全展开实现。
