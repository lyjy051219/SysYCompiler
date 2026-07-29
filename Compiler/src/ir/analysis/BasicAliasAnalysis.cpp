#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "lib/IRUtils.hpp"
#include "lib/Target.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
MemoryLocation::MemoryLocation() = default;

MemoryLocation::MemoryLocation(Value *P, std::optional<std::size_t> S)
    : Pointer(P), Size(S) {}

bool PointerProvenanceInfo::hasUniqueBase() const {
    return !HasUnknownBase && Bases.size() == 1;
}

Value *PointerProvenanceInfo::getUniqueBase() const {
    return hasUniqueBase() ? *Bases.begin() : nullptr;
}

bool PointerProvenanceInfo::hasExactByteOffset() const {
    return MinByteOffset && MaxByteOffset &&
           *MinByteOffset == *MaxByteOffset;
}

std::optional<long long> PointerProvenanceInfo::getExactByteOffset() const {
    return hasExactByteOffset() ? MinByteOffset : std::nullopt;
}

bool PointerProvenanceInfo::isMustStack() const {
    return !HasUnknownBase && !Bases.empty() && MayBeStack &&
           !MayBeGlobal && !MayBeParameter;
}

bool PointerProvenanceInfo::isMustGlobal() const {
    return !HasUnknownBase && !Bases.empty() && MayBeGlobal &&
           !MayBeStack && !MayBeParameter;
}

bool PointerProvenanceInfo::isMustParameter() const {
    return !HasUnknownBase && !Bases.empty() && MayBeParameter &&
           !MayBeStack && !MayBeGlobal;
}

BasicAAResult::BasicAAResult() = default;

BasicAAResult::BasicAAResult(SCEVResult *SCEV) : SE(SCEV) {}

BasicAAResult::BasicAAResult(BasicAAResult &&) = default;

BasicAAResult &BasicAAResult::operator=(BasicAAResult &&) = default;

std::size_t BasicAAResult::ValuePairHash::operator()(
    const std::pair<Value *, Value *> &P) const {
    auto H1 = static_cast<std::size_t>(
        reinterpret_cast<std::uintptr_t>(P.first));
    auto H2 = static_cast<std::size_t>(
        reinterpret_cast<std::uintptr_t>(P.second));
    return H1 ^ (H2 + 0x9e3779b97f4a7c15ULL + (H1 << 6) + (H1 >> 2));
}

bool BasicAAResult::checkedAdd(long long A, long long B, long long &Result) {
    if ((B > 0 && A > std::numeric_limits<long long>::max() - B) ||
        (B < 0 && A < std::numeric_limits<long long>::min() - B))
        return false;
    Result = A + B;
    return true;
}

bool BasicAAResult::checkedSub(long long A, long long B, long long &Result) {
    if (B == std::numeric_limits<long long>::min()) {
        if (A >= 0) return false;
        Result = A - B;
        return true;
    }
    return checkedAdd(A, -B, Result);
}

bool BasicAAResult::checkedMul(long long A, long long B, long long &Result) {
    if (A == 0 || B == 0) {
        Result = 0;
        return true;
    }
    if ((A == -1 && B == std::numeric_limits<long long>::min()) ||
        (B == -1 && A == std::numeric_limits<long long>::min()))
        return false;
    if (A > 0) {
        if (B > 0) {
            if (A > std::numeric_limits<long long>::max() / B) return false;
        } else if (B < std::numeric_limits<long long>::min() / A) {
            return false;
        }
    } else {
        if (B > 0) {
            if (A < std::numeric_limits<long long>::min() / B) return false;
        } else if (A < std::numeric_limits<long long>::max() / B) {
            return false;
        }
    }
    Result = A * B;
    return true;
}

bool BasicAAResult::isEmptyCycleProvenance(
    const PointerProvenanceInfo &Info) {
    return Info.Bases.empty() && !Info.HasUnknownBase &&
           Info.KnownAlignment == 0;
}

bool BasicAAResult::addOffsetToProvenance(PointerProvenanceInfo &Info,
                                           long long Offset) {
    if (!Info.MinByteOffset || !Info.MaxByteOffset) {
        Info.MinByteOffset.reset();
        Info.MaxByteOffset.reset();
        return false;
    }
    long long NewMin = 0;
    long long NewMax = 0;
    if (!checkedAdd(*Info.MinByteOffset, Offset, NewMin) ||
        !checkedAdd(*Info.MaxByteOffset, Offset, NewMax)) {
        Info.MinByteOffset.reset();
        Info.MaxByteOffset.reset();
        return false;
    }
    Info.MinByteOffset = NewMin;
    Info.MaxByteOffset = NewMax;
    return true;
}

std::size_t BasicAAResult::baseObjectAlignment(Value *Base) {
    if (!Base) return 1;
    const DataLayout &Layout = TargetInfo::get().getDataLayout();
    std::shared_ptr<IRType> ObjectType;
    if (auto *AI = dynamic_cast<AllocaInst *>(Base))
        ObjectType = AI->getAllocatedType();
    else if (dynamic_cast<Variable *>(Base))
        ObjectType = Value::getPointeeType(Base);
    else
        return 1;
    try {
        return std::max<std::size_t>(
            1, Layout.getABITypeAlign(ObjectType));
    } catch (const std::overflow_error &) {
        // 非法或宿主无法表示的聚合类型不能让分析阶段终止编译。
        return 1;
    }
}

std::optional<std::size_t> BasicAAResult::baseObjectSize(Value *Base) {
    if (!Base) return std::nullopt;
    const DataLayout &Layout = TargetInfo::get().getDataLayout();
    std::shared_ptr<IRType> ObjectType;
    if (auto *AI = dynamic_cast<AllocaInst *>(Base))
        ObjectType = AI->getAllocatedType();
    else if (dynamic_cast<Variable *>(Base))
        ObjectType = Value::getPointeeType(Base);
    else
        return std::nullopt;
    if (!ObjectType) return std::nullopt;
    try {
        const std::size_t Size = Layout.getTypeAllocSize(ObjectType);
        // 未定长对象在 DataLayout 中以零表示，不能把它误当成确定的空对象。
        return Size == 0 ? std::nullopt
                         : std::optional<std::size_t>(Size);
    } catch (const std::overflow_error &) {
        return std::nullopt;
    }
}

std::size_t BasicAAResult::greatestPowerOfTwoDivisor(
    std::uint64_t Value) {
    if (Value == 0) return 0;
    const std::uint64_t LowBit = Value & (~Value + 1U);
    if (LowBit > std::numeric_limits<std::size_t>::max())
        return std::size_t{1}
               << (std::numeric_limits<std::size_t>::digits - 1U);
    return static_cast<std::size_t>(LowBit);
}

std::size_t BasicAAResult::multiplyAlignmentFactors(
    std::size_t First, std::size_t Second) {
    // 零表示值本身恒为零；零乘任意值仍然不限制可整除的二次幂。
    if (First == 0 || Second == 0) return 0;
    if (First > std::numeric_limits<std::size_t>::max() / Second)
        return std::size_t{1}
               << (std::numeric_limits<std::size_t>::digits - 1U);
    const std::size_t Product = First * Second;
    return greatestPowerOfTwoDivisor(Product);
}

// 整数倍数查询只追踪“至少被哪个二次幂整除”，不猜测值域或运行时输入。
std::size_t BasicAAResult::getKnownIntegerMultiple(
    Value *V, IntegerMultipleQueryState &State, unsigned Depth) const {
    if (!V || Depth > MaxProvenanceDepth) return 1;
    auto Completed = State.Completed.find(V);
    if (Completed != State.Completed.end()) return Completed->second;
    if (State.VisitedNodes >= MaxIntegerMultipleQueryNodes) {
        State.BudgetExceeded = true;
        return 1;
    }
    ++State.VisitedNodes;

    if (auto *CI = dynamic_cast<ConstInt *>(V)) {
        const int64_t Signed = CI->getVal();
        const std::uint64_t Magnitude =
            Signed < 0
                ? static_cast<std::uint64_t>(-(Signed + 1)) + 1U
                : static_cast<std::uint64_t>(Signed);
        const std::size_t Result =
            greatestPowerOfTwoDivisor(Magnitude);
        State.Completed.emplace(V, Result);
        return Result;
    }

    ScopedActiveMark ActiveMark(State.Active, V);
    if (!ActiveMark.inserted()) return 1;

    std::size_t Result = 1;
    if (auto *Phi = dynamic_cast<PhiInst *>(V)) {
        std::size_t Common = 0;
        for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
            const std::size_t Incoming = getKnownIntegerMultiple(
                Phi->getIncomingValue(I), State, Depth + 1);
            Common = Common == 0 ? Incoming : std::gcd(Common, Incoming);
            if (Common == 1) break;
        }
        Result = Common == 0 ? 1 : Common;
    } else if (auto *Select = dynamic_cast<SelectInst *>(V)) {
        const std::size_t OnTrue = getKnownIntegerMultiple(
            Select->getTrueValue(), State, Depth + 1);
        const std::size_t OnFalse = getKnownIntegerMultiple(
            Select->getFalseValue(), State, Depth + 1);
        Result = std::gcd(OnTrue, OnFalse);
    } else if (auto *BI = dynamic_cast<BinaryInst *>(V)) {
        Value *LHS = BI->getOperand(0);
        Value *RHS = BI->getOperand(1);
        const std::size_t Left =
            getKnownIntegerMultiple(LHS, State, Depth + 1);
        const std::size_t Right =
            getKnownIntegerMultiple(RHS, State, Depth + 1);

        switch (BI->getOp()) {
        case BinaryInst::ADD:
        case BinaryInst::SUB:
        case BinaryInst::OR:
        case BinaryInst::XOR:
            Result = std::gcd(Left, Right);
            break;
        case BinaryInst::MUL:
            Result = multiplyAlignmentFactors(Left, Right);
            break;
        case BinaryInst::AND:
            Result = std::max(Left, Right);
            break;
        case BinaryInst::SHL: {
            int64_t Shift = 0;
            const std::shared_ptr<IRType> ValueType =
                LHS ? LHS->getIRType() : nullptr;
            const int BitWidth =
                ValueType ? ValueType->getIntegerBitWidth(0) : 0;
            if (!RHS || !RHS->tryGetInt64Const(Shift) || BitWidth <= 0 ||
                Shift < 0 || Shift >= BitWidth ||
                Shift >= static_cast<int64_t>(
                             std::numeric_limits<std::size_t>::digits)) {
                Result = 1;
                break;
            }
            const std::size_t ShiftFactor =
                std::size_t{1} << static_cast<unsigned>(Shift);
            Result = multiplyAlignmentFactors(Left, ShiftFactor);
            break;
        }
        default:
            Result = 1;
            break;
        }
    }

    // 活动环带来的 1 是保守下界，缓存它不会制造更强的对齐事实。
    State.Completed.emplace(V, Result);
    return Result;
}

// GEP 偏移对齐综合类型步长与下标的已知低零位，完全复用 CFG 的类型化步长接口。
std::size_t BasicAAResult::getGEPOffsetAlignment(
    GetElementPtrInst *GEP) const {
    if (!GEP) return 1;
    Value *Base = nullptr;
    std::vector<Value *> Indices;
    if (!GEP->getBaseAndIndices(Base, Indices)) return 1;
    std::vector<int64_t> Strides;
    int64_t FinalPointeeStride = 0;
    if (!GetElementPtrInst::getIndexStrides(
            Base, Indices, Strides, FinalPointeeStride) ||
        Strides.size() != Indices.size()) {
        return 1;
    }
    (void)FinalPointeeStride;

    std::size_t OffsetMultiple = 0;
    IntegerMultipleQueryState State;
    for (std::size_t I = 0; I < Indices.size(); ++I) {
        const std::uint64_t Magnitude =
            Strides[I] < 0
                ? static_cast<std::uint64_t>(-(Strides[I] + 1)) + 1U
                : static_cast<std::uint64_t>(Strides[I]);
        const std::size_t StrideMultiple =
            greatestPowerOfTwoDivisor(Magnitude);
        const std::size_t IndexMultiple =
            getKnownIntegerMultiple(Indices[I], State, 0);
        const std::size_t TermMultiple =
            multiplyAlignmentFactors(StrideMultiple, IndexMultiple);
        OffsetMultiple = OffsetMultiple == 0
            ? TermMultiple
            : std::gcd(OffsetMultiple, TermMultiple);
    }
    return OffsetMultiple;
}

PointerProvenanceInfo BasicAAResult::mergePointerProvenance(
    const PointerProvenanceInfo &First,
    const PointerProvenanceInfo &Second) const {
    if (isEmptyCycleProvenance(First) ||
        isEmptyCycleProvenance(Second)) {
        PointerProvenanceInfo Result =
            isEmptyCycleProvenance(First) ? Second : First;
        // 合流边回到正在求解的指针时，环外来源仍然有效，但本次迭代
        // 可能改变偏移和对齐，不能沿用入口边的精确数值事实。
        Result.MinByteOffset.reset();
        Result.MaxByteOffset.reset();
        Result.KnownAlignment = 1;
        return Result;
    }

    PointerProvenanceInfo Result;
    Result.HasUnknownBase = First.HasUnknownBase || Second.HasUnknownBase;
    Result.MayBeStack = First.MayBeStack || Second.MayBeStack;
    Result.MayBeGlobal = First.MayBeGlobal || Second.MayBeGlobal;
    Result.MayBeParameter =
        First.MayBeParameter || Second.MayBeParameter;
    Result.Bases = First.Bases;
    Result.Bases.insert(Second.Bases.begin(), Second.Bases.end());
    if (Result.Bases.size() > MaxProvenanceBases) {
        Result.Bases.clear();
        Result.HasUnknownBase = true;
    }

    if (First.MinByteOffset && First.MaxByteOffset &&
        Second.MinByteOffset && Second.MaxByteOffset) {
        Result.MinByteOffset =
            std::min(*First.MinByteOffset, *Second.MinByteOffset);
        Result.MaxByteOffset =
            std::max(*First.MaxByteOffset, *Second.MaxByteOffset);
    }
    if (First.ObjectSize && Second.ObjectSize &&
        *First.ObjectSize == *Second.ObjectSize) {
        Result.ObjectSize = First.ObjectSize;
    }

    if (First.KnownAlignment == 0)
        Result.KnownAlignment = Second.KnownAlignment;
    else if (Second.KnownAlignment == 0)
        Result.KnownAlignment = First.KnownAlignment;
    else
        Result.KnownAlignment =
            std::gcd(First.KnownAlignment, Second.KnownAlignment);
    if (Result.KnownAlignment == 0) Result.KnownAlignment = 1;
    return Result;
}

// 来源构建穿过 GEP、BitCast、PHI 和 Select；活动环本身不制造未知基址，
// 只有环外仍找不到真实来源时才退化为 unknown。
BasicAAResult::ProvenanceBuildResult
BasicAAResult::buildPointerProvenance(
    Value *V, ProvenanceQueryState &State, unsigned Depth) const {
    PointerProvenanceInfo Unknown;
    if (!V || !V->isPointerValue() || Value::isUndef(V) ||
        Depth > MaxProvenanceDepth) {
        return {std::move(Unknown), false};
    }

    auto Cached = ProvenanceCache.find(V);
    if (Cached != ProvenanceCache.end())
        return {Cached->second, false};
    auto Completed = State.Completed.find(V);
    if (Completed != State.Completed.end())
        return {Completed->second, false};
    if (State.VisitedNodes >= MaxProvenanceQueryNodes) {
        State.BudgetExceeded = true;
        return {std::move(Unknown), false};
    }
    ++State.VisitedNodes;

    ScopedActiveMark ActiveMark(State.Active, V);
    if (!ActiveMark.inserted()) {
        PointerProvenanceInfo Cycle;
        Cycle.HasUnknownBase = false;
        Cycle.KnownAlignment = 0;
        return {std::move(Cycle), true};
    }

    auto Finish = [&](PointerProvenanceInfo Info,
                      bool DependsOnCycle) {
        if (!DependsOnCycle && !isEmptyCycleProvenance(Info))
            State.Completed.emplace(V, Info);
        return ProvenanceBuildResult{
            std::move(Info), DependsOnCycle};
    };

    if (dynamic_cast<AllocaInst *>(V) || dynamic_cast<Variable *>(V) ||
        dynamic_cast<Param *>(V)) {
        PointerProvenanceInfo Root;
        Root.Bases.insert(V);
        Root.MinByteOffset = 0;
        Root.MaxByteOffset = 0;
        Root.ObjectSize = baseObjectSize(V);
        Root.KnownAlignment = baseObjectAlignment(V);
        Root.HasUnknownBase = false;
        Root.MayBeStack = dynamic_cast<AllocaInst *>(V) != nullptr;
        Root.MayBeGlobal = dynamic_cast<Variable *>(V) != nullptr;
        Root.MayBeParameter = dynamic_cast<Param *>(V) != nullptr;
        return Finish(std::move(Root), false);
    }

    if (auto *BitCast = dynamic_cast<BitCastInst *>(V)) {
        ProvenanceBuildResult Operand = buildPointerProvenance(
            BitCast->getUnaryOperand(), State, Depth + 1);
        return Finish(std::move(Operand.Info),
                      Operand.DependsOnActiveCycle);
    }

    if (auto *GEP = dynamic_cast<GetElementPtrInst *>(V)) {
        ProvenanceBuildResult BaseResult = buildPointerProvenance(
            GEP->getBasePointer(), State, Depth + 1);
        PointerProvenanceInfo Result = std::move(BaseResult.Info);
        // 指针归纳的回边形如 p.next = gep p, step。这里把“尚未走到
        // 环外根”继续向上传递，由 PHI 在看到入口来源后完成保守合流。
        if (isEmptyCycleProvenance(Result))
            return {std::move(Result), true};

        const std::size_t OffsetAlignment = getGEPOffsetAlignment(GEP);
        if (OffsetAlignment != 0)
            Result.KnownAlignment =
                std::gcd(std::max<std::size_t>(1, Result.KnownAlignment),
                         OffsetAlignment);

        Value *Base = nullptr;
        std::vector<Value *> Indices;
        std::vector<int64_t> Strides;
        int64_t FinalPointeeStride = 0;
        bool ConstantOffsetKnown =
            GEP->getBaseAndIndices(Base, Indices) &&
            GetElementPtrInst::getIndexStrides(
                Base, Indices, Strides, FinalPointeeStride) &&
            Strides.size() == Indices.size();
        long long ConstantOffset = 0;
        for (std::size_t I = 0;
             ConstantOffsetKnown && I < Indices.size(); ++I) {
            int64_t Index = 0;
            long long Term = 0;
            long long Updated = 0;
            if (!Indices[I] ||
                !Indices[I]->tryGetInt64Const(Index) ||
                !checkedMul(Strides[I], Index, Term) ||
                !checkedAdd(ConstantOffset, Term, Updated)) {
                ConstantOffsetKnown = false;
                break;
            }
            ConstantOffset = Updated;
        }
        (void)FinalPointeeStride;
        if (ConstantOffsetKnown)
            addOffsetToProvenance(Result, ConstantOffset);
        else {
            Result.MinByteOffset.reset();
            Result.MaxByteOffset.reset();
        }
        if (BaseResult.DependsOnActiveCycle) {
            // 从回边更新节点本身开始查询时，当前这次 GEP 只是递推中的
            // 一步，并不代表 PHI 在所有迭代上的固定偏移。显式清除数值
            // 事实，保证查询顺序不会把 root+step 缓存成整个递推的偏移。
            Result.MinByteOffset.reset();
            Result.MaxByteOffset.reset();
            Result.KnownAlignment = 1;
        }
        return Finish(std::move(Result),
                      BaseResult.DependsOnActiveCycle);
    }

    if (auto *Phi = dynamic_cast<PhiInst *>(V)) {
        PointerProvenanceInfo Result;
        Result.HasUnknownBase = false;
        Result.KnownAlignment = 0;
        bool SawSource = false;
        bool SawCycle = false;
        bool DependsOnCycle = false;
        for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
            ProvenanceBuildResult IncomingResult =
                buildPointerProvenance(
                    Phi->getIncomingValue(I), State, Depth + 1);
            PointerProvenanceInfo Incoming =
                std::move(IncomingResult.Info);
            DependsOnCycle =
                DependsOnCycle ||
                IncomingResult.DependsOnActiveCycle;
            if (isEmptyCycleProvenance(Incoming)) {
                SawCycle = true;
                continue;
            }
            Result = SawSource
                ? mergePointerProvenance(Result, Incoming)
                : Incoming;
            SawSource = true;
        }
        if (SawSource && SawCycle) {
            Result.MinByteOffset.reset();
            Result.MaxByteOffset.reset();
            Result.KnownAlignment = 1;
        }
        if (!SawSource)
            return Finish(std::move(Unknown), DependsOnCycle);
        return Finish(std::move(Result), DependsOnCycle);
    }

    if (auto *Select = dynamic_cast<SelectInst *>(V)) {
        if (Select->getTrueValue() == Select->getFalseValue()) {
            ProvenanceBuildResult Arm = buildPointerProvenance(
                Select->getTrueValue(), State, Depth + 1);
            return Finish(std::move(Arm.Info),
                          Arm.DependsOnActiveCycle);
        }
        ProvenanceBuildResult TrueResult = buildPointerProvenance(
            Select->getTrueValue(), State, Depth + 1);
        ProvenanceBuildResult FalseResult = buildPointerProvenance(
            Select->getFalseValue(), State, Depth + 1);
        PointerProvenanceInfo OnTrue = std::move(TrueResult.Info);
        PointerProvenanceInfo OnFalse = std::move(FalseResult.Info);
        const bool DependsOnCycle =
            TrueResult.DependsOnActiveCycle ||
            FalseResult.DependsOnActiveCycle;
        if (isEmptyCycleProvenance(OnTrue) &&
            isEmptyCycleProvenance(OnFalse)) {
            return Finish(std::move(Unknown), DependsOnCycle);
        }
        return Finish(
            mergePointerProvenance(OnTrue, OnFalse),
            DependsOnCycle);
    }
    return Finish(std::move(Unknown), false);
}

const PointerProvenanceInfo &
BasicAAResult::getPointerProvenance(Value *V) const {
    auto It = ProvenanceCache.find(V);
    if (It != ProvenanceCache.end()) return It->second;
    ProvenanceQueryState State;
    ProvenanceBuildResult Built =
        buildPointerProvenance(V, State, 0);
    PointerProvenanceInfo Result = std::move(Built.Info);
    if (isEmptyCycleProvenance(Result)) Result = PointerProvenanceInfo{};
    Result.KnownAlignment =
        std::max<std::size_t>(1, Result.KnownAlignment);
    return ProvenanceCache.emplace(V, std::move(Result)).first->second;
}

std::vector<Value *>
BasicAAResult::getUnderlyingObjects(Value *V) const {
    const PointerProvenanceInfo &Info = getPointerProvenance(V);
    std::vector<Value *> Result(Info.Bases.begin(), Info.Bases.end());
    std::sort(Result.begin(), Result.end(),
              [](Value *First, Value *Second) {
                  return std::less<Value *>{}(First, Second);
              });
    return Result;
}

Value *BasicAAResult::getUniqueUnderlyingObject(Value *V) const {
    return getPointerProvenance(V).getUniqueBase();
}

std::optional<long long>
BasicAAResult::getConstantByteOffset(Value *V) const {
    return getPointerProvenance(V).getExactByteOffset();
}

std::optional<std::size_t>
BasicAAResult::getObjectSize(Value *V) const {
    return getPointerProvenance(V).ObjectSize;
}

std::size_t BasicAAResult::getKnownAlignment(Value *V) const {
    return getPointerProvenance(V).KnownAlignment;
}

// 对象内证明只消费来源分析已经发布的完整偏移区间；动态下标、未知访问
// 宽度或未定长形参都保持未知，不能借“不同对象”隐式利用越界行为。
bool BasicAAResult::isLocationKnownWithinObject(
    const MemoryLocation &Location) const {
    if (!Location.Pointer || !Location.Size) return false;
    if (*Location.Size == 0) return true;

    const PointerProvenanceInfo &Info =
        getPointerProvenance(Location.Pointer);
    if (Info.HasUnknownBase || Info.Bases.empty() || !Info.ObjectSize ||
        !Info.MinByteOffset || !Info.MaxByteOffset ||
        *Info.MinByteOffset < 0 ||
        *Info.MaxByteOffset < *Info.MinByteOffset ||
        *Location.Size > *Info.ObjectSize) {
        return false;
    }

    const unsigned long long MaxOffset =
        static_cast<unsigned long long>(*Info.MaxByteOffset);
    if (MaxOffset >
        static_cast<unsigned long long>(
            std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    return static_cast<std::size_t>(MaxOffset) <=
           *Info.ObjectSize - *Location.Size;
}

AliasInfo BasicAAResult::enforceLocationObjectBounds(
    AliasInfo Result, const MemoryLocation &First,
    const MemoryLocation &Second) const {
    if (Result != AliasInfo::NoAlias) return Result;
    if ((First.Size && *First.Size == 0) ||
        (Second.Size && *Second.Size == 0)) {
        return Result;
    }

    const PointerProvenanceInfo &FirstInfo =
        getPointerProvenance(First.Pointer);
    const PointerProvenanceInfo &SecondInfo =
        getPointerProvenance(Second.Pointer);
    Value *FirstBase = FirstInfo.getUniqueBase();
    Value *SecondBase = SecondInfo.getUniqueBase();

    // 同一唯一对象内的常量/范围距离证明不依赖对象身份；只有不同或
    // 多来源对象带来的 NoAlias 才必须额外证明两次访问都在对象内。
    if (FirstBase && FirstBase == SecondBase) return Result;
    if (isLocationKnownWithinObject(First) &&
        isLocationKnownWithinObject(Second)) {
        return Result;
    }
    return AliasInfo::MayAlias;
}

// 防御性溯源只穿过透明指针节点；发现 undef、深度超限时禁止发布强别名结论。
bool BasicAAResult::hasUndefPointerOrigin(
    Value *V, std::unordered_set<Value *> &Active, unsigned Depth) {
    if (!V || Value::isUndef(V)) return true;
    if (Depth > 32) return true;

    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return false;
    const bool TransparentPointerProducer =
        dynamic_cast<GetElementPtrInst *>(I) ||
        dynamic_cast<BitCastInst *>(I) ||
        dynamic_cast<PhiInst *>(I) ||
        dynamic_cast<SelectInst *>(I);
    if (!TransparentPointerProducer) return false;
    ScopedActiveMark ActiveMark(Active, V);
    if (!ActiveMark.inserted()) return false;

    bool HasUndefOrigin = false;
    forEachOperand(
        I, [&](size_t, Use *, Value *Operand) {
            if (dynamic_cast<BasicBlock *>(Operand)) return true;
            if (!hasUndefPointerOrigin(Operand, Active, Depth + 1))
                return true;
            HasUndefOrigin = true;
            return false;
        });
    return HasUndefOrigin;
}

MemoryLocation MemoryLocation::get(const LoadInst *Load) {
    if (!Load) return {};
    std::shared_ptr<IRType> Ty = Load->getIRType();
    return {Load->getPointerOperand(),
            Ty ? std::optional<std::size_t>(Ty->getSize()) : std::nullopt};
}

MemoryLocation MemoryLocation::get(const StoreInst *Store) {
    if (!Store) return {};
    Value *Stored = Store->getValueOperand();
    std::shared_ptr<IRType> Ty = Stored ? Stored->getIRType() : nullptr;
    return {Store->getPointerOperand(),
            Ty ? std::optional<std::size_t>(Ty->getSize()) : std::nullopt};
}

bool BasicAAResult::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(BasicAliasAnalysis::ID()) &&
        (!SE || PA.isKept(SCEVAnalysis::ID()))) {
        return false;
    }
    (void)F;
    AliasCache.clear();
    ProvenanceCache.clear();
    return true;
}

void BasicAAResult::addObjectNoAliasFact(Value* V1, Value* V2) {
    if (!V1 || !V2 || V1 == V2) {
        return;
    }
    ObjectNoAlias.insert(makeCanonicalPair(V1, V2));
}

bool BasicAAResult::hasObjectNoAlias(Value* V1, Value* V2) const {
    if (!V1 || !V2 || V1 == V2) {
        return false;
    }
    return ObjectNoAlias.find(makeCanonicalPair(V1, V2)) != ObjectNoAlias.end();
}

bool BasicAAResult::callMayAliasPointer(CallInst *CI, Value *P,
                                        const LoopInfo &LI) {
    if (!CI || !P) return false;
    (void)LI;
    for (unsigned I = 0; I < CI->getArgCount(); ++I) {
        Value *Arg = CI->getArg(I);
        if (!Arg || !Arg->isPointerValue()) continue;

        if (!haveKnownDistinctUnderlyingObjects(P, Arg)) {
            return true;
        }
    }
    return false;
}

std::size_t BasicAAResult::getPointeeAccessSize(Value* V) const {
    std::shared_ptr<IRType> Pointee =
        V && V->getIRType() ? V->getIRType()->getPointeeType() : nullptr;
    return Pointee ? Pointee->getSize() : 0;
}

// 查询规范化只剥离零偏移 GEP 和单入 PHI，保留影响访问宽度的 BitCast。
Value* BasicAAResult::stripBaseInst(Value* V) const {
    std::unordered_set<Value*> Visited;
    while (V && Visited.insert(V).second) {
        if (!V->isPointerValue()) {
            return V;
        }

        if (auto *GEP = dynamic_cast<GetElementPtrInst*>(V)) {
            bool AllZero = true;
            for (Value *Idx : GEP->getIndices()) {
                if (!Idx || !Idx->isConstIntZero()) {
                    AllZero = false;
                    break;
                }
            }
            if (!AllZero) {
                return V;
            }
            V = GEP->getBasePointer();
            continue;
        }

        if (dynamic_cast<BitCastInst*>(V)) return V;

        if (auto *Phi = dynamic_cast<PhiInst*>(V)) {
            if (Phi->getNumIncomingValues() == 1) {
                V = Phi->getIncomingValue(0);
                continue;
            }
        }

        return V;
    }
    return V;
}

// 对象身份溯源穿过全部 GEP、BitCast 和单入 PHI，并用 visited 截断环。
Value* BasicAAResult::getBaseAddress(Value* V) const {
    std::unordered_set<Value*> Visited;
    while (V && Visited.insert(V).second) {
        if (auto *GEP = dynamic_cast<GetElementPtrInst*>(V)) {
            V = GEP->getBasePointer();
            continue;
        }

        if (auto *BC = dynamic_cast<BitCastInst*>(V)) {
            V = BC->getUnaryOperand();
            continue;
        }

        if (auto *Phi = dynamic_cast<PhiInst*>(V)) {
            if (Phi->getNumIncomingValues() == 1) {
                V = Phi->getIncomingValue(0);
                continue;
            }
        }

        return V;
    }
    return V;
}

bool BasicAAResult::haveKnownDistinctUnderlyingObjects(Value* V1,
                                                        Value* V2) const {
    auto AreDistinctRoots = [&](Value *O1, Value *O2) {
        if (!O1 || !O2 || O1 == O2 || O1->isUndef() || O2->isUndef())
            return false;

        auto *Var1 = dynamic_cast<Variable *>(O1);
        auto *Var2 = dynamic_cast<Variable *>(O2);
        const bool IsAlloca1 = dynamic_cast<AllocaInst *>(O1) != nullptr;
        const bool IsAlloca2 = dynamic_cast<AllocaInst *>(O2) != nullptr;
        const bool IsParam1 = O1->isParam();
        const bool IsParam2 = O2->isParam();
        const bool IsGlobal1 =
            Var1 && (Var1->vartag == Variable::GlobalVariable ||
                     Var1->vartag == Variable::GlobalConstant);
        const bool IsGlobal2 =
            Var2 && (Var2->vartag == Variable::GlobalVariable ||
                     Var2->vartag == Variable::GlobalConstant);

        if (IsAlloca1 && IsAlloca2) return true;
        if (IsGlobal1 && IsGlobal2) return true;
        if ((IsParam1 && IsAlloca2) || (IsParam2 && IsAlloca1))
            return true;
        if ((IsAlloca1 && IsGlobal2) || (IsAlloca2 && IsGlobal1))
            return true;
        return hasObjectNoAlias(O1, O2);
    };

    // 多来源 PHI/Select 只有在笛卡尔积中的每一对对象都互斥时才发布
    // NoAlias；任何未知来源都会阻止强结论。
    const PointerProvenanceInfo &First = getPointerProvenance(V1);
    const PointerProvenanceInfo &Second = getPointerProvenance(V2);
    if (!First.HasUnknownBase && !Second.HasUnknownBase &&
        !First.Bases.empty() && !Second.Bases.empty()) {
        bool AllDistinct = true;
        for (Value *O1 : First.Bases) {
            for (Value *O2 : Second.Bases) {
                if (!AreDistinctRoots(O1, O2)) {
                    AllDistinct = false;
                    break;
                }
            }
            if (!AllDistinct) break;
        }
        if (AllDistinct) return true;
    }

    return AreDistinctRoots(getBaseAddress(V1), getBaseAddress(V2));
}

bool BasicAAResult::isEqualAlias(Value* V1, Value* V2, const LoopInfo& LI) const {
    (void)LI;

    return V1 == V2;
}

AliasInfo BasicAAResult::mergePhiAlias(AliasInfo A, AliasInfo B) const {
    if (A == B) {
        return A;
    }
    if (A == AliasInfo::MayAlias || B == AliasInfo::MayAlias) {
        return AliasInfo::MayAlias;
    }
    return AliasInfo::MayAlias;
}

std::pair<Value*, Value*> BasicAAResult::makeCanonicalPair(Value* A, Value* B) const {
    return std::less<Value *>{}(B, A)
        ? std::make_pair(B, A)
        : std::make_pair(A, B);
}

// GEP 线性化把每个类型化下标换算成字节系数；类型缺失或算术溢出立即失败。
bool BasicAAResult::accumulateGEPIndices(GetElementPtrInst* GEP, LinearGEP& Expr) const {
    Value* BasePtr = GEP ? GEP->getBasePointer() : nullptr;
    if (!BasePtr || !BasePtr->getIRType()) {
        return false;
    }

    std::shared_ptr<IRType> CurType = BasePtr->getIRType();
    for (size_t i = 0; i < GEP->getNumIndices(); ++i) {
        CurType = CurType ? CurType->getIndexableElementType() : nullptr;
        if (!CurType) return false;
        std::size_t Stride = CurType->getSize();

        Value* Idx = GEP->getIndex(i);
        if (auto *CI = dynamic_cast<ConstInt*>(Idx)) {
            if (Stride > static_cast<std::size_t>(
                             std::numeric_limits<long long>::max()))
                return false;
            long long Scaled = 0;
            long long Updated = 0;
            if (!checkedMul(static_cast<long long>(CI->getVal()),
                            static_cast<long long>(Stride), Scaled) ||
                !checkedAdd(Expr.ConstantOffset, Scaled, Updated))
                return false;
            Expr.ConstantOffset = Updated;
        } else if (Idx) {
            if (Stride > static_cast<std::size_t>(
                             std::numeric_limits<long long>::max()))
                return false;
            long long Updated = 0;
            if (!checkedAdd(Expr.VariableOffsets[Idx],
                            static_cast<long long>(Stride), Updated))
                return false;
            Expr.VariableOffsets[Idx] = Updated;
        } else {
            return false;
        }
    }

    return true;
}

// 沿完整 GEP/BitCast 链累积线性表达式，最终 Base 必须是稳定对象。
BasicAAResult::LinearGEP BasicAAResult::fillLinearGEP(Value* V) const {
    LinearGEP Expr;
    std::unordered_set<Value*> Visited;

    while (V && Visited.insert(V).second) {
        if (auto *GEP = dynamic_cast<GetElementPtrInst*>(V)) {
            if (!accumulateGEPIndices(GEP, Expr)) {
                Expr.Valid = false;
                return Expr;
            }
            V = GEP->getBasePointer();
            continue;
        }

        if (auto *BC = dynamic_cast<BitCastInst*>(V)) {
            V = BC->getUnaryOperand();
            continue;
        }

        if (auto *Phi = dynamic_cast<PhiInst*>(V)) {
            if (Phi->getNumIncomingValues() == 1) {
                V = Phi->getIncomingValue(0);
                continue;
            }
        }

        Expr.Base = V;
        return Expr;
    }

    Expr.Valid = false;
    return Expr;
}

BasicAAResult::AdditiveDecomposition
BasicAAResult::decomposeAdditive(Value* V) const {
    AdditiveDecomposition Result;
    Result.Base = V;
    std::unordered_set<Value*> Visited;

    while (Result.Base && Visited.insert(Result.Base).second) {
        auto *BI = dynamic_cast<BinaryInst*>(Result.Base);
        if (!BI) {
            return Result;
        }

        Value* LHS = BI->getOperand(0);
        Value* RHS = BI->getOperand(1);
        auto *LC = dynamic_cast<ConstInt*>(LHS);
        auto *RC = dynamic_cast<ConstInt*>(RHS);

        if (BI->getOp() == BinaryInst::ADD) {
            if (RC) {
                long long Updated = 0;
                if (!checkedAdd(Result.Offset,
                                static_cast<long long>(RC->getVal()), Updated)) {
                    Result.Valid = false;
                    return Result;
                }
                Result.Offset = Updated;
                Result.Base = LHS;
                continue;
            }
            if (LC) {
                long long Updated = 0;
                if (!checkedAdd(Result.Offset,
                                static_cast<long long>(LC->getVal()), Updated)) {
                    Result.Valid = false;
                    return Result;
                }
                Result.Offset = Updated;
                Result.Base = RHS;
                continue;
            }
        } else if (BI->getOp() == BinaryInst::SUB && RC) {
            long long Updated = 0;
            if (!checkedSub(Result.Offset,
                            static_cast<long long>(RC->getVal()), Updated)) {
                Result.Valid = false;
                return Result;
            }
            Result.Offset = Updated;
            Result.Base = LHS;
            continue;
        }

        return Result;
    }

    Result.Valid = false;
    return Result;
}

std::optional<long long>
BasicAAResult::getAdditiveOffsetFrom(Value* V, Value* Base) const {
    if (!V || !Base) {
        return std::nullopt;
    }
    if (V == Base) {
        return 0;
    }

    auto Dec = decomposeAdditive(V);
    if (!Dec.Valid) {
        return std::nullopt;
    }
    if (Dec.Base == Base) {
        return Dec.Offset;
    }
    return std::nullopt;
}

bool BasicAAResult::isLoopInvariant(Value* V, const Loop* L) const {
    if (!V || !L) {
        return true;
    }
    if (V->isConst()) {
        return true;
    }
    if (V->isParam() || V->isGlobal()) return true;
    auto *I = dynamic_cast<Instruction*>(V);
    if (!I) {
        return true;
    }
    BasicBlock* BB = I->getParent();
    return !BB || !L->contains(BB);
}

// 循环距离辅助只接受 SCEV 证明有限、不回绕且正步长的规范归纳 PHI。
std::optional<BasicAAResult::PositivePhiRecurrence>
BasicAAResult::getPositivePhiRecurrence(PhiInst* Phi,
                                        const LoopInfo& LI) const {
    if (!Phi || !Phi->getParent()) {
        return std::nullopt;
    }

    Loop* L = LI.getLoopFor(Phi->getParent());
    if (!L || L->getHeader() != Phi->getParent()) {
        return std::nullopt;
    }

    if (!SE) return std::nullopt;
    auto Induction = SE->getCanonicalInduction(L);
    if (!Induction || !Induction->Valid || Induction->Phi != Phi ||
        !Induction->StartValue || !Induction->UpdateValue ||
        !Induction->ConstantStep || *Induction->ConstantStep <= 0 ||
        !SE->isKnownNoWrapInductionValue(L, Induction->UpdateValue)) {
        return std::nullopt;
    }
    return PositivePhiRecurrence{Induction->StartValue,
                                 Induction->UpdateValue,
                                 *Induction->ConstantStep, L};
}

// 下界证明把规范归纳更新与循环不变量起点配对；任意附加偏移不作无回绕猜测。
std::optional<long long>
BasicAAResult::lowerBoundDiff(Value* Greater, Value* Lesser,
                              const LoopInfo& LI) const {
    if (!Greater || !Lesser) {
        return std::nullopt;
    }

    if (Greater == Lesser) return 0;

    auto Dec = decomposeAdditive(Greater);
    if (!Dec.Valid) {
        return std::nullopt;
    }

    auto *Phi = dynamic_cast<PhiInst*>(Dec.Base);
    auto Rec = getPositivePhiRecurrence(Phi, LI);
    if (!Rec) {
        return std::nullopt;
    }

    if (!isLoopInvariant(Lesser, Rec->L)) {
        return std::nullopt;
    }

    auto StartOff = getAdditiveOffsetFrom(Rec->Start, Lesser);
    if (!StartOff || *StartOff != 0) {
        return std::nullopt;
    }

    if (Dec.Offset != 0 &&
        (Greater != Rec->Update || Dec.Offset != Rec->Step)) {
        return std::nullopt;
    }

    long long LB = 0;
    if (!checkedAdd(*StartOff, Dec.Offset, LB)) return std::nullopt;
    if (LB < 0) {
        return std::nullopt;
    }
    return LB;
}

// 变量下标距离查询先覆盖 i32 模环绕的显式差值，再尝试 SCEV 给出的非负距离下界。
AliasInfo BasicAAResult::aliasGEPByIndexDistance(
    const std::unordered_map<Value*, long long>& DiffVars,
    long long ConstDiff, std::size_t Threshold, const LoopInfo& LI) const {

    // 1. 先识别两个 SSA 下标系数互为相反数的仿射差值，例如 A[i+1] 与 A[i]。
    if (DiffVars.size() == 2 && Threshold > 0 &&
        Threshold <= static_cast<std::size_t>(
                         std::numeric_limits<long long>::max())) {
        auto It = DiffVars.begin();
        Value *A = It->first;
        long long Scale = It->second;
        ++It;
        Value *B = It->first;
        long long OtherScale = It->second;
        if (Scale != 0 &&
            Scale != std::numeric_limits<long long>::min() &&
            OtherScale == -Scale) {
            if (Scale < 0) {
                Scale = -Scale;
                std::swap(A, B);
            }

            std::optional<long long> Offset = getAdditiveOffsetFrom(A, B);
            if (!Offset) {
                if (auto Reverse = getAdditiveOffsetFrom(B, A);
                    Reverse &&
                    *Reverse != std::numeric_limits<long long>::min())
                    Offset = -*Reverse;
            }

            auto Ty = A ? A->getIRType() : nullptr;
            auto BTy = B ? B->getIRType() : nullptr;
            if (Offset && IRType::hasIntegerBitWidth(Ty, 32) &&
                IRType::hasIntegerBitWidth(BTy, 32)) {

                // 1.1 把链式 i32 加减归一到模 2^32 的全部有符号代表，全部分离才返回 NoAlias。
                long long Residue = *Offset % I32Modulus;
                if (Residue < 0) Residue += I32Modulus;
                std::vector<long long> Representatives{Residue};
                if (Residue != 0)
                    Representatives.push_back(Residue - I32Modulus);

                bool AllDisjoint = true;
                for (long long IndexDiff : Representatives) {
                    long long Scaled = 0;
                    long long ByteDiff = 0;
                    if (!checkedMul(Scale, IndexDiff, Scaled) ||
                        !checkedAdd(ConstDiff, Scaled, ByteDiff) ||
                        ByteDiff == std::numeric_limits<long long>::min() ||
                        std::llabs(ByteDiff) <
                            static_cast<long long>(Threshold)) {
                        AllDisjoint = false;
                        break;
                    }
                }
                if (AllDisjoint) return AliasInfo::NoAlias;
            }

            // 2. 模差值不足时，只消费 SCEV 已证明有限且不回绕的规范递推下界。
            if (auto Lower = lowerBoundDiff(A, B, LI)) {
                long long Scaled = 0;
                long long MinByteDiff = 0;
                if (checkedMul(Scale, *Lower, Scaled) &&
                    checkedAdd(ConstDiff, Scaled, MinByteDiff) &&
                    MinByteDiff >= static_cast<long long>(Threshold)) {
                    return AliasInfo::NoAlias;
                }
            }
            if (auto ReverseLower = lowerBoundDiff(B, A, LI)) {
                long long Scaled = 0;
                long long MaxByteDiff = 0;
                if (checkedMul(Scale, *ReverseLower, Scaled) &&
                    checkedSub(ConstDiff, Scaled, MaxByteDiff) &&
                    MaxByteDiff <= -static_cast<long long>(Threshold)) {
                    return AliasInfo::NoAlias;
                }
            }
        }
    }

    // 3. 非仿射、可能回绕、距离不足或算术溢出的路径均保持 MayAlias。
    return AliasInfo::MayAlias;
}

// 同对象 GEP 查询相减两条线性表达式，并用真实访问宽度判定相等或分离。
AliasInfo BasicAAResult::aliasGEP(
    Value* GEP1, Value* GEP2, const LoopInfo& LI, bool UseIndexRange,
    std::optional<std::size_t> AccessSize1,
    std::optional<std::size_t> AccessSize2) {
    // 1. 线性化两侧并要求底层对象一致；无法精确分解时退化为 MayAlias。
    auto E1 = fillLinearGEP(GEP1);
    auto E2 = fillLinearGEP(GEP2);

    if (!E1.Valid || !E2.Valid) {
        return AliasInfo::MayAlias;
    }
    if (E1.Base != E2.Base) {
        return AliasInfo::MayAlias;
    }

    // 2. 相减常量和变量字节偏移，任何宿主整数溢出都会终止证明。
    std::unordered_map<Value*, long long> DiffVars = E1.VariableOffsets;
    for (auto &KV : E2.VariableOffsets) {
        long long Updated = 0;
        if (!checkedSub(DiffVars[KV.first], KV.second, Updated))
            return AliasInfo::MayAlias;
        DiffVars[KV.first] = Updated;
    }
    for (auto It = DiffVars.begin(); It != DiffVars.end();) {
        if (It->second == 0) {
            It = DiffVars.erase(It);
        } else {
            ++It;
        }
    }

    long long ConstDiff = 0;
    if (!checkedSub(E1.ConstantOffset, E2.ConstantOffset, ConstDiff))
        return AliasInfo::MayAlias;

    std::size_t Access1 = AccessSize1.value_or(getPointeeAccessSize(GEP1));
    std::size_t Access2 = AccessSize2.value_or(getPointeeAccessSize(GEP2));

    if (Access1 == 0 || Access2 == 0)
        return ConstDiff == 0 && DiffVars.empty() ? AliasInfo::MustAlias
                                                  : AliasInfo::MayAlias;
    std::size_t Threshold = std::max<std::size_t>(1, std::max(Access1, Access2));
    if (Threshold > static_cast<std::size_t>(
                        std::numeric_limits<long long>::max()))
        return AliasInfo::MayAlias;

    // 3. 无变量项时直接比较字节区间；有变量项仅在显式启用范围证明时继续。
    if (!DiffVars.empty()) {
        if (UseIndexRange) {
            return aliasGEPByIndexDistance(DiffVars, ConstDiff, Threshold, LI);
        }
        return AliasInfo::MayAlias;
    }

    if (ConstDiff == 0) {
        return AliasInfo::MustAlias;
    }

    if (ConstDiff == std::numeric_limits<long long>::min())
        return AliasInfo::MayAlias;

    if ((ConstDiff > 0 &&
         ConstDiff >= static_cast<long long>(Access2)) ||
        (ConstDiff < 0 &&
         ConstDiff != std::numeric_limits<long long>::min() &&
         -ConstDiff >= static_cast<long long>(Access1))) {
        return AliasInfo::NoAlias;
    }
    return AliasInfo::MayAlias;
}

// PHI 查询按前驱语义配对 incoming；各路径结论不一致或结构异常时合并为 MayAlias。
AliasInfo BasicAAResult::aliasPHI(PhiInst* Phi, Value* Other, const LoopInfo& LI,
                                  bool UseIndexRange) {
    if (!Phi) {
        return AliasInfo::MayAlias;
    }
    unsigned N = Phi->getNumIncomingValues();
    if (N == 0) {
        return AliasInfo::MayAlias;
    }

    if (auto *OtherPhi = dynamic_cast<PhiInst*>(Other)) {
        if (OtherPhi->getParent() == Phi->getParent()) {
            unsigned M = OtherPhi->getNumIncomingValues();
            if (M == 0 || M != N) {
                return AliasInfo::MayAlias;
            }

            std::unordered_map<BasicBlock *, Value *> OtherByPred;
            if (!forEachPhiIncoming(
                    OtherPhi,
                    [&](unsigned, Value *Incoming, BasicBlock *Pred) {
                        return Pred &&
                               OtherByPred.emplace(Pred, Incoming).second;
                    })) {
                return AliasInfo::MayAlias;
            }

            std::unordered_set<BasicBlock *> SeenPreds;
            AliasInfo Acc = AliasInfo::MayAlias;
            bool First = true;
            bool InvalidIncoming = false;
            forEachPhiIncoming(
                Phi, [&](unsigned, Value *Incoming, BasicBlock *Pred) {
                    if (!Pred || !SeenPreds.insert(Pred).second) {
                        InvalidIncoming = true;
                        return false;
                    }
                    auto OtherIt = OtherByPred.find(Pred);
                    if (OtherIt == OtherByPred.end()) {
                        InvalidIncoming = true;
                        return false;
                    }

                    AliasInfo Cur = aliasImpl(Incoming, OtherIt->second, LI,
                                              UseIndexRange);
                    if (First) {
                        Acc = Cur;
                        First = false;
                        return true;
                    }
                    Acc = mergePhiAlias(Acc, Cur);
                    return Acc != AliasInfo::MayAlias;
                });
            if (InvalidIncoming) return AliasInfo::MayAlias;
            return Acc;
        }
    }

    AliasInfo Acc = aliasImpl(Phi->getIncomingValue(0), Other, LI, UseIndexRange);
    for (unsigned i = 1; i < N; ++i) {
        AliasInfo Cur = aliasImpl(Phi->getIncomingValue(i), Other, LI, UseIndexRange);
        Acc = mergePhiAlias(Acc, Cur);
        if (Acc == AliasInfo::MayAlias) {
            break;
        }
    }
    return Acc;
}

// Select 查询展开两条路径；条件相同的两个 Select 只比较对应分支并防止递归环。
AliasInfo BasicAAResult::aliasSelect(SelectInst *Select, Value *Other,
                                     const LoopInfo &LI,
                                     bool UseIndexRange) {
    if (!Select || !Other) return AliasInfo::MayAlias;

    auto Key = makeCanonicalPair(Select, Other);
    ScopedActiveMark ActiveMark(ActiveSelectQueries, Key);
    if (!ActiveMark.inserted())
        return AliasInfo::MayAlias;

    Value *TrueValue = Select->getOperand(1);
    Value *FalseValue = Select->getOperand(2);
    if (!TrueValue || !FalseValue)
        return AliasInfo::MayAlias;

    if (auto *OtherSelect = dynamic_cast<SelectInst *>(Other)) {
        if (Select->getOperand(0) == OtherSelect->getOperand(0)) {
            AliasInfo OnTrue = aliasImpl(TrueValue,
                                         OtherSelect->getOperand(1), LI,
                                         UseIndexRange);
            AliasInfo OnFalse = aliasImpl(FalseValue,
                                          OtherSelect->getOperand(2), LI,
                                          UseIndexRange);
            return mergePhiAlias(OnTrue, OnFalse);
        }
    }

    AliasInfo OnTrue = aliasImpl(TrueValue, Other, LI, UseIndexRange);
    AliasInfo OnFalse = aliasImpl(FalseValue, Other, LI, UseIndexRange);
    return mergePhiAlias(OnTrue, OnFalse);
}

AliasInfo BasicAAResult::alias(Value* V1, Value* V2, const LoopInfo& LI) {
    return aliasImpl(V1, V2, LI, false);
}

AliasInfo BasicAAResult::aliasWithIndexRange(Value* V1, Value* V2,
                                             const LoopInfo& LI) {
    return aliasImpl(V1, V2, LI, true);
}

// 区间查询在递归展开控制流指针时始终携带原访问宽度，并以 Active 集截断环。
AliasInfo BasicAAResult::aliasLocationImpl(
    const MemoryLocation &L1, const MemoryLocation &L2,
    const LoopInfo &LI, bool UseIndexRange, unsigned Depth,
    std::unordered_set<std::pair<Value *, Value *>, ValuePairHash> &Active) {
    auto NormalizeControlPointer = [](Value *P,
                                      const std::optional<std::size_t> &Size) {

        if (!Size) return P;
        std::unordered_set<Value *> Seen;
        while (P && Seen.insert(P).second) {
            if (auto *BC = dynamic_cast<BitCastInst *>(P)) {
                P = BC->getUnaryOperand();
                continue;
            }
            auto *GEP = dynamic_cast<GetElementPtrInst *>(P);
            if (!GEP || !GEP->hasOnlyConstantIndices()) break;
            bool AllZero = true;
            for (Value *Index : GEP->getIndices()) {
                if (!Index || !Index->isConstIntZero()) {
                    AllZero = false;
                    break;
                }
            }
            if (!AllZero) break;
            P = GEP->getBasePointer();
        }
        return P;
    };

    // 1. 显式宽度允许剥离零偏移包装；缺失指针、过深递归或活动环均保守失败。
    Value *P1 = NormalizeControlPointer(L1.Pointer, L1.Size);
    Value *P2 = NormalizeControlPointer(L2.Pointer, L2.Size);
    if (!P1 || !P2 || Depth > 32) return AliasInfo::MayAlias;

    if (P1 == P2) return aliasImpl(P1, P2, LI, UseIndexRange);

    auto Key = makeCanonicalPair(P1, P2);
    ScopedActiveMark ActiveMark(Active, Key);
    if (!ActiveMark.inserted()) return AliasInfo::MayAlias;
    auto Merge = [&](AliasInfo A, AliasInfo B) {
        return mergePhiAlias(A, B);
    };

    // 2. 优先按控制流相关性展开 Select 和 PHI，分别求各可达来源的区间关系。
    auto *S1 = dynamic_cast<SelectInst *>(P1);
    auto *S2 = dynamic_cast<SelectInst *>(P2);
    if (S1 && S2 && S1->getOperand(0) == S2->getOperand(0)) {
        AliasInfo OnTrue = aliasLocationImpl(
            {S1->getOperand(1), L1.Size}, {S2->getOperand(1), L2.Size},
            LI, UseIndexRange, Depth + 1, Active);
        AliasInfo OnFalse = aliasLocationImpl(
            {S1->getOperand(2), L1.Size}, {S2->getOperand(2), L2.Size},
            LI, UseIndexRange, Depth + 1, Active);
        return Merge(OnTrue, OnFalse);
    }
    if (S1) {
        AliasInfo OnTrue = aliasLocationImpl(
            {S1->getOperand(1), L1.Size}, L2, LI, UseIndexRange,
            Depth + 1, Active);
        AliasInfo OnFalse = aliasLocationImpl(
            {S1->getOperand(2), L1.Size}, L2, LI, UseIndexRange,
            Depth + 1, Active);
        return Merge(OnTrue, OnFalse);
    }
    if (S2) {
        AliasInfo OnTrue = aliasLocationImpl(
            L1, {S2->getOperand(1), L2.Size}, LI, UseIndexRange,
            Depth + 1, Active);
        AliasInfo OnFalse = aliasLocationImpl(
            L1, {S2->getOperand(2), L2.Size}, LI, UseIndexRange,
            Depth + 1, Active);
        return Merge(OnTrue, OnFalse);
    }

    auto *Phi1 = dynamic_cast<PhiInst *>(P1);
    auto *Phi2 = dynamic_cast<PhiInst *>(P2);
    if (Phi1 && Phi2 && Phi1->getParent() == Phi2->getParent() &&
        Phi1->getNumIncomingValues() == Phi2->getNumIncomingValues() &&
        Phi1->getNumIncomingValues() != 0) {
        std::unordered_map<BasicBlock *, Value *> Incoming2;
        if (!forEachPhiIncoming(
                Phi2, [&](unsigned, Value *Incoming, BasicBlock *Pred) {
                    return Pred && Incoming2.emplace(Pred, Incoming).second;
                })) {
            return AliasInfo::MayAlias;
        }
        bool First = true;
        AliasInfo Result = AliasInfo::MayAlias;
        std::unordered_set<BasicBlock *> Seen;
        bool InvalidIncoming = false;
        forEachPhiIncoming(
            Phi1, [&](unsigned, Value *Incoming, BasicBlock *Pred) {
                auto It = Incoming2.find(Pred);
                if (!Pred || !Seen.insert(Pred).second ||
                    It == Incoming2.end()) {
                    InvalidIncoming = true;
                    return false;
                }
                AliasInfo Current = aliasLocationImpl(
                    {Incoming, L1.Size}, {It->second, L2.Size}, LI,
                    UseIndexRange, Depth + 1, Active);
                Result = First ? Current : Merge(Result, Current);
                First = false;
                return Result != AliasInfo::MayAlias;
            });
        if (InvalidIncoming) return AliasInfo::MayAlias;
        return Result;
    }

    auto ExpandPhi = [&](PhiInst *Phi, const MemoryLocation &Other,
                         bool PhiIsFirst) {
        if (!Phi || Phi->getNumIncomingValues() == 0)
            return AliasInfo::MayAlias;
        bool First = true;
        AliasInfo Result = AliasInfo::MayAlias;
        forEachPhiIncoming(
            Phi, [&](unsigned, Value *IncomingValue, BasicBlock *) {
                MemoryLocation Incoming(
                    IncomingValue, PhiIsFirst ? L1.Size : L2.Size);
                AliasInfo Current = PhiIsFirst
                    ? aliasLocationImpl(Incoming, Other, LI, UseIndexRange,
                                        Depth + 1, Active)
                    : aliasLocationImpl(Other, Incoming, LI, UseIndexRange,
                                        Depth + 1, Active);
                Result = First ? Current : Merge(Result, Current);
                First = false;
                return Result != AliasInfo::MayAlias;
            });
        return Result;
    };
    if (Phi1) return ExpandPhi(Phi1, L2, true);
    if (Phi2) return ExpandPhi(Phi2, L1, false);

    // 3. 普通指针交给核心查询；同对象 GEP 再用显式宽度覆盖名义 pointee 宽度。
    AliasInfo Result = aliasImpl(P1, P2, LI, UseIndexRange);
    if (L1.Size && L2.Size) {
        Value *V1 = stripBaseInst(P1);
        Value *V2 = stripBaseInst(P2);

        if (V1 && V2 && getBaseAddress(V1) == getBaseAddress(V2) &&
            (dynamic_cast<GetElementPtrInst *>(V1) ||
             dynamic_cast<GetElementPtrInst *>(V2))) {
            Result = aliasGEP(V1, V2, LI, UseIndexRange,
                              L1.Size, L2.Size);
        }
    }
    return Result;
}

AliasInfo BasicAAResult::alias(const MemoryLocation &L1,
                               const MemoryLocation &L2,
                               const LoopInfo &LI) {
    std::unordered_set<std::pair<Value *, Value *>, ValuePairHash> Active;
    return enforceLocationObjectBounds(
        aliasLocationImpl(L1, L2, LI, false, 0, Active), L1, L2);
}

AliasInfo BasicAAResult::aliasWithIndexRange(
    const MemoryLocation &L1, const MemoryLocation &L2,
    const LoopInfo &LI) {
    std::unordered_set<std::pair<Value *, Value *>, ValuePairHash> Active;
    return enforceLocationObjectBounds(
        aliasLocationImpl(L1, L2, LI, true, 0, Active), L1, L2);
}

// 核心指针查询按“规范化与校验、对象区分、同对象分解、缓存结果”顺序求解。
AliasInfo BasicAAResult::aliasImpl(Value* V1, Value* V2, const LoopInfo& LI,
                                   bool UseIndexRange) {
    // 1. 剥离透明包装并拒绝非指针、undef 来源或不稳定底层对象。
    V1 = stripBaseInst(V1);
    V2 = stripBaseInst(V2);

    if (!V1 || !V2 || !V1->isPointerValue() || !V2->isPointerValue()) {
        return AliasInfo::MayAlias;
    }

    if (V1->isUndef() || V2->isUndef()) {
        return AliasInfo::MayAlias;
    }
    std::unordered_set<Value *> UndefActive;
    if (hasUndefPointerOrigin(V1, UndefActive))
        return AliasInfo::MayAlias;
    UndefActive.clear();
    if (hasUndefPointerOrigin(V2, UndefActive))
        return AliasInfo::MayAlias;

    // 2. SSA 自反关系先得 MustAlias；控制流合流指针必须在对象身份判断前
    // 按路径展开，否则多入 PHI 会被自身误当成一个未知底层对象。
    if (isEqualAlias(V1, V2, LI)) {
        return AliasInfo::MustAlias;
    }

    // 多来源控制流指针只有在来源笛卡尔积中的每一对对象都互斥时才
    // 提前返回 NoAlias；这也让带回边的指针 PHI 消费来源固定点结果。
    if (haveKnownDistinctUnderlyingObjects(V1, V2))
        return AliasInfo::NoAlias;

    if (auto *S1 = dynamic_cast<SelectInst *>(V1))
        return aliasSelect(S1, V2, LI, UseIndexRange);
    if (auto *S2 = dynamic_cast<SelectInst *>(V2))
        return aliasSelect(S2, V1, LI, UseIndexRange);
    if (auto *P1 = dynamic_cast<PhiInst *>(V1)) {
        auto ActiveKey = makeCanonicalPair(P1, V2);
        ScopedActiveMark ActiveMark(ActiveSelectQueries, ActiveKey);
        if (!ActiveMark.inserted()) return AliasInfo::MayAlias;
        return aliasPHI(P1, V2, LI, UseIndexRange);
    }
    if (auto *P2 = dynamic_cast<PhiInst *>(V2)) {
        auto ActiveKey = makeCanonicalPair(P2, V1);
        ScopedActiveMark ActiveMark(ActiveSelectQueries, ActiveKey);
        if (!ActiveMark.inserted()) return AliasInfo::MayAlias;
        return aliasPHI(P2, V1, LI, UseIndexRange);
    }

    Value* O1 = getBaseAddress(V1);
    Value* O2 = getBaseAddress(V2);
    if (!O1 || !O2 || O1->isUndef() || O2->isUndef())
        return AliasInfo::MayAlias;

    // 3. 底层对象不同只有在结构事实或调用点事实充分时才能返回 NoAlias。
    if (O1 != O2) {
        if (haveKnownDistinctUnderlyingObjects(O1, O2)) {
            return AliasInfo::NoAlias;
        }

        return AliasInfo::MayAlias;
    }

    // 4. 同对象普通查询先写入保守缓存占位，以截断递归 PHI 图。
    std::pair<Value*, Value*> Key;
    if (!UseIndexRange) {
        Key = makeCanonicalPair(V1, V2);
        auto It = AliasCache.find(Key);
        if (It != AliasCache.end()) {
            return It->second;
        }
        AliasCache[Key] = AliasInfo::MayAlias;
    }

    // 5. 同对象 GEP 继续比较字节表达式；其他未覆盖形态保持 MayAlias。
    AliasInfo Result = AliasInfo::MayAlias;
    auto *G1 = dynamic_cast<GetElementPtrInst*>(V1);
    auto *G2 = dynamic_cast<GetElementPtrInst*>(V2);
    if (G1 || G2) {

        Result = aliasGEP(V1, V2, LI, UseIndexRange);
    } else if (V1 == V2) {
        Result = AliasInfo::MayAlias;
    }

    if (!UseIndexRange) {
        AliasCache[Key] = Result;
    }
    return Result;
}

// BasicAA Pass 主流程从跨调用点的一致对象关系播种函数内查询器。
BasicAAResult BasicAliasAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    // 1. 获取 SCEV 作为可选距离证明，并收集函数的指针形参。
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    BasicAAResult Result(&SE);

    std::vector<unsigned> PointerParams;
    PointerParams.reserve(F.getParamCount());
    for (unsigned I = 0; I < F.getParamCount(); ++I) {
        Value *Param = F.getParam(I);
        if (Param && Param->isPointerValue()) {
            PointerParams.push_back(I);
        }
    }
    if (PointerParams.empty()) {
        return Result;
    }

    // 2. 只有全部用途均为直接调用且存在调用点时，跨函数事实才可安全提升。
    std::vector<CallInst*> Calls = F.getDirectCallSites();
    if (!F.hasOnlyDirectCallUses() || Calls.empty()) {
        return Result;
    }

    // 3. 对每对指针形参检查所有调用点；仅底层分配对象始终不同才记录 NoAlias。
    BasicAAResult SeedAA;
    for (unsigned I = 0; I < PointerParams.size(); ++I) {
        for (unsigned J = I + 1; J < PointerParams.size(); ++J) {
            unsigned PI = PointerParams[I];
            unsigned PJ = PointerParams[J];
            bool AllNoAlias = true;
            for (CallInst* CI : Calls) {
                Value* ArgI = CI->getArg(PI);
                Value* ArgJ = CI->getArg(PJ);

                if (!ArgI || !ArgJ ||
                    !SeedAA.haveKnownDistinctUnderlyingObjects(ArgI, ArgJ)) {
                    AllNoAlias = false;
                    break;
                }
            }
            if (AllNoAlias) {
                Result.addObjectNoAliasFact(F.getParam(PI), F.getParam(PJ));
            }
        }
    }

    // 4. 同样验证每个指针形参与全局对象，随后封装完整结果。
    Module* M = Module::getActiveModule();
    if (!M) {
        return Result;
    }
    for (auto &GV : M->getGlobalVar()) {
        Value* Global = GV.get();
        if (!(Global && Global->getIRType() && Global->getIRType()->isPointer())) {
            continue;
        }
        for (unsigned PI : PointerParams) {
            bool AllNoAlias = true;
            for (CallInst* CI : Calls) {
                Value* Arg = CI->getArg(PI);
                if (!Arg ||
                    !SeedAA.haveKnownDistinctUnderlyingObjects(Arg, Global)) {
                    AllNoAlias = false;
                    break;
                }
            }
            if (AllNoAlias) {
                Result.addObjectNoAliasFact(F.getParam(PI), Global);
            }
        }
    }
    return Result;
}
