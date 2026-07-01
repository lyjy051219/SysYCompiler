# Middle-end slow case analysis

分析范围：按运行时间 `>= 2s` 作为“较慢样例”，共 34 个。它们按 `.ll` 内容哈希可以归成 12 个热点组。`test.ipynb` 当前含 Git 冲突标记，不是合法 JSON；本报告以附件跑分文本和仓库内同名 `.sy.ll` 为准。

## 当前 pass pipeline

当前 pipeline 位于 `src/ir/opt/PassBuilder.cpp`：

- Early fixed point x2：`CFGSimplify -> Mem2Reg -> SCCP -> InstSimplify -> Reassociate -> LoopSimplify -> RangeAwareSimplify -> LoopSimplify -> ConstraintElimination -> GepCombine -> GepEvaluate -> DCE -> CFGSimplify`。
- IPO：`Local2Global -> TailRecursionElimination -> FuncInline -> Global2Local -> CFGSimplify -> Mem2Reg -> CFGSimplify -> DAE`，然后 Early fixed point x2。
- Memory/冗余：`StoreOnlyGlobalElimination -> IfConversion -> BreakCriticalEdges -> GVNPRE -> SCCP -> CFGSimplify -> LoadElimination -> DSE -> ADCE -> CFGSimplify`。
- Loop fixed point x3：`BreakCriticalEdges -> LoopSimplify -> LCSSA -> LoopRotate -> LICM -> LoopDeletion -> CFGSimplify -> DCE`。
- Late：`CFGSimplify -> InstSimplify -> DCE -> ConstantHoist -> UnifyFuncExits`。

几个和慢样例直接相关的缺口：

- `SCEV.cpp` / `SCEV.hpp` 为空，RangeAnalysis 也只是预留 SCEV 接口；因此没有稳定的 trip-count、归纳变量、地址递推、loop strength reduction。
- `LoadElimination` 只做有限深度前驱回溯，遇循环和嵌套 PHI 很容易保守放弃；内层循环里“同一地址已 store 过，随后循环中只被非别名地址写”的场景没吃到。
- `DSE` 遇到回到被分析 store 所在块的循环回边直接放弃，这对循环内数组临时值清理很保守。
- `IfConversion` 禁止 load/store/call/div/rem，并且候选指令数最多 4；对矩阵乘、bubble sort、深 if-store 链基本无能为力。
- `FuncInline` 识别到 `SafeToMemoize` 后只是拒绝内联，并没有真正 memoize；非尾递归热点仍然原样运行。
- 没有 Dead Function Elimination；内联后原函数常常还留在模块里，虽然主要影响代码体积/后端压力，不一定影响解释执行时间。

## 慢样例与对应 `.ll`

| Case | Status | Runtime | `.ll` | Group |
| --- | --- | ---: | --- | --- |
| `2024-BAV-3.sy` | AC | 17.160s | `testcases/final/h_performance/2024-BAV-3.sy.ll` | `8aeb6d2101` |
| `2024-NT1-4.sy` | AC | 16.449s | `testcases/final/h_performance/2024-NT1-4.sy.ll` | `ab548a98de` |
| `arm_h-11-01.sy` | AC | 10.684s | `testcases/performance_arm_25/arm_h-11-01.sy.ll` | `8aeb6d2101` |
| `2024-JVU-10.sy` | AC | 9.247s | `testcases/final/h_performance/2024-JVU-10.sy.ll` | `1df9d5808c` |
| `arm_h-1-01.sy` | AC | 8.794s | `testcases/performance_arm_25/arm_h-1-01.sy.ll` | `1df9d5808c` |
| `h-1-01.sy` | AC | 7.723s | `testcases/h_performance/h-1-01.sy.ll` | `1df9d5808c` |
| `arm_h-1-03.sy` | AC | 7.374s | `testcases/performance_arm_25/arm_h-1-03.sy.ll` | `1df9d5808c` |
| `2024-FAF-19.sy` | AC | 6.472s | `testcases/final/h_performance/2024-FAF-19.sy.ll` | `1df9d5808c` |
| `2024-TBR-19.sy` | AC | 5.059s | `testcases/final/performance/2024-TBR-19.sy.ll` | `f21408982e` |
| `2024-51P-30.sy` | WA | 4.844s | `testcases/final/performance/2024-51P-30.sy.ll` | `18bbbb0ac6` |
| `2024-U03-25.sy` | AC | 4.678s | `testcases/final/h_performance/2024-U03-25.sy.ll` | `8aeb6d2101` |
| `arm_matmul2.sy` | AC | 3.761s | `testcases/performance_arm_25/arm_matmul2.sy.ll` | `b1657727dc` |
| `h-1-03.sy` | AC | 3.585s | `testcases/h_performance/h-1-03.sy.ll` | `1df9d5808c` |
| `arm_fft1.sy` | AC | 3.458s | `testcases/performance_arm_25/arm_fft1.sy.ll` | `3cad09a77b` |
| `2024-U7W-16.sy` | AC | 3.132s | `testcases/final/performance/2024-U7W-16.sy.ll` | `3cad09a77b` |
| `2024-LW8-7.sy` | AC | 2.977s | `testcases/final/performance/2024-LW8-7.sy.ll` | `45764e7e7d` |
| `arm_matmul3.sy` | AC | 2.931s | `testcases/performance_arm_25/arm_matmul3.sy.ll` | `b1657727dc` |
| `arm_matmul1.sy` | AC | 2.923s | `testcases/performance_arm_25/arm_matmul1.sy.ll` | `b1657727dc` |
| `2024-U7D-6.sy` | AC | 2.922s | `testcases/final/performance/2024-U7D-6.sy.ll` | `b1657727dc` |
| `recursive_call_1.sy` | AC | 2.919s | `testcases/performance/recursive_call_1.sy.ll` | `1b31ea729d` |
| `2024-VRT-1.sy` | AC | 2.722s | `testcases/final/performance/2024-VRT-1.sy.ll` | `b1657727dc` |
| `2024-BJV-10.sy` | AC | 2.696s | `testcases/final/performance/2024-BJV-10.sy.ll` | `b1657727dc` |
| `2024-H17-1.sy` | AC | 2.643s | `testcases/final/h_performance/2024-H17-1.sy.ll` | `a16bd436b8` |
| `2024-C64-14.sy` | AC | 2.512s | `testcases/final/h_performance/2024-C64-14.sy.ll` | `59e0c83ed0` |
| `rv_fft1.sy` | AC | 2.506s | `testcases/performance_rv_25/rv_fft1.sy.ll` | `3cad09a77b` |
| `h-4-03.sy` | AC | 2.465s | `testcases/h_performance/h-4-03.sy.ll` | `84323f82d9` |
| `2024-1RK-26.sy` | AC | 2.455s | `testcases/final/performance/2024-1RK-26.sy.ll` | `3cad09a77b` |
| `2024-K0I-17.sy` | AC | 2.397s | `testcases/final/h_performance/2024-K0I-17.sy.ll` | `84323f82d9` |
| `arm_h-4-03.sy` | AC | 2.376s | `testcases/performance_arm_25/arm_h-4-03.sy.ll` | `84323f82d9` |
| `fft1.sy` | AC | 2.347s | `testcases/performance/fft1.sy.ll` | `3cad09a77b` |
| `fft2.sy` | AC | 2.301s | `testcases/performance/fft2.sy.ll` | `3cad09a77b` |
| `2024-MXP-16.sy` | AC | 2.138s | `testcases/final/h_performance/2024-MXP-16.sy.ll` | `84323f82d9` |
| `arm_h-10-02.sy` | AC | 2.039s | `testcases/performance_arm_25/arm_h-10-02.sy.ll` | `8aeb6d2101` |
| `2024-NKK-22.sy` | AC | 2.010s | `testcases/final/h_performance/2024-NKK-22.sy.ll` | `193897b3d8` |

## 分组分析

### Group `8aeb6d2101`: TRSM / 三角求解

覆盖：`2024-BAV-3.sy`, `arm_h-11-01.sy`, `2024-U03-25.sy`, `arm_h-10-02.sy`。

代表 IR：`testcases/final/h_performance/2024-BAV-3.sy.ll`。

现象：

- `trsm` 的内层循环在 `341-388`，`main` 又保留了一份内联后的 `trsm` 主体在 `473-508`。内联有效减少了 call，但原 `trsm` 函数仍然留在模块里。
- 内层循环里 `B[i][k]` 已在外层 store 完成，但每次 `j` 循环仍 `load` 一次：`trsm` 中 `357/372`，`main` 内联体中 `483/498`。这是典型的“循环内 store-to-load forwarding 被 alias 卡住”：循环里写 `B[j][k]`，但 `j` 从 `i+1` 开始，理论上和 `B[i][k]` 不别名；当前 BasicAA/RangeAnalysis 没有 SCEV 和索引范围事实，证明不了。
- GEP 数量偏高，三重循环每次都重算多维地址，缺少基址递推和 induction-variable strength reduction。

优化点：

- 做 range-aware GEP alias：在 LoopSimplify+LCSSA 后，用归纳变量事实证明 `j > i`，让 `B[j][k]` 与 `B[i][k]` NoAlias，进而让 LoadElim/LICM 消掉内层循环的重复 load。
- 加 `IndVarSimplify/LoopStrengthReduce`：把 `base + i*1024 + k` 这类地址改为循环递推指针，少算 GEP/乘法。
- 加 Dead Function Elimination：内联后 `trsm` 无调用者时可删，降低后端负担。

### Group `ab548a98de`: 非尾递归 Fibonacci

覆盖：`2024-NT1-4.sy`。

代表 IR：`testcases/final/h_performance/2024-NT1-4.sy.ll`。

现象：

- `fib` 是两个自递归调用的非尾递归，IR 在 `357-388`；这是运行时间主因，普通 TailRecursionElimination 无法处理。
- `rand` 已在 `main` 中被展开成全局 `seed` 的 load/store 序列，但原 `rand` 函数仍留在 `338-355`。
- `FuncInline` 里 `SafeToMemoize` 只是使函数“不内联”，没有真正做 memoization 或递归动态规划化。

优化点：

- 低风险：内联后跑 Dead Function Elimination；再跑一次 `Global2Local/Mem2Reg/SCCP`，尝试把只在 `main` 更新的 `seed` 标量化为 SSA 循环变量。
- 高收益但难：为“纯函数 + 小整数递归参数”做 memoization pass。此处 `fib(c,n,d)` 的 `n` 从循环变量来，`c/d` 也变化，收益取决于输入规模和参数域，需要很谨慎。
- 对 `% 2`, `% 256` 这类整数取模，在范围可证非负时替换为 `and`，或者在后端做 signed-rem-to-bitmask。

### Group `1df9d5808c`: Collatz / h-1

覆盖：`2024-JVU-10.sy`, `arm_h-1-01.sy`, `h-1-01.sy`, `arm_h-1-03.sy`, `2024-FAF-19.sy`, `h-1-03.sy`。

代表 IR：`testcases/final/h_performance/2024-JVU-10.sy.ll`。

现象：

- `fun` 的尾递归已经被变成循环，见 `339-373`，这是当前 pass 做得好的地方。
- `main` 中也有内联后的 Collatz 循环，见 `389-425`。
- `3*n+1` 在分支判断和进入分支后重复计算：`398-405`；`fun` 函数体里也有同样模式 `360-367`。
- `lim` 全局只由 `main` 初始化一次，但在热路径仍以 load 形式出现，见 `419-420`。

优化点：

- 补一个 very-local PRE/CSE：同一分支条件和 true 分支重复的 `mul/add` 应该复用。
- 让 `lim` 在 `main` 内标量化：`store @lim` 后无外部可见读写时，用 SSA 值替代后续 load。
- 对 Collatz 中 `%2` 做 bit-test 降低分支开销：需要 RangeAnalysis 或语义上证明被除数非负。

### Group `f21408982e`: 100000 元素 bubble sort

覆盖：`2024-TBR-19.sy`。

代表 IR：`testcases/final/performance/2024-TBR-19.sy.ll`。

现象：

- `array_init_and_process` 内联了初始化、bubble sort、binary search，见 `438-519`。
- bubble sort 比较阶段先 load `arr[j]` 和 `arr[j+1]`，进入 swap 分支后又重新 load 两次，见 `461-484`。这是 branch-local load forwarding/CSE 的明显机会。
- 算法本身是 O(n^2)，编译器不应指望把 bubble sort 变成更好算法；中端能做的是减少每次迭代的冗余访存和分支成本。

优化点：

- 在条件分支 true 块复用 predecessor 已经 load 出来的值，或者将小 swap if-convert 成条件选择/条件 store。
- `bubble_sort` 内联后原函数仍保留，应 Dead Function Elimination。
- 对 `i < n-1` 这类循环不变边界，做 LICM/hoist 后应只算一次；当前已有部分 hoist，但还不彻底。

### Group `18bbbb0ac6`: 深嵌套 if/store，且当前 WA

覆盖：`2024-51P-30.sy`。

代表 IR：`testcases/final/performance/2024-51P-30.sy.ll`。

现象：

- 这是唯一 `WA` 的慢样例，性能优化前应先定位正确性。
- `func` 和 `main` 都有 `[100 x i32]` 局部数组，IR 中保留 alloca，并生成 100 层左右 `if (i > k) s[k]=k` 的 store 链。`main` 的链在 `877-1225`，随后再用循环把 100 个元素 load 求和。
- 这类小固定数组非常适合 SROA：`s[0]..s[99]` 都是常量下标，拆成 100 个标量后，很多 store/load 能被 DCE/InstSimplify 清掉。

优化点：

- 优先实现 fixed-size local array SROA：只处理所有 GEP 下标均为常量、数组不逃逸的 alloca。这个 pass 对本样例收益巨大，而且比完整 scalar replacement 安全。
- 扩展 IfConversion 或新增 branch-to-select/store-chain fold：连续 `if (i > k) s[k]=k` 可以变成 `select` 后的标量赋值。
- 因为该样例 WA，建议先关闭对它相关形态最可疑的 aggressive pass 做二分：`IfConversion`, `StoreOnlyGlobalElimination`, `DSE`, `ADCE`。虽然 IfConversion 禁 store，但 CFG/PHI 改写仍可能影响深分支。

### Group `b1657727dc`: 1000x1000 矩阵乘/转置

覆盖：`arm_matmul2.sy`, `arm_matmul3.sy`, `arm_matmul1.sy`, `2024-U7D-6.sy`, `2024-VRT-1.sy`, `2024-BJV-10.sy`。

代表 IR：`testcases/performance_arm_25/arm_matmul2.sy.ll`。

现象：

- 热点三重循环在 `382-412`：先计算 `a[i][k] * b[k][j] % 2 == 0`，进入 true 分支后又重新 load `a[i][k]`、`b[k][j]` 并重新 `mul`，见 `385-405`。
- 后续 row-min、row-fill、transpose、sum 也有大量二维 GEP，见 `413-478`。

优化点：

- 做 “branch value forwarding”：把 `%110 = a*b` 在 true 分支复用，避免重复 load/mul。
- 做 loop strength reduction：二维数组访问应转成行基址 + 递推列指针。
- 若后端支持，添加 loop unroll/tiling 属于更大工程；中端短期可先做局部 CSE 和 GEP 递推。

### Group `3cad09a77b`: FFT / NTT

覆盖：`arm_fft1.sy`, `2024-U7W-16.sy`, `rv_fft1.sy`, `2024-1RK-26.sy`, `fft1.sy`, `fft2.sy`。

代表 IR：`testcases/performance_arm_25/arm_fft1.sy.ll`。

现象：

- `multiply` 是递归模乘，见 `344-394`，在 FFT 内大量调用。
- FFT butterfly 内同一轮对 `multiply(wn, y)` 调了两次，见 `501` 和 `507`。这是纯调用 CSE 的大机会。
- `main` 里 `d` 是全局变量，循环中 load/store，之后多次 load `@.G.d`，见 `559-615`；实际完全可变成局部 SSA。
- `power(d, mod-2)` 已经被移到归一化循环外，这是好的。

优化点：

- 给纯函数 call 做 CSE：被调函数 `NoModRef` 且参数相同，可以替换第二个 call。
- 对只在 `main` 内使用的全局标量 `d` 做 Global2Local/Promote 后清理。
- `multiply` 这种递归模乘若输入范围可控，可以考虑在后端用 64 位乘法取模替换；若目标架构没有 i64 则谨慎。

### Group `45764e7e7d`: Radix sort

覆盖：`2024-LW8-7.sy`。

代表 IR：`testcases/final/performance/2024-LW8-7.sy.ll`。

现象：

- `radixSort` 中 `head/tail/cnt` 是 `[16 x i32]` 局部数组，IR 保留 alloca 和大量 GEP/load/store，见 `384-637`。
- 源码中 `getNumPos(v, bitround)` 被多次调用；IR 内联后变成多个“小除以 16 循环”，例如 `445-607` 多段重复的 `sdiv 16`/`srem 16`。
- `ans` 是全局标量，在最终求和循环中每次 load/store，见 `653-660`。

优化点：

- 小定长数组 SROA：`head/tail/cnt` 长度 16，拆标量后可显著减少内存操作。
- Pure-call CSE 或 inline 后表达式 CSE：同一个 `getNumPos(t, bitround)` 在一轮循环里不应算多次。
- `x / 16`、`x % 16` 在可证非负时替换为 `ashr 4` / `and 15`。当前 RangeAnalysis 有能力做一部分，但缺少输入非负事实时会保守。
- `ans` 标量化为循环 phi，最后一次性 store/输出。

### Group `1b31ea729d`: 非尾递归 float 函数

覆盖：`recursive_call_1.sy`。

代表 IR：`testcases/performance/recursive_call_1.sy.ll`。

现象：

- `func` 中有两个递归调用，第二个调用的第一个参数依赖第一次调用结果，见 `355-370`，所以不是普通 CSE。
- `myabs` 函数在源码里未被调用，IR 中仍保留，且零路径返回 `undef`，见 `338-353`。

优化点：

- Dead Function Elimination 至少删除 `myabs`，避免后端处理无用函数。
- 更激进的递归优化需要语义推导或 memoization。因为参数含 float，通用 memoization 风险和成本都比整数递归高，不建议作为第一优先级。

### Group `a16bd436b8` / `59e0c83ed0`: 大外循环 + 局部数组 dot product

覆盖：`2024-H17-1.sy`、`2024-C64-14.sy`。

代表 IR：`testcases/final/h_performance/2024-H17-1.sy.ll` 和 `2024-C64-14.sy.ll`。

现象：

- `loop` 已经被内联进 `main`，原 `loop` 函数仍在 `338-358`。
- 外层 `COUNT=500000`，内部填充数组后做 dot product；但源码的 `j` 没在每次外层循环重置，IR 也把 `j` 作为外层 phi 保留下来，见 `367-439`。这意味着数组填充只在第一轮执行，后续大量迭代重复计算同一个 dot product。
- `COUNT` 是全局初值 `500000`，热路径仍 load `@.G.COUNT`，见 `364/441`。

优化点：

- Read-only scalar global propagation：`COUNT` 未被写，应直接常量化。
- Loop peeling + SCEV：剥离第一轮填充，证明后续内层填充循环不再执行，再把 dot product 结果视为循环不变量或至少减少外层重复工作。
- Dead Function Elimination 删除内联后残留的 `loop`。

### Group `84323f82d9`: min/max + 常数除法循环

覆盖：`h-4-03.sy`, `2024-K0I-17.sy`, `arm_h-4-03.sy`, `2024-MXP-16.sy`。

代表 IR：`testcases/h_performance/h-4-03.sy.ll`。

现象：

- `min` 已经被 if-convert 成 select，`f/loop_test` 也被内联，IR 质量相对好，见 `338-397` 和 `399-445`。
- 热循环中仍有 `sdiv 1000`、`srem 19491001/998244853`，常数除法/取模在目标机器上昂贵。

优化点：

- 增加 magic-number constant division/rem lowering（可放 IR pass，也可放 RISC-V/ARM 后端 ISel）。
- 对 `% mod` 的累加模式可考虑 bounded reduction：如果能证明加法结果范围小于 `2*mod`，用条件减法替换除法取模。

### Group `193897b3d8`: 卷积/池化/ sigmoid

覆盖：`2024-NKK-22.sy`。

代表 IR：`testcases/final/h_performance/2024-NKK-22.sy.ll`。

现象：

- `n=1000`, `ks=15`, `ps=4` 都是全局初值，但 `main` 仍 load 它们，见 `613-619/635-637`。
- `kernel_conv_pooling` 被完整内联进 `main`，原函数仍保留 `393-610`，`main` 中也有同构主体 `611-858`。
- `sigmoid/exp` 内联后存在明显的重复浮点乘：`x*x` 在 `516-535`、`764-783` 反复生成；同一个 `C[i][j]` 在 `516-519` / `764-767` load 两次。
- pooling 中 `max` 已经变成 `select`，见 `500-501` / `743-744`，这是好现象。

优化点：

- Read-only global constant propagation，把 `n/ks/ps` 常量化，后续 SCCP/LoopSimplify 会删掉大量不必要判断。
- Local CSE/GVN 应覆盖完全相同的 `fmul x,x` 和重复 load；如果 GVNPRE 出于浮点保守跳过，需要至少加一个 basic-block local CSE。
- Dead Function Elimination 删除内联后残留的 `kernel_conv_pooling/max/exp/sigmoid`。

## 建议的优化优先级

P0：先做低风险、覆盖多组的 pass。

- Dead Function Elimination：内联后删除未被 `main`/runtime 引用的普通函数。
- Read-only scalar global propagation：全模块扫描没有 store 的标量 global，替换 load 为初值常量。直接覆盖 `NKK(n/ks/ps)`, `H17/C64(COUNT)`, 一些 `const int` 被降成 global 的情况。
- Pure call CSE：被调函数 `NoModRef` 且参数相同，替换同块或支配路径上的重复 call。直接打 `FFT multiply(wn,y)`。
- Basic-block local CSE：先别上完整 GVN，先做一轮 map expression -> value，覆盖重复 `fmul x,x`、重复整数 `mul/add`。

P1：补中等工程量、性能收益很大的标量化。

- Fixed-size local array SROA：只处理不逃逸、所有 GEP 下标常量的小数组 alloca。先覆盖 `[16 x i32]` 和 `[100 x i32]`，对 `radixSort`、`51P` 很关键。
- Global scalar promotion after inline：对只在 `main` 或单函数内读写的 scalar global 建 SSA phi，最后需要时再 store。
- Range-aware GEP AA：把 loop induction 范围接进 BasicAA，先支持二维数组 `base[i][k]` vs `base[j][k]` 的 NoAlias 判定。

P2：循环专项。

- Minimal SCEV：识别 `phi(init, phi+step)`、trip count、非负范围。RangeAnalysis 已经有接口预留。
- LoopStrengthReduce / IndVarSimplify：把多维 GEP 和 `i*stride + j` 变成递推地址。
- Loop peeling/unswitch：对 `H17/C64` 这种第一轮改变状态、后续状态稳定的外循环有明显收益。

P3：递归专项。

- Tail recursion 当前已能处理 Collatz；继续保持。
- 对 `fib` 类纯整数递归可以做受限 memoization，但建议放在 correctness 稳定后，因为要考虑参数域、内存开销、递归深度和 IO 行为。

## 推荐 pipeline 调整

建议在不大改现有结构的前提下这样插：

1. Early cleanup 后、Inline 前：加入 `GlobalConstProp`，让 `n/ks/ps/COUNT` 尽早变常量。
2. Inline + DAE 后：加入 `DeadFunctionElimination -> GlobalScalarPromotion -> Mem2Reg -> SCCP -> InstSimplify -> DCE -> CFGSimplify`。
3. Heavy memory 前：加入 `SROA`，然后再跑 `Mem2Reg/LoadElimination/DSE`，否则数组 alloca 会挡住大多数标量优化。
4. Heavy memory 内部改为小 fixed point x2：`GVNPRE/LocalCSE -> SCCP -> CFGSimplify -> LoadElim -> DSE -> InstSimplify -> DCE`。不用无限迭代，2 次通常足够吃掉互相暴露的机会。
5. Loop pipeline 中在 `LoopRotate` 后插 `SCEV/IndVarSimplify/LoopStrengthReduce`，再跑 `LICM`。现在先 LICM 缺少 SCEV 事实，会错过 TRSM/matmul 的关键 load/GEP。
6. Loop pipeline 后再跑一次 `LocalCSE/GVNPRE -> LoadElim -> DSE -> ADCE`，因为 LICM/LSR/LoopDeletion 会暴露新的冗余。

最建议先实现的三个点：`GlobalConstProp + DeadFunctionElimination`、`PureCall/LocalCSE`、`小数组 SROA`。这三项工程风险最低，能直接覆盖本次慢样例中的大多数重复模式。
