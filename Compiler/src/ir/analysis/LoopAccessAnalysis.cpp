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
LoopAccessInfo::LoopAccessInfo() = default;

LoopAccessInfo::LoopAccessInfo(Function *F, LoopInfo *LoopInfoPtr,
                               SCEVResult *SCEV,
                               BasicAAResult *AliasAnalysis,
                               LoopDependenceInfo *Dependence,
                               RangeInfo *RangeAnalysis,
                               DominatorTree *DomTree)
    : Func(F), LI(LoopInfoPtr), SE(SCEV), AA(AliasAnalysis), LD(Dependence),
      RI(RangeAnalysis), DT(DomTree) {}

LoopAccessInfo::LoopAccessInfo(LoopAccessInfo &&) noexcept = default;

LoopAccessInfo &LoopAccessInfo::operator=(LoopAccessInfo &&) noexcept =
    default;

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

std::uint64_t LoopAccessInfo::absoluteMagnitude(std::int64_t Value) {
    return Value < 0
        ? static_cast<std::uint64_t>(-(Value + 1)) + 1U
        : static_cast<std::uint64_t>(Value);
}

// 常量步长先走 SCEV 自身证明；符号步长仅在 Range 给出严格方向且
// SCEV 同时证明退出方向和最坏更新不回绕时，才标记为有限。
bool LoopAccessInfo::isKnownFinite(Loop *Axis) const {
    if (!SE || !Axis) return false;
    if (SE->isKnownFiniteLoop(Axis)) return true;
    return RI && DT && SE->isKnownFiniteLoop(Axis, *RI, *DT);
}

// Gate 必须出现在全部回边之前；除循环头的零次退出外，其他提前退出也
// 必须经过 Gate。这样条件块中的访存不会被误当成每次迭代都执行。
bool LoopAccessInfo::dominatesIterationPaths(
    BasicBlock *Gate, Loop *Axis) const {
    if (!Gate || !Axis || !DT || !Axis->contains(Gate) ||
        Axis->getBackedgeSrcs().empty()) {
        return false;
    }
    if (Gate == Axis->getHeader() &&
        Axis->isExitingBlock(Axis->getHeader())) {
        // 头部退出循环中，Header 指令的执行次数通常是 TripCount+1，
        // 当前足迹表达式尚不能表示这一额外的条件检查。
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

// 相邻两轴只有在内层入口无旁路且次数表达式不依赖外层坐标时，才能
// 作为一个笛卡尔积维度。兄弟循环不会参与当前访问的轴链。
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

// LoopDependence 的仿射式描述数学系数，但 i32 中间表达式可能先回绕。
// 这里用指令上下文范围重新验证每个加减乘节点，任何证据不足都阻止
// Exact/FullAccess，避免把源语言未定义行为当成优化前提。
bool LoopAccessInfo::isIntegerAddressExpressionNoWrap(
    Value *Expression, Instruction *Context,
    AddressNoWrapQueryState &State, unsigned Depth) const {
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

void LoopAccessInfo::collectLoopAxes(Loop *Scope,
                                     std::vector<Loop *> &Axes) {
    if (!Scope) return;
    Axes.push_back(Scope);
    for (Loop *SubLoop : Scope->getSubLoops())
        collectLoopAxes(SubLoop, Axes);
}

// 地址的总字节步长是各类型化下标带中“轴系数×带步长”的精确和。
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

// 常量起始偏移只接受没有循环外符号项的仿射地址，避免把未知参数当成零。
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

bool LoopAccessInfo::isWriteConflict(const LoopMemoryAccess &First,
                                     const LoopMemoryAccess &Second) {
    return (First.Writes || Second.Writes) &&
           (First.Reads || First.Writes) &&
           (Second.Reads || Second.Writes);
}

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

// 对齐证明要求两侧均为完整混合基数地址，并且目标轴只在一个坐标带中以同向单位系数出现。
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

// 足迹构建把依赖分析的仿射地址、SCEV 次数和 BasicAA 来源事实约减为
// 一个只读摘要；任一证据不足都只清除对应字段，不影响其他已证明事实。
LoopAccessFootprint LoopAccessInfo::buildFootprint(
    Loop *Scope, const LoopMemoryAccess &Access) {
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

    // 查询一个子循环或兄弟循环时，作用域外递推在当前循环内虽是不变量，
    // 但其运行时最终值不是逻辑迭代零；不能把该项静默当成零偏移。
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
        // 指针本身若已有完整常量偏移，优先使用它；否则把
        // “Root 到底层对象”和“访问到 Root”两个坐标系显式相加。
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
        // 当前作用域可能含有多个兄弟子循环；一个具体访问只属于覆盖其
        // 指令块的轴链，不能把兄弟循环的次数误乘进该访问的足迹。
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

    // 最内层发生变化的维度若每步恰好跨一个元素，则该维内部为连续访问。
    for (auto It = Result.Dimensions.rbegin();
         It != Result.Dimensions.rend(); ++It) {
        if (!It->ByteStride || *It->ByteStride == 0) continue;
        Result.IsConsecutive = It->IsUnitStride;
        break;
    }
    Result.IsConsecutive =
        Result.IsConsecutive && MustExecute && AddressNoWrap &&
        !HasNoIteration;

    // 常量次数下可发布精确半开区间；零次循环不声称存在任何访问。
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

    // 单层运行时次数同样能表达完整连续区间；多层情况只有常量混合基数
    // 完全吻合时才发布 IsFullAccess。
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

    // 已知分配对象不能借源语言越界行为发布强足迹结论。常量区间必须
    // 完整落在 [0, ObjectSize) 内；若只有符号次数而暂时证明不了对象内，
    // 仍可保留“单位字节步长”这一局部事实，但不得标成 Exact/FullAccess。
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

// 摘要构建复用循环依赖访问记录，只补充变换关心的未知内存和写入汇总。
std::unique_ptr<LoopAccessSummary>
LoopAccessInfo::buildSummary(Loop *Scope) {
    // 1. 先绑定查询作用域；缺少循环或底层依赖结果时保留 UnknownMemory。
    auto Summary = std::make_unique<LoopAccessSummary>();
    Summary->Scope = Scope;
    if (!Scope || !LD) return Summary;

    // 2. 复用依赖分析中的稳定访问记录，并汇总是否存在写入。
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

const LoopAccessSummary &LoopAccessInfo::getSummary(Loop *Scope) {
    auto It = Cache.find(Scope);
    if (It != Cache.end()) return *It->second;
    auto Summary = buildSummary(Scope);
    LoopAccessSummary *Raw = Summary.get();
    Cache.emplace(Scope, std::move(Summary));
    return *Raw;
}

const LoopAccessFootprint *LoopAccessInfo::getFootprint(
    Loop *Scope, const LoopMemoryAccess &Access) {
    const LoopAccessSummary &Summary = getSummary(Scope);
    for (const LoopAccessFootprint &Footprint : Summary.Footprints) {
        if (Footprint.Access == &Access) return &Footprint;
    }
    return nullptr;
}

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

// 访问对分类流程先筛掉只读对，再尝试不同根 NoAlias 和同根迭代对齐证明。
LoopAccessPairKind LoopAccessInfo::classifyPair(
    Loop *FirstScope, Loop *FirstAxis, const LoopMemoryAccess &First,
    Loop *SecondScope, Loop *SecondAxis,
    const LoopMemoryAccess &Second) const {
    (void)FirstScope;
    (void)SecondScope;
    // 1. 没有写入的一对访问不会形成调度冲突。
    if (!isWriteConflict(First, Second))
        return LoopAccessPairKind::NoConflict;
    // 2. 缺少基础分析或稳定根对象时不能发布强结论。
    if (!AA || !LI || !First.Root || !Second.Root)
        return LoopAccessPairKind::Unknown;

    // 3. 不同根对象可消费 BasicAA 的 NoAlias；同根必须继续做跨迭代证明。
    if (First.Root != Second.Root &&
        AA->alias(First.Location, Second.Location, *LI) ==
            AliasInfo::NoAlias) {
        return LoopAccessPairKind::NoConflict;
    }
    // 4. 地址相等若强制两个轴坐标相等，冲突只会出现在同一逻辑迭代。
    if (equalityForcesAlignedIteration(First, FirstAxis, Second,
                                       SecondAxis)) {
        return LoopAccessPairKind::SameIterationOnly;
    }
    return LoopAccessPairKind::Unknown;
}

// 迭代独立查询枚举全部潜在写冲突；未知内存、访问对预算耗尽或任一 Unknown 均失败。
bool LoopAccessInfo::areIterationsIndependent(Loop *Scope, Loop *Axis) {
    // 1. 验证轴属于作用域，并拒绝包含未知内存的摘要。
    if (!Scope || !Axis || !Scope->contains(Axis->getHeader())) return false;
    const LoopAccessSummary &Summary = getSummary(Scope);
    if (Summary.HasUnknownMemory) return false;

    // 2. 枚举所有含写访问对；预算耗尽或任一 Unknown 都使证明失败。
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

// 融合查询交叉枚举两个循环的访问，并统计仅同次迭代发生的冲突和流依赖。
LoopFusionAccessResult LoopAccessInfo::analyzeFusion(Loop *First,
                                                     Loop *Second) {
    // 1. 两个循环必须不同且都具有完整、无未知内存的访问摘要。
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

    // 2. 交叉枚举访问对并分类；遇到未知关系或预算耗尽立即保守退出。
    std::size_t Examined = 0;
    for (const LoopMemoryAccess *FirstAccess : A.Accesses) {
        for (const LoopMemoryAccess *SecondAccess : B.Accesses) {
            if (!FirstAccess || !SecondAccess ||
                !isWriteConflict(*FirstAccess, *SecondAccess)) {
                continue;
            }
            if (++Examined > MaxTransformAccessPairs) {
                Result.Unknown = true;
                return Result;
            }
            LoopAccessPairKind Kind = classifyPair(
                First, First, *FirstAccess, Second, Second, *SecondAccess);
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
    // 3. 所有潜在冲突均已排除或限制在同次迭代，融合合法。
    Result.Legal = true;
    return Result;
}

bool LoopAccessInfo::isFusionLegal(Loop *First, Loop *Second) {
    return analyzeFusion(First, Second).Legal;
}

// 矩形循环巢要求唯一父子关系、两层均有限且内层起点/步长/边界不依赖外层。
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

std::optional<std::int64_t>
LoopAccessInfo::getByteStride(Instruction *I, Loop *Scope, Loop *Axis) {
    if (!I || !Scope || !Axis || !LD || !Scope->contains(Axis->getHeader()))
        return std::nullopt;
    return LD->getByteStride(I, Axis);
}

// 跨轴复用要求同一读取在复用轴步长为零、在另一轴步长非零。
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

// Pass 主流程取得循环、范围、别名和依赖事实，并将其封装为函数级查询器。
LoopAccessInfo LoopAccessAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    // 1. Range 已依赖 SCEV；在这里显式消费二者，避免 SCEV 反向注册 Range。
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LD = FAM.getResult<LoopDependenceAnalysis>(F);
    // 2. 封装惰性缓存查询器；具体循环摘要由首次查询触发构建。
    return LoopAccessInfo(&F, &LI, &SE, &AA, &LD, &RI, &DT);
}
