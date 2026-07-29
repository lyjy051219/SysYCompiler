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
#include <unordered_set>
#include <vector>

// SCEV 表达式语言覆盖常量、未知值、算术/位运算树、条件选择和循环递推；
// 无法证明的形态保留为 Unknown，符号节点本身不携带无回绕承诺。
enum class SCEVExprKind {
    Constant,
    Unknown,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Shl,
    LShr,
    AShr,
    BitAnd,
    BitOr,
    BitXor,
    Binomial,
    Min,
    Max,
    Select,
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

// 表达式节点由结构 Key 驻留共享，Operands 与 RecLoop 描述不可变的符号结构。
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

    bool isConstant(int64_t V) const;
    bool isRecurrence() const;
    bool mentionsLoop(const Loop *L) const;
    static const char *kindName(SCEVExprKind K);
    static const char *castName(SCEVCastKind K);
    static const char *predicateName(BinaryInst::Operation Op);
};

struct SCEVAffineInfo {
    const Loop *RecLoop = nullptr;
    const SCEVExpr *Start = nullptr;
    const SCEVExpr *Step = nullptr;
    std::optional<int64_t> ConstantStart;
    std::optional<int64_t> ConstantStep;
};

// AddRec 的系数采用二项式基底：
//   E(n) = C0*C(n,0) + C1*C(n,1) + ... + Cd*C(n,d)。
// 两个系数仍是原有 affine 表示，三个及以上系数才表示高阶多项式递推。
struct SCEVAddRecInfo {
    const Loop *RecLoop = nullptr;
    std::vector<const SCEVExpr *> Coefficients;

    unsigned getDegree() const;
    bool isAffine() const;
};

// Range 只提供步长方向证据，不能由此单独推出循环有限或算术无回绕。
enum class SCEVStepSign {
    Unknown,
    Zero,
    StrictlyPositive,
    StrictlyNegative,
    NonNegative,
    NonPositive
};

struct SCEVStepRangeInfo {
    const Loop *RecLoop = nullptr;
    const SCEVExpr *Step = nullptr;
    IntRange Range;
    SCEVStepSign Sign = SCEVStepSign::Unknown;
    bool Valid = false;
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

enum class LoopExitComparePhase {
    Unknown,
    HeaderPhi,
    BackedgeUpdate
};

// TripCountInfo 绑定规范循环形状、退出比较阶段、符号次数以及独立的无回绕证明。
struct LoopTripCountInfo {
    const Loop *L = nullptr;

    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    BasicBlock *ExitingBlock = nullptr;
    BasicBlock *ExitBlock = nullptr;

    BinaryInst *ExitCmp = nullptr;
    bool ContinueOnTrue = true;
    LoopExitComparePhase ComparePhase = LoopExitComparePhase::Unknown;

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

    bool ProvenNoWrap = false;
    bool Valid = false;
};

// 余数摘要把总迭代数拆为主循环次数与余数次数，显式区分常量和运行时路径。
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

// 函数级结果按需构造并缓存 SCEV，同时向循环变换提供归纳、次数和有限性查询。
class SCEVResult {
public:
    SCEVResult();
    SCEVResult(Function *F, LoopInfo *LI, DominatorTree *DT);
    SCEVResult(SCEVResult &&);
    SCEVResult &operator=(SCEVResult &&);
    SCEVResult(const SCEVResult &) = delete;
    SCEVResult &operator=(const SCEVResult &) = delete;

    // 查询接口只在形状和算术安全性得到证明时返回结构化循环事实，否则返回空值或 Unknown。
    const SCEVExpr *getSCEV(Value *V);
    const SCEVExpr *getTripCount(const Loop *L);
    const SCEVExpr *getBackedgeTakenCount(const Loop *L);
    std::optional<SCEVAffineInfo> getAffine(Value *V);
    std::optional<SCEVAffineInfo> getAffine(const SCEVExpr *E) const;
    std::optional<SCEVAddRecInfo> getAddRecurrence(Value *V);
    std::optional<SCEVAddRecInfo>
    getAddRecurrence(const SCEVExpr *E) const;
    const std::unordered_map<Value *, LoopInductionInfo> &getLoopInductions();

    const SCEVExpr *mod(const SCEVExpr *L, const SCEVExpr *R);

    std::optional<LoopTripCountInfo> analyzeLoopTripCount(const Loop *L);
    std::optional<int64_t> getSmallConstantTripCount(const Loop *L);
    std::optional<int64_t> getSmallConstantBackedgeTakenCount(const Loop *L);
    std::optional<int64_t> getSmallConstantTripMultiple(const Loop *L);
    bool isTripCountMultipleOf(const Loop *L, int64_t Factor);
    std::optional<LoopRemainderInfo> getRemainderInfo(const Loop *L,
                                                      int64_t Factor);
    std::optional<InductionDescriptor> getCanonicalInduction(const Loop *L);

    bool isKnownFiniteLoop(const Loop *L);
    // Range 辅助版本只在步长严格定向且起点/边界/末次更新共同证明
    // i32 不回绕时返回 true；它不会写回或放宽基础 SCEV 的 no-wrap 状态。
    bool isKnownFiniteLoop(const Loop *L, RangeInfo &RI,
                           DominatorTree &DT);

    bool isKnownNoWrapInductionValue(const Loop *L, Value *V);

    bool proveRuntimeNoWrapAtPreheader(const LoopTripCountInfo &Info,
                                       RangeInfo &RI,
                                       DominatorTree &DT);
    // 调用者显式传入 Range 结果，避免 SCEVAnalysis 反向依赖 RangeAnalysis。
    // 返回值只描述 affine 步长的有符号范围，不改变任何 no-wrap 结论。
    std::optional<SCEVStepRangeInfo>
    analyzeAffineStepRange(const SCEVExpr *E, BasicBlock *Context,
                           RangeInfo &RI, DominatorTree &DT) const;
    std::optional<SCEVStepRangeInfo>
    analyzeAffineStepRange(Value *V, BasicBlock *Context,
                           RangeInfo &RI, DominatorTree &DT);
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
    static constexpr unsigned MaxAddRecDegree = 8;

    static std::optional<int64_t> getTrivialExactIntConstant(
        Value *V, std::unordered_set<Value *> &Active, unsigned Depth = 0);

    // 构造阶段用活动值集合截断 SSA 环，并用 UpdateDelta 识别 Header PHI 的统一加法更新。
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
    const SCEVExpr *makeSelect(const SCEVExpr *Cond,
                               const SCEVExpr *TrueExpr,
                               const SCEVExpr *FalseExpr,
                               std::shared_ptr<IRType> Ty);
    const SCEVExpr *makeBinomial(const SCEVExpr *N, unsigned K,
                                 std::shared_ptr<IRType> Ty = nullptr);
    const SCEVExpr *makeAddRec(
        const Loop *L, const std::vector<const SCEVExpr *> &Coefficients,
        std::shared_ptr<IRType> Ty = nullptr);
    const SCEVExpr *makeAddRecFromDelta(
        const Loop *L, const SCEVExpr *Base, const SCEVExpr *Delta,
        std::shared_ptr<IRType> Ty = nullptr);
    const SCEVExpr *makeRec(SCEVExprKind K, const Loop *L, const SCEVExpr *Base,
                            const SCEVExpr *Step, std::shared_ptr<IRType> Ty = nullptr);
    const SCEVExpr *intern(std::unique_ptr<SCEVExpr> E);

    const SCEVExpr *add(const SCEVExpr *L, const SCEVExpr *R);
    const SCEVExpr *sub(const SCEVExpr *L, const SCEVExpr *R);
    const SCEVExpr *mul(const SCEVExpr *L, const SCEVExpr *R);
    const SCEVExpr *div(const SCEVExpr *L, const SCEVExpr *R);
    const SCEVExpr *neg(const SCEVExpr *E);
    const SCEVExpr *scaleAddRec(const SCEVExpr *Rec,
                                const SCEVExpr *Factor);
    const SCEVExpr *multiplyAddRecs(const SCEVExpr *L,
                                    const SCEVExpr *R);
    std::optional<int64_t>
    getBinomialProductFactor(unsigned LeftDegree, unsigned RightDegree,
                             unsigned ResultDegree) const;

    const SCEVExpr *create(Value *V);
    const SCEVExpr *createFromBinary(BinaryInst *BI);
    const SCEVExpr *createFromPhi(PhiInst *Phi);
    const SCEVExpr *createFromSelect(SelectInst *Sel);
    const SCEVExpr *createFromGEP(GetElementPtrInst *GEP);
    const SCEVExpr *createFromCast(Instruction *I, SCEVCastKind CK);

    const SCEVExpr *tryHeaderPhi(PhiInst *Phi, const Loop *L);
    std::optional<UpdateDelta> findAdditiveDelta(PhiInst *Self, Value *V,
                                                 const Loop *L);
    std::optional<UpdateDelta> findAdditiveDeltaImpl(
        PhiInst *Self, Value *V, const Loop *L,
        std::unordered_set<Value *> &Active, unsigned Depth);
    const SCEVExpr *trySimpleMulDivRec(PhiInst *Self, Value *Update,
                                       Value *Start, const Loop *L);

    bool isLoopInvariantExpr(const SCEVExpr *E, const Loop *L) const;
    bool isPolynomialAddRec(const SCEVExpr *E, const Loop *L) const;
    bool isSafeToDiscard(const SCEVExpr *E) const;
    bool isSafeToDiscardImpl(
        const SCEVExpr *E,
        std::unordered_set<const SCEVExpr *> &Active,
        unsigned Depth) const;
    bool isDirectLoopBlock(const Loop *L, BasicBlock *BB) const;
    bool sameExpr(const SCEVExpr *A, const SCEVExpr *B) const;
    std::optional<int64_t> asConstant(const SCEVExpr *E) const;
    std::string typeKey(std::shared_ptr<IRType> Ty) const;
    std::optional<IntRange> evaluateRangeForDirection(
        const SCEVExpr *E, BasicBlock *Context, RangeInfo &RI,
        DominatorTree &DT, std::unordered_set<const SCEVExpr *> &Active,
        unsigned Depth) const;
    SCEVStepSign classifyStepSign(const IntRange &Range) const;

    std::vector<BasicBlock *> collectDirectLoopBlocks(const Loop *L) const;
    const SCEVExpr *computeTripCount(const Loop *L);
    const SCEVExpr *tripFromComparison(const Loop *L, BinaryInst *Cmp,
                                       bool ContinueOnTrue);
    bool fillTripCountFromComparison(LoopTripCountInfo &Info);
    bool hasStrictUnitStepExit(
        const InductionDescriptor &Induction,
        const LoopTripCountInfo &Trip) const;
    bool isBackedgeUpdateStartSafe(
        const Loop *L, const InductionDescriptor &Induction,
        const LoopTripCountInfo &Trip);
    PhiInst *findPhiForAddRec(const Loop *L, const SCEVExpr *AddRec);
    PhiInst *findPhiForAffineRec(const Loop *L, const SCEVAffineInfo &Affine);
};

// Pass 入口获取循环树和支配树，表达式及循环事实随后按需生成。
class SCEVAnalysis : public AnalysisDescriptor<SCEVAnalysis> {
public:
    using Result = SCEVResult;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<SCEVAnalysis>;
    inline static PassID Key;
};
