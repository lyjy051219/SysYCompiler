#pragma once

#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

struct IndexComponent {
    Value *Raw = nullptr;
    const SCEVExpr *Expr = nullptr;
};

struct LSRCandidate {
    GetElementPtrInst *OldGEP = nullptr;
    Value *Base = nullptr;
    Value *Index = nullptr;
    const SCEVExpr *Start = nullptr;
    std::optional<int64_t> StartConst;
    std::vector<IndexComponent> Prefix;
    std::vector<IndexComponent> Suffix;
    Loop *L = nullptr;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    int64_t Step = 0;
    int64_t LastIndexStride = 0;
    bool HasLeadingZero = false;
    bool AllowOffsetReuse = true;
    bool Used = false;
};

struct LSRGroup {
    size_t Order = 0;
    std::vector<LSRCandidate *> Items;
    Value *Base = nullptr;
    const SCEVExpr *Start = nullptr;
    std::optional<int64_t> StartConst;
    Loop *L = nullptr;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    int64_t Step = 0;
    int64_t LastIndexStride = 0;
    std::vector<IndexComponent> Prefix;
    std::vector<IndexComponent> Suffix;
    std::shared_ptr<IRType> ResultTy;
};

struct OffsetReuseGroup {
    size_t Order = 0;
    std::vector<LSRCandidate *> Items;
    Value *Base = nullptr;
    Loop *L = nullptr;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    int64_t Step = 0;
    int64_t LastIndexStride = 0;
    int64_t AnchorStart = 0;
    int64_t MaxStart = 0;
    std::vector<IndexComponent> Prefix;
    std::shared_ptr<IRType> ResultTy;
};

struct PointerIV {
    GetElementPtrInst *Start = nullptr;
    PhiInst *Phi = nullptr;
    GetElementPtrInst *Next = nullptr;
};

struct AddressGEPNode {
    GetElementPtrInst *GEP = nullptr;
    int64_t Offset = 0;
};

// Rewrites affine final-index GEPs into pointer induction variables and shares
// one pointer IV across nearby constant offsets when doing so is profitable.
class LoopStrengthReductionPass
    : public PassDescriptor<LoopStrengthReductionPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static constexpr int UnsupportedMaterializeCost = 1000000;
    static constexpr int64_t MaxOffsetRange = 16;
    static constexpr int MaxPointerIVsPerInnerLoop = 8;
    static constexpr int MaxPointerIVsPerOuterLoop = 4;
    static bool hasCostlyElementStride(int64_t Stride);
    static std::string scevKey(const SCEVExpr *E);
    static bool hasNonTrivialPrefix(const std::vector<IndexComponent> &Prefix);
    static bool canUseInPreheader(Value *V, BasicBlock *PreHeader,
                                  const DominatorTree &DT);
    static bool canMaterializeSCEVAtPreheader(
        const SCEVExpr *E, const Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    static int estimateMaterializeCost(
        const SCEVExpr *E, const Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    static Value *makeSCEVConstant(int64_t V, const SCEVExpr *E);
    static Value *materializeSCEVAtPreheader(
        const SCEVExpr *E, const Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    static int estimatePrefixMaterializeCost(
        const std::vector<IndexComponent> &Prefix, const Loop *L,
        BasicBlock *PreHeader, const DominatorTree &DT);
    static bool materializePrefix(
        const std::vector<IndexComponent> &Prefix, const Loop *L,
        BasicBlock *PreHeader, const DominatorTree &DT,
        std::vector<Value *> &Indices);
    static bool isLoadStoreAddressUse(Instruction *UserI, Use *U);
    static int countAddressUses(GetElementPtrInst *GEP);
    static int countLoopMemoryAddressUses(Loop *L);
    static bool getConstantOffsetGEP(
        GetElementPtrInst *Parent, Instruction *UserI, Use *U,
        GetElementPtrInst *&Child, int64_t &Offset);
    static bool collectAddressGEPFamily(
        GetElementPtrInst *GEP, Loop *L, int64_t CurrentOffset,
        std::vector<AddressGEPNode> &Nodes,
        std::unordered_set<GetElementPtrInst *> &Visited);
    static bool isSingleIndexArrayStep(Value *Base, size_t NumIndices);
    static bool matchGEP(
        GetElementPtrInst *GEP, Loop *L, SCEVResult &SE, Value *&Base,
        Value *&Index, std::vector<IndexComponent> &Prefix,
        std::vector<IndexComponent> &Suffix, int64_t &ActiveIndexStride,
        int64_t &FinalPointeeStride, bool &HasLeadingZero,
        bool &AllowOffsetReuse);
    static std::vector<LSRCandidate> collectCandidates(
        Function &F, LoopInfo &LI, SCEVResult &SE,
        const DominatorTree &DT);
    static std::string candidateShapeKey(const LSRCandidate &Candidate,
                                         bool IncludeStart);
    static std::vector<OffsetReuseGroup> buildOffsetReuseGroups(
        std::vector<LSRCandidate> &Candidates);
    static std::vector<LSRGroup> buildExactGroups(
        std::vector<LSRCandidate> &Candidates);
    static bool isRemovableIndexInst(Instruction *I);
    static bool collectRemovableIndexChain(
        Instruction *I, std::unordered_set<User *> &AllowedUsers,
        std::unordered_set<Instruction *> &Out);
    static int countRemovableIndexInsts(
        const std::vector<LSRCandidate *> &Items);
    static int countCoveredAddressUses(
        const std::vector<LSRCandidate *> &Items);
    static bool coversDenseInnermostAddresses(
        Loop *L, const std::vector<LSRCandidate *> &Items);
    static void cleanupDeadIndexChains(
        const std::vector<Instruction *> &Seeds);
    static bool isSimpleSingleGEP(const LSRCandidate &Candidate);
    static bool shouldRewriteExactGroup(const LSRGroup &Group,
                                        const DominatorTree &DT);
    static bool shouldRewriteOffsetGroup(const OffsetReuseGroup &Group,
                                         const DominatorTree &DT);
    static PointerIV createPointerIV(
        Value *Base, const std::vector<Value *> &StartIndices, int64_t Step,
        const std::shared_ptr<IRType> &ResultTy, BasicBlock *PreHeader,
        BasicBlock *Header, BasicBlock *Latch);
    static bool rewriteExactGroup(
        LSRGroup &Group, const DominatorTree &DT,
        std::vector<Instruction *> &DeadSeeds);
    static bool rewriteOffsetGroup(
        OffsetReuseGroup &Group, const DominatorTree &DT,
        std::vector<Instruction *> &DeadSeeds);
    static int pointerIVLimit(const Loop *L);
    static bool runAddressLSR(Function &F, LoopInfo &LI, SCEVResult &SE,
                              DominatorTree &DT);
};
