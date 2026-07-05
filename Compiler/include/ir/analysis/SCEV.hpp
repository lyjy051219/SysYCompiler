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
    Mod,
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

struct InductionDescriptor {
    PhiInst *Phi = nullptr;
    const Loop *L = nullptr;

    Value *StartValue = nullptr;
    Value *UpdateValue = nullptr;

    const SCEVExpr *Start = nullptr;
    const SCEVExpr *Step = nullptr;

    std::optional<int64_t> ConstantStart;
    std::optional<int64_t> ConstantStep;

    bool Valid = false;
};

struct LoopTripCountInfo {
    const Loop *L = nullptr;

    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    BasicBlock *ExitingBlock = nullptr;
    BasicBlock *ExitBlock = nullptr;

    BinaryInst *ExitCmp = nullptr;
    bool ContinueOnTrue = true;

    PhiInst *IndVar = nullptr;

    const SCEVExpr *Start = nullptr;
    const SCEVExpr *Step = nullptr;
    const SCEVExpr *Bound = nullptr;

    const SCEVExpr *TripCount = nullptr;
    const SCEVExpr *BackedgeTakenCount = nullptr;

    std::optional<int64_t> ConstantTripCount;
    std::optional<int64_t> ConstantBackedgeTakenCount;

    bool IsRuntime = false;
    bool MayBeZeroTrip = true;
    bool Valid = false;
};

struct LoopRemainderInfo {
    const Loop *L = nullptr;
    int64_t Factor = 1;

    const SCEVExpr *TripCount = nullptr;
    const SCEVExpr *MainTripCount = nullptr;
    const SCEVExpr *RemainderCount = nullptr;

    std::optional<int64_t> ConstantTripCount;
    std::optional<int64_t> ConstantMainTripCount;
    std::optional<int64_t> ConstantRemainderCount;

    bool NeedRemainderLoop = false;
    bool IsRuntimeRemainder = false;
    bool Valid = false;
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

    const SCEVExpr *mod(const SCEVExpr *L, const SCEVExpr *R);

    // Structured loop facts used by full/partial/runtime unroll and peeling.
    // These helpers deliberately fail instead of guessing when the loop shape
    // is outside the simplified SCEV model.
    std::optional<LoopTripCountInfo> analyzeLoopTripCount(const Loop *L);
    std::optional<int64_t> getSmallConstantTripCount(const Loop *L);
    std::optional<int64_t> getSmallConstantBackedgeTakenCount(const Loop *L);
    std::optional<int64_t> getSmallConstantTripMultiple(const Loop *L);
    bool isTripCountMultipleOf(const Loop *L, int64_t Factor);
    std::optional<LoopRemainderInfo> getRemainderInfo(const Loop *L,
                                                      int64_t Factor);
    std::optional<InductionDescriptor> getCanonicalInduction(const Loop *L);
    const SCEVExpr *getSCEVAtIteration(const SCEVExpr *E,
                                       const SCEVExpr *Iter,
                                       const Loop *L);

    void forgetValue(Value *V);
    void forgetLoop(const Loop *L);
    void forgetAllLoops();
    void clear();

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
    bool isLoopInvariantExpr(const SCEVExpr *E, const Loop *L) const;
    bool isDirectLoopBlock(const Loop *L, BasicBlock *BB) const;
    bool sameExpr(const SCEVExpr *A, const SCEVExpr *B) const;
    std::optional<int64_t> asConstant(const SCEVExpr *E) const;
    std::string typeKey(std::shared_ptr<IRType> Ty) const;

    std::vector<BasicBlock *> collectLoopBlocks(const Loop *L) const;
    std::vector<BasicBlock *> collectDirectLoopBlocks(const Loop *L) const;
    const SCEVExpr *computeTripCount(const Loop *L);
    const SCEVExpr *tripFromComparison(const Loop *L, BinaryInst *Cmp,
                                       bool ContinueOnTrue);
    bool fillTripCountFromComparison(LoopTripCountInfo &Info);
    PhiInst *findPhiForAddRec(const Loop *L, const SCEVExpr *AddRec);
    PhiInst *findPhiForAffineRec(const Loop *L, const SCEVAffineInfo &Affine);
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
