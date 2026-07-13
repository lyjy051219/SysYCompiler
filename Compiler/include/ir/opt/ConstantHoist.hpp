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
};

// 常量提升：共享的大整数做 value-key 分桶 + base/offset 重组（相近值用
// `add base, offset` rebase）。只改写不依赖 literal 形态的 use：除余的
// divisor、shift amount、GEP index、compare/call 等仍保留原字面量，
// 以便后续强度削弱和后端目标匹配。浮点常量暂不物化：在没有
// fast-math/目标成本证明时，用 fadd wrapper 会引入真实浮点运算和活跃区间。
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
    static bool isLiteralSensitiveUse(
        const Instruction *User, size_t OperandIndex);
    static ConstantHoistCollectResult collectCandidates(
        Function &F, const LoopInfo &LI);
    static void closeGroup(
        ConstantHoistGroup &Current,
        std::vector<ConstantHoistGroup> &Groups);
    static std::vector<ConstantHoistGroup> groupByBaseOffset(
        std::vector<ConstantHoistCandidate> &Sorted);
    static bool materializeGroup(
        ConstantHoistGroup &Group, BasicBlock *InsertBB);
};
