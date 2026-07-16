#include "ir/analysis/LoopDependence.hpp"

#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <utility>

bool LoopDependenceInfo::checkedAdd(int64_t A, int64_t B, int64_t &Result) {
    const __int128 Wide = static_cast<__int128>(A) +
                          static_cast<__int128>(B);
    if (Wide < std::numeric_limits<int64_t>::min() ||
        Wide > std::numeric_limits<int64_t>::max()) {
        return false;
    }
    Result = static_cast<int64_t>(Wide);
    return true;
}

bool LoopDependenceInfo::checkedMul(int64_t A, int64_t B, int64_t &Result) {
    const __int128 Wide = static_cast<__int128>(A) *
                          static_cast<__int128>(B);
    if (Wide < std::numeric_limits<int64_t>::min() ||
        Wide > std::numeric_limits<int64_t>::max()) {
        return false;
    }
    Result = static_cast<int64_t>(Wide);
    return true;
}

void LoopDependenceInfo::eraseZeroTerms(LoopAffineExpr &Expression) {
    for (auto It = Expression.LoopCoefficients.begin();
         It != Expression.LoopCoefficients.end();) {
        if (It->second == 0)
            It = Expression.LoopCoefficients.erase(It);
        else
            ++It;
    }
    for (auto It = Expression.InvariantTerms.begin();
         It != Expression.InvariantTerms.end();) {
        if (It->second == 0)
            It = Expression.InvariantTerms.erase(It);
        else
            ++It;
    }
}

bool LoopDependenceInfo::addScaledAffine(LoopAffineExpr &Destination,
                                         const LoopAffineExpr &Source,
                                         int64_t Scale) {
    if (!Destination.Exact || !Source.Exact) {
        Destination.Exact = false;
        return false;
    }

    int64_t Scaled = 0;
    int64_t Updated = 0;
    if (!checkedMul(Source.Constant, Scale, Scaled) ||
        !checkedAdd(Destination.Constant, Scaled, Updated)) {
        Destination.Exact = false;
        return false;
    }
    Destination.Constant = Updated;

    for (const auto &Entry : Source.LoopCoefficients) {
        if (!checkedMul(Entry.second, Scale, Scaled) ||
            !checkedAdd(Destination.LoopCoefficients[Entry.first], Scaled,
                        Updated)) {
            Destination.Exact = false;
            return false;
        }
        Destination.LoopCoefficients[Entry.first] = Updated;
    }
    for (const auto &Entry : Source.InvariantTerms) {
        if (!checkedMul(Entry.second, Scale, Scaled) ||
            !checkedAdd(Destination.InvariantTerms[Entry.first], Scaled,
                        Updated)) {
            Destination.Exact = false;
            return false;
        }
        Destination.InvariantTerms[Entry.first] = Updated;
    }

    eraseZeroTerms(Destination);
    return true;
}

bool LoopDependenceInfo::loopIsInScope(const Loop *Candidate,
                                       const Loop *Scope) {
    if (!Candidate || !Scope) return false;
    return Candidate == Scope ||
           (Candidate->getHeader() && Scope->contains(Candidate->getHeader()));
}

void LoopDependenceInfo::collectLoopsPreorder(
    Loop *L, std::vector<Loop *> &Loops) {
    if (!L) return;
    Loops.push_back(L);
    for (Loop *SubLoop : L->getSubLoops())
        collectLoopsPreorder(SubLoop, Loops);
}

bool LoopDependenceInfo::isPureAxisCoordinate(
    const LoopAffineExpr &Expression, Loop *Axis, int64_t &Coefficient) {
    if (!Expression.Exact || !Axis || !Expression.InvariantTerms.empty() ||
        Expression.LoopCoefficients.size() != 1) {
        return false;
    }
    auto It = Expression.LoopCoefficients.find(Axis);
    if (It == Expression.LoopCoefficients.end() || It->second == 0)
        return false;
    Coefficient = It->second;
    return true;
}

LoopDependenceInfo::LoopDependenceInfo() = default;

LoopDependenceInfo::LoopDependenceInfo(Function *F, LoopInfo *LoopInfoPtr,
                                       SCEVResult *SCEV,
                                       BasicAAResult *AliasAnalysis)
    : Func(F), LI(LoopInfoPtr), SE(SCEV), AA(AliasAnalysis) {}

LoopDependenceInfo::~LoopDependenceInfo() = default;
LoopDependenceInfo::LoopDependenceInfo(LoopDependenceInfo &&) noexcept =
    default;
LoopDependenceInfo &
LoopDependenceInfo::operator=(LoopDependenceInfo &&) noexcept = default;

LoopAffineExpr LoopDependenceInfo::buildAffine(const SCEVExpr *E,
                                               Loop *Scope) const {
    LoopAffineExpr Result;
    if (!E || !Scope) return Result;

    switch (E->Kind) {
    case SCEVExprKind::Constant:
        Result.Exact = true;
        Result.Constant = E->ConstValue;
        return Result;

    case SCEVExprKind::Unknown: {
        Value *Raw = E->RawValue;
        if (!Raw) return Result;
        auto *I = dynamic_cast<Instruction *>(Raw);
        BasicBlock *BB = I ? I->getParent() : nullptr;
        if (BB && Scope->contains(BB)) return Result;
        Result.Exact = true;
        Result.InvariantTerms[E] = 1;
        return Result;
    }

    case SCEVExprKind::Add:
    case SCEVExprKind::Sub: {
        if (E->Operands.size() != 2) return Result;
        Result = buildAffine(E->Operands[0], Scope);
        LoopAffineExpr RHS = buildAffine(E->Operands[1], Scope);
        if (!addScaledAffine(Result, RHS,
                             E->Kind == SCEVExprKind::Add ? 1 : -1)) {
            Result.Exact = false;
        }
        return Result;
    }

    case SCEVExprKind::Mul: {
        if (E->Operands.size() != 2) return Result;
        const SCEVExpr *Constant = nullptr;
        const SCEVExpr *Other = nullptr;
        if (E->Operands[0] &&
            E->Operands[0]->Kind == SCEVExprKind::Constant) {
            Constant = E->Operands[0];
            Other = E->Operands[1];
        } else if (E->Operands[1] &&
                   E->Operands[1]->Kind == SCEVExprKind::Constant) {
            Constant = E->Operands[1];
            Other = E->Operands[0];
        }
        if (!Constant || !Other) return Result;
        Result.Exact = true;
        LoopAffineExpr Operand = buildAffine(Other, Scope);
        if (!addScaledAffine(Result, Operand, Constant->ConstValue))
            Result.Exact = false;
        return Result;
    }

    case SCEVExprKind::Cast:
        // Truncation is modular and cannot be represented by this affine
        // vocabulary.  The remaining integer-preserving casts are transparent
        // for the canonical non-wrapping induction values consumed here.
        if (E->CastKind == SCEVCastKind::Trunc || E->Operands.size() != 1)
            return Result;
        return buildAffine(E->Operands[0], Scope);

    case SCEVExprKind::AddRec: {
        if (E->Operands.size() != 2 || !E->RecLoop) return Result;

        // A recurrence outside Scope is a symbolic invariant for this query.
        // Keeping it whole avoids incorrectly exposing a parent-loop axis in
        // an inner-loop-only dependence vector.
        if (!loopIsInScope(E->RecLoop, Scope)) {
            Result.Exact = true;
            Result.InvariantTerms[E] = 1;
            return Result;
        }

        Result = buildAffine(E->Operands[0], Scope);
        LoopAffineExpr Step = buildAffine(E->Operands[1], Scope);
        if (!Result.Exact || !Step.Exact ||
            !Step.LoopCoefficients.empty() ||
            !Step.InvariantTerms.empty()) {
            Result.Exact = false;
            return Result;
        }

        int64_t Updated = 0;
        if (!checkedAdd(Result.LoopCoefficients[E->RecLoop], Step.Constant,
                        Updated)) {
            Result.Exact = false;
            return Result;
        }
        Result.LoopCoefficients[E->RecLoop] = Updated;
        eraseZeroTerms(Result);
        return Result;
    }

    case SCEVExprKind::Div:
    case SCEVExprKind::Mod:
    case SCEVExprKind::Min:
    case SCEVExprKind::Max:
    case SCEVExprKind::Compare:
    case SCEVExprKind::MulRec:
    case SCEVExprKind::DivRec:
        return Result;
    }
    return Result;
}

bool LoopDependenceInfo::buildMemoryAccess(Instruction *I, Loop *Scope,
                                           std::size_t Order,
                                           LoopMemoryAccess &Access) const {
    if (!I || !Scope || !SE) return false;

    if (auto *Load = dynamic_cast<LoadInst *>(I)) {
        Access.Inst = I;
        Access.Pointer = Load->getPointerOperand();
        Access.Location = MemoryLocation::get(Load);
        Access.Reads = true;
    } else if (auto *Store = dynamic_cast<StoreInst *>(I)) {
        Access.Inst = I;
        Access.Pointer = Store->getPointerOperand();
        Access.Location = MemoryLocation::get(Store);
        Access.Writes = true;
    } else {
        return false;
    }
    Access.Order = Order;
    if (!Access.Pointer) return false;

    // Group index expressions by their immediate typed byte stride.  This is
    // intentionally not flattenZeroBasedChain(): split row/element GEPs often
    // have a non-zero first index and are still exactly representable.
    std::map<int64_t, LoopAffineExpr, std::greater<int64_t>> Bands;
    Value *Current = Access.Pointer;
    std::unordered_set<Value *> Visited;
    unsigned Depth = 0;
    unsigned IndexTerms = 0;
    bool Exact = true;

    while (Current) {
        if (Depth++ >= MaxPointerChainDepth ||
            !Visited.insert(Current).second) {
            Exact = false;
            break;
        }
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Current)) {
            Value *Base = nullptr;
            std::vector<Value *> Indices;
            if (!GEP->getBaseAndIndices(Base, Indices)) {
                Exact = false;
                break;
            }

            std::vector<int64_t> Strides;
            int64_t FinalPointeeStride = 0;
            if (!GetElementPtrInst::getIndexStrides(
                    Base, Indices, Strides, FinalPointeeStride) ||
                Strides.size() != Indices.size()) {
                Exact = false;
                break;
            }
            (void)FinalPointeeStride;

            for (std::size_t Index = 0; Index < Indices.size(); ++Index) {
                if (++IndexTerms > MaxGEPIndexTerms || Strides[Index] <= 0) {
                    Exact = false;
                    break;
                }
                LoopAffineExpr Component =
                    buildAffine(SE->getSCEV(Indices[Index]), Scope);
                auto It = Bands.find(Strides[Index]);
                if (It == Bands.end()) {
                    LoopAffineExpr Zero;
                    Zero.Exact = true;
                    It = Bands.emplace(Strides[Index], std::move(Zero)).first;
                }
                if (!addScaledAffine(It->second, Component, 1)) {
                    Exact = false;
                    break;
                }
            }
            if (!Exact) break;
            Current = Base;
            continue;
        }

        if (auto *BitCast = dynamic_cast<BitCastInst *>(Current)) {
            Current = BitCast->getUnaryOperand();
            continue;
        }
        break;
    }

    if (!Current) Exact = false;
    Access.Root = Current;

    // A loop-local pointer PHI/select/load that was not decomposed above can
    // change its allocation object between iterations.  It is not a stable
    // affine root even when all explicit GEP indices were affine.
    if (auto *RootI = dynamic_cast<Instruction *>(Current)) {
        BasicBlock *RootBB = RootI->getParent();
        if (RootBB && Scope->contains(RootBB)) Exact = false;
    }

    Access.Subscripts.reserve(Bands.size());
    Access.ByteStrides.reserve(Bands.size());
    for (auto &Band : Bands) {
        Access.ByteStrides.push_back(Band.first);
        Access.Subscripts.push_back(std::move(Band.second));
        if (!Access.Subscripts.back().Exact) Exact = false;
    }
    Access.AddressExact = Exact && Access.Root;
    return Access.AddressExact;
}

bool LoopDependenceInfo::isWriteConflict(const LoopMemoryAccess &A,
                                         const LoopMemoryAccess &B) {
    return (A.Writes || B.Writes) &&
           (A.Reads || A.Writes) && (B.Reads || B.Writes);
}

bool LoopDependenceInfo::areNoAlias(const LoopMemoryAccess &A,
                                    const LoopMemoryAccess &B) const {
    if (!AA || !LI || !A.Pointer || !B.Pointer) return false;
    // A same-root NoAlias can describe only the two current SSA instances
    // (for example A[i] versus A[i+1]); those locations may still meet in
    // different iterations.  Consume BasicAA only after the complete GEP
    // chains lead to different roots.
    if (!A.Root || !B.Root || A.Root == B.Root) return false;
    // NoAlias is the only BasicAA answer consumed here.  MustAlias describes
    // two SSA pointer values in one execution; it is not a loop-carried
    // dependence proof.
    return AA->alias(A.Location, B.Location, *LI) == AliasInfo::NoAlias;
}

bool LoopDependenceInfo::areForcedEqual(const LoopMemoryAccess &A,
                                        const LoopMemoryAccess &B,
                                        Loop *Axis) const {
    if (!Axis || !A.AddressExact || !B.AddressExact || !A.Root ||
        A.Root != B.Root || !A.Location.Size || !B.Location.Size ||
        *A.Location.Size == 0 || *A.Location.Size != *B.Location.Size ||
        A.ByteStrides.empty() || A.ByteStrides != B.ByteStrides ||
        A.Subscripts.size() != A.ByteStrides.size() ||
        B.Subscripts.size() != B.ByteStrides.size()) {
        return false;
    }

    // The typed bands must form a proper mixed-radix SysY array address and
    // the access itself must fit in its smallest element.  SysY defines an
    // execution with an out-of-range array digit as invalid; this digit-bound
    // premise is stronger than LLVM's `inbounds` flag alone.  It makes every
    // well-defined typed coordinate unique.  Flattened or partially
    // overlapping shapes such as an i32 access through an i8 GEP are rejected
    // and require a byte-range dependence solver instead.  Equal-stride terms
    // were folded while building the access.
    if (*A.Location.Size >
        static_cast<std::size_t>(A.ByteStrides.back())) {
        return false;
    }
    for (std::size_t I = 1; I < A.ByteStrides.size(); ++I) {
        const int64_t OuterStride = A.ByteStrides[I - 1];
        const int64_t InnerStride = A.ByteStrides[I];
        if (OuterStride <= InnerStride || InnerStride <= 0 ||
            OuterStride % InnerStride != 0) {
            return false;
        }
    }

    for (std::size_t AI = 0; AI < A.ByteStrides.size(); ++AI) {
        const LoopAffineExpr &AExpr = A.Subscripts[AI];
        const LoopAffineExpr &BExpr = B.Subscripts[AI];
        int64_t ACoefficient = 0;
        int64_t BCoefficient = 0;
        if (!isPureAxisCoordinate(AExpr, Axis, ACoefficient) ||
            !isPureAxisCoordinate(BExpr, Axis, BCoefficient)) {
            continue;
        }
        if (ACoefficient == BCoefficient &&
            AExpr.Constant == BExpr.Constant) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<LoopDependenceInfo::LoopSummary>
LoopDependenceInfo::buildSummary(Loop *L) {
    auto Summary = std::make_unique<LoopSummary>();
    if (!L || !Func || !LI || !SE || !AA) {
        Summary->UnknownMemory = true;
        return Summary;
    }

    collectLoopsPreorder(L, Summary->Loops);

    std::size_t Order = 0;
    for (BasicBlock *BB : L->getBlocksInFunctionOrder()) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            const std::size_t ThisOrder = Order++;
            if (dynamic_cast<CallInst *>(I)) {
                Summary->UnknownMemory = true;
                continue;
            }
            if (!dynamic_cast<LoadInst *>(I) &&
                !dynamic_cast<StoreInst *>(I)) {
                continue;
            }
            if (Summary->Accesses.size() >= MaxMemoryAccesses) {
                Summary->UnknownMemory = true;
                continue;
            }
            LoopMemoryAccess Access;
            if (!buildMemoryAccess(I, L, ThisOrder, Access))
                Summary->UnknownMemory = true;
            Summary->Accesses.push_back(std::move(Access));
        }
    }

    const std::size_t Count = Summary->Accesses.size();
    std::size_t ExaminedPairs = 0;
    for (std::size_t I = 0; I < Count; ++I) {
        for (std::size_t J = I; J < Count; ++J) {
            LoopMemoryAccess &Source = Summary->Accesses[I];
            LoopMemoryAccess &Sink = Summary->Accesses[J];
            if (!isWriteConflict(Source, Sink)) continue;
            if (++ExaminedPairs > MaxDependencePairs) {
                Summary->UnknownMemory = true;
                break;
            }
            if (areNoAlias(Source, Sink)) continue;

            LoopDependenceRecord Record;
            Record.Source = &Source;
            Record.Sink = &Sink;
            if (Source.Writes && Sink.Writes)
                Record.Kind = LoopDependenceKind::Output;
            else if (Source.Writes && Sink.Reads)
                Record.Kind = LoopDependenceKind::Flow;
            else if (Source.Reads && Sink.Writes)
                Record.Kind = LoopDependenceKind::Anti;
            else
                Record.Kind = LoopDependenceKind::Unknown;

            Record.Loops = Summary->Loops;
            Record.DirectionMasks.reserve(Record.Loops.size());
            Record.Exact = Source.AddressExact && Sink.AddressExact;
            for (Loop *Axis : Record.Loops) {
                const bool Equal = areForcedEqual(Source, Sink, Axis);
                Record.DirectionMasks.push_back(
                    Equal ? static_cast<uint8_t>(LoopDepSameIteration)
                          : static_cast<uint8_t>(LoopDepAnyDirection));
                Record.Exact = Record.Exact && Equal;
            }
            Summary->Dependences.push_back(std::move(Record));
        }
        if (ExaminedPairs > MaxDependencePairs) break;
    }

    return Summary;
}

LoopDependenceInfo::LoopSummary &
LoopDependenceInfo::getOrBuildSummary(Loop *L) {
    auto It = Cache.find(L);
    if (It != Cache.end()) return *It->second;
    auto Summary = buildSummary(L);
    LoopSummary *Raw = Summary.get();
    Cache.emplace(L, std::move(Summary));
    return *Raw;
}

const std::vector<LoopMemoryAccess> &
LoopDependenceInfo::getMemoryAccesses(Loop *L) {
    return L ? getOrBuildSummary(L).Accesses : EmptyMemoryAccesses;
}

const std::vector<LoopDependenceRecord> &
LoopDependenceInfo::getDependences(Loop *L) {
    return L ? getOrBuildSummary(L).Dependences : EmptyDependenceRecords;
}

bool LoopDependenceInfo::hasUnknownMemory(Loop *L) {
    return !L || getOrBuildSummary(L).UnknownMemory;
}

bool LoopDependenceInfo::isInterchangeLegal(Loop *Outer, Loop *Inner) {
    if (!Outer || !Inner || Outer == Inner || !Inner->getHeader() ||
        !Outer->contains(Inner->getHeader())) {
        return false;
    }

    LoopSummary &Summary = getOrBuildSummary(Outer);
    if (Summary.UnknownMemory) return false;

    for (const LoopDependenceRecord &Record : Summary.Dependences) {
        bool OuterEqual = false;
        bool InnerEqual = false;
        for (std::size_t I = 0;
             I < Record.Loops.size() && I < Record.DirectionMasks.size();
             ++I) {
            if (Record.Loops[I] == Outer)
                OuterEqual =
                    Record.DirectionMasks[I] == LoopDepSameIteration;
            if (Record.Loops[I] == Inner)
                InnerEqual =
                    Record.DirectionMasks[I] == LoopDepSameIteration;
        }
        if (!OuterEqual && !InnerEqual) return false;
    }
    return true;
}

bool LoopDependenceInfo::isDistributionLegal(
    Loop *L, const InstructionSet &First, const InstructionSet &Second) {
    if (!L) return false;
    LoopSummary &Summary = getOrBuildSummary(L);
    if (Summary.UnknownMemory) return false;

    for (const LoopMemoryAccess &SecondAccess : Summary.Accesses) {
        if (!Second.count(SecondAccess.Inst)) continue;
        for (const LoopMemoryAccess &FirstAccess : Summary.Accesses) {
            if (!First.count(FirstAccess.Inst) ||
                !isWriteConflict(SecondAccess, FirstAccess)) {
                continue;
            }
            if (areNoAlias(SecondAccess, FirstAccess)) continue;
            // Fission executes the complete First partition before Second.
            // Prove that this agrees with the original dynamic order.  A
            // function-layout order across blocks is not an execution-order
            // proof, so this compact implementation accepts only the exact
            // same-block instruction order and otherwise refuses the cut.
            if (!FirstAccess.Inst || !SecondAccess.Inst ||
                FirstAccess.Inst->getParent() !=
                    SecondAccess.Inst->getParent() ||
                FirstAccess.Order >= SecondAccess.Order) {
                return false;
            }
            // Distribution serializes two copies of L's complete loop tree.
            // A Second -> First conflict is therefore safe only when address
            // equality forces the two dynamic accesses into the same
            // iteration on every cloned axis.  Checking L alone would miss a
            // dependence carried by one of its descendants.
            for (Loop *Axis : Summary.Loops) {
                if (!areForcedEqual(SecondAccess, FirstAccess, Axis))
                    return false;
            }
        }
    }
    return true;
}

std::optional<int64_t>
LoopDependenceInfo::getByteStride(Instruction *I, Loop *L) {
    if (!I || !L) return std::nullopt;
    LoopSummary &Summary = getOrBuildSummary(L);
    for (const LoopMemoryAccess &Access : Summary.Accesses) {
        if (Access.Inst != I) continue;
        if (!Access.AddressExact ||
            Access.Subscripts.size() != Access.ByteStrides.size()) {
            return std::nullopt;
        }

        int64_t Total = 0;
        for (std::size_t Index = 0; Index < Access.Subscripts.size();
             ++Index) {
            const LoopAffineExpr &Expression = Access.Subscripts[Index];
            auto It = Expression.LoopCoefficients.find(L);
            if (It == Expression.LoopCoefficients.end()) continue;
            int64_t Contribution = 0;
            int64_t Updated = 0;
            if (!checkedMul(It->second, Access.ByteStrides[Index],
                            Contribution) ||
                !checkedAdd(Total, Contribution, Updated)) {
                return std::nullopt;
            }
            Total = Updated;
        }
        return Total;
    }
    return std::nullopt;
}

bool LoopDependenceInfo::invalidate(Function &,
                                    const PreservationStatus &PA) {
    return !PA.isKept(LoopDependenceAnalysis::ID()) ||
           !PA.isKept(LoopAnalysis::ID()) ||
           !PA.isKept(SCEVAnalysis::ID()) ||
           !PA.isKept(BasicAliasAnalysis::ID());
}

LoopDependenceInfo LoopDependenceAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    return LoopDependenceInfo(&F, &LI, &SE, &AA);
}
