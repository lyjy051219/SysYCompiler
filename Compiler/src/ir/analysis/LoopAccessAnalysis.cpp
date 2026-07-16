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

std::unique_ptr<LoopAccessSummary>
LoopAccessInfo::buildSummary(Loop *Scope) {
    auto Summary = std::make_unique<LoopAccessSummary>();
    Summary->Scope = Scope;
    if (!Scope || !LD) return Summary;

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

LoopAccessPairKind LoopAccessInfo::classifyPair(
    Loop *FirstScope, Loop *FirstAxis, const LoopMemoryAccess &First,
    Loop *SecondScope, Loop *SecondAxis,
    const LoopMemoryAccess &Second) const {
    (void)FirstScope;
    (void)SecondScope;
    if (!isWriteConflict(First, Second))
        return LoopAccessPairKind::NoConflict;
    if (!AA || !LI || !First.Root || !Second.Root)
        return LoopAccessPairKind::Unknown;

    // A same-root BasicAA answer relates only the two current SSA executions;
    // it cannot exclude a shifted loop-carried meeting.  Consume NoAlias only
    // for distinct affine roots.
    if (First.Root != Second.Root &&
        AA->alias(First.Location, Second.Location, *LI) ==
            AliasInfo::NoAlias) {
        return LoopAccessPairKind::NoConflict;
    }
    if (equalityForcesAlignedIteration(First, FirstAxis, Second,
                                       SecondAxis)) {
        return LoopAccessPairKind::SameIterationOnly;
    }
    return LoopAccessPairKind::Unknown;
}

bool LoopAccessInfo::areIterationsIndependent(Loop *Scope, Loop *Axis) {
    if (!Scope || !Axis || !Scope->contains(Axis->getHeader())) return false;
    const LoopAccessSummary &Summary = getSummary(Scope);
    if (Summary.HasUnknownMemory) return false;

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

LoopFusionAccessResult LoopAccessInfo::analyzeFusion(Loop *First,
                                                     Loop *Second) {
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
    Result.Legal = true;
    return Result;
}

bool LoopAccessInfo::isFusionLegal(Loop *First, Loop *Second) {
    return analyzeFusion(First, Second).Legal;
}

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

LoopAccessInfo LoopAccessAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LD = FAM.getResult<LoopDependenceAnalysis>(F);
    return LoopAccessInfo(&F, &LI, &SE, &AA, &LD);
}
