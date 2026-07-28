#include "ir/analysis/LoopAccessAnalysis.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"

#include <utility>
LoopAccessInfo::LoopAccessInfo() = default;

LoopAccessInfo::LoopAccessInfo(Function *F, LoopInfo *LoopInfoPtr,
                               SCEVResult *SCEV,
                               BasicAAResult *AliasAnalysis,
                               LoopDependenceInfo *Dependence)
    : Func(F), LI(LoopInfoPtr), SE(SCEV), AA(AliasAnalysis), LD(Dependence) {}

LoopAccessInfo::LoopAccessInfo(LoopAccessInfo &&) noexcept = default;

LoopAccessInfo &LoopAccessInfo::operator=(LoopAccessInfo &&) noexcept =
    default;

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
    for (const LoopMemoryAccess &Access : Memory) {
        Summary->Accesses.push_back(&Access);
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
        !SE->isKnownFiniteLoop(Outer) || !SE->isKnownFiniteLoop(Inner)) {
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
           !PA.isKept(LoopAnalysis::ID());
}

// Pass 主流程依次获取四项基础分析，并将其指针封装为函数级访问查询器。
LoopAccessInfo LoopAccessAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    // 1. 按依赖关系取得循环、SCEV、别名和循环依赖四项基础结果。
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LD = FAM.getResult<LoopDependenceAnalysis>(F);
    // 2. 封装惰性缓存查询器；具体循环摘要由首次查询触发构建。
    return LoopAccessInfo(&F, &LI, &SE, &AA, &LD);
}
