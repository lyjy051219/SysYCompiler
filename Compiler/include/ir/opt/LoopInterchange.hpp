#pragma once

#include "ir/opt/NewPassManager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class BasicBlock;
class BinaryInst;
class BrInst;
class DominatorTree;
class Instruction;
class Loop;
class LoopDependenceInfo;
class LoopInfo;
class PhiInst;
class PostDominatorTree;
class SCEVResult;
struct SCEVExpr;
class Use;
class Value;

// LoopInterchange 的整体流程：
// 1. 从内到外枚举相邻循环，并用函数级次数上限与循环头记录阻止重复交换。
// 2. 优先识别旋转后的受限非完美嵌套，完整证明 CFG、SSA、依赖方向和局部性后物理重排循环轴。
// 3. 其余矩形同域嵌套走角色交换路径，只交换两个规范 IV 的负载使用，必要时下沉外轴纯计算前缀。
// 4. 先构造游离克隆并完成所有可失败检查，再集中提交 CFG/SSA 改写；失败时销毁未挂接对象。
// 5. 角色交换保留支配、后支配和循环结构，物理交换则使所有函数分析失效。
class LoopInterchangePass : public PassDescriptor<LoopInterchangePass> {
public:
    PassResult run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static constexpr unsigned MaxInterchangesPerFunction = 4;
    static constexpr unsigned MaxSinkInstructions = 8;
    static constexpr long long MinLocalityBenefit = 6;

    struct HeaderPair {
        BasicBlock *Outer = nullptr;
        BasicBlock *Inner = nullptr;

        bool operator==(const HeaderPair &Other) const {
            return Outer == Other.Outer && Inner == Other.Inner;
        }
    };

    struct HeaderPairHash {
        std::size_t operator()(const HeaderPair &Pair) const;
    };

    struct Candidate {
        Loop *Outer = nullptr;
        Loop *Inner = nullptr;
        PhiInst *OuterIV = nullptr;
        PhiInst *InnerIV = nullptr;
        BasicBlock *InnerPreHeader = nullptr;
        std::unordered_set<BasicBlock *> InnerBlocks;
        std::unordered_set<Instruction *> OuterControl;
        std::unordered_set<Instruction *> InnerControl;
        std::vector<Instruction *> SinkablePrefix;
        std::vector<Use *> OuterUses;
        std::vector<Use *> InnerUses;
        long long LocalityBenefit = 0;
    };

    struct RotatedNestCandidate {
        Loop *Outer = nullptr;
        Loop *Inner = nullptr;
        BasicBlock *Guard = nullptr;
        BasicBlock *OuterHeader = nullptr;
        BasicBlock *Prefix = nullptr;
        BasicBlock *InnerHeader = nullptr;
        BasicBlock *InnerBody = nullptr;
        BasicBlock *OuterLatch = nullptr;
        BasicBlock *Exit = nullptr;
        PhiInst *OuterIV = nullptr;
        PhiInst *InnerIV = nullptr;
        BinaryInst *OuterFirstCmp = nullptr;
        Value *OuterStartValue = nullptr;
        unsigned OuterStartIndex = 0;
        unsigned InnerStartIndex = 0;
        unsigned InnerBackedgeIndex = 0;
        bool OuterContinueOnTrue = true;
        bool InnerContinueOnTrue = true;
        long long LocalityBenefit = 0;
    };

    // 3.1 证明两轴起点、步长、迭代次数与零次语义一致，并收集角色交换的 SSA 切片。
    static bool sameIterationDomain(Loop *Outer, Loop *Inner,
                                    SCEVResult &SE,
                                    PhiInst *&OuterIV,
                                    PhiInst *&InnerIV);
    static bool hasOnlyCanonicalHeaderPhi(Loop *L, PhiInst *IV);
    static bool hasUnsupportedDescendantDomain(Loop *Inner,
                                               SCEVResult &SE);
    static bool isPureSinkInstruction(Instruction *I);
    static bool collectSinkablePrefix(Candidate &C);
    static bool collectRoleUses(Candidate &C);
    static bool collectControlSlice(
        Value *V, PhiInst *IV, Loop *L,
        std::unordered_set<Instruction *> &Control,
        std::unordered_set<Value *> &Active, unsigned Depth = 0);
    static long long computeLocalityBenefit(const Candidate &C,
                                            LoopDependenceInfo &LD);
    // 3.2 要求 LoopSimplify/LCSSA 形态、必经内循环与无外层内存负载，再提交 IV 角色交换。
    static bool validateNestShape(const Candidate &C, const LoopInfo &LI,
                                  const DominatorTree &DT,
                                  const PostDominatorTree &PDT);
    static bool applyCandidate(Function &F, Candidate &C);
    // 2.1 为物理交换建立完整候选：固定六块 CFG，核验归纳 PHI、可移动控制与依赖方向。
    static bool buildRotatedNestCandidate(
        Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
        DominatorTree &DT, PostDominatorTree &PDT,
        LoopDependenceInfo &LD, RotatedNestCandidate &C);
    static bool applyRotatedNestCandidate(
        Function &F, RotatedNestCandidate &C);
    static bool expressionKeysEqual(const SCEVExpr *A,
                                    const SCEVExpr *B);
    static unsigned stridePenalty(std::int64_t Stride);
    static bool isInstructionInBlocks(
        Instruction *I,
        const std::unordered_set<BasicBlock *> &Blocks);
    static void destroyDetachedInstructions(
        std::vector<Instruction *> &Instructions);

    // 1.1 状态跨 pass 固定点轮次保留，同时约束同一循环对和整个函数的变换次数。
    std::unordered_set<HeaderPair, HeaderPairHash> Transformed;
    std::unordered_map<Function *, unsigned> TransformCount;
};
