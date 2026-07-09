# 中端 Pass 能力差距与决赛 SIMD/Parallel 规划

- 赛制技术方案中与中端有关的约束：初赛不启用 SIMD；决赛根据官方新增文档决定 SIMD/并行能力

## 1. 我们当前中端 pipeline 基线

和其他队伍相比，我们缺的是下面四类能力：

1. 循环和数组的专项模式识别。
2. 更稳定的 loop legality / dependence / access analysis。
3. SIMD/vector IR 和 target feature gate。
4. parallel/vector 的收益模型、变换骨架和 cleanup 链。

## 2. 当前还缺哪些中端能力

### 2.1 循环归纳变量规范化不足

我们已有 `SCEV` 和 `LoopStrengthReduction`，但缺少一个独立的 `IndVarSimplifyPass`。

建议新增：

```text
IndVarSimplifyPass
```

功能：

- 建立 canonical induction variable，例如 `i = 0; i < trip; ++i`。
- 合并等价 IV。
- 把多个 IV 改写成同一个 canonical IV 的线性表达。
- 尽量把 exit condition 规范成 `iv < bound` 或 `iv <= bound`。
- 为 unroll、LSR、parallel、loop vectorizer 提供稳定输入。

第一版支持模式：

```c
for (i = init; i < bound; i += c)
for (i = init; i <= bound; i += c)
for (i = init; i > bound; i -= c)
for (i = init; i >= bound; i -= c)
```

限制：

- 只处理单 latch、单 exit、step 为常量的整数循环。
- 不证明复杂 overflow/no-wrap。
- 无法证明 trip count 时不改。

### 2.2 LoopAccessAnalysis 缺失

我们有 `BasicAliasAnalysis`，但 parallel/vector 需要更细的循环访问分析。

建议新增：

```text
LoopAccessAnalysis
LoopDependenceAnalysis
AlignmentAnalysis
```

功能：

- 识别 `A[i]`、`A[i + c]`、`A[base + i * stride]`。
- 判断 stride 是否为 1。
- 判断相邻 load/store 是否连续。
- 判断两个访问是否 noalias。
- 判断是否存在 loop-carried dependence。
- 估计 base alignment 和 offset alignment。
- 给 SLP/LoopVectorizer/LoopParallel 提供统一接口。

需要回答的问题：

```text
getBase(ptr)
getAffineIndex(ptr, loop)
isConsecutiveAccess(a, b)
isStrideOne(ptr, loop)
mayAliasAcrossIterations(load/store)
hasLoopCarriedDependence(loop)
getAlignOnBase(ptr)
```

第一版支持模式：

```c
A[i]
A[i + k]
A[base + i]
A[base + i * c]
A[i][j]       // 内层 j stride-1
```

拒绝模式：

- 非 affine GEP。
- 指针从未知 call 返回。
- 指针经过复杂 phi/select。
- 可能逃逸的指针。
- 同一 base 的写读/写写且无法证明不同迭代不冲突。

### 2.3 Scalar recurrence 和闭式化不足

我们有 `LateStrengthReduce` 和 `LoopStrengthReduction`，但缺少专门把循环递推改成闭式表达的 pass。

建议新增：

```text
ScalarRecurrenceSimplifyPass
LoopClosedFormSimplifyPass
LoopSumReductionPass
ModLoopReductionPass
AddChainReductionPass
```

这些可以分阶段实现，不必一次做全。

#### ScalarRecurrenceSimplifyPass

功能：

- 识别循环中只由自己递推的 scalar phi。
- 如果最终值只在 exit 使用，则在 exit 用闭式表达替代。
- 删除原循环内递推或帮助后续 `LoopDeletion` 删除整个循环。

支持模式：

```c
x = init;
for (i = 0; i < n; ++i)
    x = x + c;
// x = init + n * c
```

```c
x = init;
for (i = 0; i < n; ++i)
    x = x - c;
// x = init - n * c
```

```c
x = init;
for (i = 0; i < n; ++i)
    x = (x + c) % m;
// 保守改成更少的 mod 或闭式 mod
```

#### LoopSumReductionPass

功能：

- 把整数求和循环替换成公式。

支持模式：

```c
sum = base;
for (i = 0; i < n; ++i)
    sum += i;
// sum = base + n * (n - 1) / 2
```

```c
sum = base;
for (i = l; i < r; i += s)
    sum += a * i + b;
```

限制：

- 先只做整数。
- 需要能证明 trip count。
- 浮点求和默认不做，因为重排会改变舍入。

#### ModLoopReductionPass

功能：

- 降低循环内 `%` 的频率。

支持模式：

```c
x = (x + c) % m
x = (x - c) % m
```

可替换为：

```c
x += c;
if (x >= m) x -= m;
```

或者在 exit 做闭式：

```c
x = (init + trip * c) % m;
```

#### AddChainReductionPass

功能：

- 合并同一路径上的加法链。
- 和 `Reassociate/InstSimplify/SCCP` 配合，暴露更多常量。

支持模式：

```text
(((x + c1) + c2) + c3) -> x + (c1 + c2 + c3)
(x + y) + x -> 2 * x + y
```

### 2.4 post-unroll cleanup 不够强

当前 `LoopUnroll` 后的 cleanup 主要是 CFG、SCCP、DCE、LatePhiCleanup。展开后会暴露大量跨副本机会，强队经常补一轮内存和局部 CSE。

建议新增或重组：

```text
PostUnrollMemoryCleanupPass
LocalCSEPass
StoreToLoadForwardingPass
SingleSourcePhiEliminationPass
```

建议 post-unroll 链：

```text
CFGSimplify
InstSimplify
SCCP
LocalCSE
LoadElimination
StoreToLoadForwarding
DSE
SingleSourcePhiElimination
DCE
CFGSimplify
LoopSimplify
LCSSA
```

第一版限制：

- 只做 block-local 或 dominator-local。
- 不跑完整 GVNPRE fixed point。
- 只清理当前函数，不做模块级扰动。

### 2.5 数组专项 pass 仍偏少

我们有 `LocalAggregateSimplify`、`GepCombine`、`GepEvaluate`，但和强队相比还缺：

```text
SROAPass
LocalArrayPromotionPass
OnlyWriteArrayEliminationPass
ArrayAccessNormalizePass
LoopGEPCombinePass
GEPFlattenPass
```

#### SROAPass / LocalArrayPromotionPass

功能：

- 把小型局部数组或结构拆成 scalar。
- 让后续 Mem2Reg 提升。

支持模式：

```c
int a[4];
a[0] = x;
a[1] = y;
return a[0] + a[1];
```

限制：

- 数组不逃逸。
- 下标常量或可枚举。
- 不传给未知函数。

#### OnlyWriteArrayEliminationPass

功能：

- 删除只写不读的数组和相关 store。

支持模式：

```c
int tmp[100];
for (...) tmp[i] = expr;
// tmp never read
```

限制：

- 局部数组优先。
- 全局数组必须证明不被 call 或其他函数读取。

#### ArrayAccessNormalizePass / LoopGEPCombinePass

功能：

- 把数组访问规范成 `base + affine index`。
- 合并循环内重复 GEP。
- 为 LoopAccessAnalysis、SIMD、parallel 提供统一表达。

支持模式：

```text
gep(gep(base, i), j) -> gep(base, i * stride + j)
gep(base, i + c)     -> base + IV-derived offset
```

### 2.6 Loop locality pass 缺失

决赛如果有矩阵、卷积、stencil 类性能用例，locality pass 会变重要。

建议新增候选：

```text
LoopInterchangePass
LoopFusionPass
LoopDistributionPass
LoopTilingPass
```

初赛不建议优先做完整版本。决赛可先做两个窄模式：

#### LoopInterchangePass

支持模式：

```c
for (i)
  for (j)
    A[j][i] = ...
```

目标：

- 让内层访问 stride-1。
- 给 vectorizer 创造连续 load/store。

限制：

- 完美嵌套。
- 单一矩阵主访问。
- 无 carried dependence。

#### LoopDistributionPass

支持模式：

```c
for (i) {
    A[i] = B[i] + C[i];
    scalar += D[i];
}
```

目标：

- 把可 vectorize 的 map 部分和 reduction/side-effect 部分拆开。

### 2.7 Function memoization / cache lookup 缺失

强队里有 `FuncMemorize`、`MemoizationPass`、`CacheLookUp` 这类功能。它不属于 SIMD/parallel，但属于中端特殊收益点。

建议新增：

```text
PureFunctionAnalysis
FunctionMemoizationPass
CacheLookupInsertionPass
FunctionSpecializationPass
```

支持模式：

```c
int f(int x, int y) {
    if (...) return ...
    return f(...) + f(...);
}
```

启用条件：

- 无 side effect。
- 参数数量少。
- 参数类型简单，优先 i32。
- 返回 i32/f32。
- 调用次数高或递归。
- cache 大小有硬上限。

风险：

- 不建议放初赛主线。
- parallel runtime 与 memoization runtime 可能互相影响。
- 需要 target/runtime gate。

## 3. 各队中端能力对比，只看中端

### 3.1 miniC-compiler

miniC 值得借鉴的中端点：

- `CacheLookUp` 代表纯函数缓存可以作为特殊性能 pass。

### 3.2 Gnalc

Gnalc 值得借鉴的中端点：

- `TargetAnalysis` 作为 feature gate，target 不支持就不运行 parallel/vector。
- `LoopAliasAnalysis` 给 parallel/vector 共享使用。
- `LoopParallel` 拆成 `analyzeParallelInfo` 和 `doParallelize` 两段。
- `Vectorizer` 是 Bottom-Up SLP，从连续 store chain 出发。
- Vectorizer 有 cost model、alignment rewrite、scheduler、external user extract。

### 3.3 EDUCG 分组里的代表队伍

| 队伍 | 中端特点 | 对我们的借鉴 |
| --- | --- | --- |
| sysc-2455 | 结构化控制流阶段、`Parallelizable`、`ColumnMajor`、`TidyMemory`、`ConstLoopUnroll`、`SCEV` | 借鉴 structured-to-flat pipeline、数组布局/内存整理、parallel 前置分析 |
| OhmCompiler | `ArrayFParamMem2Reg`、`FuncMemorizeV3`、`LoopMemoryLift`、`TripCountAnalyzer`、`VectorizationDetect` | 借鉴连续 load/store、vector reduction、mul-add store 检测 |
| GeeCeeCee | `LocalArrayLift`、`SROA`、`LoopGEPCombine`、`LoopInterchange`、`ModuloLoopOptimization`、`FunctionMemoization` | 借鉴数组提升、GEP 规范化、loop locality、memoization |
| LSCCLL | `BranchCSE`、`GEPStrengthReduce`、`TSCCP`、`GCM`、`SCEV`、`LoopFullUnroll`、`UnusedArrEliminator` | 借鉴 GEP/loop/full-unroll 后清理、数组删除 |
| SystemF | 固定点 cleanup、`InBlockMergeLoad`、`GlobalMergeLoad`、`GlobalRemoveStore`、`MoveCodeUp/Down`、`AdjustArrayLayout` | 借鉴局部/全局 load-store 合并和保守 code motion |
| SysYCompiler2025 | `LoopSumReduction`、`ModLoopReduction`、`AddChainReduction`、`RemoveOnlyWriteArray`、`ArrayElimination` | 借鉴竞赛数学模式和数组专项删除 |
| Gnalc | `LoopParallel`、`Vectorizer`、`LoopAA`、`TargetAnalysis` | 决赛 parallel/vector 的主要参考模型 |

## 4. miniC 的 LoopParallel 详细流程

miniC 的 `LoopParallel` 是一个中端 IR 变换 pass，不是泛化并行框架。它的特点是：限制很窄、改写直接、依赖运行库执行区间任务。

### 4.1 pass 入口

流程：

```text
LoopParallel::Run
  reject non-normal function
  only process main
  get dominance and LoopAnalysis
  fold trivial phi
  sort loops by dominance
  for each loop:
    if not processed and CanBeParallel(loop):
      ExtractLoopParallelBody(loop)
      MakeWorkThread(begin, end, body_call)
      return changed
```

关键限制：

- 只处理 `main`。
- inline 必须先完成。
- 每次只改一个 loop，改完返回，让 pass manager 后续重跑分析。
- 会先删除所有 incoming value 相同的平凡 phi。

这给我们的启发：

- 第一版 parallel 不要追求覆盖率。
- 一次变换一个 loop 更容易维护分析失效。
- parallel pass 前必须固定 loop form。

### 4.2 CanBeParallel 的合法性检查

miniC 的合法性检查包括：

```text
require Rotate attribute
compute LoopTrait
require compare op in <, >, <=, >=
constant trip count <= 100 skip
allow at most one non-IV phi
non-IV phi must be add reduction-like
no illegal control exit
no loop-internal value used outside except allowed ones
trip trait must be calculable
DependencyAnalysis must pass
```

它支持的循环控制形态：

```c
for (i = init; i < bound; i += step)
for (i = init; i <= bound; i += step)
for (i = init; i > bound; i -= step)
for (i = init; i >= bound; i -= step)
```

它对 reduction 的支持很窄：

```c
res = res + expr;
```

而且要求 reduction 不能同时既在循环内部复杂使用，又在循环外使用。

### 4.3 DependencyAnalysis

miniC 的依赖分析非常保守。

处理流程：

```text
for each inst in loop:
  if load/store:
    find pointer base
  if call with side effect:
    reject

for each base:
  if only load or only store:
    accept
  if load and store:
    require exactly one simple load-store update pattern
```

支持的特殊模式：

```c
A[i] = A[i] + x;
```

它会识别：

```text
ld = load ptr
bin = ld + change
store bin, ptr
```

然后改成 atomic add 形式。

对我们的启发：

- 第一版 `LoopDependenceAnalysis` 可以先保守到“同 base 写读直接拒绝”。
- 第二版再支持 `A[i] += x` 的 atomic 或 privatization。
- global scalar reduction 不建议一开始用每次迭代 atomic，最好用 per-thread local reduction。

### 4.4 ExtractLoopParallelBody

miniC 会创建一个新函数：

```text
Loop_Body(...)
```

做的事情：

1. 收集 loop body 外部用到的 value，变成 `Loop_Body` 参数。
2. 把原 loop header 里的 phi 移到 substitute block。
3. 原函数里创建 substitute block，调用 `Loop_Body`。
4. 修改 preheader 指向 substitute。
5. 修改 exit phi。
6. 把原 loop blocks 移到 `Loop_Body`。
7. latch 的 terminator 改成 return。

抽象后形态：

```text
original main:
  preheader -> substitute
  substitute:
    call Loop_Body(captured values)
    br exit

new function:
  Loop_Body(captures...)
    original loop blocks
    return reduction/result if any
```

### 4.5 MakeWorkThread

miniC 接着创建：

```text
Parallel_Target(begin, end, ...)
```

这个函数负责在 `[begin, end)` 或 `[begin, end]` 区间内执行原来的循环 body。

它会：

- 创建 entry、while_loop、exit。
- 用 `beg/end` 重建 IV phi。
- 如果有 reduction，初始化/保存结果。
- 把 capture 参数映射到新函数参数。
- 把 `Loop_Body` call 和 loop update/cmp 移进 while loop。
- 原 substitute block 插入对 `Parallel_Target` 的调用。

中端输出的核心形态可以抽象成：

```c
void Parallel_Target(int beg, int end, Env env) {
    for (i = beg; i < end; ++i) {
        Loop_Body(i, env);
    }
}

main:
    parallel_runtime_call(begin, end, Parallel_Target, env);
```

### 4.6 miniC 方案的优缺点

优点：

- 工程上能跑通。
- 形态限制窄，正确性相对容易守住。
- 能处理简单 add reduction 和 `A[i] += x`。
- pass manager 中插入位置合理：loop 已经 rotate/simplify。

缺点：

- 只处理 `main`。
- 捕获参数和 reduction 处理偏 ad hoc。
- 对嵌套、递归、重入支持差。
- 依赖运行库和线程环境。
- 依赖分析以 base 为主，容易错过可并行循环，也不适合复杂 alias。

## 5. Gnalc 的 LoopParallel 详细流程

Gnalc 的 parallel pass 比 miniC 更体系化，建议作为我们决赛设计蓝本。

### 5.1 pass 启用条件

入口逻辑：

```text
LoopParallelPass::run
  require function has ExecuteExactlyOnce
  reject ParallelBody
  require target supports ParallelForEntry
  fold PHI
  get LoopAnalysis / SCEV / LoopAliasAnalysis
  skip nested parallelized loops
  skip unrolled remainder loops
  skip loop depth too large
  analyzeParallelInfo
  doParallelize
```

启发：

- 必须有 `TargetFeatureAnalysis`。
- 必须避免嵌套 parallel。
- parallel body 不能再被 parallel。
- pass 需要给每个失败原因打 debug log。

### 5.2 analyzeParallelInfo

Gnalc 将合法性分析单独整理成 `ParallelLoopInfo`：

```text
ParallelLoopInfo:
  header / preheader / latch / exit
  iv / iv_base / iv_bound
  compare kind
  reductions
  scalar global vars
  local arrays
```

主要检查：

- single exit。
- header exiting。
- exit condition 是 `icmp slt/sle`。
- IV 是 header phi。
- IV step 必须是 1。
- bound 不能在 loop 内定义。
- SCEV 能给出 affine add recurrence。
- trip count 小于阈值跳过。
- 不允许 sylib call、recursive call、未知 side-effect call。
- LoopAA 必须证明内存跨迭代独立。

支持的 reduction：

```text
ADD
MUL
AND
OR
XOR
FADD
FMUL
```

其中 float reduction 是选项控制，默认应谨慎。

支持的 scalar global 模式：

```c
g = g op inc;
```

要求能找到：

```text
load g
bin = load op inc
store bin, g
```

### 5.3 doParallelize

Gnalc 的变换分成几个动作：

1. reduction 改写成 global variable。
2. 在 preheader 初始化 reduction global。
3. 在 body 末尾调用 atomic intrinsic 合并。
4. local array 必要时提升成 global。
5. 新建 body function：

```c
void body(int beg, int end)
```

6. 收集 shared values。
7. 原 preheader 中存 shared values，并调用：

```text
ParallelForEntry(iv_base, iv_bound, body_fn)
```

8. 把原 loop blocks 移到 body function。
9. 修复 header phi、exit phi 和 CFG。
10. 对 scalar global reduction 做 atomic 替换。

Gnalc 当前用 global variable 传 shared values。我们可以借鉴流程，但建议把 shared values 打包为 `env` 结构，语义更清晰。

## 6. Gnalc 的 Vectorizer 详细流程

Gnalc 的 vectorizer 是 Bottom-Up SLP。它不是 loop vectorizer，而是从同一基本块的连续 store 出发构建 vector tree。

### 6.1 启用条件

入口：

```text
VectorizerPass::run
  target = TargetAnalysis
  if !target.isVectorSupported(): return
  loop_aa = LoopAliasAnalysis
  for block in postorder:
    collectSeeds(block)
    vectorizeStoreChains()
```

`isVectorSupported` 要求 target 支持：

```text
INSERT
EXTRACT
SHUFFLE
```

这点很关键。RISC-V 初赛或不支持 SIMD 的目标不能运行 vectorizer。

### 6.2 seed 收集

Gnalc 只从 store 开始：

```text
collectSeeds(block):
  for inst in block:
    if store and value is not vector:
      seed_stores.push_back(store)
```

然后用 LoopAA 找连续 store 链：

```text
store A[i]
store A[i+1]
store A[i+2]
store A[i+3]
```

### 6.3 vector tree 构建

对一组 scalar value，要求：

- opcode 一致。
- target 支持该 opcode vectorize。
- 全部在同一基本块。
- 每个 scalar 在 bundle 中只出现一次。
- scheduler 能证明打包后重排合法。

支持 opcode：

```text
PHI
EXTRACT
LOAD
STORE
ZEXT / SEXT / FPTOSI / SITOFP / BITCAST
ICMP / FCMP
SELECT
ADD / SUB / MUL
FADD / FSUB / FMUL
SDIV / UDIV / FDIV
SREM / UREM / FREM
SHL / LSHR / ASHR
AND / OR / XOR
FNEG
GEP
```

实际第一版不建议支持这么多。我们决赛第一版应该只做：

```text
LOAD / STORE
ADD / SUB / MUL
FADD / FSUB / FMUL
AND / OR / XOR
ICMP
SELECT
CAST
```

### 6.4 scheduler 合法性

SLP 需要把多条 scalar 指令作为 bundle 调度到一起。Gnalc 的 scheduler 维护：

- use-def dependency。
- memory dependency。
- bundle dependency。
- scheduling region。
- dry-run schedule。

内存依赖规则：

- load-load 可以重排。
- write 不能越过可能 alias 的 read/write。
- call 按读写集合保守处理。

这说明我们的 `LoopAccessAnalysis/DependenceAnalysis` 不只服务 loop vectorizer，也服务 SLP scheduler。

### 6.5 cost model

Gnalc 的 cost model 是 target-specific：

- gather cost。
- insert/extract cost。
- shuffle cost。
- cast/cmp/select/binary/unary cost。
- memory cost。
- external scalar user 的 extract cost。

收益判断：

```text
cost < -SLP_COST_THRESHOLD
```

我们第一版可以不用复杂模型，先做 pattern-based：

- 连续 store 个数正好等于 VF。
- 上游 tree 完全同构。
- 无 shuffle。
- external user 很少。
- 所有 load/store 对齐或 target 允许非对齐。

### 6.6 vector IR 形态

Gnalc 有显式 vector IR：

```text
<n x ty>
extractelement
insertelement
shufflevector
vector load/store
vector binary/cmp/select
```

我们当前缺少这层抽象。决赛如果开放 SIMD，不管是前端显式 vector，还是标量 auto-vectorization，都需要先补 vector IR 或 vector intrinsic。

## 7. 各队如何利用 SIMD/并行化

这里的“利用 SIMD/并行化”只从中端角度分析。

| 项目 | SIMD/parallel 状态 | 中端思路 | 可借鉴点 |
| --- | --- | --- | --- |
| miniC-compiler | 没有看到通用 SIMD vectorizer；有实际 `LoopParallel` | 规范 loop 后做 legality，抽 `Loop_Body` 和 `Parallel_Target`，简单 reduction/atomic | 决赛 thread parallel 的第一版模型 |
| Gnalc | 有 `VectorizerPass` 和 `LoopParallelPass`；RISC-V 目标默认 gate 掉 | SLP vectorizer + target cost；parallel 分析/变换分离 | 决赛 SIMD/parallel 的体系化模型 |
| sysc-2455 | vectorize 主要面向 ARM；RISC-V 因无 SIMD 不启用 | `Parallelizable` 分析、`ColumnMajor`、structured loop pipeline | parallel 前置分析和数组布局 |
| OhmCompiler | `VectorizationDetect` 主要在 ARM 路径；有 runtime 相关代码 | 检测 0,+1 IV、连续 4 load/store、vector reduction、mul-add-store | 决赛第一版 vector pattern |
| GeeCeeCee | 未见完整 SIMD pass，但有 loop locality 和数组提升 | `LoopInterchange`、`LocalArrayLift`、`LoopGEPCombine`、`ModuloLoopOptimization` | 给 vectorizer 创造 stride-1 输入 |
| LSCCLL | 无明确 SIMD pass；循环优化强 | `SCEV`、`LSR`、`LoopFullUnroll`、`GCM`、`GEPStrengthReduce` | vector/parallel 前的 canonical 和 cleanup |
| SystemF | 无明确 SIMD pass；固定点 cleanup 和 code motion 强 | load/store merge、array layout、unroll | 向量化前的内存形态整理 |
| SysYCompiler2025 | 无明确 SIMD pass；竞赛数学模式强 | `LoopSumReduction`、`ModLoopReduction`、`AddChainReduction` | 有些循环可直接闭式化，胜过 vectorize |

结论：

- 真正可参考的 SIMD vectorizer 主要是 Gnalc。
- OhmCompiler 的 `VectorizationDetect` 适合做第一版轻量 pattern detector。
- miniC 的价值主要在 thread parallel，不在 SIMD。
- 其他强队更多是“给 vector/parallel 创造好输入”的中端 pass。

## 8. 我们需要补齐的 pass 名称与功能

### 8.1 初赛建议补齐

初赛不启用 SIMD/vectorizer/parallel，重点补标量中端。

| Pass | 功能 | 支持模式 | 优先级 |
| --- | --- | --- | --- |
| `IndVarSimplifyPass` | 归纳变量规范化 | single latch/exit, constant step IV | P0 |
| `PostUnrollMemoryCleanupPass` | 展开后清理内存和局部冗余 | 展开副本中的 load/store/CSE/DSE | P0 |
| `SingleSourcePhiEliminationPass` | 删除退化 phi | phi 所有 incoming 相同或只有一个有效来源 | P0 |
| `StoreToLoadForwardingPass` | store 后同地址 load 替换 | 同块/支配路径，无 clobber | P1 |
| `LocalCSEPass` | 局部公共子表达式 | 纯 binary/cast/GEP/load noalias | P1 |
| `ScalarRecurrenceSimplifyPass` | 简单递推闭式化 | `x=x+c`, `x=x-c` | P1 |
| `LoopSumReductionPass` | 求和公式化 | `sum += i`, `sum += a*i+b` | P1 |
| `ModLoopReductionPass` | 循环取模削弱 | `x=(x+c)%m` | P1 |
| `OnlyWriteArrayEliminationPass` | 删除只写数组 | local non-escaping array | P1 |
| `ArrayAccessNormalizePass` | GEP/数组访问规范化 | affine GEP | P1 |
| `SROAPass` | 小聚合拆 scalar | 常量下标局部数组 | P2 |
| `FunctionMemoizationPass` | 纯函数缓存 | 小参数纯递归/重复调用 | P2 |

### 8.2 决赛必须补齐

| Pass/Analysis | 功能 | 支持模式 |
| --- | --- | --- |
| `TargetFeatureAnalysis` | 判断 SIMD/thread/reduction intrinsic 是否可用 | target feature gate |
| `LoopAccessAnalysis` | base/stride/alignment/noalias | affine array access |
| `LoopDependenceAnalysis` | loop-carried dependency 判定 | read/write, write/write |
| `ReductionRecognitionPass` | 识别 scalar/global/array reduction | add/mul/min/max/bitwise |
| `SCEVExpander` | 把 SCEV 表达式重新物化成 IR | trip count、exit value |
| `LoopParallelAnalysis` | 收集可并行 loop 信息 | canonical counted loop |
| `LoopParallelPass` | outline loop body 并插入 parallel intrinsic | map loop + reduction |
| `VectorTypeLegalizePass` | 引入/规范 vector IR | fixed/scalable vector |
| `SLPVectorizerPass` | 基本块内 scalar pack | consecutive store chain |
| `LoopVectorizerPass` | 循环向量化 | stride-1 map/reduction |
| `VectorCleanupPass` | 清理 insert/extract/shuffle 噪声 | vector IR cleanup |

## 9. 决赛 Parallel 方案

### 9.1 目标

把大粒度、无跨迭代依赖的 counted loop 改写为运行库并行调用。

中端输出建议是 target-independent intrinsic：

```text
parallel.for(begin, end, step, body_fn, env)
```

再由后续合法化阶段决定如何链接运行库。本文只讨论 IR 形态。

### 9.2 推荐 pass 分层

不要写一个巨型 `LoopParallelPass`。建议拆成：

```text
ParallelTargetFeatureAnalysis
LoopParallelAnalysis
ReductionRecognitionPass
LoopOutlinerPass
LoopParallelPass
ParallelCleanupPass
```

### 9.3 LoopParallelAnalysis

输入：

- `LoopInfo`
- `Dominant`
- `SCEV`
- `LoopAccessAnalysis`
- `SideEffect`
- `TargetFeatureAnalysis`

输出：

```text
ParallelLoopCandidate:
  loop
  begin
  end
  step
  trip_count
  live_ins
  live_outs
  reductions
  memory_accesses
  estimated_cost
```

合法性：

- single preheader。
- single latch。
- single exit。
- canonical IV。
- trip count 可计算。
- step 为正，第一版只支持 step = 1。
- 无 side-effect call。
- 无 I/O。
- 无异常/早退。
- memory no carried dependence。
- live-out 只允许 reduction 或可闭式计算的 IV。

支持模式：

```c
for (i = l; i < r; ++i)
    A[i] = B[i] + C[i];
```

```c
for (i = l; i < r; ++i)
    A[i] = scalar_expr(i);
```

```c
for (i = l; i < r; ++i)
    sum += expr(i);
```

```c
for (i = l; i < r; ++i)
    g = g + expr(i);    // 第二版，global reduction
```

拒绝模式：

```c
A[i] = A[i - 1] + x;    // carried dependency
if (cond) break;         // data-dependent exit
putint(A[i]);            // I/O
foo(&A[i]);              // unknown side effect
```

### 9.4 ReductionRecognitionPass

第一版支持：

```text
int add
int mul
int min/max
bitwise and/or/xor
```

谨慎支持：

```text
float add
float mul
```

浮点 reduction 只有在赛制允许浮点重排误差或官方说明允许 fast-math 时启用。

识别模式：

```text
phi:
  incoming from preheader = init
  incoming from latch = update
update:
  phi op expr
```

可接受：

```c
sum += A[i];
prod *= A[i];
mx = max(mx, A[i]);
bits ^= A[i];
```

### 9.5 LoopOutlinerPass

把 loop body 抽成：

```c
void body(int begin, int end, Env *env)
```

Env 中放：

- live-in scalar。
- array base pointer。
- reduction buffer。
- 其他 invariant 参数。

原函数改成：

```c
Env env = ...
parallel.for(begin, end, 1, body, &env)
merge_reduction(env)
```

相比 miniC 的全局 buffer，Env 更适合我们：

- 支持多个 parallel call。
- 支持函数内多个候选 loop。
- 更容易做 nested 禁止和调试。
- 参数语义更清晰。

### 9.6 Profit model

必须有收益模型，否则 parallel 会拖慢小循环。

第一版规则：

```text
constant trip count < 1024: reject
unknown trip count: reject
body has too few operations: reject
memory stride != 1 and no heavy compute: reject
nested loop: only choose outermost profitable loop
already vectorized loop: require larger threshold
```

miniC 用 `iteration <= 100` 跳过，这是一个很好的起点，但决赛应更保守。

## 10. 决赛 SIMD / Vector 方案

### 10.1 先确认 SIMD 形态

决赛文档可能有两种情况：

1. 固定宽度 SIMD。
2. RVV-like 可变长度 SIMD。

中端必须抽象：

```text
VectorKind:
  FixedWidth
  Scalable

TargetFeature:
  supportsVector
  supportedElementTypes
  supportedOps
  minVectorBits
  maxVectorBits
  requiresAlignment
```

不确认 feature 时不运行 vector pass。

### 10.2 Vector IR

必须新增或模拟：

```text
VectorType<n x ty>
VectorLoad
VectorStore
VectorSplat
VectorExtract
VectorInsert
VectorShuffle
VectorBinary
VectorCmp
VectorSelect
VectorReduction
```

如果时间紧，可以先用 intrinsic：

```text
simd.load.v4i32(ptr)
simd.store.v4i32(ptr, val)
simd.add.v4i32(a, b)
simd.extract.v4i32(v, lane)
```

但长期最好有一等 vector type。

### 10.3 SLPVectorizerPass

第一版最现实，借鉴 Gnalc。

流程：

```text
for each basic block:
  collect store seeds
  build consecutive store chains
  choose VF
  build vector tree bottom-up
  check scheduler legality
  check cost/profit
  rewrite scalar tree to vector IR
  insert extract for external scalar users
  delete dead scalar insts
```

支持模式：

```c
A[i+0] = B[i+0] + C[i+0];
A[i+1] = B[i+1] + C[i+1];
A[i+2] = B[i+2] + C[i+2];
A[i+3] = B[i+3] + C[i+3];
```

```c
A[i+0] = x;
A[i+1] = x;
A[i+2] = x;
A[i+3] = x;
```

```c
A[i+0] = B[i+0] * k + C[i+0];
A[i+1] = B[i+1] * k + C[i+1];
A[i+2] = B[i+2] * k + C[i+2];
A[i+3] = B[i+3] * k + C[i+3];
```

第一版支持 op：

```text
i32/f32 load/store
add/sub/mul
fadd/fsub/fmul
and/or/xor
icmp/fcmp
select
zext/sext/bitcast
```

第一版拒绝：

- div/rem。
- function call。
- gather/scatter。
- 需要复杂 shuffle 的包。
- 多个外部 scalar user。
- 未知 alias 的 memory region。

### 10.4 LoopVectorizerPass

SLP 稳定后，再做 loop vectorizer。

流程：

```text
LoopSimplify
LCSSA
LoopRotate
IndVarSimplify
LoopAccessAnalysis
ReductionRecognition
choose VF
create vector loop
create scalar remainder
cleanup
```

支持模式：

```c
for (i = 0; i < n; ++i)
    A[i] = B[i] + C[i];
```

```c
for (i = 0; i < n; ++i)
    A[i] = B[i] * k + C[i];
```

```c
for (i = 0; i < n; ++i)
    sum += A[i];
```

```c
for (i = 1; i < n - 1; ++i)
    A[i] = B[i - 1] + B[i] + B[i + 1];
```

最后一个 stencil 模式只有在 `A` 和 `B` noalias 或者写入不会影响后续读时才能做。

### 10.5 Fixed SIMD 与 RVV-like 的差异

#### Fixed-width SIMD

中端生成：

```text
vector loop with VF = vector_bits / element_bits
scalar remainder loop
```

适合：

- SLP。
- 固定 VF loop vectorizer。

#### RVV-like scalable SIMD

中端更适合生成：

```text
while (i < n) {
  vl = setvl(n - i)
  vector body with vl
  i += vl
}
```

这里 `setvl` 可以先作为 target intrinsic 存在于 IR。中端不需要决定最终指令，只需要保留 scalable vector 语义。

### 10.6 SIMD profit model

第一版规则：

```text
must have at least one vector store
must reduce scalar instruction count
external extract count <= 1 or small
no shuffle required
no call in schedule region
load/store are consecutive
alignment satisfies target or target allows unaligned
VF is power of two for fixed SIMD
```

如果 cost model 不成熟，就宁可少向量化。

## 11. Parallel 与 SIMD 的组合策略

如果决赛同时开放线程并行和 SIMD：

### 11.1 推荐顺序

对嵌套循环：

```text
inner loop: vectorize
outer loop: parallelize
```

典型：

```c
for (i = 0; i < n; ++i)          // parallel
  for (j = 0; j < m; ++j)        // vector
    A[i][j] = B[i][j] + C[i][j];
```

pipeline：

```text
LoopSimplify
LCSSA
LoopRotate
IndVarSimplify
LoopAccessAnalysis
LoopVectorizer on inner loops
Cleanup
LoopParallelAnalysis on outer loops
LoopParallelPass
Cleanup
```

### 11.2 单层循环如何选择

单层循环不要默认同时 parallel + vector。先通过 profit model 选择：

- trip count 很大、body 较重：parallel 优先。
- stride-1、body 同构、无 reduction：vector 优先。
- reduction：先 vector reduction；如果 trip count 极大，再考虑 parallel reduction。

### 11.3 避免冲突

需要 loop attrs：

```text
Vectorized
Parallelized
NoVectorize(reason)
NoParallel(reason)
RemainderLoop
ParallelBody
```

规则：

- `ParallelBody` 内默认不再 parallel。
- scalar remainder loop 不 parallel。
- 已 parallel 的 loop 不再做 loop vectorizer，除非明确处理 body function。
- SLP 可以在 parallel body 内运行，但必须在 outline 后重新跑。

## 12. 建议的新中端 pipeline

### 12.1 初赛中端 pipeline 调整

在现有 pipeline 上小改：

```text
Early cleanup fixed point
IPO
LocalAggregateSimplify
StoreOnlyGlobalElimination
HeavyMemoryPipeline

Loop pipeline fixed point:
  BreakCriticalEdges
  LoopSimplify
  LCSSA
  LoopRotate
  IndVarSimplify
  LICM
  LoopDeletion
  ScalarRecurrenceSimplify
  LoopStrengthReduction
  LoopUnroll
  PostUnrollMemoryCleanup

Post-loop cleanup:
  LoopSumReduction / ModLoopReduction if conservative
  GepCombine / GepEvaluate
  StoreToLoadForwarding
  DSE / DCE
  LatePhiCleanup

Late cleanup
```

### 12.2 决赛 SIMD/parallel pipeline

```text
Early cleanup
IPO
ArrayAccessNormalize
SROA / LocalArrayPromotion
HeavyMemoryPipeline

Loop canonicalization:
  LoopSimplify
  LCSSA
  LoopRotate
  IndVarSimplify
  SCEV refresh
  LoopAccessAnalysis

Scalar loop transforms:
  LICM
  LoopClosedFormSimplify
  LoopIdiomRecognize
  LoopStrengthReduction

SIMD stage:
  TargetFeatureAnalysis
  VectorTypeLegalize
  SLPVectorizer
  LoopVectorizer
  VectorCleanup

Parallel stage:
  ReductionRecognition
  LoopParallelAnalysis
  LoopParallelPass
  ParallelCleanup

Final cleanup:
  SCCP
  InstSimplify
  CFGSimplify
  DCE
  LCSSA if loops remain
```

## 13. 实施优先级

### 13.1 现在就能做

```text
IndVarSimplifyPass
PostUnrollMemoryCleanupPass
SingleSourcePhiEliminationPass
StoreToLoadForwardingPass
ScalarRecurrenceSimplifyPass
OnlyWriteArrayEliminationPass
ArrayAccessNormalizePass
```

这些都不依赖 SIMD 或线程环境。

### 13.2 决赛前准备但默认关闭

```text
TargetFeatureAnalysis
LoopAccessAnalysis
ReductionRecognitionPass
VectorType / VectorIntrinsic IR
LoopParallelAnalysis skeleton
SLPVectorizer skeleton
```

这些可以先在 flag 下开发，不影响初赛。

### 13.3 决赛根据文档开启

```text
LoopParallelPass
SLPVectorizerPass
LoopVectorizerPass
ParallelRuntimeIntrinsic emission
SIMD intrinsic emission
```

## 14. 最终结论

我们的中端主干已经接近强队水平，当前真正缺的不是再堆一个普通 `DCE/GVN/LICM`，而是：

1. `IndVarSimplify + LoopAccessAnalysis + ReductionRecognition` 这一组 parallel/vector 前置分析。
2. `ScalarRecurrenceSimplify + LoopSumReduction + ModLoopReduction` 这一组竞赛数学专项。
3. `ArrayAccessNormalize + SROA + OnlyWriteArrayElimination` 这一组数组专项。
4. `PostUnrollMemoryCleanup` 这一组展开后收敛能力。
5. 决赛的 `TargetFeatureAnalysis + Vector IR + SLPVectorizer + LoopParallelPass`。

初赛建议稳扎稳打补标量中端。决赛如果开放 SIMD，先做 SLP，再做 loop vectorizer；如果开放多线程，先做 miniC/Gnalc 风格的窄版 LoopParallel。两者同时开放时，优先“内层 vector、外层 parallel”，并用 loop attrs 和 profit model 防止重复变换。

