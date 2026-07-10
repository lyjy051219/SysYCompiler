#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

class DominatorTree;
class LoopInfo;

struct ConstantHoistUseSite {
    Use *u = nullptr;
    Instruction *inst = nullptr;
    BasicBlock *block = nullptr;
    unsigned loopDepth = 0;
};

struct ConstantHoistCandidate {
    int32_t value = 0;
    std::vector<ConstantHoistUseSite> uses;
    unsigned maxDepth = 0;
};

struct ConstantHoistGroup {
    std::vector<ConstantHoistCandidate> members;
    size_t baseIdx = 0;
};

struct ConstantHoistCollectResult {
    std::unordered_map<int32_t, ConstantHoistCandidate> intBuckets;
    std::unordered_map<uint32_t, ConstantHoistCandidate> floatBuckets;
};

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

private:
    static constexpr int64_t ImmediateThreshold = 2048;
    static constexpr int UseThreshold = 2;
    static constexpr int StraightLineUseThreshold = 3;
    static BasicBlock *foldCommonDominator(
        const DominatorTree &DT,
        const std::vector<ConstantHoistUseSite> &Uses);
    static BasicBlock *pickInsertionBlock(
        BasicBlock *Common, BasicBlock *Entry, const LoopInfo &LI);
    static ConstantHoistCollectResult collectCandidates(
        Function &F, const LoopInfo &LI);
    static void closeGroup(
        ConstantHoistGroup &Current,
        std::vector<ConstantHoistGroup> &Groups);
    static std::vector<ConstantHoistGroup> groupByBaseOffset(
        std::vector<ConstantHoistCandidate> &Sorted);
    static bool materializeGroup(
        ConstantHoistGroup &Group, BasicBlock *InsertBB);
    static bool materializeFloatCandidate(
        ConstantHoistCandidate &Candidate, BasicBlock *InsertBB);
};
