#pragma once

#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// A compact Scalar Evolution analysis tailored to this IR.
//
// The analysis recognizes the loop shapes produced by our LoopSimplify /
// Mem2Reg pipeline and keeps the expression language deliberately small:
// constants, unknown IR values, arithmetic trees, min/max, casts, and simple
// loop recurrences.  Unknown is a first-class result instead of a failure.

enum class SCEVExprKind {
    Constant,
    Unknown,
    Add,
    Sub,
    Mul,
    Div,
    Min,
    Max,
    Cast,
    Compare,
    AddRec,
    MulRec,
    DivRec
};

enum class SCEVCastKind {
    Trunc,
    ZExt,
    SExt,
    BitCast
};

struct SCEVExpr {
    SCEVExprKind Kind = SCEVExprKind::Unknown;
    SCEVCastKind CastKind = SCEVCastKind::BitCast;
    BinaryInst::Operation Predicate = BinaryInst::E;
    Value *RawValue = nullptr;
    const Loop *RecLoop = nullptr;
    int64_t ConstValue = 0;
    std::shared_ptr<IRType> Ty;
    std::vector<const SCEVExpr *> Operands;
    std::string Key;

    bool isConstant(int64_t V) const {
        return Kind == SCEVExprKind::Constant && ConstValue == V;
    }
};

struct SCEVAffineInfo {
    const Loop *RecLoop = nullptr;
    const SCEVExpr *Start = nullptr;
    const SCEVExpr *Step = nullptr;
    std::optional<int64_t> ConstantStart;
    std::optional<int64_t> ConstantStep;
};

class SCEVResult {
public:
    SCEVResult() = default;
    SCEVResult(Function *F, LoopInfo *LI, DominatorTree *DT);
    SCEVResult(SCEVResult &&) = default;
    SCEVResult &operator=(SCEVResult &&) = default;
    SCEVResult(const SCEVResult &) = delete;
    SCEVResult &operator=(const SCEVResult &) = delete;

    const SCEVExpr *getSCEV(Value *V);
    const SCEVExpr *getTripCount(const Loop *L);
    const SCEVExpr *getBackedgeTakenCount(const Loop *L);
    std::optional<SCEVAffineInfo> getAffine(Value *V);
    std::optional<SCEVAffineInfo> getAffine(const SCEVExpr *E) const;
    const std::unordered_map<Value *, LoopInductionInfo> &getLoopInductions();

    std::string format(const SCEVExpr *E) const;
    bool invalidate(Function &F, const PreservationStatus &PA);

private:
    struct UpdateDelta {
        bool SeenSelf = false;
        const SCEVExpr *Delta = nullptr;
    };

    Function *Func = nullptr;
    LoopInfo *LI = nullptr;
    DominatorTree *DT = nullptr;

    std::vector<std::unique_ptr<SCEVExpr>> ExprStorage;
    std::unordered_map<std::string, const SCEVExpr *> Interned;
    std::unordered_map<Value *, const SCEVExpr *> ValueCache;
    std::unordered_map<Value *, bool> ActiveValues;
    std::unordered_map<const Loop *, const SCEVExpr *> TripCountCache;
    std::unordered_map<const Loop *, const SCEVExpr *> BackedgeCountCache;
    std::unordered_map<Value *, LoopInductionInfo> LoopInductions;
    bool LoopInductionsReady = false;

    const SCEVExpr *makeConstant(int64_t V, std::shared_ptr<IRType> Ty = nullptr);
    const SCEVExpr *makeUnknown(Value *V);
    const SCEVExpr *makeBinary(SCEVExprKind K, const SCEVExpr *L,
                               const SCEVExpr *R, std::shared_ptr<IRType> Ty = nullptr);
    const SCEVExpr *makeCast(SCEVCastKind K, const SCEVExpr *Op,
                             std::shared_ptr<IRType> Ty);
    const SCEVExpr *makeCompare(BinaryInst::Operation Pred, const SCEVExpr *L,
                                const SCEVExpr *R);
    const SCEVExpr *makeRec(SCEVExprKind K, const Loop *L, const SCEVExpr *Base,
                            const SCEVExpr *Step, std::shared_ptr<IRType> Ty = nullptr);
    const SCEVExpr *intern(std::unique_ptr<SCEVExpr> E);

    const SCEVExpr *add(const SCEVExpr *L, const SCEVExpr *R);
    const SCEVExpr *sub(const SCEVExpr *L, const SCEVExpr *R);
    const SCEVExpr *mul(const SCEVExpr *L, const SCEVExpr *R);
    const SCEVExpr *div(const SCEVExpr *L, const SCEVExpr *R);
    const SCEVExpr *neg(const SCEVExpr *E);

    const SCEVExpr *create(Value *V);
    const SCEVExpr *createFromBinary(BinaryInst *BI);
    const SCEVExpr *createFromPhi(PhiInst *Phi);
    const SCEVExpr *createFromSelect(SelectInst *Sel);
    const SCEVExpr *createFromGEP(GetElementPtrInst *GEP);
    const SCEVExpr *createFromCast(Instruction *I, SCEVCastKind CK);

    const SCEVExpr *tryHeaderPhi(PhiInst *Phi, const Loop *L);
    std::optional<UpdateDelta> findAdditiveDelta(PhiInst *Self, Value *V,
                                                 const Loop *L);
    const SCEVExpr *trySimpleMulDivRec(PhiInst *Self, Value *Update,
                                       Value *Start, const Loop *L);

    bool isLoopInvariant(Value *V, const Loop *L) const;
    bool sameExpr(const SCEVExpr *A, const SCEVExpr *B) const;
    std::optional<int64_t> asConstant(const SCEVExpr *E) const;
    std::string typeKey(std::shared_ptr<IRType> Ty) const;

    std::vector<BasicBlock *> collectLoopBlocks(const Loop *L) const;
    const SCEVExpr *computeTripCount(const Loop *L);
    const SCEVExpr *tripFromComparison(const Loop *L, BinaryInst *Cmp,
                                       bool ContinueOnTrue);
    BinaryInst::Operation invertPredicate(BinaryInst::Operation Op) const;
    BinaryInst::Operation swapPredicate(BinaryInst::Operation Op) const;
};

class SCEVAnalysis : public AnalysisDescriptor<SCEVAnalysis> {
public:
    using Result = SCEVResult;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<SCEVAnalysis>;
    static PassID Key;
};
