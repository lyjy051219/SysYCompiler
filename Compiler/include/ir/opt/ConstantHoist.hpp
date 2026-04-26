#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

// 常量提升：共享的大整数做 value-key 分桶 + base/offset 重组（相近值用
// `add base, offset` rebase）；共享的浮点用 `fadd -0.0, fv` wrapper 拎出
// 一个 SSA 值，只共享不做 base/offset。
//
// 给后续迭代看：浮点路径的实际收益依赖后端对 ConstFloat use 的 lowering 策略
// 若后端 per-use 发射常量装载，IR 层 wrapper 让这些 load 合并为一条；若
// 后端已经按 ConstFloat 指针做全局 dedup，本 pass 的 wrapper 是净增一条
// fadd，反而 regress。IR 层 ConstFloat 本身是 interning 的，所以 dedup
// 不是目的，暴露"跨 use 活的 SSA 值"给后端寄存器分配才是。
class ConstantHoistPass : public PassDescriptor<ConstantHoistPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
