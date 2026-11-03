// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <unordered_set>
#include <utility>
//构造一个尚未绑定任何基础分析的空访问分析结果
LoopAccessInfo::LoopAccessInfo() = default;
//使用函数及各项基础分析构造循环访问查询器
LoopAccessInfo::LoopAccessInfo(Function *F, LoopInfo *LoopInfoPtr,
                               SCEVResult *SCEV,
                                BasicAAResult *AliasAnalysis,
                               LoopDependenceInfo *Dependence,
                               RangeInfo *RangeAnalysis,
                               DominatorTree *DomTree)
    : Func(F), LI(LoopInfoPtr), SE(SCEV), AA(AliasAnalysis), LD(Dependence),
      RI(RangeAnalysis), DT(DomTree) {}
//移动构造访问分析结果并转移惰性摘要缓存
LoopAccessInfo::LoopAccessInfo(LoopAccessInfo &&) noexcept = default;
//移动赋值访问分析结果并转移惰性摘要缓存
LoopAccessInfo &LoopAccessInfo::operator=(LoopAccessInfo &&) noexcept =
    default;
//返回循环作用域的访问摘要并在首次查询时构建缓存
const LoopAccessSummary &LoopAccessInfo::getSummary(Loop *Scope) {
    auto It = Cache.find(Scope);
    if (It != Cache.end()) return *It->second;
    auto Summary = buildSummary(Scope);
    LoopAccessSummary *Raw = Summary.get();
    Cache.emplace(Scope, std::move(Summary));
    return *Raw;
}
//返回指定稳定访问记录在循环作用域中的足迹
const LoopAccessFootprint *LoopAccessInfo::getFootprint(
    Loop *Scope, const LoopMemoryAccess &Access) {
    const LoopAccessSummary &Summary = getSummary(Scope);
    for (const LoopAccessFootprint &Footprint : Summary.Footprints) {
        if (Footprint.Access == &Access) return &Footprint;
    }
    return nullptr;
}
//返回指定指令在循环作用域中对应的全部访问足迹
std::vector<const LoopAccessFootprint *> LoopAccessInfo::getFootprints(
    Loop *Scope, Instruction *I) {
    std::vector<const LoopAccessFootprint *> Result;
    if (!I) return Result;
    const LoopAccessSummary &Summary = getSummary(Scope);
    for (const LoopAccessFootprint &Footprint : Summary.Footprints) {
        if (Footprint.Access && Footprint.Access->Inst == I)
            Result.push_back(&Footprint);
    }
    return Result;
}
//判断指定指令沿给定循环轴的全部访问是否连续
bool LoopAccessInfo::isConsecutiveAccess(
    Loop *Scope, Instruction *I, Loop *Axis) {
    if (!Scope || !I || !Axis) return false;
    std::vector<const LoopAccessFootprint *> Footprints =
        getFootprints(Scope, I);
    if (Footprints.empty()) return false;
    bool FoundAxis = false;
    for (const LoopAccessFootprint *Footprint : Footprints) {
        if (!Footprint || !Footprint->Exact || Footprint->IsEmpty)
            return false;
        bool FoundInFootprint = false;
        for (const LoopAccessDimension &Dimension :
             Footprint->Dimensions) {
            if (Dimension.Axis != Axis) continue;
            FoundInFootprint = true;
            FoundAxis = true;
            if (!Dimension.IsFinite || !Dimension.IsUnitStride)
                return false;
            break;
        }
        if (!FoundInFootprint) return false;
    }
    return FoundAxis;
}
//判断两个循环是否满足访问层面的融合合法性
bool LoopAccessInfo::isFusionLegal(Loop *First, Loop *Second) {
    return analyzeFusion(First, Second).Legal;
}
//判断两个循环是否形成次数独立且有限的矩形循环巢
bool LoopAccessInfo::isRectangularNest(Loop *Outer, Loop *Inner) {
    if (!Outer || !Inner || !SE || Inner->getParentLoop() != Outer ||
        Outer->getSubLoops().size() != 1 ||
        Outer->getSubLoops().front() != Inner ||
        !isKnownFinite(Outer) || !isKnownFinite(Inner)) {
        return false;
    }
    auto InnerTrip = SE->analyzeLoopTripCount(Inner);
    if (!InnerTrip || !InnerTrip->Valid || !InnerTrip->Start ||
        !InnerTrip->Step || !InnerTrip->Bound) {
        return false;
    }
    return !InnerTrip->Start->mentionsLoop(Outer) &&
           !InnerTrip->Step->mentionsLoop(Outer) &&
           !InnerTrip->Bound->mentionsLoop(Outer);
}
//返回指定指令沿给定循环轴的已知字节步长
std::optional<std::int64_t>
LoopAccessInfo::getByteStride(Instruction *I, Loop *Scope, Loop *Axis) {
    if (!I || !Scope || !Axis || !LD || !Scope->contains(Axis->getHeader()))
        return std::nullopt;
    return LD->getByteStride(I, Axis);
}
//求源写到宿读在指定循环轴上的唯一常量依赖距离。
std::optional<std::int64_t>
LoopAccessInfo::getConstantDependenceDistance(
    Instruction *Source, Instruction *Sink,
    Loop *Scope, Loop *Axis) {
    //1.只处理同一作用域中的标量写后读；两个访问都必须在每个有效迭代必执行，
    //地址坐标完整可追踪、目标归纳不回绕且循环轴有限。
    if (!Source || !Sink || !Scope || !Axis || !SE ||
        !Scope->contains(Axis->getHeader()) ||
        !Axis->contains(Source->getParent()) ||
        !Axis->contains(Sink->getParent()) || !isKnownFinite(Axis)) {
        return std::nullopt;
    }
    std::vector<const LoopAccessFootprint *> SourceFootprints =
        getFootprints(Scope, Source);
    std::vector<const LoopAccessFootprint *> SinkFootprints =
        getFootprints(Scope, Sink);
    if (SourceFootprints.size() != 1 || SinkFootprints.size() != 1 ||
        !SourceFootprints.front() || !SinkFootprints.front()) {
        return std::nullopt;
    }
    const LoopAccessFootprint &SourceFootprint = *SourceFootprints.front();
    const LoopAccessFootprint &SinkFootprint = *SinkFootprints.front();
    const LoopMemoryAccess *SourceAccess = SourceFootprint.Access;
    const LoopMemoryAccess *SinkAccess = SinkFootprint.Access;
    auto Induction = SE->getCanonicalInduction(Axis);
    if (!SourceAccess || !SinkAccess || !SourceAccess->Writes ||
        !SinkAccess->Reads || !SourceAccess->AddressExact ||
        !SinkAccess->AddressExact ||
        !Induction || !Induction->Valid || !Induction->Phi ||
        !Induction->UpdateValue ||
        !SE->isKnownNoWrapInductionValue(Axis, Induction->Phi) ||
        !SE->isKnownNoWrapInductionValue(
            Axis, Induction->UpdateValue) ||
        !isMustExecuteInIterationDomain(
            *SourceAccess, SourceFootprint.Dimensions) ||
        !isMustExecuteInIterationDomain(
            *SinkAccess, SinkFootprint.Dimensions)) {
        return std::nullopt;
    }
    //2.要求相同底层对象、访问宽度和类型化地址布局。后续逐带正向证明
    //Source(t) 与 Sink(t+Distance) 坐标相同，不从越界或线性地址碰撞反推别名。
    if (!SourceAccess->Root ||
        SourceAccess->Root != SinkAccess->Root ||
        !SourceAccess->Location.Size || !SinkAccess->Location.Size ||
        *SourceAccess->Location.Size == 0 ||
        *SourceAccess->Location.Size != *SinkAccess->Location.Size ||
        SourceAccess->ByteStrides != SinkAccess->ByteStrides ||
        SourceAccess->Subscripts.size() !=
            SinkAccess->Subscripts.size() ||
        !isMixedRadixAccess(*SourceAccess) ||
        !isMixedRadixAccess(*SinkAccess)) {
        return std::nullopt;
    }
    std::optional<std::int64_t> Distance;
    bool SawAxis = false;
    for (std::size_t Index = 0;
         Index < SourceAccess->Subscripts.size(); ++Index) {
        const LoopAffineExpr &SourceExpr =
            SourceAccess->Subscripts[Index];
        const LoopAffineExpr &SinkExpr =
            SinkAccess->Subscripts[Index];
        if (!SourceExpr.Exact || !SinkExpr.Exact ||
            !sameInvariantTerms(SourceExpr, SinkExpr) ||
            !sameLoopTermsWithAxisMapping(
                SourceExpr, Axis, SinkExpr, Axis)) {
            return std::nullopt;
        }
        auto SourceAxis = SourceExpr.LoopCoefficients.find(Axis);
        auto SinkAxis = SinkExpr.LoopCoefficients.find(Axis);
        const std::int64_t SourceCoefficient =
            SourceAxis == SourceExpr.LoopCoefficients.end()
                ? 0 : SourceAxis->second;
        const std::int64_t SinkCoefficient =
            SinkAxis == SinkExpr.LoopCoefficients.end()
                ? 0 : SinkAxis->second;
        if (SourceCoefficient != SinkCoefficient)
            return std::nullopt;
        if (SourceCoefficient == 0) {
            if (SourceExpr.Constant != SinkExpr.Constant)
                return std::nullopt;
            continue;
        }
        SawAxis = true;
        const __int128 Difference =
            static_cast<__int128>(SourceExpr.Constant) -
            static_cast<__int128>(SinkExpr.Constant);
        const __int128 Coefficient = SourceCoefficient;
        if (Difference % Coefficient != 0)
            return std::nullopt;
        const __int128 ThisDistance = Difference / Coefficient;
        if (ThisDistance < std::numeric_limits<std::int64_t>::min() ||
            ThisDistance > std::numeric_limits<std::int64_t>::max()) {
            return std::nullopt;
        }
        const std::int64_t NarrowDistance =
            static_cast<std::int64_t>(ThisDistance);
        if (Distance && *Distance != NarrowDistance)
            return std::nullopt;
        Distance = NarrowDistance;
    }
    return SawAxis ? Distance : std::nullopt;
}
//判断同一读取是否在一个循环轴复用并在另一轴变化
bool LoopAccessInfo::hasCrossAxisReuse(Loop *Scope, Loop *ReusedAxis,
                                       Loop *VaryingAxis) {
    if (!Scope || !ReusedAxis || !VaryingAxis ||
        ReusedAxis == VaryingAxis || !LD) {
        return false;
    }
    const LoopAccessSummary &Summary = getSummary(Scope);
    if (Summary.HasUnknownMemory) return false;
    for (const LoopMemoryAccess *Access : Summary.Accesses) {
        if (!Access || !Access->Reads || !Access->Inst) continue;
        auto ReusedStride = getByteStride(Access->Inst, Scope, ReusedAxis);
        auto VaryingStride = getByteStride(Access->Inst, Scope, VaryingAxis);
        if (ReusedStride && VaryingStride && *ReusedStride == 0 &&
            *VaryingStride != 0) {
            return true;
        }
    }
    return false;
}
//根据本Pass及其依赖分析的保留状态判断结果是否失效
bool LoopAccessInfo::invalidate(Function &,
                                const PreservationStatus &PA) {
    return !PA.isKept(LoopAccessAnalysis::ID()) ||
           !PA.isKept(LoopDependenceAnalysis::ID()) ||
           !PA.isKept(BasicAliasAnalysis::ID()) ||
           !PA.isKept(SCEVAnalysis::ID()) ||
           !PA.isKept(RangeAnalysis::ID()) ||
           !PA.isKept(DominantAnalysis::ID()) ||
           !PA.isKept(LoopAnalysis::ID());
}
//执行带溢出检查的有符号加法
bool LoopAccessInfo::checkedAdd(std::int64_t First, std::int64_t Second,
                                std::int64_t &Result) {
    const __int128 Wide = static_cast<__int128>(First) +
                          static_cast<__int128>(Second);
    if (Wide < std::numeric_limits<std::int64_t>::min() ||
        Wide > std::numeric_limits<std::int64_t>::max()) {
        return false;
    }
    Result = static_cast<std::int64_t>(Wide);
    return true;
}
//执行带溢出检查的有符号乘法
bool LoopAccessInfo::checkedMul(std::int64_t First, std::int64_t Second,
                                std::int64_t &Result) {
    const __int128 Wide = static_cast<__int128>(First) *
                          static_cast<__int128>(Second);
    if (Wide < std::numeric_limits<std::int64_t>::min() ||
        Wide > std::numeric_limits<std::int64_t>::max()) {
        return false;
    }
    Result = static_cast<std::int64_t>(Wide);
    return true;
}
//返回有符号整数绝对值对应的无符号幅值并处理最小值
std::uint64_t LoopAccessInfo::absoluteMagnitude(std::int64_t Value) {
    return Value < 0
        ? static_cast<std::uint64_t>(-(Value + 1)) + 1U
        : static_cast<std::uint64_t>(Value);
}
//判断循环轴是否已由SCEV及可选范围事实证明有限
bool LoopAccessInfo::isKnownFinite(Loop *Axis) const {
    if (!SE || !Axis) return false;
    if (SE->isKnownFiniteLoop(Axis)) return true;
    return RI && DT && SE->isKnownFiniteLoop(Axis, *RI, *DT);
}
//判断指定门控块是否支配循环轴所有有效迭代路径
bool LoopAccessInfo::dominatesIterationPaths(
    BasicBlock *Gate, Loop *Axis) const {
    if (!Gate || !Axis || !DT || !Axis->contains(Gate) ||
        Axis->getBackedgeSrcs().empty()) {
        return false;
    }
    if (Gate == Axis->getHeader() &&
        Axis->isExitingBlock(Axis->getHeader())) {
        //头部退出循环中，Header指令的执行次数通常是TripCount+1，
        //当前足迹表达式尚不能表示这一额外的条件检查。
        return false;
    }
    for (BasicBlock *Backedge : Axis->getBackedgeSrcs()) {
        if (!Backedge || !DT->dominates(Gate, Backedge))
            return false;
    }
    for (BasicBlock *BB : Axis->getBlocksIncludingSubLoops()) {
        if (!BB || BB == Axis->getHeader() ||
            !Axis->isExitingBlock(BB)) {
            continue;
        }
        if (!DT->dominates(Gate, BB)) return false;
    }
    return true;
}
//判断直接父子循环轴是否构成次数不依赖外层的矩形维度
bool LoopAccessInfo::isRectangularAxisPair(
    Loop *Outer, Loop *Inner) const {
    if (!Outer || !Inner || !SE || Inner->getParentLoop() != Outer ||
        !dominatesIterationPaths(Inner->getHeader(), Outer)) {
        return false;
    }
    auto Trip = SE->analyzeLoopTripCount(Inner);
    if (!Trip || !Trip->Valid || !Trip->Start || !Trip->Step ||
        !Trip->Bound) {
        return false;
    }
    return !Trip->Start->mentionsLoop(Outer) &&
           !Trip->Step->mentionsLoop(Outer) &&
           !Trip->Bound->mentionsLoop(Outer);
}
//判断内存访问是否在足迹覆盖的每个逻辑迭代中必定执行
bool LoopAccessInfo::isMustExecuteInIterationDomain(
    const LoopMemoryAccess &Access,
    const std::vector<LoopAccessDimension> &Dimensions) const {
    BasicBlock *AccessBlock =
        Access.Inst ? Access.Inst->getParent() : nullptr;
    if (!AccessBlock || Dimensions.empty()) return false;
    Loop *Innermost = Dimensions.back().Axis;
    if (!Innermost ||
        !dominatesIterationPaths(AccessBlock, Innermost)) {
        return false;
    }
    for (std::size_t I = Dimensions.size(); I > 1; --I) {
        const LoopAccessDimension &Inner = Dimensions[I - 1];
        Loop *Outer = Dimensions[I - 2].Axis;
        if (!Inner.Axis || !Inner.ConstantTripCount ||
            *Inner.ConstantTripCount <= 0 ||
            !isRectangularAxisPair(Outer, Inner.Axis)) {
            return false;
        }
    }
    return true;
}
//递归证明整数地址表达式在指令上下文中不会发生有符号回绕
bool LoopAccessInfo::isIntegerAddressExpressionNoWrap(
    Value *Expression, Instruction *Context,
    AddressNoWrapQueryState &State, unsigned Depth) const {
    //1.拒绝缺少上下文、未定义值、递归过深和节点预算耗尽的查询。
    if (!Expression || !Context || !RI || !DT || !SE || !LI ||
        Value::isUndef(Expression) || Depth > 64) {
        return false;
    }
    auto Completed = State.Completed.find(Expression);
    if (Completed != State.Completed.end())
        return Completed->second;
    if (State.VisitedNodes >= MaxAddressNoWrapQueryNodes)
        return false;
    ++State.VisitedNodes;
    if (dynamic_cast<ConstInt *>(Expression)) {
        State.Completed.emplace(Expression, true);
        return true;
    }
    //2.递归处理PHI与整数扩展，循环PHI直接消费SCEV无回绕证明。
    ScopedActiveMark Mark(State.Active, Expression);
    if (!Mark.inserted()) return false;
    auto Finish = [&](bool Result) {
        State.Completed.emplace(Expression, Result);
        return Result;
    };
    if (auto *Phi = dynamic_cast<PhiInst *>(Expression)) {
        Loop *RecLoop = Phi->getParent()
            ? LI->getLoopFor(Phi->getParent()) : nullptr;
        if (RecLoop)
            return Finish(
                SE->isKnownNoWrapInductionValue(RecLoop, Phi));
        for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
            if (!isIntegerAddressExpressionNoWrap(
                    Phi->getIncomingValue(I), Context, State,
                    Depth + 1)) {
                return Finish(false);
            }
        }
        return Finish(true);
    }
    if (auto *ZExt = dynamic_cast<ZextInst *>(Expression)) {
        Value *Operand = ZExt->getOperand(0);
        IntRange Range = RI->query(Operand, Context, *DT);
        return Finish(
            !Range.isBottom() && Range.min >= 0 &&
            isIntegerAddressExpressionNoWrap(
                Operand, Context, State, Depth + 1));
    }
    if (auto *SExt = dynamic_cast<SextInst *>(Expression)) {
        return Finish(isIntegerAddressExpressionNoWrap(
            SExt->getOperand(0), Context, State, Depth + 1));
    }
    if (dynamic_cast<TruncInst *>(Expression))
        return Finish(false);
    auto *Binary = dynamic_cast<BinaryInst *>(Expression);
    if (!Binary) return Finish(true);
    //3.只接受32位整数加减乘，并先证明两个操作数的完整计算链。
    if (!IRType::hasIntegerBitWidth(Binary->getIRType(), 32))
        return Finish(false);
    if (Binary->getOp() != BinaryInst::ADD &&
        Binary->getOp() != BinaryInst::SUB &&
        Binary->getOp() != BinaryInst::MUL) {
        return Finish(false);
    }
    Value *LeftValue = Binary->getOperand(0);
    Value *RightValue = Binary->getOperand(1);
    if (!isIntegerAddressExpressionNoWrap(
            LeftValue, Context, State, Depth + 1) ||
        !isIntegerAddressExpressionNoWrap(
            RightValue, Context, State, Depth + 1)) {
        return Finish(false);
    }
    IntRange Left = RI->query(LeftValue, Context, *DT);
    IntRange Right = RI->query(RightValue, Context, *DT);
    if (Left.isBottom() || Right.isBottom())
        return Finish(false);
    //4.在宽整数域中计算结果极值，确认整个范围仍落在i32内。
    __int128 Minimum = 0;
    __int128 Maximum = 0;
    if (Binary->getOp() == BinaryInst::ADD) {
        Minimum = static_cast<__int128>(Left.min) + Right.min;
        Maximum = static_cast<__int128>(Left.max) + Right.max;
    } else if (Binary->getOp() == BinaryInst::SUB) {
        Minimum = static_cast<__int128>(Left.min) - Right.max;
        Maximum = static_cast<__int128>(Left.max) - Right.min;
    } else {
        const __int128 Products[4] = {
            static_cast<__int128>(Left.min) * Right.min,
            static_cast<__int128>(Left.min) * Right.max,
            static_cast<__int128>(Left.max) * Right.min,
            static_cast<__int128>(Left.max) * Right.max};
        Minimum = Products[0];
        Maximum = Products[0];
        for (unsigned I = 1; I < 4; ++I) {
            Minimum = std::min(Minimum, Products[I]);
            Maximum = std::max(Maximum, Products[I]);
        }
    }
    return Finish(
        Minimum >= IntRange::I32_MIN &&
        Maximum <= IntRange::I32_MAX);
}
//判断内存访问完整地址计算链是否已证明不回绕
bool LoopAccessInfo::isAddressArithmeticKnownNoWrap(
    const LoopMemoryAccess &Access) const {
    if (!Access.Pointer || !Access.Inst) return false;
    Value *Current = Access.Pointer;
    std::unordered_set<Value *> SeenPointers;
    AddressNoWrapQueryState State;
    unsigned Depth = 0;
    while (Current && SeenPointers.insert(Current).second) {
        if (Depth++ > 64) return false;
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Current)) {
            for (Value *Index : GEP->getIndices()) {
                if (!isIntegerAddressExpressionNoWrap(
                        Index, Access.Inst, State, 0)) {
                    return false;
                }
            }
            Current = GEP->getBasePointer();
            continue;
        }
        if (auto *BitCast = dynamic_cast<BitCastInst *>(Current)) {
            Current = BitCast->getUnaryOperand();
            continue;
        }
        break;
    }
    return Current != nullptr;
}
//按嵌套顺序收集作用域及其所有子循环轴
void LoopAccessInfo::collectLoopAxes(Loop *Scope,
                                     std::vector<Loop *> &Axes) {
    if (!Scope) return;
    Axes.push_back(Scope);
    for (Loop *SubLoop : Scope->getSubLoops())
        collectLoopAxes(SubLoop, Axes);
}
//计算一个内存访问沿指定循环轴的总字节步长
std::optional<std::int64_t>
LoopAccessInfo::computeAccessByteStride(
    const LoopMemoryAccess &Access, Loop *Axis) {
    if (!Axis || !Access.AddressExact ||
        Access.Subscripts.size() != Access.ByteStrides.size()) {
        return std::nullopt;
    }
    std::int64_t Result = 0;
    for (std::size_t I = 0; I < Access.Subscripts.size(); ++I) {
        const LoopAffineExpr &Expression = Access.Subscripts[I];
        if (!Expression.Exact) return std::nullopt;
        auto It = Expression.LoopCoefficients.find(Axis);
        if (It == Expression.LoopCoefficients.end()) continue;
        std::int64_t Term = 0;
        std::int64_t Updated = 0;
        if (!checkedMul(It->second, Access.ByteStrides[I], Term) ||
            !checkedAdd(Result, Term, Updated)) {
            return std::nullopt;
        }
        Result = Updated;
    }
    return Result;
}
//计算仿射访问地址中不含符号项的常量起始偏移
std::optional<std::int64_t>
LoopAccessInfo::computeConstantStartOffset(
    const LoopMemoryAccess &Access) {
    if (!Access.AddressExact ||
        Access.Subscripts.size() != Access.ByteStrides.size()) {
        return std::nullopt;
    }
    std::int64_t Result = 0;
    for (std::size_t I = 0; I < Access.Subscripts.size(); ++I) {
        const LoopAffineExpr &Expression = Access.Subscripts[I];
        if (!Expression.Exact || !Expression.InvariantTerms.empty())
            return std::nullopt;
        std::int64_t Term = 0;
        std::int64_t Updated = 0;
        if (!checkedMul(Expression.Constant, Access.ByteStrides[I], Term) ||
            !checkedAdd(Result, Term, Updated)) {
            return std::nullopt;
        }
        Result = Updated;
    }
    return Result;
}
//判断两个内存访问是否至少包含一个写入并可能形成冲突
bool LoopAccessInfo::isWriteConflict(const LoopMemoryAccess &First,
                                     const LoopMemoryAccess &Second) {
    return (First.Writes || Second.Writes) &&
           (First.Reads || First.Writes) &&
           (Second.Reads || Second.Writes);
}
//判断两个仿射表达式的循环外符号项及其系数是否相同
bool LoopAccessInfo::sameInvariantTerms(const LoopAffineExpr &First,
                                        const LoopAffineExpr &Second) {
    if (First.InvariantTerms.size() != Second.InvariantTerms.size())
        return false;
    for (const auto &Entry : First.InvariantTerms) {
        auto It = Second.InvariantTerms.find(Entry.first);
        if (It == Second.InvariantTerms.end() || It->second != Entry.second)
            return false;
    }
    return true;
}
//在映射两个目标循环轴后判断其余循环项及系数是否相同
bool LoopAccessInfo::sameLoopTermsWithAxisMapping(
    const LoopAffineExpr &First, Loop *FirstAxis,
    const LoopAffineExpr &Second, Loop *SecondAxis) {
    if (!FirstAxis || !SecondAxis ||
        First.LoopCoefficients.size() !=
            Second.LoopCoefficients.size()) {
        return false;
    }
    std::unordered_map<const Loop *, std::int64_t> Mapped;
    for (const auto &Entry : First.LoopCoefficients) {
        const Loop *MappedLoop = Entry.first == FirstAxis
            ? SecondAxis
            : Entry.first;
        if (!Mapped.emplace(MappedLoop, Entry.second).second) return false;
    }
    if (Mapped.size() != Second.LoopCoefficients.size()) return false;
    for (const auto &Entry : Mapped) {
        auto It = Second.LoopCoefficients.find(Entry.first);
        if (It == Second.LoopCoefficients.end() ||
            It->second != Entry.second) {
            return false;
        }
    }
    return true;
}
//判断内存访问是否满足完整混合基数地址布局约束
bool LoopAccessInfo::isMixedRadixAccess(const LoopMemoryAccess &Access) {
    if (!Access.AddressExact || !Access.Root || !Access.Location.Size ||
        *Access.Location.Size == 0 || Access.ByteStrides.empty() ||
        Access.ByteStrides.size() != Access.Subscripts.size() ||
        *Access.Location.Size >
            static_cast<std::size_t>(Access.ByteStrides.back())) {
        return false;
    }
    for (std::size_t Index = 0; Index < Access.ByteStrides.size(); ++Index) {
        if (Access.ByteStrides[Index] <= 0 ||
            !Access.Subscripts[Index].Exact) {
            return false;
        }
        if (Index == 0) continue;
        const std::int64_t Outer = Access.ByteStrides[Index - 1];
        const std::int64_t Inner = Access.ByteStrides[Index];
        if (Outer <= Inner || Outer % Inner != 0) return false;
    }
    return true;
}
//判断仿射表达式是否仅包含目标循环轴并返回其非零系数
bool LoopAccessInfo::isolateAxis(const LoopAffineExpr &Expression, Loop *Axis,
                                 std::int64_t &Coefficient) {
    if (!Expression.Exact || !Axis) return false;
    auto It = Expression.LoopCoefficients.find(Axis);
    if (It == Expression.LoopCoefficients.end() || It->second == 0)
        return false;
    for (const auto &Entry : Expression.LoopCoefficients) {
        if (Entry.first != Axis && Entry.second != 0) return false;
    }
    Coefficient = It->second;
    return true;
}
//判断两个访问地址相等是否强制两个循环轴位于相同逻辑迭代
bool LoopAccessInfo::equalityForcesAlignedIteration(
    const LoopMemoryAccess &First, Loop *FirstAxis,
    const LoopMemoryAccess &Second, Loop *SecondAxis) {
    if (!FirstAxis || !SecondAxis || First.Root != Second.Root ||
        !First.Location.Size || !Second.Location.Size ||
        *First.Location.Size != *Second.Location.Size ||
        First.ByteStrides != Second.ByteStrides ||
        !isMixedRadixAccess(First) || !isMixedRadixAccess(Second)) {
        return false;
    }
    bool FoundAxisBand = false;
    for (std::size_t Index = 0; Index < First.Subscripts.size(); ++Index) {
        const LoopAffineExpr &A = First.Subscripts[Index];
        const LoopAffineExpr &B = Second.Subscripts[Index];
        if (A.Constant != B.Constant || !sameInvariantTerms(A, B) ||
            !sameLoopTermsWithAxisMapping(A, FirstAxis, B, SecondAxis)) {
            return false;
        }
        const auto AAxis = A.LoopCoefficients.find(FirstAxis);
        const auto BAxis = B.LoopCoefficients.find(SecondAxis);
        const bool AContainsAxis =
            AAxis != A.LoopCoefficients.end() && AAxis->second != 0;
        const bool BContainsAxis =
            BAxis != B.LoopCoefficients.end() && BAxis->second != 0;
        if (AContainsAxis != BContainsAxis) return false;
        if (!AContainsAxis) continue;
        if (FoundAxisBand) return false;
        std::int64_t ACoefficient = 0;
        std::int64_t BCoefficient = 0;
        if (!isolateAxis(A, FirstAxis, ACoefficient) ||
            !isolateAxis(B, SecondAxis, BCoefficient)) {
            return false;
        }
        const bool UnitCoefficient =
            (ACoefficient == 1 && BCoefficient == 1) ||
            (ACoefficient == -1 && BCoefficient == -1);
        if (!UnitCoefficient) return false;
        FoundAxisBand = true;
    }
    return FoundAxisBand;
}
//为单个稳定内存访问构建完整迭代域上的访问足迹
LoopAccessFootprint LoopAccessInfo::buildFootprint(
    Loop *Scope, const LoopMemoryAccess &Access) {
    //1.归一化访问元素大小、底层对象、对象大小和对齐事实。
    LoopAccessFootprint Result;
    Result.Access = &Access;
    Result.ElementSize =
        Access.Location.Size ? *Access.Location.Size : 0;
    const PointerProvenanceInfo *PointerProvenance = nullptr;
    if (AA && Access.Pointer) {
        PointerProvenance =
            &AA->getPointerProvenance(Access.Pointer);
        Result.Base = PointerProvenance->getUniqueBase();
        if (Result.Base)
            Result.ObjectSize = PointerProvenance->ObjectSize;
        Result.Alignment =
            std::max<std::size_t>(
                1, PointerProvenance->KnownAlignment);
    }
    if (!Result.Base) Result.Base = Access.Root;
    std::vector<Loop *> Axes;
    collectLoopAxes(Scope, Axes);
    std::unordered_set<const Loop *> AccessAxes;
    for (Loop *Axis : Axes) {
        BasicBlock *AccessBlock =
            Access.Inst ? Access.Inst->getParent() : nullptr;
        if (Axis && AccessBlock && Axis->contains(AccessBlock))
            AccessAxes.insert(Axis);
    }
    //2.检查是否存在作用域外递推，避免把运行时最终值静默当成零偏移。
    bool HasOutsideLoopTerm = false;
    for (const LoopAffineExpr &Subscript : Access.Subscripts) {
        for (const auto &Term : Subscript.LoopCoefficients) {
            if (Term.second != 0 && !AccessAxes.count(Term.first)) {
                HasOutsideLoopTerm = true;
                break;
            }
        }
        if (HasOutsideLoopTerm) break;
    }
    if (!HasOutsideLoopTerm) {
        //3.优先使用指针完整偏移，否则合并Root偏移与访问相对偏移。
        if (PointerProvenance && Result.Base &&
            PointerProvenance->getUniqueBase() == Result.Base) {
            Result.StartByteOffset =
                PointerProvenance->getExactByteOffset();
        }
        if (!Result.StartByteOffset) {
            std::optional<std::int64_t> Relative =
                computeConstantStartOffset(Access);
            if (Relative && Result.Base == Access.Root) {
                Result.StartByteOffset = Relative;
            } else if (Relative && AA && Access.Root && Result.Base) {
                const PointerProvenanceInfo &RootProvenance =
                    AA->getPointerProvenance(Access.Root);
                auto RootOffset =
                    RootProvenance.getExactByteOffset();
                if (RootProvenance.getUniqueBase() == Result.Base &&
                    RootOffset) {
                    std::int64_t Combined = 0;
                    if (checkedAdd(
                            *Relative,
                            static_cast<std::int64_t>(*RootOffset),
                            Combined)) {
                        Result.StartByteOffset = Combined;
                    }
                }
            }
        }
    }
    Result.Dimensions.reserve(Axes.size());
    bool AllDimensionsExact = !HasOutsideLoopTerm;
    for (Loop *Axis : Axes) {
        //4.只收集覆盖访问指令的循环轴，排除同一作用域中的兄弟循环。
        BasicBlock *AccessBlock =
            Access.Inst ? Access.Inst->getParent() : nullptr;
        if (!AccessBlock || !Axis || !Axis->contains(AccessBlock))
            continue;
        LoopAccessDimension Dimension;
        Dimension.Axis = Axis;
        if (SE && Axis) {
            Dimension.TripCount = SE->getTripCount(Axis);
            Dimension.ConstantTripCount =
                SE->getSmallConstantTripCount(Axis);
            Dimension.IsFinite = isKnownFinite(Axis);
        }
        Dimension.ByteStride =
            computeAccessByteStride(Access, Axis);
        if (Dimension.ByteStride && Result.ElementSize != 0) {
            Dimension.IsUnitStride =
                absoluteMagnitude(*Dimension.ByteStride) ==
                Result.ElementSize;
        }
        AllDimensionsExact =
            AllDimensionsExact && Dimension.ByteStride.has_value() &&
            Dimension.IsFinite && Dimension.TripCount;
        Result.Dimensions.push_back(Dimension);
    }
    const bool MustExecute =
        isMustExecuteInIterationDomain(Access, Result.Dimensions);
    const bool AddressNoWrap =
        isAddressArithmeticKnownNoWrap(Access);
    bool HasNoIteration = false;
    for (const LoopAccessDimension &Dimension : Result.Dimensions) {
        if (Dimension.ConstantTripCount &&
            *Dimension.ConstantTripCount == 0) {
            HasNoIteration = true;
            break;
        }
    }
    Result.IsEmpty = HasNoIteration;
    //5.最内层变化维度每步恰好跨一个元素时发布连续访问事实。
    for (auto It = Result.Dimensions.rbegin();
         It != Result.Dimensions.rend(); ++It) {
        if (!It->ByteStride || *It->ByteStride == 0) continue;
        Result.IsConsecutive = It->IsUnitStride;
        break;
    }
    Result.IsConsecutive =
        Result.IsConsecutive && MustExecute && AddressNoWrap &&
        !HasNoIteration;
    //6.在常量次数下计算足迹的精确最小偏移和半开区间右端点。
    if (MustExecute && AddressNoWrap && !HasNoIteration &&
        Result.StartByteOffset && Result.ElementSize != 0) {
        std::int64_t MinOffset = *Result.StartByteOffset;
        std::int64_t MaxOffset = *Result.StartByteOffset;
        bool ConstantExtent = true;
        for (const LoopAccessDimension &Dimension : Result.Dimensions) {
            if (!Dimension.ConstantTripCount || !Dimension.ByteStride ||
                *Dimension.ConstantTripCount < 0) {
                ConstantExtent = false;
                break;
            }
            if (*Dimension.ConstantTripCount == 0) {
                ConstantExtent = false;
                break;
            }
            std::int64_t CountMinusOne =
                *Dimension.ConstantTripCount - 1;
            std::int64_t Delta = 0;
            if (!checkedMul(CountMinusOne, *Dimension.ByteStride, Delta)) {
                ConstantExtent = false;
                break;
            }
            if (Delta < 0) {
                if (!checkedAdd(MinOffset, Delta, MinOffset)) {
                    ConstantExtent = false;
                    break;
                }
            } else if (!checkedAdd(MaxOffset, Delta, MaxOffset)) {
                ConstantExtent = false;
                break;
            }
        }
        if (ConstantExtent) {
            std::int64_t End = 0;
            if (Result.ElementSize <=
                    static_cast<std::size_t>(
                        std::numeric_limits<std::int64_t>::max()) &&
                checkedAdd(
                    MaxOffset,
                    static_cast<std::int64_t>(Result.ElementSize), End)) {
                Result.MinByteOffset = MinOffset;
                Result.MaxByteOffsetExclusive = End;
            }
        }
    }
    //7.单层运行时次数或常量混合基数完全吻合时发布完整连续足迹。
    std::vector<const LoopAccessDimension *> Varying;
    for (const LoopAccessDimension &Dimension : Result.Dimensions) {
        if (Dimension.ByteStride && *Dimension.ByteStride != 0)
            Varying.push_back(&Dimension);
    }
    if (MustExecute && AddressNoWrap && !HasNoIteration &&
        Result.StartByteOffset && Result.IsConsecutive &&
        Varying.size() == 1 && Varying.front()->TripCount &&
        Varying.front()->IsFinite) {
        Result.IsFullAccess = true;
    } else if (MustExecute && AddressNoWrap && !HasNoIteration &&
               Result.StartByteOffset && Result.ElementSize != 0 &&
               !Varying.empty()) {
        std::uint64_t Expected = Result.ElementSize;
        bool Full = true;
        for (auto It = Varying.rbegin(); It != Varying.rend(); ++It) {
            const LoopAccessDimension *Dimension = *It;
            if (!Dimension->ByteStride ||
                absoluteMagnitude(*Dimension->ByteStride) != Expected ||
                !Dimension->ConstantTripCount ||
                *Dimension->ConstantTripCount <= 0) {
                Full = false;
                break;
            }
            const std::uint64_t Count =
                static_cast<std::uint64_t>(
                    *Dimension->ConstantTripCount);
            if (Count != 0 &&
                Expected > std::numeric_limits<std::uint64_t>::max() /
                               Count) {
                Full = false;
                break;
            }
            Expected *= Count;
        }
        Result.IsFullAccess = Full;
    }
    //8.用已知分配对象边界约束强足迹结论并撤销越界常量区间。
    bool ObjectBoundsProven = true;
    bool ConstantFootprintOutOfBounds = false;
    if (Result.ObjectSize && !HasNoIteration) {
        ObjectBoundsProven =
            Result.MinByteOffset &&
            Result.MaxByteOffsetExclusive &&
            *Result.MinByteOffset >= 0 &&
            *Result.MaxByteOffsetExclusive >=
                *Result.MinByteOffset &&
            static_cast<std::uint64_t>(
                *Result.MaxByteOffsetExclusive) <=
                static_cast<std::uint64_t>(*Result.ObjectSize);
        ConstantFootprintOutOfBounds =
            Result.MinByteOffset &&
            Result.MaxByteOffsetExclusive &&
            !ObjectBoundsProven;
    }
    if (!ObjectBoundsProven) Result.IsFullAccess = false;
    if (ConstantFootprintOutOfBounds) {
        Result.IsConsecutive = false;
        Result.MinByteOffset.reset();
        Result.MaxByteOffsetExclusive.reset();
    }
    Result.Exact = Access.AddressExact && Result.Base &&
                   Result.StartByteOffset &&
                   Result.ElementSize != 0 && AllDimensionsExact &&
                   MustExecute && AddressNoWrap &&
                   ObjectBoundsProven;
    return Result;
}
//为循环作用域构建访问记录及其足迹摘要
std::unique_ptr<LoopAccessSummary>
LoopAccessInfo::buildSummary(Loop *Scope) {
    //1.先绑定查询作用域；缺少循环或底层依赖结果时保留UnknownMemory。
    auto Summary = std::make_unique<LoopAccessSummary>();
    Summary->Scope = Scope;
    if (!Scope || !LD) return Summary;
    //2.复用依赖分析中的稳定访问记录，并汇总是否存在写入。
    Summary->HasUnknownMemory = LD->hasUnknownMemory(Scope);
    const auto &Memory = LD->getMemoryAccesses(Scope);
    Summary->Accesses.reserve(Memory.size());
    Summary->Footprints.reserve(Memory.size());
    for (const LoopMemoryAccess &Access : Memory) {
        Summary->Accesses.push_back(&Access);
        Summary->Footprints.push_back(buildFootprint(Scope, Access));
        Summary->HasWrite = Summary->HasWrite || Access.Writes;
    }
    return Summary;
}
//分类两个循环轴上的内存访问是否冲突或仅同次迭代冲突
LoopAccessPairKind LoopAccessInfo::classifyPair(
    Loop *FirstScope, Loop *FirstAxis, const LoopMemoryAccess &First,
    Loop *SecondScope, Loop *SecondAxis,
    const LoopMemoryAccess &Second) const {
    (void)FirstScope;
    (void)SecondScope;
    //1.没有写入的一对访问不会形成调度冲突。
    if (!isWriteConflict(First, Second))
        return LoopAccessPairKind::NoConflict;
    //2.缺少基础分析或稳定根对象时不能发布强结论。
    if (!AA || !LI || !First.Root || !Second.Root)
        return LoopAccessPairKind::Unknown;
    //3.不同根对象消费BasicAA的NoAlias，同根继续做跨迭代证明。
    if (First.Root != Second.Root &&
        AA->alias(First.Location, Second.Location, *LI) ==
            AliasInfo::NoAlias) {
        return LoopAccessPairKind::NoConflict;
    }
    //4.地址相等若强制两个轴坐标相等，冲突只会出现在同一逻辑迭代。
    if (equalityForcesAlignedIteration(First, FirstAxis, Second,
                                       SecondAxis)) {
        return LoopAccessPairKind::SameIterationOnly;
    }
    return LoopAccessPairKind::Unknown;
}
//判断作用域内各次指定轴迭代之间是否不存在未知写冲突
bool LoopAccessInfo::areIterationsIndependent(Loop *Scope, Loop *Axis) {
    //1.验证轴属于作用域，并拒绝包含未知内存的摘要。
    if (!Scope || !Axis || !Scope->contains(Axis->getHeader())) return false;
    const LoopAccessSummary &Summary = getSummary(Scope);
    if (Summary.HasUnknownMemory) return false;
    //2.枚举所有含写访问对，预算耗尽或任一Unknown都使证明失败。
    std::size_t Examined = 0;
    for (std::size_t I = 0; I < Summary.Accesses.size(); ++I) {
        for (std::size_t J = I; J < Summary.Accesses.size(); ++J) {
            const LoopMemoryAccess *First = Summary.Accesses[I];
            const LoopMemoryAccess *Second = Summary.Accesses[J];
            if (!First || !Second || !isWriteConflict(*First, *Second))
                continue;
            if (++Examined > MaxTransformAccessPairs) return false;
            if (classifyPair(Scope, Axis, *First, Scope, Axis, *Second) ==
                LoopAccessPairKind::Unknown) {
                return false;
            }
        }
    }
    return true;
}
//分析两个循环的访问关系并生成融合合法性摘要
LoopFusionAccessResult LoopAccessInfo::analyzeFusion(Loop *First,
                                                     Loop *Second) {
    //1.两个循环必须不同且都具有完整、无未知内存的访问摘要。
    LoopFusionAccessResult Result;
    if (!First || !Second || First == Second) {
        Result.Unknown = true;
        return Result;
    }
    const LoopAccessSummary &A = getSummary(First);
    const LoopAccessSummary &B = getSummary(Second);
    if (A.HasUnknownMemory || B.HasUnknownMemory) {
        Result.Unknown = true;
        return Result;
    }
    //2.交叉枚举访问对并分类，遇到未知关系或预算耗尽立即保守退出。
    std::size_t Examined = 0;
    for (std::size_t FirstIndex = 0;
         FirstIndex < A.Accesses.size(); ++FirstIndex) {
        const LoopMemoryAccess *FirstAccess = A.Accesses[FirstIndex];
        for (std::size_t SecondIndex = 0;
             SecondIndex < B.Accesses.size(); ++SecondIndex) {
            const LoopMemoryAccess *SecondAccess = B.Accesses[SecondIndex];
            if (!FirstAccess || !SecondAccess ||
                !isWriteConflict(*FirstAccess, *SecondAccess)) {
                continue;
            }
            if (++Examined > MaxTransformAccessPairs) {
                Result.Unknown = true;
                return Result;
            }
            LoopAccessPairKind Kind = LoopAccessPairKind::Unknown;
            const LoopAccessFootprint *FirstFootprint =
                FirstIndex < A.Footprints.size()
                    ? &A.Footprints[FirstIndex]
                    : nullptr;
            const LoopAccessFootprint *SecondFootprint =
                SecondIndex < B.Footprints.size()
                    ? &B.Footprints[SecondIndex]
                    : nullptr;

            // 不同底层对象的普通 Location 查询会因动态下标暂时缺少
            // 对象内证明而退回 MayAlias；循环足迹已经独立证明完整边界时，
            // 可以安全复用对象级 NoAlias，且不依赖越界未定义行为。
            if (FirstAccess->Root != SecondAccess->Root &&
                FirstFootprint != nullptr && SecondFootprint != nullptr &&
                FirstFootprint->ObjectSize.has_value() &&
                SecondFootprint->ObjectSize.has_value() &&
                FirstFootprint->Exact && SecondFootprint->Exact && AA && LI &&
                AA->alias(FirstAccess->Root, SecondAccess->Root, *LI) ==
                    AliasInfo::NoAlias) {
                Kind = LoopAccessPairKind::NoConflict;
            } else {
                Kind = classifyPair(
                    First, First, *FirstAccess,
                    Second, Second, *SecondAccess);
            }
            if (Kind == LoopAccessPairKind::Unknown) {
                Result.Unknown = true;
                return Result;
            }
            if (Kind == LoopAccessPairKind::SameIterationOnly) {
                ++Result.SameIterationConflictPairs;
                if (FirstAccess->Writes && SecondAccess->Reads)
                    ++Result.SameIterationFlowPairs;
            }
        }
    }
    //3.所有潜在冲突均已排除或限制在同次迭代，融合合法。
    Result.Legal = true;
    return Result;
}
//构建函数级LoopAccessInfo并绑定所需基础分析结果
LoopAccessInfo LoopAccessAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    //1.显式取得循环、SCEV、范围、支配、别名和依赖分析结果。
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LD = FAM.getResult<LoopDependenceAnalysis>(F);
    //2.封装惰性缓存查询器，具体循环摘要由首次查询触发构建。
    return LoopAccessInfo(&F, &LI, &SE, &AA, &LD, &RI, &DT);
}
