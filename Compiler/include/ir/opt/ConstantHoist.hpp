#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>
class DominatorTree;
class LoopInfo;
class TargetCostInfo;

//ConstantHoistUseSite描述一处可由公共基值重写的大整数常量使用。
struct ConstantHoistUseSite {
    //记录待替换的具体Use对象。
    Use *u = nullptr;
    //记录拥有该常量操作数的用户指令。
    Instruction *inst = nullptr;
    //记录用户指令所属的基本块。
    BasicBlock *block = nullptr;
    //记录使用点所在循环的嵌套深度。
    unsigned loopDepth = 0;
};

//ConstantHoistCandidate汇总同一整数常量的全部合法使用点。
struct ConstantHoistCandidate {
    //记录候选i32常量的字面值。
    int32_t value = 0;
    //记录该常量所有可以重写的使用点。
    std::vector<ConstantHoistUseSite> uses;
    //记录所有使用点中的最大循环嵌套深度。
    unsigned maxDepth = 0;
};

//ConstantHoistGroup保存差值均可由小立即数编码的一组常量。
struct ConstantHoistGroup {
    //保存当前窗口内的常量候选成员。
    std::vector<ConstantHoistCandidate> members;
    //记录被选择作为公共基值的成员索引。
    size_t baseIdx = 0;
};

//ConstantHoistCollectResult保存按整数值分桶的候选收集结果。
struct ConstantHoistCollectResult {
    //把每个i32字面值映射到其使用摘要。
    std::unordered_map<int32_t, ConstantHoistCandidate> intBuckets;
};

//将可共享的大整数常量改写成“公共基值 + 块内小偏移”：
//1. 过滤依赖字面量形态的操作数，并按常量值收集使用点；
//2. 把差值可由目标立即数编码的常量聚成一组，执行次数不足的组直接放弃；
//3. 用支配关系和循环层次选择公共物化点；
//4. 在公共点生成基值，在各使用块生成偏移值并替换原常量。
class ConstantHoistPass : public PassDescriptor<ConstantHoistPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //限定循环内候选组获得收益所需的最少动态使用次数。
    static constexpr int UseThreshold = 2;
    //限定直线代码候选组获得收益所需的最少静态使用次数。
    static constexpr int StraightLineUseThreshold = 3;
    //服务步骤 3：计算覆盖整组使用点的最近公共支配位置，并在需要时外提到
    //最外层循环的预头；返回的位置必须支配组内每个改写点。
    static BasicBlock *foldCommonDominator(
        const DominatorTree &DT,
        const std::vector<ConstantHoistUseSite> &Uses);
    //把公共支配块提升到最外层规范循环预头或安全的函数入口。
    static BasicBlock *pickInsertionBlock(
        BasicBlock *Common, BasicBlock *Entry, const LoopInfo &LI);
    //判断用户操作数是否必须保持字面常量形态供后续专用匹配使用。
    static bool isLiteralSensitiveUse(
        const Instruction *User, size_t OperandIndex);
    //服务步骤 1：只把后端不要求保持字面量、且超出立即数范围的 i32 使用
    //纳入候选，记录所在循环深度供收益判断使用。
    static ConstantHoistCollectResult collectCandidates(
        Function &F, const LoopInfo &LI,
        const TargetCostInfo &TargetCost);
    //完成当前常量窗口的收益筛选并选择使用最频繁的公共基值。
    static void closeGroup(
        ConstantHoistGroup &Current,
        std::vector<ConstantHoistGroup> &Groups);
    //服务步骤 2：按数值滑动窗口分组，并选择使用次数最多的成员作为基值，
    //从而保证任一成员到基值的偏移都可编码为小立即数。
    static std::vector<ConstantHoistGroup> groupByBaseOffset(
        std::vector<ConstantHoistCandidate> &Sorted,
        const TargetCostInfo &TargetCost);
    //在公共支配点物化基值并在各使用块生成、复用小偏移表达式。
    static bool materializeGroup(
        ConstantHoistGroup &Group, BasicBlock *InsertBB);
};
