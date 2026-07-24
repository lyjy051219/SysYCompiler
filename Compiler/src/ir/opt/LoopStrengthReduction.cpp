#include "ir/opt/LoopStrengthReduction.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SCEVExpander.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

bool LoopStrengthReductionPass::hasCostlyElementStride(int64_t Stride) {
    return Stride > 0 && !IRMath::isPowerOf2(Stride);
}

std::string LoopStrengthReductionPass::scevKey(const SCEVExpr *E) {
    if (!E) return "<null>";
    if (!E->Key.empty()) return E->Key;
    return "scev@" + IRKey::pointer(E);
}

bool LoopStrengthReductionPass::hasNonTrivialPrefix(const std::vector<IndexComponent> &Prefix) {
    if (Prefix.empty()) return false;
    if (Prefix.size() == 1 && Prefix.front().Expr &&
        Prefix.front().Expr->isConstant(0))
        return false;
    return true;
}

bool LoopStrengthReductionPass::canUseInPreheader(Value *V, BasicBlock *PreHeader,
                              const DominatorTree &DT) {
    if (!V || !PreHeader) return false;
    if (V->isConst()) return true;

    if (auto *Var = dynamic_cast<Variable *>(V)) {
        if (Var->isParam() || Var->isGlobal()) return true;
    }

    auto *Inst = dynamic_cast<Instruction *>(V);
    if (!Inst || !Inst->getParent()) return false;
    return DT.dominates(Inst->getParent(), PreHeader);
}

bool LoopStrengthReductionPass::canMaterializeSCEVAtPreheader(const SCEVExpr *E, const Loop *L,
                                          BasicBlock *PreHeader,
                                          const DominatorTree &DT) {
    SCEVExpander Expander(L, PreHeader, DT, 32);
    return Expander.canExpand(E);
}

int LoopStrengthReductionPass::estimateMaterializeCost(const SCEVExpr *E, const Loop *L,
                                   BasicBlock *PreHeader,
                                   const DominatorTree &DT) {
    SCEVExpander Expander(L, PreHeader, DT, 32);
    int Cost = Expander.estimateCost(E);
    return Cost < 0 ? UnsupportedMaterializeCost : Cost;
}

Value *LoopStrengthReductionPass::makeSCEVConstant(int64_t V, const SCEVExpr *E) {
    auto IntegerType =
        E && E->Ty
            ? std::dynamic_pointer_cast<INTType>(E->Ty)
            : INTType::getInt32Ty();
    if (IntegerType && IntegerType->getIsBool()) {
        return ConstBool::newConstBool(V != 0);
    }
    if (!IntegerType || IntegerType->getBitWidth() > 64)
        return nullptr;
    return ConstInt::newConstIntForType(V, IntegerType);
}

Value *LoopStrengthReductionPass::materializeSCEVAtPreheader(const SCEVExpr *E, const Loop *L,
                                         BasicBlock *PreHeader,
                                         const DominatorTree &DT) {
    SCEVExpander Expander(L, PreHeader, DT, 32);
    return Expander.expand(E);
}

int LoopStrengthReductionPass::estimatePrefixMaterializeCost(
    const std::vector<IndexComponent> &Prefix, const Loop *L,
    BasicBlock *PreHeader, const DominatorTree &DT) {
    int Cost = 0;
    for (const IndexComponent &Component : Prefix) {
        if (Component.Raw &&
            canUseInPreheader(Component.Raw, PreHeader, DT)) {
            continue;
        }
        int ComponentCost =
            estimateMaterializeCost(Component.Expr, L, PreHeader, DT);
        if (ComponentCost >= UnsupportedMaterializeCost)
            return UnsupportedMaterializeCost;
        Cost += ComponentCost;
    }
    return Cost;
}

bool LoopStrengthReductionPass::materializePrefix(const std::vector<IndexComponent> &Prefix,
                              const Loop *L, BasicBlock *PreHeader,
                              const DominatorTree &DT,
                              std::vector<Value *> &Indices) {
    for (const IndexComponent &Component : Prefix) {
        Value *V = nullptr;
        if (Component.Raw &&
            canUseInPreheader(Component.Raw, PreHeader, DT)) {
            V = Component.Raw;
        } else {
            V = materializeSCEVAtPreheader(Component.Expr, L, PreHeader, DT);
        }
        if (!V) return false;
        Indices.push_back(V);
    }
    return true;
}

bool LoopStrengthReductionPass::isLoadStoreAddressUse(Instruction *UserI, Use *U) {
    if (!UserI || !U) return false;
    auto OpIdx = U->getOperandIndex();
    if (dynamic_cast<LoadInst *>(UserI)) return OpIdx == 0;
    if (dynamic_cast<StoreInst *>(UserI)) return OpIdx == 1;
    return false;
}

int LoopStrengthReductionPass::countAddressUses(GetElementPtrInst *GEP) {
    if (!GEP) return 0;
    int Count = 0;
    for (Use *U : GEP->getUses()) {
        auto *UserI = dynamic_cast<Instruction *>(U->getUser());
        if (isLoadStoreAddressUse(UserI, U)) ++Count;
    }
    return Count;
}

int LoopStrengthReductionPass::countLoopMemoryAddressUses(Loop *L) {
    int Count = 0;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            if (dynamic_cast<LoadInst *>(I) ||
                dynamic_cast<StoreInst *>(I)) {
                ++Count;
            }
        }
    }
    return Count;
}

bool LoopStrengthReductionPass::getConstantOffsetGEP(GetElementPtrInst *Parent,
                                 Instruction *UserI, Use *U,
                                 GetElementPtrInst *&Child,
                                 int64_t &Offset) {
    Child = dynamic_cast<GetElementPtrInst *>(UserI);
    if (!Parent || !Child || !U || U->getOperandIndex() != 0)
        return false;

    if (Child->getNumIndices() != 1 || Child->getBasePointer() != Parent)
        return false;
    auto *CI = dynamic_cast<ConstInt *>(Child->getIndex(0));
    if (!CI) return false;
    Offset = CI->getVal();
    return true;
}

bool LoopStrengthReductionPass::collectAddressGEPFamily(
    GetElementPtrInst *GEP, Loop *L, int64_t CurrentOffset,
    std::vector<AddressGEPNode> &Nodes,
    std::unordered_set<GetElementPtrInst *> &Visited) {
    if (!GEP || !L || !L->contains(GEP->getParent()) ||
        !Visited.insert(GEP).second) {
        return false;
    }

    bool HasAddressUse = false;
    std::vector<std::pair<GetElementPtrInst *, int64_t>> Children;
    for (Use *U : GEP->getUses()) {
        if (!U || U->getValue() != GEP) continue;
        auto *UserI = dynamic_cast<Instruction *>(U->getUser());
        if (!UserI || !L->contains(UserI->getParent())) return false;

        if (isLoadStoreAddressUse(UserI, U)) {
            HasAddressUse = true;
            continue;
        }

        GetElementPtrInst *Child = nullptr;
        int64_t Offset = 0;
        if (!getConstantOffsetGEP(GEP, UserI, U, Child, Offset))
            return false;
        if ((Offset > 0 &&
             CurrentOffset > std::numeric_limits<int64_t>::max() - Offset) ||
            (Offset < 0 &&
             CurrentOffset < std::numeric_limits<int64_t>::min() - Offset)) {
            return false;
        }
        Children.emplace_back(Child, CurrentOffset + Offset);
    }

    if (HasAddressUse) Nodes.push_back({GEP, CurrentOffset});
    bool HasSupportedUse = HasAddressUse;
    for (const auto &[Child, Offset] : Children) {
        if (!collectAddressGEPFamily(Child, L, Offset, Nodes, Visited))
            return false;
        HasSupportedUse = true;
    }
    return HasSupportedUse;
}

bool LoopStrengthReductionPass::isSingleIndexArrayStep(Value *Base, size_t NumIndices) {
    if (NumIndices != 1 || !Base) return false;
    auto PointeeTy = IRType::getPointeeType(Base->getIRType());
    return PointeeTy && PointeeTy->isArray();
}

bool LoopStrengthReductionPass::matchGEP(GetElementPtrInst *GEP, Loop *L, SCEVResult &SE,
                     Value *&Base, Value *&Index,
                     std::vector<IndexComponent> &Prefix,
                     std::vector<IndexComponent> &Suffix,
                     int64_t &ActiveIndexStride,
                     int64_t &FinalPointeeStride,
                     bool &HasLeadingZero,
                     bool &AllowOffsetReuse) {
    Base = nullptr;
    Index = nullptr;
    Prefix.clear();
    Suffix.clear();
    ActiveIndexStride = 0;
    FinalPointeeStride = 0;
    HasLeadingZero = false;
    AllowOffsetReuse = false;
    if (!GEP || !L) return false;

    std::vector<Value *> Indices;
    if (!GEP->flattenZeroBasedChain(Base, Indices)) return false;
    if (Indices.empty()) return false;

    std::vector<int64_t> IndexStrides;
    if (!GetElementPtrInst::getIndexStrides(Base, Indices, IndexStrides,
                                            FinalPointeeStride)) {
        return false;
    }

    int ActivePos = -1;
    std::vector<IndexComponent> Components;
    Components.reserve(Indices.size());
    for (size_t I = 0; I < Indices.size(); ++I) {
        Value *Raw = Indices[I];
        if (!Raw) return false;
        const SCEVExpr *Expr = SE.getSCEV(Raw);
        if (!Expr) return false;
        bool Varies = Expr && Expr->mentionsLoop(L);
        if (Varies) {
            if (ActivePos != -1) return false;
            ActivePos = static_cast<int>(I);
        }
        Components.push_back({Raw, Expr});
    }
    if (ActivePos == -1) return false;

    Index = Components[ActivePos].Raw;
    ActiveIndexStride = IndexStrides[ActivePos];
    AllowOffsetReuse = ActivePos == static_cast<int>(Indices.size() - 1) &&
                       ActiveIndexStride == FinalPointeeStride;

    for (int I = 0; I < ActivePos; ++I)
        Prefix.push_back(Components[I]);
    for (size_t I = static_cast<size_t>(ActivePos + 1);
         I < Components.size(); ++I) {
        if (Components[I].Expr && Components[I].Expr->mentionsLoop(L))
            return false;
        Suffix.push_back(Components[I]);
    }

    if (ActiveIndexStride <= 0 || FinalPointeeStride <= 0 ||
        ActiveIndexStride % FinalPointeeStride != 0) {
        return false;
    }
    HasLeadingZero = !Prefix.empty() && Prefix.front().Raw &&
                     Prefix.front().Raw->isConstIntZero();
    return true;
}

std::vector<LSRCandidate>
LoopStrengthReductionPass::collectCandidates(Function &F, LoopInfo &LI, SCEVResult &SE,
                  const DominatorTree &DT) {
    std::unordered_map<BasicBlock *, size_t> BlockOrder;
    size_t NextBlockOrder = 0;
    for (BasicBlock *BB : F) BlockOrder[BB] = NextBlockOrder++;

    std::vector<Loop *> Loops;
    Loops.reserve(LI.Loops.size());
    for (auto &LoopPtr : LI.Loops) {
        if (LoopPtr) Loops.push_back(LoopPtr.get());
    }
    std::sort(Loops.begin(), Loops.end(), [&](Loop *A, Loop *B) {
        if (A->getDepth() != B->getDepth())
            return A->getDepth() > B->getDepth();
        size_t AO = BlockOrder.count(A->getHeader())
                        ? BlockOrder.at(A->getHeader())
                        : std::numeric_limits<size_t>::max();
        size_t BO = BlockOrder.count(B->getHeader())
                        ? BlockOrder.at(B->getHeader())
                        : std::numeric_limits<size_t>::max();
        return AO < BO;
    });

    std::vector<LSRCandidate> Candidates;
    for (Loop *L : Loops) {
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Latch = nullptr;
        if (!L || !L->getLoopSimplifyForm(PreHeader, Latch)) continue;

        BasicBlock *Header = L->getHeader();
        if (!PreHeader || !Header || !Latch) continue;

        std::vector<BasicBlock *> LoopBlocks = L->getBlocksIncludingSubLoops();
        std::sort(LoopBlocks.begin(), LoopBlocks.end(),
                  [&](BasicBlock *A, BasicBlock *B) {
                      size_t AO = BlockOrder.count(A)
                                      ? BlockOrder.at(A)
                                      : std::numeric_limits<size_t>::max();
                      size_t BO = BlockOrder.count(B)
                                      ? BlockOrder.at(B)
                                      : std::numeric_limits<size_t>::max();
                      return AO < BO;
                  });
        for (BasicBlock *BB : LoopBlocks) {
            if (!BB) continue;
            for (Instruction *I : *BB) {
                auto *GEP = dynamic_cast<GetElementPtrInst *>(I);
                if (!GEP) continue;

                Value *Base = nullptr;
                Value *Index = nullptr;
                std::vector<IndexComponent> Prefix;
                std::vector<IndexComponent> Suffix;
                int64_t ActiveIndexStride = 0;
                int64_t FinalPointeeStride = 0;
                bool HasLeadingZero = false;
                bool AllowOffsetReuse = false;
                if (!matchGEP(GEP, L, SE, Base, Index, Prefix, Suffix,
                              ActiveIndexStride, FinalPointeeStride,
                              HasLeadingZero, AllowOffsetReuse)) {
                    continue;
                }

                auto Affine = SE.getAffine(Index);
                if (!Affine || Affine->RecLoop != L || !Affine->Start ||
                    !Affine->ConstantStep) {
                    continue;
                }

                int64_t Step = *Affine->ConstantStep;
                if (Step == 0 || Step < std::numeric_limits<int>::min() ||
                    Step > std::numeric_limits<int>::max()) {
                    continue;
                }
                int64_t AddressScale = ActiveIndexStride / FinalPointeeStride;
                if (AddressScale <= 0) continue;
                if ((Step > 0 &&
                     AddressScale >
                         std::numeric_limits<int64_t>::max() / Step) ||
                    (Step < 0 &&
                     AddressScale >
                         std::numeric_limits<int64_t>::max() / -Step)) {
                    continue;
                }
                int64_t AddressStep = Step * AddressScale;
                if (AddressStep < std::numeric_limits<int>::min() ||
                    AddressStep > std::numeric_limits<int>::max()) {
                    continue;
                }
                if (!canUseInPreheader(Base, PreHeader, DT)) continue;

                int PrefixCost =
                    estimatePrefixMaterializeCost(Prefix, L, PreHeader, DT);
                if (PrefixCost >= UnsupportedMaterializeCost) continue;
                int SuffixCost =
                    estimatePrefixMaterializeCost(Suffix, L, PreHeader, DT);
                if (SuffixCost >= UnsupportedMaterializeCost) continue;

                std::optional<int64_t> StartConst = Affine->ConstantStart;
                if (StartConst &&
                    (*StartConst < std::numeric_limits<int>::min() ||
                     *StartConst > std::numeric_limits<int>::max())) {
                    continue;
                }
                if (!StartConst &&
                    !canMaterializeSCEVAtPreheader(Affine->Start, L,
                                                   PreHeader, DT)) {
                    continue;
                }

                std::vector<AddressGEPNode> Family;
                std::unordered_set<GetElementPtrInst *> Visited;
                if (!collectAddressGEPFamily(GEP, L, 0, Family, Visited))
                    continue;

                for (const AddressGEPNode &Node : Family) {
                    std::optional<int64_t> NodeStartConst;
                    if (!AllowOffsetReuse && Node.Offset != 0) {
                        continue;
                    }
                    if (StartConst) {
                        if ((Node.Offset > 0 &&
                             *StartConst >
                                 std::numeric_limits<int64_t>::max() -
                                     Node.Offset) ||
                            (Node.Offset < 0 &&
                             *StartConst <
                                 std::numeric_limits<int64_t>::min() -
                                     Node.Offset)) {
                            continue;
                        }
                        int64_t Start = *StartConst + Node.Offset;
                        if (Start < std::numeric_limits<int>::min() ||
                            Start > std::numeric_limits<int>::max()) {
                            continue;
                        }
                        NodeStartConst = Start;
                    } else if (Node.Offset != 0) {
                        continue;
                    }

                    Candidates.push_back(
                        {Node.GEP, Base, Index, Affine->Start,
                         NodeStartConst, Prefix, Suffix, L, PreHeader,
                         Header, Latch, AddressStep, ActiveIndexStride,
                         HasLeadingZero, AllowOffsetReuse, false});
                }
            }
        }
    }
    return Candidates;
}

std::string LoopStrengthReductionPass::candidateShapeKey(const LSRCandidate &C,
                                     bool IncludeStart) {
    std::ostringstream Key;
    Key << "L=" << IRKey::pointer(C.L) << ";B=" << IRKey::pointer(C.Base)
        << ";Step=" << C.Step
        << ";Stride=" << C.LastIndexStride
        << ";Lead0=" << (C.HasLeadingZero ? 1 : 0)
        << ";Ty=" << IRKey::type(C.OldGEP ? C.OldGEP->getIRType() : nullptr)
        << ";PrefixN=" << C.Prefix.size();
    for (const IndexComponent &Component : C.Prefix)
        Key << ";P=" << scevKey(Component.Expr);
    Key << ";SuffixN=" << C.Suffix.size();
    for (const IndexComponent &Component : C.Suffix)
        Key << ";S=" << scevKey(Component.Expr);
    if (IncludeStart) {
        if (C.StartConst)
            Key << ";StartConst=" << *C.StartConst;
        else
            Key << ";Start=" << scevKey(C.Start);
    }
    return Key.str();
}

std::vector<OffsetReuseGroup>
LoopStrengthReductionPass::buildOffsetReuseGroups(std::vector<LSRCandidate> &Candidates) {
    std::vector<OffsetReuseGroup> Groups;
    std::unordered_map<std::string, size_t> GroupIndex;

    for (LSRCandidate &C : Candidates) {
        if (C.Used || !C.StartConst || !C.AllowOffsetReuse) continue;
        std::string Key = candidateShapeKey(C, false);
        auto It = GroupIndex.find(Key);
        if (It == GroupIndex.end()) {
            size_t Index = Groups.size();
            GroupIndex.emplace(Key, Index);
            OffsetReuseGroup Group;
            Group.Order = Index;
            Group.Base = C.Base;
            Group.L = C.L;
            Group.PreHeader = C.PreHeader;
            Group.Header = C.Header;
            Group.Latch = C.Latch;
            Group.Step = C.Step;
            Group.LastIndexStride = C.LastIndexStride;
            Group.Prefix = C.Prefix;
            Group.ResultTy = C.OldGEP ? C.OldGEP->getIRType() : nullptr;
            Group.AnchorStart = *C.StartConst;
            Group.MaxStart = *C.StartConst;
            Groups.push_back(std::move(Group));
            It = GroupIndex.find(Key);
        }

        OffsetReuseGroup &Group = Groups[It->second];
        Group.Items.push_back(&C);
        Group.AnchorStart = std::min(Group.AnchorStart, *C.StartConst);
        Group.MaxStart = std::max(Group.MaxStart, *C.StartConst);
    }

    Groups.erase(
        std::remove_if(Groups.begin(), Groups.end(),
                       [](const OffsetReuseGroup &Group) {
                           if (Group.Items.size() < 2) return true;
                           std::unordered_set<int64_t> Starts;
                           for (const LSRCandidate *C : Group.Items)
                               Starts.insert(*C->StartConst);
                           return Starts.size() < 2 ||
                                  Group.MaxStart - Group.AnchorStart >
                                      MaxOffsetRange;
                       }),
        Groups.end());

    std::sort(Groups.begin(), Groups.end(),
              [](const OffsetReuseGroup &A, const OffsetReuseGroup &B) {
                  unsigned AD = A.L ? A.L->getDepth() : 0;
                  unsigned BD = B.L ? B.L->getDepth() : 0;
                  if (AD != BD) return AD > BD;
                  if (A.Items.size() != B.Items.size())
                      return A.Items.size() > B.Items.size();
                  int64_t ARange = A.MaxStart - A.AnchorStart;
                  int64_t BRange = B.MaxStart - B.AnchorStart;
                  if (ARange != BRange) return ARange < BRange;
                  return A.Order < B.Order;
              });
    return Groups;
}

std::vector<LSRGroup>
LoopStrengthReductionPass::buildExactGroups(std::vector<LSRCandidate> &Candidates) {
    std::vector<LSRGroup> Groups;
    std::unordered_map<std::string, size_t> GroupIndex;

    for (LSRCandidate &C : Candidates) {
        if (C.Used) continue;
        std::string Key = candidateShapeKey(C, true);
        auto It = GroupIndex.find(Key);
        if (It == GroupIndex.end()) {
            size_t Index = Groups.size();
            GroupIndex.emplace(Key, Index);
            LSRGroup Group;
            Group.Order = Index;
            Group.Base = C.Base;
            Group.Start = C.Start;
            Group.StartConst = C.StartConst;
            Group.L = C.L;
            Group.PreHeader = C.PreHeader;
            Group.Header = C.Header;
            Group.Latch = C.Latch;
            Group.Step = C.Step;
            Group.LastIndexStride = C.LastIndexStride;
            Group.Prefix = C.Prefix;
            Group.Suffix = C.Suffix;
            Group.ResultTy = C.OldGEP ? C.OldGEP->getIRType() : nullptr;
            Groups.push_back(std::move(Group));
            It = GroupIndex.find(Key);
        }
        Groups[It->second].Items.push_back(&C);
    }

    std::sort(Groups.begin(), Groups.end(),
              [](const LSRGroup &A, const LSRGroup &B) {
                  unsigned AD = A.L ? A.L->getDepth() : 0;
                  unsigned BD = B.L ? B.L->getDepth() : 0;
                  if (AD != BD) return AD > BD;
                  if (A.Items.size() != B.Items.size())
                      return A.Items.size() > B.Items.size();
                  return A.Order < B.Order;
              });
    return Groups;
}

bool LoopStrengthReductionPass::isRemovableIndexInst(Instruction *I) {
    if (!I || !I->getParent()) return false;
    return dynamic_cast<BinaryInst *>(I) ||
           dynamic_cast<GetElementPtrInst *>(I) ||
           dynamic_cast<SelectInst *>(I) || dynamic_cast<TruncInst *>(I) ||
           dynamic_cast<ZextInst *>(I) || dynamic_cast<SextInst *>(I) ||
           dynamic_cast<BitCastInst *>(I);
}

bool LoopStrengthReductionPass::collectRemovableIndexChain(
    Instruction *I, std::unordered_set<User *> &AllowedUsers,
    std::unordered_set<Instruction *> &Out) {
    if (!I || Out.count(I)) return true;
    if (!isRemovableIndexInst(I)) return false;

    for (Use *U : I->getUses()) {
        if (!U || U->getValue() != I) continue;
        if (!AllowedUsers.count(U->getUser())) return false;
    }

    Out.insert(I);
    AllowedUsers.insert(I);
    forEachOperand(I, [&](size_t, Use *, Value *Operand) {
        auto *OperandI = dynamic_cast<Instruction *>(Operand);
        if (OperandI && OperandI != I)
            collectRemovableIndexChain(OperandI, AllowedUsers, Out);
    });
    return true;
}

int LoopStrengthReductionPass::countRemovableIndexInsts(
    const std::vector<LSRCandidate *> &Items) {
    std::unordered_set<User *> AllowedUsers;
    for (LSRCandidate *C : Items) {
        if (C && C->OldGEP) AllowedUsers.insert(C->OldGEP);
    }

    std::unordered_set<Instruction *> Removable;
    for (LSRCandidate *C : Items) {
        auto *IndexI = C ? dynamic_cast<Instruction *>(C->Index) : nullptr;
        if (IndexI)
            collectRemovableIndexChain(IndexI, AllowedUsers, Removable);
    }
    return static_cast<int>(Removable.size());
}

int LoopStrengthReductionPass::countCoveredAddressUses(
    const std::vector<LSRCandidate *> &Items) {
    int Count = 0;
    for (const LSRCandidate *C : Items) {
        if (C) Count += countAddressUses(C->OldGEP);
    }
    return Count;
}

bool LoopStrengthReductionPass::coversDenseInnermostAddresses(
    Loop *L, const std::vector<LSRCandidate *> &Items) {
    if (!L || !L->isInnermost()) return false;
    int CoveredAddressUses = countCoveredAddressUses(Items);
    int LoopAddressUses = countLoopMemoryAddressUses(L);
    return CoveredAddressUses >= 3 && LoopAddressUses <= 8 &&
           CoveredAddressUses * 2 >= LoopAddressUses;
}

void LoopStrengthReductionPass::cleanupDeadIndexChains(
    const std::vector<Instruction *> &Seeds) {
    UniqueWorkList<
        Instruction *, WorkListOrder::LIFO> WorkList;
    for (Instruction *Seed : Seeds) {
        if (Seed) WorkList.push(Seed);
    }

    while (!WorkList.empty()) {
        Instruction *Current = WorkList.pop();
        if (!isRemovableIndexInst(Current) || Current->hasUses()) continue;

        std::vector<Instruction *> Operands;
        forEachOperand(
            Current, [&](size_t, Use *, Value *Operand) {
                auto *OperandI = dynamic_cast<Instruction *>(Operand);
                if (OperandI && OperandI != Current)
                    Operands.push_back(OperandI);
            });

        delete Current;
        for (Instruction *OperandI : Operands) {
            if (OperandI && isRemovableIndexInst(OperandI) &&
                !OperandI->hasUses()) {
                WorkList.push(OperandI);
            }
        }
    }
}

bool LoopStrengthReductionPass::isSimpleSingleGEP(const LSRCandidate &C) {
    if (C.Step != 1 || !C.Start || !C.Start->isConstant(0) ||
        hasNonTrivialPrefix(C.Prefix)) {
        return false;
    }
    auto *Phi = dynamic_cast<PhiInst *>(C.Index);
    return Phi && Phi->getParent() == C.Header;
}

bool LoopStrengthReductionPass::shouldRewriteExactGroup(const LSRGroup &Group,
                                    const DominatorTree &DT) {
    if (Group.Items.empty()) return false;
    bool CostlyStride = hasCostlyElementStride(Group.LastIndexStride);

    int PrefixCost = estimatePrefixMaterializeCost(
        Group.Prefix, Group.L, Group.PreHeader, DT);
    int SuffixCost = estimatePrefixMaterializeCost(
        Group.Suffix, Group.L, Group.PreHeader, DT);
    int StartCost =
        Group.StartConst
            ? 0
            : estimateMaterializeCost(Group.Start, Group.L, Group.PreHeader,
                                      DT);
    int MaxMaterializeCost = CostlyStride ? 2 : 1;
    if (PrefixCost > MaxMaterializeCost || SuffixCost > MaxMaterializeCost ||
        StartCost > MaxMaterializeCost)
        return false;

    if (Group.Items.size() == 1) {
        const LSRCandidate &C = *Group.Items.front();
        int Removable = countRemovableIndexInsts(Group.Items);
        if (isSimpleSingleGEP(C) && !CostlyStride) return false;
        if (CostlyStride) return countAddressUses(C.OldGEP) > 0;
        if (C.Step == 1 || C.Step == -1) return false;
        return Removable > 0 || hasNonTrivialPrefix(C.Prefix);
    }

    if (Group.Items.size() >= 3) return true;

    int Removable = countRemovableIndexInsts(Group.Items);
    return CostlyStride || Removable > 0 ||
           (Group.Step != 1 && Group.Step != -1) ||
           hasNonTrivialPrefix(Group.Prefix);
}

bool LoopStrengthReductionPass::shouldRewriteOffsetGroup(const OffsetReuseGroup &Group,
                                     const DominatorTree &DT) {
    if (Group.Items.size() < 2) return false;
    int64_t Range = Group.MaxStart - Group.AnchorStart;
    if (Range <= 0 || Range > MaxOffsetRange) return false;

    bool CostlyStride = hasCostlyElementStride(Group.LastIndexStride);
    int MaxPrefixCost = CostlyStride ? 2 : 1;
    if (estimatePrefixMaterializeCost(Group.Prefix, Group.L, Group.PreHeader,
                                      DT) > MaxPrefixCost) {
        return false;
    }

    std::unordered_set<int64_t> Starts;
    for (const LSRCandidate *C : Group.Items)
        Starts.insert(*C->StartConst);
    if (Starts.size() < 2) return false;

    int Removable = countRemovableIndexInsts(Group.Items);
    int OffsetGEPs = static_cast<int>(Starts.size()) - 1;
    int NewLoopGEPs = 1 + OffsetGEPs;
    if (Group.Items.size() == 2) {
        if (CostlyStride) return true;
        if (Range > 4) return false;
        if (Group.Step != 1 && Group.Step != -1) return true;
        return Removable > 0 && Range <= 2;
    }

    if (NewLoopGEPs < static_cast<int>(Group.Items.size()) ||
        CostlyStride || Removable > 0 ||
        (Group.Step != 1 && Group.Step != -1)) {
        return true;
    }

    return coversDenseInnermostAddresses(Group.L, Group.Items);
}

PointerIV LoopStrengthReductionPass::createPointerIV(Value *Base,
                                 const std::vector<Value *> &StartIndices,
                                 int64_t Step,
                                 const std::shared_ptr<IRType> &ResultTy,
                                 BasicBlock *PreHeader, BasicBlock *Header,
                                 BasicBlock *Latch) {
    PointerIV Result;

    std::vector<Value *> Indices = StartIndices;
    Result.Start = new GetElementPtrInst(Base, Indices);
    Result.Start->updateType();
    PreHeader->insertBeforeTerminator(Result.Start);

    Result.Phi = new PhiInst(ResultTy);
    Header->pushFront(Result.Phi);

    std::vector<Value *> NextIndices;
    NextIndices.push_back(
        ConstInt::newConstInt(static_cast<int>(Step)));
    Result.Next = new GetElementPtrInst(Result.Phi, NextIndices);
    Result.Next->updateType();
    Latch->insertBeforeTerminator(Result.Next);

    Result.Phi->addIncoming(Result.Start, PreHeader);
    Result.Phi->addIncoming(Result.Next, Latch);
    return Result;
}

bool LoopStrengthReductionPass::rewriteExactGroup(LSRGroup &Group, const DominatorTree &DT,
                              std::vector<Instruction *> &DeadSeeds) {
    std::vector<Value *> StartIndices;
    if (!materializePrefix(Group.Prefix, Group.L, Group.PreHeader, DT,
                           StartIndices)) {
        return false;
    }

    Value *StartValue =
        Group.StartConst
            ? makeSCEVConstant(*Group.StartConst, Group.Start)
            : materializeSCEVAtPreheader(Group.Start, Group.L,
                                         Group.PreHeader, DT);
    if (!StartValue) return false;
    StartIndices.push_back(StartValue);
    if (!materializePrefix(Group.Suffix, Group.L, Group.PreHeader, DT,
                           StartIndices)) {
        return false;
    }

    PointerIV IV = createPointerIV(
        Group.Base, StartIndices, Group.Step, Group.ResultTy,
        Group.PreHeader, Group.Header, Group.Latch);

    for (LSRCandidate *C : Group.Items) {
        if (!C || C->Used || !C->OldGEP || !C->OldGEP->getParent())
            continue;
        if (auto *IndexI = dynamic_cast<Instruction *>(C->Index))
            DeadSeeds.push_back(IndexI);
        C->OldGEP->replaceAllUsesWith(IV.Phi);
        delete C->OldGEP;
        C->Used = true;
    }
    return true;
}

bool LoopStrengthReductionPass::rewriteOffsetGroup(OffsetReuseGroup &Group,
                               const DominatorTree &DT,
                               std::vector<Instruction *> &DeadSeeds) {
    std::vector<Value *> StartIndices;
    if (!materializePrefix(Group.Prefix, Group.L, Group.PreHeader, DT,
                           StartIndices)) {
        return false;
    }

    const SCEVExpr *StartTypeExpr =
        Group.Items.empty() ? nullptr : Group.Items.front()->Start;
    Value *Anchor =
        makeSCEVConstant(Group.AnchorStart, StartTypeExpr);
    if (!Anchor) return false;
    StartIndices.push_back(Anchor);

    PointerIV IV = createPointerIV(
        Group.Base, StartIndices, Group.Step, Group.ResultTy,
        Group.PreHeader, Group.Header, Group.Latch);

    std::unordered_map<int64_t, Value *> OffsetValues;
    OffsetValues.emplace(0, IV.Phi);
    for (LSRCandidate *C : Group.Items) {
        int64_t Offset = *C->StartConst - Group.AnchorStart;
        if (OffsetValues.count(Offset)) continue;

        std::vector<Value *> OffsetIndices;
        OffsetIndices.push_back(
            ConstInt::newConstInt(static_cast<int>(Offset)));
        auto *OffsetGEP = new GetElementPtrInst(IV.Phi, OffsetIndices);
        OffsetGEP->updateType();

        BasicBlock *CommonBB = nullptr;
        bool SameBlock = true;
        std::unordered_set<GetElementPtrInst *> Positions;
        for (LSRCandidate *Item : Group.Items) {
            if (!Item || !Item->StartConst ||
                *Item->StartConst - Group.AnchorStart != Offset ||
                !Item->OldGEP || !Item->OldGEP->getParent()) {
                continue;
            }
            Positions.insert(Item->OldGEP);
            BasicBlock *ItemBB = Item->OldGEP->getParent();
            if (!CommonBB)
                CommonBB = ItemBB;
            else if (CommonBB != ItemBB)
                SameBlock = false;
        }

        Instruction *InsertBefore = nullptr;
        if (SameBlock && CommonBB) {
            for (Instruction *I : *CommonBB) {
                auto *Position = dynamic_cast<GetElementPtrInst *>(I);
                if (Position && Positions.count(Position)) {
                    InsertBefore = Position;
                    break;
                }
            }
        }
        if (!Instruction::insertBefore(InsertBefore, OffsetGEP))
            Group.Header->insertAfterPhiNodes(OffsetGEP);
        OffsetValues.emplace(Offset, OffsetGEP);
    }

    for (LSRCandidate *C : Group.Items) {
        if (!C || C->Used || !C->OldGEP || !C->OldGEP->getParent())
            continue;
        int64_t Offset = *C->StartConst - Group.AnchorStart;
        if (auto *IndexI = dynamic_cast<Instruction *>(C->Index))
            DeadSeeds.push_back(IndexI);
        C->OldGEP->replaceAllUsesWith(OffsetValues.at(Offset));
        delete C->OldGEP;
        C->Used = true;
    }
    return true;
}

int LoopStrengthReductionPass::pointerIVLimit(const Loop *L) {
    return L && L->isInnermost() ? MaxPointerIVsPerInnerLoop
                                 : MaxPointerIVsPerOuterLoop;
}

bool LoopStrengthReductionPass::runAddressLSR(Function &F, LoopInfo &LI, SCEVResult &SE,
                       DominatorTree &DT) {
    std::vector<LSRCandidate> Candidates =
        collectCandidates(F, LI, SE, DT);
    if (Candidates.empty()) return false;

    std::unordered_map<Loop *, int> LoopPointerIVs;
    std::vector<Instruction *> DeadSeeds;
    bool Changed = false;

    std::vector<OffsetReuseGroup> OffsetGroups =
        buildOffsetReuseGroups(Candidates);
    for (OffsetReuseGroup &Group : OffsetGroups) {
        if (!shouldRewriteOffsetGroup(Group, DT)) continue;
        if (LoopPointerIVs[Group.L] >= pointerIVLimit(Group.L)) continue;
        if (!rewriteOffsetGroup(Group, DT, DeadSeeds)) continue;
        ++LoopPointerIVs[Group.L];
        Changed = true;
    }

    std::vector<LSRGroup> ExactGroups = buildExactGroups(Candidates);
    for (LSRGroup &Group : ExactGroups) {
        if (!shouldRewriteExactGroup(Group, DT)) continue;
        if (LoopPointerIVs[Group.L] >= pointerIVLimit(Group.L)) continue;
        if (!rewriteExactGroup(Group, DT, DeadSeeds)) continue;
        ++LoopPointerIVs[Group.L];
        Changed = true;
    }

    cleanupDeadIndexChains(DeadSeeds);
    return Changed;
}

PreservationStatus LoopStrengthReductionPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);

    bool Changed = runAddressLSR(F, LI, SE, DT);
    if (!Changed) return PreservationStatus::all();

    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<SCEVAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
