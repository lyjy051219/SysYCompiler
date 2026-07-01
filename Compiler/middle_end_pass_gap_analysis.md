# 中端 Pass 缺口与扩展建议

本文基于当前 `PassBuilder.cpp` pipeline、`src/ir/opt` 已有 pass，以及前一份慢样例 `.ll` 分析整理。
目标是把“已有 pass 已经做了什么”“为什么对慢例收益不够”“还应该补哪些 pass”分开说清楚。

## 0. 总体结论

当前中端不是缺少基础清理，而是缺少能吃掉热点循环和内存访问的证明能力。
已有 pipeline 已经覆盖了 SSA、CFG 清理、SCCP、简单 GVN/PRE、LoadElim、DSE、LICM、TailRec、Inline、IfConversion、Range 简化等基础能力。

慢例的共同瓶颈主要集中在 7 类：

1. **没有真正实现 SCEV**：`SCEV.cpp` 和 `SCEV.hpp` 目前是空文件，`RangeAnalysis` 里只是预留接口；循环 trip count、归纳变量、地址递推都推不出来。
2. **别名分析不够循环敏感**：`LICM`、`LoadElimination`、`DSE`、`GVNPRE` 碰到 GEP/数组/循环写入时会保守放弃。
3. **缺少 SROA / 小数组标量替换**：`[16 x i32]`、小型临时数组、局部 bucket 仍然以内存形态存在，`Mem2Reg` 吃不到。
4. **缺少全局常量/只读全局传播**：`n/ks/ps/d/COUNT/lim/seed` 这类全局标量很多没有变成 SSA 常量，导致 SCCP、LoopUnroll、LoopUnSwitch、Range 都被卡住。
5. **缺少纯函数调用 CSE / 函数属性推断**：FFT 的 `multiply(wn, y)`、radix 的 `getNumPos`、卷积中的小函数调用即使纯，也常重复计算。
6. **循环优化链条太短**：目前只有 LoopSimplify、LCSSA、LoopRotate、LICM、LoopDeletion，没有 IndVarSimplify、LSR、Unroll、Unswitch、Vectorize 等真正面向热点循环的 pass。
7. **除 2 的幂外的除法/取模优化不足**：`RangeAwareSimplify` 能做部分 `x / 2^k` 和 `x % 2^k`，但不能做一般常数除法、常数取模的 magic-number strength reduction。

建议优先级：

1. P0：`SCEVAnalysis`、`ReadOnlyGlobal/GlobalConstProp`、`DeadFunctionElimination`、`EarlyCSE/LocalCSE`。
2. P1：`LoopAliasAnalysis`、`IndVarSimplify`、`LoopStrengthReducePass`、`SROA`、`PureCallCSE`、`ConstantDivRemStrengthReduction`。
3. P2：`LoopUnroll`、`LoopUnSwitch`、`LoopPeeling/LoopVersioning`、`MemorySSA/MemoryDependence`。
4. P3：`LoopTiling/Blocking`、`LoopInterchange`、`Vector/SLP/LoopVectorize`。
5. P4：`LoopParallel`，收益可能很大，但依赖运行时、评测环境和并发语义，风险最高。

## 1. 当前已有 pass 与 pipeline

当前 O1/O2 共用主要 pipeline：

```text
Early cleanup fixed point x2:
  CFGSimplify
  Mem2Reg / Promote
  SCCP
  InstSimplify
  Reassociate
  LoopSimplify
  RangeAwareSimplify
  LoopSimplify
  ConstraintElimination
  GepCombine
  GepEvaluate
  DCE
  CFGSimplify

Module / IPO:
  Local2Global
  TailRecursionElimination
  FuncInline(enableRecursiveInline=true)
  Global2Local
  CFGSimplify
  Mem2Reg / Promote
  CFGSimplify
  DAE
  Early cleanup fixed point x2
  StoreOnlyGlobalElimination
  IfConversion

Heavy memory:
  BreakCriticalEdges
  GVNPRE
  SCCP
  CFGSimplify
  LoadElimination
  DSE
  ADCE
  CFGSimplify

Loop fixed point x3:
  BreakCriticalEdges
  LoopSimplify
  LCSSA
  LoopRotate
  LICM
  LoopDeletion
  CFGSimplify
  DCE

Late:
  CFGSimplify
  InstSimplify
  DCE
  ConstantHoist
  UnifyFuncExits
```

当前注册的函数分析：

```text
DominatorTree
BasicAA
LoopInfo
SideEffect
PostDominatorTree
RangeAnalysis
```

缺口很明确：没有 `SCEVAnalysis`，没有 MemorySSA，没有 FunctionAttrs/PureFunction 属性分析，没有模块级 GlobalDCE/IPSCCP。

## 2. 已有 pass 的收益不足点

### 2.1 CFG / SSA / 规范化类

| pass | 当前作用 | 对慢例收益不好的地方 | 影响样例 | 优化方向 |
|---|---|---|---|---|
| `CFGSimplify` | 删除不可达块、合并空块、折叠简单分支 | 能清 CFG 噪声，但不理解循环 trip count 和内存依赖 | 几乎所有慢例 | 保留；配合 SCEV、Unroll、Unswitch 后再跑，收益会变大 |
| `Mem2Reg/Promote` | 把普通 alloca 标量提升到 SSA | 只处理可提升标量 alloca；数组、小 bucket、GEP 后内存无法提升 | radix sort、51P、NKK、H17/C64 | 在 Mem2Reg 前加入 SROA/ScalarReplacement |
| `LoopSimplify` | 建 preheader、规范化 latch/exits | 只规范结构，不提供 trip count/归纳变量语义 | 所有循环慢例 | 作为 SCEV、LICM、LSR、Unroll 前置保留 |
| `LCSSA` | 循环出口值转 LCSSA | 本身不提速，是循环变换前置 | 所有循环慢例 | 保留；后续 LoopUnroll/Unswitch/Vector 都需要 |
| `BreakCriticalEdges` | 拆 critical edge | 主要服务 PRE/LCSSA/PHI，单独性能收益很小 | 所有控制流复杂慢例 | 保留；避免过度重复运行造成 IR 变大 |
| `UnifyFuncExits` | 统一函数出口 | 主要帮助后端；对热点循环无直接收益 | 递归/多返回样例 | 保留在 late |

### 2.2 标量化、常量传播、范围推理类

| pass | 当前作用 | 对慢例收益不好的地方 | 影响样例 | 优化方向 |
|---|---|---|---|---|
| `SCCP` | 稀疏条件常量传播，删除死分支 | 对全局只读标量和循环 PHI 的能力有限 | NKK、FFT、H17/C64、Collatz | 加 `IPSCCP` 和 `ReadOnlyGlobalConstProp` |
| `InstSimplify` | 局部代数折叠、冗余指令简化 | 更偏 peephole，跨块/跨循环重复计算吃不掉 | matmul、Collatz、NKK | 加 EarlyCSE 和 PureCallCSE |
| `Reassociate` | 重结合，暴露常量折叠/CSE | 对浮点、内存、循环地址帮助有限 | matmul、NKK、FFT | 保留；LSR/Vector 前后可再跑 |
| `RangeAwareSimplify` | 利用 range/knownbits 简化比较、select、部分 div/rem | 对非 2 的幂常数除法/取模支持不足；没有 SCEV 输入 | min/max+除法组、radix、NKK | 接入 SCEV；新增常数除法/取模 strength reduction |
| `ConstraintElimination` | 利用支配路径上的分支约束删除冗余判断 | 主要处理整数比较和简单 affine 条件，不处理内存/调用/复杂循环关系 | 51P、Collatz、NKK | 保留；结合 BranchValuePropagation 提升分支内表达式折叠 |

### 2.3 GVN / 内存冗余 / GEP 类

| pass | 当前作用 | 对慢例收益不好的地方 | 影响样例 | 优化方向 |
|---|---|---|---|---|
| `GVNPRE` | 局部/全局值编号和部分冗余消除 | 函数过大时跳过；内存表达依赖 BasicAA；backward scan 有限制；不能替代 MemorySSA | NKK、51P、matmul、TRSM | 加 EarlyCSE 降低压力；用 LoopAA/MemorySSA 增强内存等价 |
| `LoadElimination` | 消除可证明重复 load | 搜索深度有限；遇到 loop、call、MayAlias 容易放弃 | TBR bubble、TRSM、matmul、FFT | 加 LoopAliasAnalysis、MemoryDependence/MemorySSA、store-to-load forwarding |
| `DSE` | 删除被覆盖且不再读取的 store | block budget 有限；非局部指针/global/param 上很保守 | 51P、radix、NKK、TBR | 加 MemorySSA；对局部小数组先 SROA |
| `GepCombine/GepEvaluate` | 合并 GEP 链、折叠常量 GEP、局部提取 | 只做局部 GEP 形态优化，不做循环地址递推 | TRSM、matmul、NKK、radix | 加 SCEV + LSR，把 `base + i*stride + j` 变成递增指针 |
| `StoreOnlyGlobalElimination` | 删除只写不读全局 store | 不是 GlobalOpt，不会把只读全局标量变常量，也不会删死函数 | NKK、FFT、Collatz、H17/C64 | 加 GlobalOpt / ReadOnlyGlobalConstProp / GlobalDCE |

### 2.4 过程间优化和全局变量类

| pass | 当前作用 | 对慢例收益不好的地方 | 影响样例 | 优化方向 |
|---|---|---|---|---|
| `FuncInline` | 内联普通函数，带递归内联保护 | 对非尾递归和纯递归只是不乱内联；不会 memoize；大函数/多 callsite 受阈值限制 | 2024-NT1-4、recursive_call_1、FFT/NKK 小函数 | 加 FunctionAttrs、PureCallCSE、GlobalDCE；递归 memoization 只能作为专项优化 |
| `TailRecursionElimination` | 处理尾递归和部分 accumulator 形式 | Collatz 这类有收益；非尾递归 fib/float recursion 基本无力 | Collatz 已受益；NT1/recursive_call_1 不足 | 保留；可补 RecursionToLoop 专项和纯递归 memoization |
| `DAE` | 删除死参数 | 内联后有帮助，但不删死函数体 | 多数含 helper 函数慢例 | 加 DeadFunctionElimination |
| `Local2Global` | 大 alloca 变 global，避免栈压力 | 可能让后续内存优化更难，因为大局部数组变成全局对象 | H17/C64、NKK、51P | 只对真正超过栈限制的大数组做；小数组先给 SROA |
| `Global2Local` | 小全局在 main 内可安全转局部 | 只覆盖 main 内安全全局；循环内读写/跨函数/复杂 use 会放弃 | NKK、FFT、Collatz、H17/C64 | 加 ReadOnlyGlobalConstProp 和 GlobalScalarPromotion |

### 2.5 循环优化类

| pass | 当前作用 | 对慢例收益不好的地方 | 影响样例 | 优化方向 |
|---|---|---|---|---|
| `LoopRotate` | 把部分 while/for 旋转为更适合优化的形态 | 条件严格：单 exit、header exiting、latch 无条件回 header 等；复杂循环常跳过 | NKK、51P、H17/C64 | 先补 SCEV/IndVarSimplify；降低结构限制前要加验证 |
| `LICM` | 外提循环不变量和纯调用 | load 外提需要证明循环内没有 mod 同一地址；没有 LoopAA 时非常保守 | TRSM、matmul、FFT、NKK | 加 LoopAliasAnalysis、MemorySSA、FunctionAttrs |
| `LoopDeletion` | 删除不可达/无副作用循环 | SCEV 回边次数检查是预留注释，当前无法基于 trip count 删除 | H17/C64、空循环/初始化循环 | 实现 SCEV 后补回 backedge count 判断 |
| `IfConversion` | 小菱形分支转 select | 禁止 LOAD/STORE/CALL/DIV/REM，候选指令数 >4 放弃，且要求汇合块有 PHI | TBR、matmul、51P、NKK | 保留当前安全版本；另加 BranchValuePropagation、PredicatedStoreElim 专项 |
| `ConstantHoist` | 晚期大整数/浮点常量提升 | 主要减代码体积/寄存器装载；对热点循环算法性收益有限 | 全局普遍 | 后端若已经能合并常量池，应评估是否放 late 或按目标机选择 |

## 3. 慢例对应的主要 pass 短板

### 3.1 TRSM 组

涉及样例：

```text
2024-BAV-3.sy
arm_h-11-01.sy
2024-U03-25.sy
arm_h-10-02.sy
```

主要问题：

- 内层循环中反复计算二维数组地址。
- `B[i][k]` 一类循环不变量 load 难以被 `LICM` 外提，因为当前 AA 证明不了循环内 store 不会修改它。
- GEP 形态虽然有 `GepCombine`，但没有 SCEV/LSR，地址递推仍然是乘加/GEP 反复算。

现有 pass 不足：

- `LICM` 没有 LoopAA 支撑。
- `LoadElimination` 无法跨循环稳定消除。
- `GepCombine` 只做局部表达式，不做循环归纳地址优化。

建议：

1. `SCEVAnalysis` 识别 `i/k/j` 的 trip count 和数组下标 affine 形式。
2. `LoopAliasAnalysis` 证明 `B[i][k]` 与循环内写入地址不 alias。
3. `LoopStrengthReducePass` 把二维 GEP 变成递增指针。
4. 若矩阵规模固定，再考虑 `LoopTiling/Blocking`。

### 3.2 非尾递归组

涉及样例：

```text
2024-NT1-4.sy
recursive_call_1.sy
```

主要问题：

- `fib(n-1)+fib(n-2)` 和 float 非尾递归不是 TailRec 能处理的形态。
- `FuncInline` 对这种递归做了保护是正确的，但性能不会因此变好。

现有 pass 不足：

- `TailRecursionElimination` 只适合尾递归/accumulator 形式。
- 没有纯函数 memoization，也没有特殊递推识别。
- 没有 DeadFunctionElimination 删除无用 helper。

建议：

1. 先加 `DeadFunctionElimination`，删掉不再可达的函数体，减少代码体积。
2. 可选加 `PureRecursiveMemoization`，只对无副作用、参数小整数、返回值纯函数、递归规模可控的函数启用。
3. 这类 pass 风险较高，不应排在 SCEV/LoopAA/LSR 前面。

### 3.3 Collatz 组

涉及样例：

```text
2024-JVU-10.sy
arm_h-1-01.sy
h-1-01.sy
arm_h-1-03.sy
2024-FAF-19.sy
h-1-03.sy
```

主要问题：

- Tail recursion 已经基本处理。
- 热点中仍有全局 `lim`/边界值重复 load，以及 `3*n+1`、奇偶判断相关表达式重复。

现有 pass 不足：

- `Global2Local`/`StoreOnlyGlobalElimination` 不能把只读全局标量稳定传播成常量/SSA。
- `InstSimplify` 是局部 peephole，跨块重复表达式不稳定。

建议：

1. `ReadOnlyGlobalConstProp` / `GlobalScalarPromotion`。
2. `EarlyCSE/LocalCSE`。
3. `BranchValuePropagation`：在 `n % 2 == 0` 分支内传播奇偶事实，减少重复判断。

### 3.4 Bubble sort / compare-swap 组

涉及样例：

```text
2024-TBR-19.sy
```

主要问题：

- compare-swap 模式中 `arr[j]`、`arr[j+1]` 会在比较和交换路径重复 load。
- 分支里有 load/store，当前 `IfConversion` 明确禁止转换。

现有 pass 不足：

- `LoadElimination` 对循环、store、alias 组合保守。
- `IfConversion` 只能处理非常小、无内存访问、带 PHI 的菱形。

建议：

1. `MemorySSA` 或更强 `MemoryDependenceAnalysis`。
2. `StoreToLoadForwarding`。
3. 专项 `CompareSwapCanonicalization`：把 compare-swap 识别为 min/max/select/store 序列。

### 3.5 Matmul / TRMM / numeric kernel 组

涉及样例：

```text
01_mm1.sy
2024-8F9-2.sy
2024-NVD-12.sy
2024-GVN-14.sy
matrix_multiply.sy
```

主要问题：

- 内层循环重复加载 `a[i][k]`、`b[k][j]`，重复计算 `a*b`。
- 地址计算是典型 affine GEP，但当前没有 SCEV/LSR。
- 某些奇偶/条件路径导致同一值在分支两侧重复算。

现有 pass 不足：

- `GVNPRE` 能消一部分标量冗余，但内存和循环地址不够。
- `LICM` 因 alias 不足，不能可靠外提 invariant load。
- 没有 tiling/vector，缓存局部性差。

建议：

1. P0/P1：`SCEVAnalysis` + `LoopAliasAnalysis` + `LoopStrengthReducePass`。
2. P1：`EarlyCSE` 消掉同块重复 `a*b`。
3. P2/P3：`LoopInterchange`、`LoopTiling/Blocking`、`Vector`，这类对矩阵类收益最大。

### 3.6 FFT / 纯函数重复调用组

涉及样例：

```text
fft0.sy
fft1.sy
fft2.sy
```

主要问题：

- `multiply(wn, y)` 一类纯函数调用会重复出现。
- `d`、`wn` 等全局/循环不变量传播不足。

现有 pass 不足：

- `SideEffectAnalysis` 已有，但没有完整 FunctionAttrs/PureFunction 结果供 CSE/LICM 使用。
- `GVNPRE` 对 call 的处理较保守。

建议：

1. `FunctionAttrs/PureFunctionAnalysis`：标记 readonly/readnone/nocapture。
2. `PureCallCSE`：同一支配路径上相同实参的纯调用复用。
3. `LICM` 接入函数属性，把循环不变纯调用外提。

### 3.7 Radix sort / 小数组 bucket 组

涉及样例：

```text
2024-GGN-14.sy
```

主要问题：

- 局部 `[16 x i32]` bucket/count 数组频繁 load/store。
- `getNumPos`、`/16`、`%16` 相关计算重复。

现有 pass 不足：

- `Mem2Reg` 不能直接提升数组元素。
- `RangeAwareSimplify` 只能处理部分 2 的幂模式，不能全局重写所有常数 div/rem。
- `FuncInline` 后没有 PureCallCSE。

建议：

1. `SROA` 把 `[16 x i32]` 拆成 16 个标量或小 SSA slot。
2. `LoopUnroll` 对固定 16 次循环展开。
3. `ConstantDivRemStrengthReduction` 把 `/16`、`%16` 稳定变成 shift/and；一般常数用 magic number。
4. `PureCallCSE` 消重复 `getNumPos`。

### 3.8 H17 / C64 / 全局循环控制组

涉及样例：

```text
H17-LoopIf-6.sy
C64-LoopIf.sy
```

主要问题：

- `COUNT` 等全局标量反复 load。
- 某些初始化/填充只在首轮有意义，但后续循环仍保留复杂控制。

现有 pass 不足：

- `LoopDeletion` 预留了 SCEV backedge count，但当前不能用。
- `SCCP` 没有足够的全局常量传播。
- 没有 LoopPeeling 来单独处理第一轮。

建议：

1. `ReadOnlyGlobalConstProp`。
2. `SCEVAnalysis` + `LoopDeletion` 完整回边次数判断。
3. `LoopPeeling`：剥离第一轮后，后续循环的条件更容易 SCCP/CFG 简化。

### 3.9 min/max + 常数除法组

主要问题：

- select/min/max 优化已经有一定效果。
- 运行时间仍被常数除法/取模拖住。

现有 pass 不足：

- 没有一般常数除法/取模 lowering。

建议：

1. `ConstantDivRemStrengthReduction`：
   - `x / C` 转 magic multiply + shift。
   - `x % C` 转 `x - (x/C)*C`，其中 `x/C` 用 magic lowering。
   - 继续保留 `C=2^k` 的 shift/and 快路径。

### 3.10 NKK conv/pooling/sigmoid 组

涉及样例：

```text
2024-NKK-22.sy
```

主要问题：

- `n/ks/ps` 这类维度全局值没有稳定常量化。
- kernel/pooling 小固定循环没有展开。
- 卷积地址和累加形式是典型 affine loop nest，但没有 SCEV/LSR/vector/tiling。

现有 pass 不足：

- `SCCP` 吃不到全局只读维度。
- `LoopUnroll` 缺失，固定小循环仍有分支/计数开销。
- `Vector`/`Tiling` 缺失，内存局部性和 SIMD 都没有利用。

建议：

1. `ReadOnlyGlobalConstProp` / `IPSCCP`。
2. `LoopUnroll` 展开 `ks`、`ps` 等小固定 trip count。
3. `SCEV + LSR` 优化 GEP。
4. 后续做 `LoopTiling/Vector`。

## 4. 用户列出的新增 pass 分析

### 4.1 `SCEVAnalysis`

作用：

- 分析循环归纳变量、初值、步长、trip count、backedge taken count。
- 识别表达式是否是 affine recurrence，例如 `{base,+,step}`。
- 给 RangeAnalysis、LoopDeletion、LSR、Unroll、LoopAA 提供统一事实。

为什么优先级最高：

- 你的代码里已经有 SCEV 的空文件和 RangeAnalysis 的 SCEV 接口占位。
- `LoopDeletion.cpp` 也已经注释预留了 SCEV 回边次数判断。
- 说明设计上已经打算接入，只是还没实现。

主要收益：

- TRSM/matmul/NKK：识别数组下标 affine，支撑 LSR/LoopAA。
- radix：识别固定 16 次循环，支撑 Unroll。
- H17/C64：识别循环是否会执行、执行多少次，支撑 LoopDeletion/Peeling。
- RangeAwareSimplify：循环 PHI range 更准，比较/取模/边界判断更容易被删。

建议实现范围：

1. 先支持整数 PHI induction：`phi(init, next)`，其中 `next = phi + const` 或 `phi - const`。
2. 支持简单 exit condition：`iv < C`、`iv <= C`、`iv != C`。
3. 支持从 GEP 下标构造 affine expression。
4. 把结果挂到 `RangeInfo::LoopInductions` 或单独 `SCEVResult`。

推荐优先级：P0。

### 4.2 `LoopAliasAnalysis`

作用：

- 在单个循环内基于 SCEV、BasicAA、RangeAnalysis 判断两个内存访问是否可能 alias。
- 重点处理数组/GEP：不同 base、不同常量维度、不同 induction 范围、不同 offset 的 noalias。

主要收益：

- `LICM` 可以外提循环不变 load。
- `LoadElimination` 可以跨迭代/跨块复用 load。
- `DSE` 可以更大胆删除被覆盖 store。
- `LoopVectorize`、`LoopParallel`、`LoopUnroll` 都需要依赖它证明安全性。

影响样例：

- TRSM：证明 `B[i][k]` 不被内层 store 改写。
- matmul：证明不同矩阵 base 或不同下标范围不冲突。
- NKK：证明 input/kernel/output 访问分离。
- TBR：证明 compare-swap 的两个相邻元素和其他 store 的关系。

建议：

1. 先实现 loop-local `NoAlias` 查询，不急着替代 BasicAA。
2. 只对同一 base 的 GEP 做 affine interval disjoint 判断。
3. 对不同 global/alloca base 直接 NoAlias。
4. 对未知 call 保守，纯函数/readnone 函数可忽略 mod。

推荐优先级：P0/P1。

### 4.3 `LoopStrengthReducePass`

作用：

- 把循环中的乘法、GEP、`i * stride + base` 变成递增变量。
- 典型转换：每轮重新算 `base + i * 400 + j * 4`，变成 preheader 算初值，latch 中 `ptr += stride`。

主要收益：

- 降低内层循环整数乘法、加法、GEP 指令数。
- 降低寄存器压力和后端地址计算压力。
- 给后端生成更好的 load/store 地址模式。

影响样例：

- TRSM、matmul、NKK：二维/多维数组地址计算密集。
- radix：固定 stride bucket 访问。
- H17/C64：大数组线性扫描。

依赖：

- SCEVAnalysis。
- LoopSimplify + LCSSA。
- BasicAA/LoopAA 用于不改变内存语义。

建议：

- `IndVarSimplify` 和 `LSR` 分开做更清晰。
- `IndVarSimplify` 先统一 canonical IV；`LSR` 再做地址和强度削减。

推荐优先级：P1。

### 4.4 `LoopUnroll`

作用：

- 对小 trip count 或热点短循环复制循环体，减少分支/归纳变量开销，并暴露 CSE/constant folding。

主要收益：

- 固定小循环：`ks=15`、`ps=4`、radix bucket 16、部分卷积 kernel。
- 展开后 `SCCP/InstSimplify/GVN` 更容易消掉索引计算和边界判断。
- 有利于后续 vector/SLP。

影响样例：

- radix sort：16 桶循环。
- NKK：卷积 kernel/pooling 固定小循环。
- FFT：部分 butterfly 小循环可能受益。
- matmul：只建议小因子 partial unroll，不能盲目完全展开。

风险：

- 代码体积暴涨。
- 寄存器压力增加。
- 如果后端寄存器分配弱，过度 unroll 会变慢。

建议策略：

1. trip count <= 4：默认完全展开。
2. trip count <= 16 且循环体小：完全或 partial unroll。
3. 大循环只做 factor 2/4 partial unroll。
4. 展开后立即跑 `InstSimplify + DCE + CFGSimplify + EarlyCSE`。

推荐优先级：P1/P2。

### 4.5 `LoopUnSwitch`

作用：

- 把循环内与循环无关的分支搬到循环外，复制循环体。
- 典型转换：

```text
for (...) {
  if (invariant_cond) A else B
}

if (invariant_cond) {
  for (...) A
} else {
  for (...) B
}
```

主要收益：

- 删除循环内重复判断。
- 分支外提后，每个版本更容易 LICM/GVN/Vector。

影响样例：

- NKK：如果 `n/ks/ps` 等全局维度先常量化，很多循环内边界条件可变成 invariant。
- H17/C64：全局/外层不变量条件剥离后可简化。
- 部分 51P/LoopIf：循环内不变量分支可拆版本。

注意：

- matmul 中按 `k` 或元素值变化的分支不是 unswitch 目标。
- 只有在 `ReadOnlyGlobalConstProp` 和 SCEV 后，才能发现更多 invariant condition。
- 会复制代码，必须有 size threshold。

推荐优先级：P2。

### 4.6 `Vector`

作用：

- 包括 LoopVectorize 和 SLP Vectorize。
- LoopVectorize 把连续迭代打包成 SIMD。
- SLP 把同一基本块内的同构标量表达式打包。

主要收益：

- matmul、NKK conv、H17/C64 数组扫描、min/max、部分 FFT 都可能显著受益。
- 对浮点累加、整数 min/max、连续 load/store 尤其有价值。

依赖：

- 后端必须支持向量 IR 或目标机 SIMD 指令。
- 需要 LoopAA/SCEV 证明内存访问连续且无依赖。
- 需要处理 remainder loop。

建议：

1. 若后端暂时没有向量指令，先做 SLP-like 标量打包意义不大。
2. 可以先实现 target-independent 的 loop metadata/识别，但真正收益要等后端。
3. 对 SysY 类测试，优先做简单连续 int/float load-store/reduction vector。

推荐优先级：P3。

### 4.7 `LoopParallel`

作用：

- 识别无循环依赖的循环，把迭代分发到多线程或并行 runtime。

理论收益：

- matmul、conv、独立数组初始化/扫描可能非常大。

现实风险：

- 需要线程 runtime、同步、任务划分、目标平台 ABI 支持。
- 评测环境可能不允许 pthread 或多线程不稳定。
- 浮点 reduction 并行化会改变结合顺序，可能影响精度。
- 编译器课程项目里性价比通常不如 SCEV/LSR/SROA/Unroll。

建议：

- 放在后期。
- 先只处理无副作用、无 reduction、无跨迭代写冲突的 `parallel for`。
- 没有 runtime 支撑前，不建议进入默认 O2 pipeline。

推荐优先级：P4。

## 5. 除用户列出的 pass 外，建议新增的 pass

### 5.1 `DeadFunctionElimination` / `GlobalDCE`

作用：

- 从 `main` 和外部可见函数出发，删除不可达函数。
- 删除未使用全局变量和未引用 helper。

收益：

- 减少 `.ll` 中大量已内联但残留的函数体。
- 降低后续 pass 编译时间和后端压力。
- 对 runtime 不一定巨大，但对代码体积和编译时间稳定有收益。

影响样例：

- FFT、NKK、NT1、recursive_call_1、Collatz 组都有 helper 函数残留可能。

放置位置：

- `FuncInline + DAE + cleanup` 之后跑一次。
- late 前再跑一次。

优先级：P0。

### 5.2 `ReadOnlyGlobalConstProp` / `GlobalOpt`

作用：

- 找出初始化后不再写的 global scalar。
- 把 load global 替换为常量或入口处 SSA 值。
- 对只在 `main` 初始化一次、之后只读的 global 做 scalar promotion。

收益：

- 解锁 SCCP、Range、LoopUnroll、LoopUnSwitch、LICM。
- 删除热点循环里的 global load。

影响样例：

- NKK：`n/ks/ps`。
- FFT：`d`。
- Collatz：`lim`。
- H17/C64：`COUNT`。
- 其他 benchmark 中的 seed/size/config 全局。

注意：

- 必须结合 call mod/ref 和地址逃逸分析。
- 数组 global 不要轻易整体常量化，先从 scalar global 做起。

放置位置：

- Early cleanup 后。
- Inline 后再跑一次。

优先级：P0。

### 5.3 `IPSCCP`

作用：

- 跨函数传播常量实参、常量返回值、全局只读事实。

收益：

- 小 helper 内联前就能简化。
- 对函数调用链较多的 NKK/FFT/radix 有用。

影响样例：

- NKK：维度参数。
- FFT：常量 flag / direction。
- radix：base/pos 参数。

优先级：P1。

### 5.4 `SROA` / `ScalarReplacementOfAggregates`

作用：

- 把小数组、小聚合拆成独立标量 alloca，再交给 Mem2Reg。

收益：

- 小数组 load/store 变 SSA。
- 后续 SCCP、GVN、DCE、Unroll 的效果会明显提升。

影响样例：

- radix sort 的 `[16 x i32]` bucket/count。
- 51P 中小局部数组/状态数组。
- 部分 NKK 临时小 buffer。

实现建议：

1. 第一阶段只处理常量下标访问的小数组。
2. 数组元素个数 <= 32 或 <= 64。
3. 有动态 GEP、地址逃逸、传参给未知函数则放弃。

放置位置：

- `Mem2Reg` 前。
- Inline 后再跑一次。

优先级：P1，实际收益接近 P0。

### 5.5 `EarlyCSE` / `LocalCSE`

作用：

- 在基本块内或支配树路径上消除重复纯表达式、重复 GEP、重复 load。

收益：

- 比 GVNPRE 更轻，更适合在多个阶段反复跑。
- 能提前减小 IR，让 GVNPRE/LICM 压力变低。

影响样例：

- Collatz：重复 `3*n+1` 和奇偶相关表达式。
- matmul：重复 `a*b`。
- TBR：同块重复 load/GEP。
- NKK：重复地址计算。

建议：

- 第一版做 local block CSE：Binary/Cmp/GEP/Cast/纯 Call。
- load CSE 只在中间无 alias store 的情况下做。

放置位置：

- InstSimplify 后。
- Inline 后。
- Unroll 后。

优先级：P0/P1。

### 5.6 `FunctionAttrs` / `PureFunctionAnalysis`

作用：

- 推断函数属性：readnone、readonly、writeonly、nocapture、willreturn、norecurse 等。

收益：

- `LICM` 可外提纯调用。
- `GVNPRE/EarlyCSE` 可复用纯调用结果。
- `DSE/LoadElim` 可更准处理 call。

影响样例：

- FFT：`multiply`。
- radix：`getNumPos`。
- NKK：小 math/helper。

优先级：P1。

### 5.7 `PureCallCSE`

作用：

- 对相同 callee、相同实参、无副作用/只读且中间无 clobber 的调用做 CSE。

收益：

- 消除 FFT/radix/NKK 中重复 helper 调用。

依赖：

- FunctionAttrs/SideEffectAnalysis。
- 对 readonly 函数还需要判断参数指向内存是否被中间 store 改写。

优先级：P1。

### 5.8 `MemorySSA` / `MemoryDependenceAnalysis`

作用：

- 给 load/store/call 建立 MemoryDef/MemoryUse 链。
- 替代大量 backward scan 和 block budget。

收益：

- `LoadElimination`、`DSE`、`LICM` 都会更稳。
- 对 TBR、TRSM、matmul、NKK 这种内存密集样例非常关键。

成本：

- 实现量较大。
- 需要 pass invalidate/preserve 管理。

建议：

- 如果时间有限，先做简化版 MemoryDependence：
  - 单函数。
  - 基于支配树。
  - 针对 alloca/global/GEP base 的最近 def 查询。

优先级：P2，但它是后续高级内存优化的地基。

### 5.9 `ConstantDivRemStrengthReduction`

作用：

- 对常数除法、常数取模做 strength reduction。
- 2 的幂走 shift/and。
- 非 2 的幂走 magic multiply + shift。

收益：

- 减少后端慢速 div/rem 指令。
- 对大量整数 benchmark 直接有效。

影响样例：

- min/max + constant division 组。
- radix。
- Collatz 中部分 `%2` 已可优化，但更一般的 `%C` 仍需要。
- NKK/其他索引计算。

放置位置：

- InstSimplify/RangeAwareSimplify 后。
- Late codegen 前。

优先级：P1。

### 5.10 `IndVarSimplify`

作用：

- 统一循环归纳变量，删除冗余 IV，规范 exit compare。
- 把多个等价 induction 合并到 canonical IV。

收益：

- 给 LSR、Unroll、LoopDeletion、Vector 提供更干净的循环形态。
- 删除重复 `i+1`、`i*stride`、边界比较。

影响样例：

- 所有 nested loop：TRSM、matmul、NKK、H17/C64、radix。

依赖：

- SCEV。

优先级：P1。

### 5.11 `BranchValuePropagation`

作用：

- 在 `if (cond)` 的 true/false 分支内传播 `cond` 已知为真/假。
- 把分支内重复比较、重复 select、部分 range 判断删掉。

收益：

- 对控制流密集测试比 IfConversion 更安全，因为不强行投机 load/store。

影响样例：

- Collatz：奇偶分支。
- 51P：大量嵌套 if。
- H17/C64：LoopIf 类。

优先级：P2。

### 5.12 `LoopPeeling` / `LoopVersioning`

作用：

- LoopPeeling：剥离前几轮或最后几轮循环。
- LoopVersioning：根据 runtime/invariant condition 生成 fast path 和 slow path。

收益：

- 第一轮初始化特殊、后续规律稳定的循环会变简单。
- 可以配合 LoopAA：运行时检查 noalias 后走优化版本。

影响样例：

- H17/C64：首轮特殊控制。
- NKK：边界区和内部区分离。
- matmul/conv：noalias fast path。

优先级：P2。

### 5.13 `LoopTiling/Blocking`

作用：

- 重排循环分块，提高缓存局部性。

收益：

- 对矩阵乘、卷积、TRSM 这种 memory-bound kernel 可能比单纯 CSE 更大。

影响样例：

- matmul 组。
- NKK conv。
- TRSM 组。

风险：

- 实现复杂。
- 要非常小心数组边界、别名和循环顺序依赖。

优先级：P3。

### 5.14 `LoopInterchange`

作用：

- 交换嵌套循环顺序，让内层访问更连续。

收益：

- 改善 cache locality。
- 给 vector 提供连续访问。

影响样例：

- matmul。
- NKK conv。
- 部分二维数组扫描。

依赖：

- DependenceAnalysis / LoopAA / SCEV。

优先级：P3。

### 5.15 `LoopIdiomRecognize`

作用：

- 识别 memset/memcpy、数组清零、线性拷贝、简单 reduction 等 idiom。

收益：

- 数组初始化、清零、复制循环可替换成更高效 runtime 或后端模式。

影响样例：

- radix bucket 清零。
- NKK 临时 buffer 清零。
- H17/C64 初始化循环。

优先级：P2/P3，取决于后端/runtime 支持。

## 6. 推荐的新 pipeline

### 6.1 短期可实现版本

```text
Early:
  SROA
  Mem2Reg
  SCCP
  InstSimplify
  EarlyCSE
  Reassociate
  RangeAwareSimplify
  ConstraintElimination
  GepCombine/GepEvaluate
  DCE/CFGSimplify

IPO:
  FunctionAttrs
  ReadOnlyGlobalConstProp / GlobalOpt
  TailRecursionElimination
  FuncInline
  SROA
  Mem2Reg
  IPSCCP
  DAE
  DeadFunctionElimination
  Early cleanup

Memory:
  EarlyCSE
  GVNPRE
  LoadElimination
  DSE
  ADCE

Loop:
  LoopSimplify
  LCSSA
  SCEVAnalysis
  IndVarSimplify
  LoopRotate
  LoopAliasAnalysis
  LICM
  LoopDeletion
  LoopStrengthReduce
  small LoopUnroll
  cleanup

Late:
  InstSimplify
  ConstantDivRemStrengthReduction
  DCE
  DeadFunctionElimination
  ConstantHoist
  UnifyFuncExits
```

### 6.2 中期增强版本

```text
Loop:
  LoopSimplify
  LCSSA
  SCEVAnalysis
  IndVarSimplify
  LoopUnSwitch(size-limited)
  LICM(with LoopAA/MemorySSA)
  LoopPeeling/Versioning
  LoopStrengthReduce
  LoopUnroll
  LoopIdiomRecognize
  cleanup

Vector-ready:
  LoopVectorize
  SLPVectorize
  cleanup
```

### 6.3 不建议马上进入默认 pipeline 的 pass

```text
LoopParallel
LoopTiling/Blocking
LoopInterchange
Full MemorySSA
Pure recursive memoization
```

这些 pass 潜在收益大，但实现和正确性风险也大。建议先把 SCEV、LoopAA、SROA、GlobalOpt、EarlyCSE 做稳，再考虑它们。

## 7. 最建议先做的 8 个任务

1. 实现 `DeadFunctionElimination`，内联/DAE 后删除不可达 helper。
2. 实现 `ReadOnlyGlobalConstProp`，先只支持 global scalar。
3. 实现 `EarlyCSE/LocalCSE`，先处理 Binary/Cmp/GEP/Cast/纯 Call。
4. 实现最小版 `SCEVAnalysis`：canonical IV、常量 step、简单 exit compare、trip count。
5. 把 `RangeAnalysis` 接上 SCEV 的 induction range。
6. 实现 `SROA`，先处理常量下标小数组。
7. 实现 `LoopStrengthReducePass`，先优化 GEP/乘法地址递推。
8. 实现 `ConstantDivRemStrengthReduction`，优先处理 int 常数除法/取模。

这 8 个完成后，再加入 `LoopAliasAnalysis`、`LoopUnroll`、`LoopUnSwitch`，收益会更稳定。

## 8. 对用户列出 pass 的最终排序

```text
P0  SCEVAnalysis
P0  LoopAliasAnalysis
P1  LoopStrengthReducePass
P1  LoopUnroll
P2  LoopUnSwitch
P3  Vector
P4  LoopParallel
```

如果把“用户没列出但应该加”的也放进总排序：

```text
P0  DeadFunctionElimination
P0  ReadOnlyGlobalConstProp / GlobalOpt
P0  EarlyCSE / LocalCSE
P0  SCEVAnalysis
P1  SROA
P1  LoopAliasAnalysis
P1  IndVarSimplify
P1  LoopStrengthReducePass
P1  ConstantDivRemStrengthReduction
P1  FunctionAttrs + PureCallCSE
P2  LoopUnroll
P2  LoopUnSwitch
P2  BranchValuePropagation
P2  LoopPeeling / LoopVersioning
P2  MemoryDependence / simplified MemorySSA
P3  LoopIdiomRecognize
P3  LoopTiling / Blocking
P3  LoopInterchange
P3  Vector
P4  LoopParallel
```

最关键的一句话：先补“证明能力”和“小而稳的通用优化”，再补激进循环变换。否则 Unroll、Unswitch、Vector、Parallel 很多时候会因为 alias、trip count、global 常量不足而跑不动，或者跑了但代码变大、收益不稳定。
