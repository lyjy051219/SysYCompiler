#pragma once

#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <memory>
#include <optional>
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
};
