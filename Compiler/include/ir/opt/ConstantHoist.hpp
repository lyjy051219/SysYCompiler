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

// 将可共享的大整数常量改写成“公共基值 + 块内小偏移”：
// 1. 过滤依赖字面量形态的操作数，并按常量值收集使用点；
// 2. 把差值可由目标立即数编码的常量聚成一组，执行次数不足的组直接放弃；
// 3. 用支配关系和循环层次选择公共物化点，并拒绝跨越计时屏障的提升；
// 4. 在公共点生成基值，在各使用块生成偏移值并替换原常量。
class ConstantHoistPass : public PassDescriptor<ConstantHoistPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static constexpr int64_t ImmediateThreshold = 2048;
    static constexpr int UseThreshold = 2;
    static constexpr int StraightLineUseThreshold = 3;
    // 服务步骤 3：计算覆盖整组使用点的最近公共支配位置，并在需要时外提到
    // 最外层循环的预头；返回的位置必须支配组内每个改写点。
    static BasicBlock *foldCommonDominator(
        const DominatorTree &DT,
        const std::vector<ConstantHoistUseSite> &Uses);
    static BasicBlock *pickInsertionBlock(
        BasicBlock *Common, BasicBlock *Entry, const LoopInfo &LI);
    static bool isLiteralSensitiveUse(
        const Instruction *User, size_t OperandIndex);
    // 服务步骤 1：只把后端不要求保持字面量、且超出立即数范围的 i32 使用
    // 纳入候选，记录所在循环深度供收益判断使用。
    static ConstantHoistCollectResult collectCandidates(
        Function &F, const LoopInfo &LI);
    static void closeGroup(
        ConstantHoistGroup &Current,
        std::vector<ConstantHoistGroup> &Groups);
    // 服务步骤 2：按数值滑动窗口分组，并选择使用次数最多的成员作为基值，
    // 从而保证任一成员到基值的偏移都可编码为小立即数。
    static std::vector<ConstantHoistGroup> groupByBaseOffset(
        std::vector<ConstantHoistCandidate> &Sorted);
    static bool materializeGroup(
        ConstantHoistGroup &Group, BasicBlock *InsertBB);
};
