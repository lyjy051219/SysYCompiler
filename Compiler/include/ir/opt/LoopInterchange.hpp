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

// Conservatively permute two adjacent loop axes which enumerate the exact
// same iteration domain.  The CFG stays in place; payload uses of the two
// canonical IVs are exchanged simultaneously.  For a rectangular domain this
// is exactly the schedule transformation (outer, inner) -> (inner, outer),
// while avoiding the fragile PHI/exit rewiring of a physical CFG swap.
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

    static bool sameIterationDomain(Loop *Outer, Loop *Inner,
                                    SCEVResult &SE,
                                    PhiInst *&OuterIV,
                                    PhiInst *&InnerIV);
    static bool hasOnlyCanonicalHeaderPhi(Loop *L, PhiInst *IV);
    static bool hasTriangularDescendant(Loop *Inner, SCEVResult &SE);
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
