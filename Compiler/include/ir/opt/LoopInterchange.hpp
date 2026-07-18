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

// 保守交换两个相邻循环轴。矩形同域优先交换规范归纳变量的角色；经过
// 分布形成的受限非完美循环则在完整依赖与 CFG 证明后执行物理交换。
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
    static bool validateNestShape(const Candidate &C, const LoopInfo &LI,
                                  const DominatorTree &DT,
                                  const PostDominatorTree &PDT);
    static bool applyCandidate(Function &F, Candidate &C);
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

    std::unordered_set<HeaderPair, HeaderPairHash> Transformed;
    std::unordered_map<Function *, unsigned> TransformCount;
};
