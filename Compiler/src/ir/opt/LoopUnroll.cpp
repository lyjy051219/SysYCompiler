#include "ir/opt/LoopUnroll.hpp"
#include "ir/opt/LCSSA.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

int LoopUnrollPass::nextUnrollSerial() {
    static int Serial = 0;
    return ++Serial;
}

std::string LoopUnrollPass::makeUnrollBlockIdent(Function *F,
                                 BasicBlock *BB,
                                 int Iter,
                                 int UnrollSerial) {
    std::string Base = BB ? BB->getIdent() : "bb";

    Base += ".unr";
    Base += std::to_string(UnrollSerial);
    Base += ".";
    Base += std::to_string(Iter);

    return F ? F->makeUniqueBlockIdent(Base) : Base;
}

std::string LoopUnrollPass::makePeelBlockIdent(Function *F,
                               BasicBlock *BB,
                               int Iter,
                               int PeelSerial) {
    std::string Base = BB ? BB->getIdent() : "bb";

    Base += ".peel";
    Base += std::to_string(PeelSerial);
    Base += ".";
    Base += std::to_string(Iter);

    return F ? F->makeUniqueBlockIdent(Base) : Base;
}

bool LoopUnrollPass::isSupportedCloneInstruction(Instruction *I) {
    if (!I) return false;

    switch (I->instType) {
    case Instruction::RET:
    case Instruction::ALLOCA:
    case Instruction::BinaryUnknown:
        return false;
    default:
        return true;
    }
}

Value *LoopUnrollPass::stripSimpleCast(Value *V) {
    while (auto *I = dynamic_cast<Instruction *>(V)) {
        switch (I->instType) {
        case Instruction::TRUNC:
        case Instruction::ZEXT:
        case Instruction::SEXT:
        case Instruction::BITCAST:
            V = I->getUnaryOperand();
            continue;
        default:
            return V;
        }
    }
    return V;
}

std::optional<PeelPredicate> LoopUnrollPass::matchCompareWithIV(BinaryInst *Cmp, Value *IV) {
    Value *RawLHS = nullptr;
    Value *RawRHS = nullptr;
    if (!Cmp || !IV || !Cmp->getOperands(RawLHS, RawRHS))
        return std::nullopt;

    Value *LHS = stripSimpleCast(RawLHS);
    Value *RHS = stripSimpleCast(RawRHS);
    IV = stripSimpleCast(IV);

    if (LHS == IV) {
        if (auto C = RHS ? RHS->getIntConstant() : std::nullopt) {
            return PeelPredicate{Cmp->getOp(), *C};
        }
    }

    if (RHS == IV) {
        if (auto C = LHS ? LHS->getIntConstant() : std::nullopt) {
            return PeelPredicate{BinaryInst::reversePredicate(Cmp->getOp()), *C};
        }
    }

    return std::nullopt;
}

int LoopUnrollPass::peelCountForPredicate(const PeelPredicate &P) {
    switch (P.Op) {
    case BinaryInst::E:
        if (P.Constant == 0) return 1;
        return 0;
    case BinaryInst::L:
        if (P.Constant == 1) return 1;
        if (P.Constant == 2) return 2;
        return 0;
    case BinaryInst::LE:
        if (P.Constant == 0) return 1;
        if (P.Constant == 1) return 2;
        return 0;
    default:
        return 0;
    }
}

bool LoopUnrollPass::isInternalConditionalBranch(Loop *L, BrInst *Br) {
    if (!L || !Br || !Br->isConditional()) {
        return false;
    }

    auto *TrueBB = Br->getTrueSuccessor();
    auto *FalseBB = Br->getFalseSuccessor();
    return TrueBB && FalseBB && L->contains(TrueBB) && L->contains(FalseBB);
}

UnrollDecision LoopUnrollPass::makeNoUnrollDecision(std::string Reason) {
    UnrollDecision Decision;
    Decision.Kind = UnrollKind::None;
    Decision.Count = 0;
    Decision.ShouldUnroll = false;
    Decision.NeedRemainder = false;
    Decision.IsRuntime = false;
    Decision.IsPeeling = false;
    Decision.Reason = std::move(Reason);
    return Decision;
}

bool LoopUnrollPass::withinThreshold(int64_t Cost, int Threshold) {
    return Threshold >= 0 && Cost <= static_cast<int64_t>(Threshold);
}

int LoopUnrollPass::chooseFactorBySize(int BodyCost, int SizeLimit, int MaxCount) {
    if (BodyCost <= 0 || SizeLimit <= 0 || MaxCount < 2)
        return 0;

    int Estimated = SizeLimit / BodyCost;
    int Factor = std::min(Estimated, MaxCount);

    if (Factor >= 4)
        return 4;
    if (Factor >= 2)
        return 2;
    return 0;
}

int LoopUnrollPass::localSCEVNodeCost(const SCEVExpr *E) {
    if (!E) return 1000000;

    switch (E->Kind) {
    case SCEVExprKind::Constant:
        return 0;
    case SCEVExprKind::Unknown:
        return 1;
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub:
    case SCEVExprKind::Cast:
    case SCEVExprKind::Compare:
    case SCEVExprKind::Min:
    case SCEVExprKind::Max:
        return 1;
    case SCEVExprKind::Mul:
    case SCEVExprKind::AddRec:
        return 2;
    case SCEVExprKind::Div:
    case SCEVExprKind::Mod:
    case SCEVExprKind::MulRec:
    case SCEVExprKind::DivRec:
        return 4;
    }

    return 1000000;
}

std::optional<LoopExecutionShape>
LoopUnrollPass::getLoopExecutionShape(Loop *L, SCEVResult &SE) {
    if (!L) return std::nullopt;

    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch)) return std::nullopt;

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->Valid) return std::nullopt;
    if (!TCI->Header || !TCI->ExitingBlock || !TCI->ExitBlock) {
        return std::nullopt;
    }

    LoopExecutionShape Shape;
    Shape.PreHeader = PreHeader;
    Shape.Header = TCI->Header;
    Shape.Latch = Latch;
    Shape.Exiting = TCI->ExitingBlock;
    Shape.Exit = TCI->ExitBlock;
    return Shape;
}

bool LoopUnrollPass::canCloneAllInstructions(const std::vector<BasicBlock *> &Blocks) {
    std::unordered_map<Value *, Value *> VMap;
    for (BasicBlock *BB : Blocks) {
        if (BB) VMap[BB] = BB;
    }

    for (BasicBlock *BB : Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!isSupportedCloneInstruction(I)) return false;
            Instruction *Probe = I->clone(VMap);
            if (!Probe) return false;
            delete Probe;
        }
    }

    return true;
}

Value *LoopUnrollPass::mapValueAtIteration(Value *V,
                           int Iter,
                           const UnrolledBodyCore &Core) {
    if (!V || Iter <= 0) return V;
    if (Iter >= static_cast<int>(Core.ValueMaps.size())) return V;

    auto It = Core.ValueMaps[Iter].find(V);
    return It == Core.ValueMaps[Iter].end() ? V : It->second;
}

bool LoopUnrollPass::isBlockValue(Value *V) {
    return dynamic_cast<BasicBlock *>(V) != nullptr;
}

bool LoopUnrollPass::isDataValue(Value *V) {
    return V && !isBlockValue(V) && V->getIRType() &&
           V->getIRType()->getEnum() != IR_LABEL;
}

bool LoopUnrollPass::isValueCompatibleWithPhi(Value *V, PhiInst *Phi) {
    if (!Phi || !isDataValue(V)) return false;
    auto Ty = Phi->getIRType();
    auto VTy = V->getIRType();
    if (!Ty || !VTy) return false;
    return Ty->getEnum() == VTy->getEnum() &&
           Ty->getSize() == VTy->getSize();
}

bool LoopUnrollPass::isValueCompatibleWithType(Value *V, std::shared_ptr<IRType> Ty) {
    if (!isDataValue(V) || !Ty) return false;
    auto VTy = V->getIRType();
    if (!VTy) return false;
    return Ty->getEnum() == VTy->getEnum() &&
           Ty->getSize() == VTy->getSize();
}

Value *LoopUnrollPass::mapDataValueAtIteration(Value *V,
                               int Iter,
                               const UnrolledBodyCore &Core) {
    if (!V) return nullptr;
    if (Iter > 0 && Iter < static_cast<int>(Core.ValueMaps.size())) {
        auto It = Core.ValueMaps[Iter].find(V);
        if (It != Core.ValueMaps[Iter].end()) {
            return isDataValue(It->second) ? It->second : nullptr;
        }
    }
    return isDataValue(V) ? V : nullptr;
}

Value *LoopUnrollPass::mapDataValueAtPeelIteration(Value *V,
                                   int Iter,
                                   const PeeledBodyCore &Core) {
    if (!V) return nullptr;
    if (Iter >= 0 && Iter < static_cast<int>(Core.ValueMaps.size())) {
        auto It = Core.ValueMaps[Iter].find(V);
        if (It != Core.ValueMaps[Iter].end()) {
            return isDataValue(It->second) ? It->second : nullptr;
        }
    }
    return isDataValue(V) ? V : nullptr;
}

Value *LoopUnrollPass::getCapturedDataValueMappedAtIteration(Value *V,
                                             int Iter,
                                             const UnrolledBodyCore &Core) {
    if (!V || Iter <= 0 ||
        Iter >= static_cast<int>(Core.ValueMaps.size())) {
        return nullptr;
    }

    auto It = Core.ValueMaps[Iter].find(V);
    if (It == Core.ValueMaps[Iter].end()) return nullptr;
    return isDataValue(It->second) ? It->second : nullptr;
}

bool LoopUnrollPass::addIncomingChecked(PhiInst *Phi, Value *V, BasicBlock *Pred) {
    if (!Phi || !Pred || !isValueCompatibleWithPhi(V, Phi)) {
        return false;
    }
    Phi->addIncoming(V, Pred);
    return true;
}

bool LoopUnrollPass::rebuildPhiFromIncomingChecked(
    PhiInst *Phi,
    const std::vector<PhiInst::IncomingEdge> &Incoming,
    Value **Replacement) {
    if (!Phi) return false;
    for (const auto &Edge : Incoming) {
        if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi)) {
            return false;
        }
    }

    Value *Result = nullptr;
    if (Incoming.empty()) {
        Result = UndefValue::NewUndefValue(Phi->getIRType());
        Phi->replaceAllUsesWith(Result);
        delete Phi;
    } else if (Incoming.size() == 1) {
        Result = Incoming.front().V;
        Phi->replaceAllUsesWith(Result);
        delete Phi;
    } else {
        auto *NewPhi = new PhiInst(Phi->getIRType());
        for (const auto &Edge : Incoming) {
            NewPhi->addIncoming(Edge.V, Edge.Pred);
        }
        Phi->replaceAllUsesWith(NewPhi);
        Phi->replaceWith(NewPhi);
        delete Phi;
        Result = NewPhi;
    }

    if (Replacement) *Replacement = Result;
    return true;
}

bool LoopUnrollPass::replaceOperandsUsingMap(Instruction *I,
                             const std::unordered_map<Value *, Value *> &VMap) {
    if (!I) return false;

    bool Changed = false;
    for (size_t OperandIndex = 0;
         OperandIndex < I->getNumOperands(); ++OperandIndex) {
        Use *U = I->getOperandUse(OperandIndex);
        if (!U) continue;
        auto It = VMap.find(U->getValue());
        if (It == VMap.end()) continue;
        Changed |= U->replaceUseeWith(It->second);
    }
    return Changed;
}

bool LoopUnrollPass::replaceDataOperandsUsingMap(
    Instruction *I,
    const std::unordered_map<Value *, Value *> &VMap) {
    if (!I) return false;

    for (size_t OperandIndex = 0;
         OperandIndex < I->getNumOperands(); ++OperandIndex) {
        Use *U = I->getOperandUse(OperandIndex);
        if (!U) continue;
        Value *Old = U->getValue();
        if (isBlockValue(Old)) continue;
        auto It = VMap.find(Old);
        if (It == VMap.end()) continue;
        if (!isValueCompatibleWithType(It->second, Old->getIRType())) continue;
        U->replaceUseeWith(It->second);
    }
    return true;
}

bool LoopUnrollPass::removeClonedHeaderPhis(UnrolledBodyCore &Core, int Iter) {
    if (Iter <= 0 || Iter >= Core.Count) return false;

    BasicBlock *OrigHeader = Core.Headers.front();
    BasicBlock *OrigLatch = Core.Latches.front();
    BasicBlock *ClonedHeader = Core.Headers[Iter];
    if (!OrigHeader || !OrigLatch || !ClonedHeader) return false;

    auto OrigPhis = OrigHeader->collectPhiNodes();
    auto ClonedPhis = ClonedHeader->collectPhiNodes();
    if (ClonedPhis.size() < OrigPhis.size()) return false;

    for (size_t I = 0; I < OrigPhis.size(); ++I) {
        PhiInst *OrigPhi = OrigPhis[I];
        PhiInst *ClonedPhi = ClonedPhis[I];
        if (!OrigPhi || !ClonedPhi) return false;

        Value *BackedgeValue = OrigPhi->getIncomingFromPred(OrigLatch);
        if (!BackedgeValue) return false;

        Value *Replacement =
            mapDataValueAtIteration(BackedgeValue, Iter - 1, Core);
        if (!Replacement) return false;

        ClonedPhi->replaceAllUsesWith(Replacement);
        Core.ValueMaps[Iter][OrigPhi] = Replacement;
        delete ClonedPhi;
    }

    return true;
}

bool LoopUnrollPass::removePeeledHeaderPhis(PeeledBodyCore &Core, int Iter) {
    if (Iter < 0 || Iter >= Core.PeelCount) return false;

    BasicBlock *OrigHeader = Core.Shape.Header;
    BasicBlock *OrigLatch = Core.Shape.Latch;
    BasicBlock *PreHeader = Core.Shape.PreHeader;
    BasicBlock *ClonedHeader = Core.Headers[Iter];
    if (!OrigHeader || !OrigLatch || !PreHeader || !ClonedHeader) {
        return false;
    }

    auto OrigPhis = OrigHeader->collectPhiNodes();
    auto ClonedPhis = ClonedHeader->collectPhiNodes();
    if (ClonedPhis.size() < OrigPhis.size()) return false;

    for (size_t I = 0; I < OrigPhis.size(); ++I) {
        PhiInst *OrigPhi = OrigPhis[I];
        PhiInst *ClonedPhi = ClonedPhis[I];
        if (!OrigPhi || !ClonedPhi) return false;

        Value *Replacement = nullptr;
        if (Iter == 0) {
            Replacement = OrigPhi->getIncomingFromPred(PreHeader);
        } else {
            Value *BackedgeValue = OrigPhi->getIncomingFromPred(OrigLatch);
            if (!BackedgeValue) return false;
            Replacement =
                mapDataValueAtPeelIteration(BackedgeValue, Iter - 1, Core);
        }

        if (!isValueCompatibleWithPhi(Replacement, ClonedPhi)) {
            return false;
        }

        ClonedPhi->replaceAllUsesWith(Replacement);
        Core.ValueMaps[Iter][OrigPhi] = Replacement;
        delete ClonedPhi;
    }

    return true;
}

std::optional<UnrolledBodyCore>
LoopUnrollPass::cloneUnrolledBodyCore(Loop *L, int Count, SCEVResult &SE) {
    if (!L || Count < 2 || !L->isInnermost()) return std::nullopt;

    auto Shape = getLoopExecutionShape(L, SE);
    if (!Shape || !Shape->Header || !Shape->Header->getParent()) {
        return std::nullopt;
    }

    std::vector<BasicBlock *> LoopBlocks = L->getBlocksInFunctionOrder();
    if (LoopBlocks.empty() || !canCloneAllInstructions(LoopBlocks)) {
        return std::nullopt;
    }

    Function *F = Shape->Header->getParent();
    int UnrollSerial = nextUnrollSerial();
    UnrolledBodyCore Core;
    Core.Shape = *Shape;
    Core.Count = Count;
    Core.IterBlocks.resize(Count);
    Core.IterBlocks[0] = LoopBlocks;
    Core.Headers.assign(Count, nullptr);
    Core.Latches.assign(Count, nullptr);
    Core.Exitings.assign(Count, nullptr);
    Core.ValueMaps.resize(Count);
    Core.Headers[0] = Shape->Header;
    Core.Latches[0] = Shape->Latch;
    Core.Exitings[0] = Shape->Exiting;

    for (int Iter = 1; Iter < Count; ++Iter) {
        auto &VMap = Core.ValueMaps[Iter];
        std::unordered_map<BasicBlock *, BasicBlock *> BlockMap;

        for (BasicBlock *BB : LoopBlocks) {
            auto *NewBB = new BasicBlock();
            NewBB->setIdent(makeUnrollBlockIdent(F, BB, Iter, UnrollSerial));
            F->pushBlock(NewBB);
            VMap[BB] = NewBB;
            BlockMap[BB] = NewBB;
            Core.IterBlocks[Iter].push_back(NewBB);
        }

        Core.Headers[Iter] = BlockMap[Shape->Header];
        Core.Latches[Iter] = BlockMap[Shape->Latch];
        Core.Exitings[Iter] = BlockMap[Shape->Exiting];
        if (!Core.Headers[Iter] || !Core.Latches[Iter] || !Core.Exitings[Iter]) {
            return std::nullopt;
        }

        for (BasicBlock *BB : LoopBlocks) {
            BasicBlock *NewBB = BlockMap[BB];
            if (!NewBB) return std::nullopt;

            for (Instruction *I : *BB) {
                Instruction *NewI = I->clone(VMap);
                if (!NewI) return std::nullopt;
                NewBB->push_back(NewI);
                VMap[I] = NewI;
            }
        }

        for (BasicBlock *BB : LoopBlocks) {
            BasicBlock *NewBB = BlockMap[BB];
            if (!NewBB) return std::nullopt;
            for (Instruction *I : *NewBB) {
                replaceOperandsUsingMap(I, VMap);
            }
        }

        if (!removeClonedHeaderPhis(Core, Iter)) {
            return std::nullopt;
        }
    }

    return Core;
}

std::optional<PeeledBodyCore>
LoopUnrollPass::clonePeeledIterations(Loop *L, int PeelCount, SCEVResult &SE) {
    if (!L || PeelCount <= 0 || !L->isInnermost()) return std::nullopt;

    auto Shape = getLoopExecutionShape(L, SE);
    if (!Shape || !Shape->Header || !Shape->PreHeader || !Shape->Latch ||
        !Shape->Exiting || !Shape->Exit || !Shape->Header->getParent()) {
        return std::nullopt;
    }

    std::vector<BasicBlock *> LoopBlocks = L->getBlocksInFunctionOrder();
    if (LoopBlocks.empty() || !canCloneAllInstructions(LoopBlocks)) {
        return std::nullopt;
    }

    Function *F = Shape->Header->getParent();
    int PeelSerial = nextUnrollSerial();
    PeeledBodyCore Core;
    Core.Shape = *Shape;
    Core.PeelCount = PeelCount;
    Core.IterBlocks.resize(PeelCount);
    Core.Headers.resize(PeelCount);
    Core.Latches.resize(PeelCount);
    Core.Exitings.resize(PeelCount);
    Core.ValueMaps.resize(PeelCount);

    for (int Iter = 0; Iter < PeelCount; ++Iter) {
        auto &VMap = Core.ValueMaps[Iter];
        std::unordered_map<BasicBlock *, BasicBlock *> BlockMap;

        for (BasicBlock *BB : LoopBlocks) {
            auto *NewBB = new BasicBlock();
            NewBB->setIdent(makePeelBlockIdent(F, BB, Iter, PeelSerial));
            F->pushBlock(NewBB);
            VMap[BB] = NewBB;
            BlockMap[BB] = NewBB;
            Core.IterBlocks[Iter].push_back(NewBB);
        }

        Core.Headers[Iter] = BlockMap[Shape->Header];
        Core.Latches[Iter] = BlockMap[Shape->Latch];
        Core.Exitings[Iter] = BlockMap[Shape->Exiting];
        if (!Core.Headers[Iter] || !Core.Latches[Iter] ||
            !Core.Exitings[Iter]) {
            return std::nullopt;
        }

        for (BasicBlock *BB : LoopBlocks) {
            BasicBlock *NewBB = BlockMap[BB];
            if (!NewBB) return std::nullopt;

            for (Instruction *I : *BB) {
                Instruction *NewI = I->clone(VMap);
                if (!NewI) return std::nullopt;
                NewBB->push_back(NewI);
                VMap[I] = NewI;
            }
        }

        for (BasicBlock *BB : LoopBlocks) {
            BasicBlock *NewBB = BlockMap[BB];
            if (!NewBB) return std::nullopt;
            for (Instruction *I : *NewBB) {
                replaceOperandsUsingMap(I, VMap);
            }
        }

        if (!removePeeledHeaderPhis(Core, Iter)) {
            return std::nullopt;
        }
    }

    return Core;
}

bool LoopUnrollPass::rewriteOriginalHeaderPhisForPartial(UnrolledBodyCore &Core,
                                         int LastIter,
                                         std::unordered_map<PhiInst *, Value *>
                                             *Replacements) {
    BasicBlock *Header = Core.Headers.front();
    BasicBlock *OrigLatch = Core.Latches.front();
    BasicBlock *LastLatch =
        LastIter >= 0 && LastIter < Core.Count ? Core.Latches[LastIter]
                                               : nullptr;
    if (!Header || !OrigLatch || !LastLatch || LastIter <= 0) return false;

    auto HeaderPhis = Header->collectPhiNodes();
    std::vector<PhiRewritePlan> Plans;
    Plans.reserve(HeaderPhis.size());

    for (PhiInst *Phi : HeaderPhis) {
        if (!Phi) continue;

        auto Incoming = Phi->collectIncomingEdges();
        PhiRewritePlan Plan;
        Plan.OldPhi = Phi;
        Plan.Incoming.reserve(Incoming.size());

        Value *BackedgeValue = nullptr;
        for (const auto &Edge : Incoming) {
            if (Edge.Pred == OrigLatch) {
                BackedgeValue = Edge.V;
                continue;
            }
            Plan.Incoming.push_back(Edge);
        }

        if (!BackedgeValue) return false;
        Value *MappedBackedge =
            mapDataValueAtIteration(BackedgeValue, LastIter, Core);
        if (!MappedBackedge) return false;

        Plan.Incoming.push_back({MappedBackedge, LastLatch});
        for (const auto &Edge : Plan.Incoming) {
            if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi)) {
                return false;
            }
        }
        Plans.push_back(std::move(Plan));
    }

    for (PhiRewritePlan &Plan : Plans) {
        if (!Plan.OldPhi) return false;
        if (Plan.Incoming.empty()) {
            Plan.Replacement =
                UndefValue::NewUndefValue(Plan.OldPhi->getIRType());
        } else if (Plan.Incoming.size() == 1) {
            Plan.Replacement = Plan.Incoming.front().V;
        } else {
            Plan.NewPhi = new PhiInst(Plan.OldPhi->getIRType());
            for (const auto &Edge : Plan.Incoming) {
                Plan.NewPhi->addIncoming(Edge.V, Edge.Pred);
            }
            Plan.Replacement = Plan.NewPhi;
        }
        if (!isDataValue(Plan.Replacement)) return false;
    }

    if (Replacements) {
        for (const PhiRewritePlan &Plan : Plans) {
            (*Replacements)[Plan.OldPhi] = Plan.Replacement;
        }
    }

    for (const PhiRewritePlan &Plan : Plans) {
        Plan.OldPhi->replaceAllUsesWith(Plan.Replacement);
    }
    for (const PhiRewritePlan &Plan : Plans) {
        if (Plan.NewPhi) {
            Plan.OldPhi->replaceWith(Plan.NewPhi);
        }
    }
    for (const PhiRewritePlan &Plan : Plans) {
        delete Plan.OldPhi;
    }

    return true;
}

bool LoopUnrollPass::rewriteOriginalHeaderPhisAfterPeeling(PeeledBodyCore &Core) {
    BasicBlock *Header = Core.Shape.Header;
    BasicBlock *PreHeader = Core.Shape.PreHeader;
    BasicBlock *OrigLatch = Core.Shape.Latch;
    int LastIter = Core.PeelCount - 1;
    BasicBlock *LastPeeledLatch =
        LastIter >= 0 && LastIter < static_cast<int>(Core.Latches.size())
            ? Core.Latches[LastIter]
            : nullptr;

    if (!Header || !PreHeader || !OrigLatch || !LastPeeledLatch) {
        return false;
    }

    auto HeaderPhis = Header->collectPhiNodes();
    for (PhiInst *Phi : HeaderPhis) {
        if (!Phi) continue;

        auto Incoming = Phi->collectIncomingEdges();
        std::vector<PhiInst::IncomingEdge> NewIncoming;
        NewIncoming.reserve(Incoming.size());

        Value *BackedgeValue = nullptr;
        for (const auto &Edge : Incoming) {
            if (Edge.Pred == PreHeader) {
                continue;
            }

            if (Edge.Pred == OrigLatch) {
                BackedgeValue = Edge.V;
            }

            NewIncoming.push_back(Edge);
        }

        if (!BackedgeValue) return false;

        Value *NewInitial =
            mapDataValueAtPeelIteration(BackedgeValue, LastIter, Core);
        if (!isValueCompatibleWithPhi(NewInitial, Phi)) {
            return false;
        }

        NewIncoming.push_back({NewInitial, LastPeeledLatch});

        if (!rebuildPhiFromIncomingChecked(Phi, NewIncoming)) {
            return false;
        }
    }

    return true;
}

bool LoopUnrollPass::removeIncomingFromExitPhisForPeeledBlocks(PeeledBodyCore &Core) {
    BasicBlock *Exit = Core.Shape.Exit;
    if (!Exit) return false;

    std::unordered_set<BasicBlock *> RemovedPreds;
    for (BasicBlock *BB : Core.Exitings) {
        if (BB) RemovedPreds.insert(BB);
    }

    for (PhiInst *Phi : Exit->collectPhiNodes()) {
        if (!Phi) continue;

        std::vector<PhiInst::IncomingEdge> Incoming;
        for (const auto &Edge : Phi->collectIncomingEdges()) {
            if (!RemovedPreds.count(Edge.Pred)) {
                Incoming.push_back(Edge);
            }
        }

        if (!rebuildPhiFromIncomingChecked(Phi, Incoming)) {
            return false;
        }
    }

    return true;
}

bool LoopUnrollPass::hasLatchExitShape(const UnrolledBodyCore &Core) {
    return Core.Shape.Exiting && Core.Shape.Latch &&
           Core.Shape.Exiting == Core.Shape.Latch;
}

std::unordered_set<BasicBlock *>
LoopUnrollPass::collectCoreExitings(const UnrolledBodyCore &Core) {
    std::unordered_set<BasicBlock *> Result;
    for (BasicBlock *BB : Core.Exitings) {
        if (BB) Result.insert(BB);
    }
    return Result;
}

bool LoopUnrollPass::rewriteExitPhisForSingleExiting(UnrolledBodyCore &Core,
                                     int ActiveIter) {
    if (ActiveIter < 0 || ActiveIter >= Core.Count) return false;

    BasicBlock *Exit = Core.Shape.Exit;
    BasicBlock *OrigExiting = Core.Shape.Exiting;
    BasicBlock *ActiveExiting = Core.Exitings[ActiveIter];
    if (!Exit || !OrigExiting || !ActiveExiting) return false;

    auto RemovedPreds = collectCoreExitings(Core);
    auto ExitPhis = Exit->collectPhiNodes();
    for (PhiInst *Phi : ExitPhis) {
        if (!Phi) continue;

        Value *OrigValue = Phi->getIncomingFromPred(OrigExiting);
        Value *MappedValue =
            OrigValue ? mapDataValueAtIteration(OrigValue, ActiveIter, Core)
                      : UndefValue::NewUndefValue(Phi->getIRType());
        if (!MappedValue) return false;

        std::vector<PhiInst::IncomingEdge> Incoming;
        for (const auto &Edge : Phi->collectIncomingEdges()) {
            if (!RemovedPreds.count(Edge.Pred)) Incoming.push_back(Edge);
        }
        Incoming.push_back({MappedValue, ActiveExiting});
        if (!rebuildPhiFromIncomingChecked(Phi, Incoming)) return false;
    }
    return true;
}

std::vector<BasicBlock *> LoopUnrollPass::collectExitRegionBlocksFrom(
    BasicBlock *Exit,
    const std::unordered_set<BasicBlock *> &StopBlocks,
    bool StopAtConditionalBranch) {
    std::vector<BasicBlock *> Region;
    if (!Exit) return Region;

    std::unordered_set<BasicBlock *> Seen;
    std::vector<BasicBlock *> Stack;
    Stack.push_back(Exit);

    while (!Stack.empty()) {
        BasicBlock *BB = Stack.back();
        Stack.pop_back();
        if (!BB || Seen.count(BB)) continue;
        if (BB != Exit && StopBlocks.count(BB)) continue;

        Seen.insert(BB);
        Region.push_back(BB);

        if (StopAtConditionalBranch &&
            BasicBlock::isCondBr(BB->getBranchTerminator())) {
            continue;
        }

        for (BasicBlock *Succ : BB->getSuccessors()) {
            if (!Succ || Seen.count(Succ)) continue;
            if (StopBlocks.count(Succ)) continue;
            Stack.push_back(Succ);
        }
    }

    return Region;
}

std::vector<ExitRegionDataUse> LoopUnrollPass::collectExitRegionDataUsesFrom(
    BasicBlock *Exit,
    const std::unordered_set<BasicBlock *> &StopBlocks,
    BasicBlock *Exiting,
    bool StopAtConditionalBranch) {
    std::vector<ExitRegionDataUse> Uses;

    for (BasicBlock *BB : collectExitRegionBlocksFrom(
             Exit, StopBlocks, StopAtConditionalBranch)) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<PhiInst *>(I)) continue;
            for (size_t OperandIndex = 0;
                 OperandIndex < I->getNumOperands(); ++OperandIndex) {
                Use *U = I->getOperandUse(OperandIndex);
                if (!U) continue;
                Value *Old = U->getValue();
                if (!isDataValue(Old)) continue;
                Value *Lookup = Old;
                if (auto *Phi = dynamic_cast<PhiInst *>(Old)) {
                    if (Phi->getParent() == Exit && Exiting) {
                        if (Value *Incoming = Phi->getIncomingFromPred(Exiting)) {
                            Lookup = Incoming;
                        }
                    }
                }
                Uses.push_back({U, Old, Lookup, Old->getIRType()});
            }
        }
    }

    return Uses;
}

bool LoopUnrollPass::rewriteExitRegionDataOperandsForIteration(
    UnrolledBodyCore &Core,
    int ActiveIter,
    const std::vector<ExitRegionDataUse> &CapturedUses,
    const std::unordered_map<Value *, Value *> *HeaderExitValues) {
    if (ActiveIter <= 0 ||
        ActiveIter >= static_cast<int>(Core.ValueMaps.size())) {
        return false;
    }

    for (const ExitRegionDataUse &Entry : CapturedUses) {
        if (!Entry.OperandUse || !Entry.OriginalValue) continue;
        Value *Lookup = Entry.LookupValue ? Entry.LookupValue
                                          : Entry.OriginalValue;
        if (HeaderExitValues) {
            auto It = HeaderExitValues->find(Lookup);
            if (It != HeaderExitValues->end()) {
                if (isValueCompatibleWithType(It->second,
                                              Entry.ExpectedType)) {
                    Entry.OperandUse->replaceUseeWith(It->second);
                    continue;
                }
            }
        }
        Value *Mapped = getCapturedDataValueMappedAtIteration(
            Lookup, ActiveIter, Core);
        if (!Mapped) continue;
        if (!isValueCompatibleWithType(Mapped, Entry.ExpectedType)) continue;
        Entry.OperandUse->replaceUseeWith(Mapped);
    }

    return true;
}

bool LoopUnrollPass::replaceDataOperandsInReachableRegionFrom(
    BasicBlock *Start,
    const std::unordered_set<BasicBlock *> &StopBlocks,
    const std::unordered_map<Value *, Value *> &Replacements) {
    for (BasicBlock *BB :
         collectExitRegionBlocksFrom(Start, StopBlocks,
                                     /*StopAtConditionalBranch=*/false)) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            if (!replaceDataOperandsUsingMap(I, Replacements)) {
                return false;
            }
        }
    }
    return true;
}

void LoopUnrollPass::refreshCapturedExitRegionUses(
    std::vector<ExitRegionDataUse> &Uses,
    const std::unordered_map<Value *, Value *> &Replacements) {
    for (ExitRegionDataUse &Entry : Uses) {
        auto OrigIt = Replacements.find(Entry.OriginalValue);
        if (OrigIt != Replacements.end()) {
            Entry.OriginalValue = OrigIt->second;
        }

        auto LookupIt = Replacements.find(Entry.LookupValue);
        if (LookupIt != Replacements.end()) {
            Entry.LookupValue = LookupIt->second;
        }
    }
}

bool LoopUnrollPass::removeOriginalHeaderPhisForFull(UnrolledBodyCore &Core) {
    BasicBlock *Header = Core.Headers.front();
    BasicBlock *PreHeader = Core.Shape.PreHeader;
    if (!Header || !PreHeader) return false;

    auto HeaderPhis = Header->collectPhiNodes();
    for (PhiInst *Phi : HeaderPhis) {
        if (!Phi) continue;
        Value *InitialValue = Phi->getIncomingFromPred(PreHeader);
        if (!InitialValue) return false;
        Phi->replaceAllUsesWith(InitialValue);
        delete Phi;
    }

    return true;
}

bool LoopUnrollPass::makeStraightLineMain(UnrolledBodyCore &Core, int MainCount,
                          BasicBlock *FinalTarget, Value *ContinueCondition) {
    if (MainCount < 2 || MainCount > Core.Count || !FinalTarget) return false;
    if (!hasLatchExitShape(Core)) return false;

    for (int Iter = 0; Iter + 1 < MainCount; ++Iter) {
        if (!BasicBlock::replaceCondBrWithUncond(Core.Exitings[Iter],
                                                Core.Headers[Iter + 1])) {
            return false;
        }
    }

    BasicBlock *LastExiting = Core.Exitings[MainCount - 1];
    if (!LastExiting) return false;
    if (!ContinueCondition) {
        return BasicBlock::replaceCondBrWithUncond(LastExiting, FinalTarget);
    }

    return BasicBlock::replaceTerminator(
        LastExiting,
        new BrInst(ContinueCondition, Core.Headers.front(), FinalTarget));
}

Value *LoopUnrollPass::makeTypedIntConstant(int64_t V, std::shared_ptr<IRType> Ty) {
    if (V < std::numeric_limits<int>::min() ||
        V > std::numeric_limits<int>::max()) {
        return nullptr;
    }
    size_t Size = 4;
    if (Ty && Ty->getEnum() == IR_INT) Size = Ty->getSize();
    return ConstInt::newConstInt(static_cast<int>(V), Size);
}

bool LoopUnrollPass::isValueAvailableAtPreheader(Value *V, Loop *L, BasicBlock *PreHeader,
                                 const DominatorTree &DT) {
    if (!V || !L || !PreHeader) return false;
    if (V->isConst()) return true;
    if (auto *Var = dynamic_cast<Variable *>(V)) {
        return Var->isParam() || Var->isGlobal();
    }
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || L->contains(I->getParent())) return false;
    return I->getParent() == PreHeader ||
           DT.dominates(I->getParent(), PreHeader);
}

bool LoopUnrollPass::canMaterializeSCEV(const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
                        const DominatorTree &DT) {
    if (!E) return false;
    switch (E->Kind) {
    case SCEVExprKind::Constant:
        return E->ConstValue >= std::numeric_limits<int>::min() &&
               E->ConstValue <= std::numeric_limits<int>::max();
    case SCEVExprKind::Unknown:
        return isValueAvailableAtPreheader(E->RawValue, L, PreHeader, DT);
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub:
    case SCEVExprKind::Mul:
    case SCEVExprKind::Div:
    case SCEVExprKind::Mod:
    case SCEVExprKind::Min:
    case SCEVExprKind::Max:
    case SCEVExprKind::Compare:
        return E->Operands.size() == 2 &&
               canMaterializeSCEV(E->Operands[0], L, PreHeader, DT) &&
               canMaterializeSCEV(E->Operands[1], L, PreHeader, DT);
    case SCEVExprKind::Cast:
        return E->Ty && E->Operands.size() == 1 &&
               canMaterializeSCEV(E->Operands[0], L, PreHeader, DT);
    case SCEVExprKind::AddRec:
    case SCEVExprKind::MulRec:
    case SCEVExprKind::DivRec:
        return false;
    }
    return false;
}

Value *LoopUnrollPass::materializeSCEV(const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
                       const DominatorTree &DT,
                       std::unordered_map<const SCEVExpr *, Value *> &Cache) {
    if (!canMaterializeSCEV(E, L, PreHeader, DT)) return nullptr;
    auto Cached = Cache.find(E);
    if (Cached != Cache.end()) return Cached->second;

    Value *Result = nullptr;
    if (E->Kind == SCEVExprKind::Constant) {
        Result = makeTypedIntConstant(E->ConstValue, E->Ty);
    } else if (E->Kind == SCEVExprKind::Unknown) {
        Result = E->RawValue;
    } else if (E->Kind == SCEVExprKind::Cast) {
        Value *Op =
            materializeSCEV(E->Operands[0], L, PreHeader, DT, Cache);
        if (!Op) return nullptr;

        Instruction *Cast = nullptr;
        switch (E->CastKind) {
        case SCEVCastKind::Trunc:
            Cast = new TruncInst(E->Ty);
            Cast->addUse(Op);
            break;
        case SCEVCastKind::ZExt:
            Cast = new ZextInst(E->Ty);
            Cast->addUse(Op);
            break;
        case SCEVCastKind::SExt:
            Cast = new SextInst(E->Ty);
            Cast->addUse(Op);
            break;
        case SCEVCastKind::BitCast:
            Cast = new BitCastInst(Op, E->Ty);
            break;
        }
        if (!Cast) return nullptr;
        BasicBlock::insertBeforeTerminator(PreHeader, Cast);
        Result = Cast;
    } else {
        Value *LHS =
            materializeSCEV(E->Operands[0], L, PreHeader, DT, Cache);
        Value *RHS =
            materializeSCEV(E->Operands[1], L, PreHeader, DT, Cache);
        if (!LHS || !RHS) return nullptr;

        if (E->Kind == SCEVExprKind::Min ||
            E->Kind == SCEVExprKind::Max) {
            BinaryInst::Operation Pred =
                E->Kind == SCEVExprKind::Min ? BinaryInst::L
                                             : BinaryInst::G;
            auto *Cmp = new BinaryInst(LHS, Pred, RHS);
            BasicBlock::insertBeforeTerminator(PreHeader, Cmp);
            auto *Select = new SelectInst(Cmp, LHS, RHS);
            BasicBlock::insertBeforeTerminator(PreHeader, Select);
            Result = Select;
        } else {
            BinaryInst::Operation Op = BinaryInst::ADD;
            switch (E->Kind) {
            case SCEVExprKind::Add: Op = BinaryInst::ADD; break;
            case SCEVExprKind::Sub: Op = BinaryInst::SUB; break;
            case SCEVExprKind::Mul: Op = BinaryInst::MUL; break;
            case SCEVExprKind::Div: Op = BinaryInst::DIV; break;
            case SCEVExprKind::Mod: Op = BinaryInst::MOD; break;
            case SCEVExprKind::Compare: Op = E->Predicate; break;
            default: return nullptr;
            }
            auto *I = new BinaryInst(LHS, Op, RHS);
            BasicBlock::insertBeforeTerminator(PreHeader, I);
            Result = I;
        }
    }

    if (Result) Cache[E] = Result;
    return Result;
}

bool LoopUnrollPass::replaceValuesInIterationBlocks(
    UnrolledBodyCore &Core, int FirstIter,
    const std::unordered_map<Value *, Value *> &Replacements) {
    for (int Iter = FirstIter; Iter < Core.Count; ++Iter) {
        if (Iter < 0 || Iter >= static_cast<int>(Core.IterBlocks.size())) {
            return false;
        }
        for (BasicBlock *NewBB : Core.IterBlocks[Iter]) {
            if (!NewBB) continue;
            for (Instruction *I : *NewBB) {
                if (!replaceDataOperandsUsingMap(I, Replacements)) {
                    return false;
                }
            }
        }
    }
    return true;
}

Value *LoopUnrollPass::getValueBeforeLoop(Value *V, const UnrolledBodyCore &Core,
                          std::shared_ptr<IRType> FallbackTy) {
    if (!V) return UndefValue::NewUndefValue(std::move(FallbackTy));
    if (!isDataValue(V)) return nullptr;
    if (auto *Phi = dynamic_cast<PhiInst *>(V)) {
        if (Phi->getParent() == Core.Shape.Header) {
            Value *Initial = Phi->getIncomingFromPred(Core.Shape.PreHeader);
            if (isDataValue(Initial)) return Initial;
            return nullptr;
        }
    }
    if (auto *I = dynamic_cast<Instruction *>(V)) {
        if (I->getParent() && I->getParent() != Core.Shape.PreHeader) {
            for (const auto &MapEntry : Core.ValueMaps) {
                if (MapEntry.count(I)) {
                    return UndefValue::NewUndefValue(std::move(FallbackTy));
                }
            }
        }
    }
    return isDataValue(V) ? V : nullptr;
}

BasicBlock *LoopUnrollPass::getNonExitSuccessor(BasicBlock *Exiting, BasicBlock *Exit) {
    if (!Exiting || !Exit) return nullptr;
    for (BasicBlock *Succ : Exiting->getSuccessors()) {
        if (Succ && Succ != Exit) return Succ;
    }
    return nullptr;
}

Value *LoopUnrollPass::mapValueAfterIteration(Value *V, int Iter,
                              const std::vector<HeaderState> &States,
                              const UnrolledBodyCore &Core) {
    if (!isDataValue(V)) return nullptr;
    for (const HeaderState &State : States) {
        if (V == State.CurrentValue) {
            Value *Mapped = State.BackedgeIsOriginalPhi
                                ? State.CurrentValue
                                : mapDataValueAtIteration(
                                      State.BackedgeValue, Iter, Core);
            if (Iter >= State.TailStartIter && State.TailStateValue &&
                Mapped == State.CurrentValue) {
                return State.TailStateValue;
            }
            return Mapped;
        }
    }
    return mapDataValueAtIteration(V, Iter, Core);
}

bool LoopUnrollPass::buildHeaderExitValueMap(
    const std::vector<HeaderState> &HeaderStates,
    int ExitIter,
    const UnrolledBodyCore &Core,
    BasicBlock *PreHeader,
    std::unordered_map<Value *, Value *> &HeaderExitValues) {
    HeaderExitValues.clear();

    for (const HeaderState &State : HeaderStates) {
        Value *ExitValue = mapValueAfterIteration(State.CurrentValue,
                                                  ExitIter,
                                                  HeaderStates,
                                                  Core);
        if (!isDataValue(ExitValue)) return false;

        HeaderExitValues[State.CurrentValue] = ExitValue;
        HeaderExitValues[State.MainValue] = ExitValue;
        HeaderExitValues[State.BackedgeValue] = ExitValue;
        if (State.CurrentPhi) {
            for (const auto &Edge : State.CurrentPhi->collectIncomingEdges()) {
                if (Edge.Pred != PreHeader && isDataValue(Edge.V)) {
                    HeaderExitValues[Edge.V] = ExitValue;
                }
            }
        }
    }

    return true;
}

bool LoopUnrollPass::buildHeaderTailInputValueMap(
    const std::vector<HeaderState> &HeaderStates,
    BasicBlock *PreHeader,
    std::unordered_map<Value *, Value *> &TailInputValues) {
    TailInputValues.clear();

    for (const HeaderState &State : HeaderStates) {
        Value *InputValue = State.TailStateValue ? State.TailStateValue
                                                 : State.CurrentValue;
        if (!isDataValue(InputValue)) return false;

        TailInputValues[State.CurrentValue] = InputValue;
        TailInputValues[State.MainValue] = InputValue;
        TailInputValues[State.BackedgeValue] = InputValue;
        if (State.CurrentPhi) {
            for (const auto &Edge : State.CurrentPhi->collectIncomingEdges()) {
                if (Edge.Pred != PreHeader && isDataValue(Edge.V)) {
                    TailInputValues[Edge.V] = InputValue;
                }
            }
        }
    }

    return true;
}

LoopUnrollResult LoopUnrollPass::executeUnrollWithTail(
    Loop *L, int MainCount, int TailCopies, Value *NumGroups,
    Value *RemainderValue, bool IsRuntime, SCEVResult &SE) {
    if (!L || MainCount < 2 || TailCopies < 1 || !NumGroups) {
        return LoopUnrollResult::Unmodified;
    }

    auto PreCloneShape = getLoopExecutionShape(L, SE);
    if (!PreCloneShape || !PreCloneShape->Exit) {
        return LoopUnrollResult::Unmodified;
    }
    std::unordered_set<BasicBlock *> PreCloneLoopBlocks;
    for (BasicBlock *BB : L->getBlocksInFunctionOrder()) {
        if (BB) PreCloneLoopBlocks.insert(BB);
    }
    bool PreCloneHeaderExiting =
        PreCloneShape->Exiting && PreCloneShape->Header &&
        PreCloneShape->Exiting == PreCloneShape->Header;
    std::vector<ExitRegionDataUse> ExitRegionUses =
        collectExitRegionDataUsesFrom(PreCloneShape->Exit,
                                      PreCloneLoopBlocks,
                                      PreCloneShape->Exiting,
                                      PreCloneHeaderExiting);

    auto Core = cloneUnrolledBodyCore(L, MainCount + TailCopies, SE);
    if (!Core) {
        return LoopUnrollResult::Unmodified;
    }
    bool LatchExiting = hasLatchExitShape(*Core);
    bool HeaderExiting =
        Core->Shape.Exiting && Core->Shape.Header &&
        Core->Shape.Exiting == Core->Shape.Header;
    if (!LatchExiting && !HeaderExiting) {
        return LoopUnrollResult::Unmodified;
    }

    BasicBlock *Header = Core->Shape.Header;
    BasicBlock *PreHeader = Core->Shape.PreHeader;
    BasicBlock *MainLast = Core->Latches[MainCount - 1];
    BasicBlock *MainTailPred =
        HeaderExiting ? Header : Core->Exitings[MainCount - 1];
    BasicBlock *Exit = Core->Shape.Exit;
    Function *F = Header ? Header->getParent() : nullptr;
    if (!Header || !PreHeader || !MainLast || !MainTailPred || !Exit || !F) {
        return LoopUnrollResult::Unmodified;
    }

    std::vector<HeaderState> HeaderStates;
    for (PhiInst *Phi : Header->collectPhiNodes()) {
        Value *Initial = Phi->getIncomingFromPred(PreHeader);
        Value *Backedge = Phi->getIncomingFromPred(Core->Shape.Latch);
        Value *MainValue =
            mapDataValueAtIteration(Backedge, MainCount - 1, *Core);
        if (!isDataValue(Initial) || !isDataValue(Backedge) ||
            !isDataValue(MainValue)) {
            return LoopUnrollResult::Unmodified;
        }
        HeaderStates.push_back(
            {Phi, nullptr, nullptr, Initial, Backedge, MainValue, nullptr,
             std::numeric_limits<int>::max(), MainValue == Phi,
             Backedge == Phi});
    }

    std::unordered_map<PhiInst *, Value *> HeaderPhiReplacements;
    if (!rewriteOriginalHeaderPhisForPartial(*Core, MainCount - 1,
                                             &HeaderPhiReplacements)) {
        return LoopUnrollResult::Unmodified;
    }

    for (HeaderState &State : HeaderStates) {
        auto It = HeaderPhiReplacements.find(State.OriginalPhi);
        if (It == HeaderPhiReplacements.end() ||
            !isDataValue(It->second)) {
            return LoopUnrollResult::Unmodified;
        }
        State.CurrentValue = It->second;
        State.CurrentPhi = dynamic_cast<PhiInst *>(State.CurrentValue);
        if (State.MainValueIsOriginalPhi) {
            State.MainValue = State.CurrentValue;
        }
    }
    std::unordered_map<Value *, Value *> HeaderValueReplacements;
    for (const auto &Entry : HeaderPhiReplacements) {
        HeaderValueReplacements[Entry.first] = Entry.second;
    }
    if (HeaderExiting) {
        refreshCapturedExitRegionUses(ExitRegionUses,
                                      HeaderValueReplacements);
    }

    auto *TailEntry = new BasicBlock();
    std::string TailBase =
        Header->getIdent() +
        (IsRuntime ? ".unr.runtime.tail" : ".unr.constant.tail");
    TailEntry->setIdent(F->makeUniqueBlockIdent(TailBase));
    F->pushBlock(TailEntry);

    if (IsRuntime || HeaderExiting) {
        std::unordered_map<Value *, Value *> TailStateMap;
        for (HeaderState &State : HeaderStates) {
            Value *TailState = State.CurrentValue;
            if (IsRuntime) {
                auto *Merge = new PhiInst(State.InitialValue->getIRType());
                Value *MainIncoming =
                    HeaderExiting ? State.CurrentValue : State.MainValue;
                if (!addIncomingChecked(Merge, State.InitialValue,
                                        PreHeader) ||
                    !addIncomingChecked(Merge, MainIncoming,
                                        MainTailPred)) {
                    delete Merge;
                    return LoopUnrollResult::Unmodified;
                }
                TailEntry->push_back(Merge);
                TailState = Merge;
            }
            if (!isDataValue(State.MainValue) || !isDataValue(TailState)) {
                return LoopUnrollResult::Unmodified;
            }
            State.TailStateValue = TailState;
            State.TailStartIter = MainCount;
            TailStateMap[State.MainValue] = TailState;
            if (HeaderExiting) {
                TailStateMap[State.CurrentValue] = TailState;
                TailStateMap[State.BackedgeValue] = TailState;
                if (State.CurrentPhi) {
                    for (const auto &Edge :
                         State.CurrentPhi->collectIncomingEdges()) {
                        if (Edge.Pred != PreHeader && isDataValue(Edge.V)) {
                            TailStateMap[Edge.V] = TailState;
                        }
                    }
                }
            }
        }
        if (!replaceValuesInIterationBlocks(*Core, MainCount, TailStateMap)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    auto *GroupPhi = new PhiInst(NumGroups->getIRType());
    Value *Zero = makeTypedIntConstant(0, NumGroups->getIRType());
    Value *One = makeTypedIntConstant(1, NumGroups->getIRType());
    if (!Zero || !One) {
        return LoopUnrollResult::Unmodified;
    }
    GroupPhi->addIncoming(Zero, PreHeader);
    Header->push_front(GroupPhi);

    auto *NextGroup = new BinaryInst(GroupPhi, BinaryInst::ADD, One);
    BasicBlock::insertBeforeTerminator(MainLast, NextGroup);
    GroupPhi->addIncoming(NextGroup, MainLast);

    if (LatchExiting) {
        auto *MoreGroups =
            new BinaryInst(NextGroup, BinaryInst::L, NumGroups);
        BasicBlock::insertBeforeTerminator(MainLast, MoreGroups);
        if (!makeStraightLineMain(*Core, MainCount, TailEntry,
                                  MoreGroups)) {
            return LoopUnrollResult::Unmodified;
        }
    } else {
        BasicBlock *FirstBody = getNonExitSuccessor(Header, Exit);
        if (!FirstBody) {
            return LoopUnrollResult::Unmodified;
        }

        auto *HasGroup =
            new BinaryInst(GroupPhi, BinaryInst::L, NumGroups);
        BasicBlock::insertBeforeTerminator(Header, HasGroup);
        if (!BasicBlock::replaceTerminator(
                Header, new BrInst(HasGroup, FirstBody, TailEntry))) {
            return LoopUnrollResult::Unmodified;
        }

        for (int Iter = 1; Iter < MainCount; ++Iter) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Exitings[Iter], Exit);
            if (!Body ||
                !BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Body)) {
                return LoopUnrollResult::Unmodified;
            }
        }
        for (int Iter = 0; Iter + 1 < MainCount; ++Iter) {
            if (!BasicBlock::redirectBranchTarget(
                    Core->Latches[Iter], Core->Headers[Iter],
                    Core->Headers[Iter + 1])) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!BasicBlock::redirectBranchTarget(
                Core->Latches[MainCount - 1],
                Core->Headers[MainCount - 1], Header)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    for (int Tail = 0; Tail < TailCopies; ++Tail) {
        int Iter = MainCount + Tail;
        BasicBlock *TailExiting =
            LatchExiting ? Core->Exitings[Iter] : Core->Latches[Iter];
        if (!TailExiting) {
            return LoopUnrollResult::Unmodified;
        }

        if (HeaderExiting) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Exitings[Iter], Exit);
            if (!Body ||
                !BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Body)) {
                return LoopUnrollResult::Unmodified;
            }
        }

        if (!IsRuntime) {
            BasicBlock *Target =
                Tail + 1 < TailCopies ? Core->Headers[Iter + 1] : Exit;
            if (!BasicBlock::replaceCondBrWithUncond(TailExiting, Target)) {
                return LoopUnrollResult::Unmodified;
            }
            continue;
        }

        if (Tail + 1 == TailCopies) {
            if (!BasicBlock::replaceCondBrWithUncond(TailExiting, Exit)) {
                return LoopUnrollResult::Unmodified;
            }
            continue;
        }

        Value *Threshold =
            makeTypedIntConstant(Tail + 1, RemainderValue->getIRType());
        if (!Threshold) {
            return LoopUnrollResult::Unmodified;
        }
        auto *HasMore =
            new BinaryInst(RemainderValue, BinaryInst::G, Threshold);
        BasicBlock::insertBeforeTerminator(TailExiting, HasMore);
        if (!BasicBlock::replaceTerminator(TailExiting,
                           new BrInst(HasMore, Core->Headers[Iter + 1],
                                      Exit))) {
            return LoopUnrollResult::Unmodified;
        }
    }

    auto RemovedPreds = collectCoreExitings(*Core);
    auto ExitPhis = Exit->collectPhiNodes();
    for (PhiInst *Phi : ExitPhis) {
        Value *OrigValue = Phi->getIncomingFromPred(Core->Shape.Exiting);
        std::vector<PhiInst::IncomingEdge> Incoming;
        for (const auto &Edge : Phi->collectIncomingEdges()) {
            if (!RemovedPreds.count(Edge.Pred)) Incoming.push_back(Edge);
        }

        if (IsRuntime) {
            Value *InitialExit =
                getValueBeforeLoop(OrigValue, *Core, Phi->getIRType());
            Value *MainExit =
                OrigValue ? (HeaderExiting
                                 ? OrigValue
                                 : mapValueAfterIteration(
                                       OrigValue, MainCount - 1,
                                       HeaderStates, *Core))
                          : UndefValue::NewUndefValue(Phi->getIRType());
            if (!InitialExit || !MainExit) {
                return LoopUnrollResult::Unmodified;
            }
            auto *NoTailValue = new PhiInst(Phi->getIRType());
            if (!addIncomingChecked(NoTailValue, InitialExit, PreHeader) ||
                !addIncomingChecked(NoTailValue, MainExit, MainTailPred)) {
                delete NoTailValue;
                return LoopUnrollResult::Unmodified;
            }
            TailEntry->push_back(NoTailValue);
            Incoming.push_back({NoTailValue, TailEntry});

            for (int Tail = 0; Tail < TailCopies; ++Tail) {
                int Iter = MainCount + Tail;
                Value *TailValue =
                    OrigValue ? mapValueAfterIteration(
                                    OrigValue, Iter, HeaderStates, *Core)
                              : UndefValue::NewUndefValue(Phi->getIRType());
                if (!TailValue) {
                    return LoopUnrollResult::Unmodified;
                }
                Incoming.push_back(
                    {TailValue, LatchExiting ? Core->Exitings[Iter]
                                             : Core->Latches[Iter]});
            }
        } else {
            int LastTailIter = MainCount + TailCopies - 1;
            Value *TailValue =
                OrigValue ? mapValueAfterIteration(
                                OrigValue, LastTailIter, HeaderStates, *Core)
                    : UndefValue::NewUndefValue(Phi->getIRType());
            if (!TailValue) {
                return LoopUnrollResult::Unmodified;
            }
            Incoming.push_back(
                {TailValue, LatchExiting ? Core->Exitings[LastTailIter]
                                         : Core->Latches[LastTailIter]});
        }

        if (!rebuildPhiFromIncomingChecked(Phi, Incoming)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    if (!IsRuntime) {
        int LastTailIter = MainCount + TailCopies - 1;
        std::unordered_map<Value *, Value *> HeaderExitValues;
        if (HeaderExiting) {
            if (!buildHeaderExitValueMap(HeaderStates, LastTailIter,
                                         *Core, PreHeader,
                                         HeaderExitValues)) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!rewriteExitRegionDataOperandsForIteration(
                *Core, LastTailIter, ExitRegionUses,
                HeaderExiting ? &HeaderExitValues : nullptr)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    if (IsRuntime) {
        if (!RemainderValue) {
            return LoopUnrollResult::Unmodified;
        }
        Value *ZeroRemainder =
            makeTypedIntConstant(0, RemainderValue->getIRType());
        if (!ZeroRemainder) {
            return LoopUnrollResult::Unmodified;
        }
        auto *HasRemainder =
            new BinaryInst(RemainderValue, BinaryInst::NE, ZeroRemainder);
        TailEntry->push_back(HasRemainder);
        TailEntry->push_back(
            new BrInst(HasRemainder, Core->Headers[MainCount], Exit));

        Value *ZeroGroups =
            makeTypedIntConstant(0, NumGroups->getIRType());
        if (!ZeroGroups) {
            return LoopUnrollResult::Unmodified;
        }
        auto *HasMain =
            new BinaryInst(NumGroups, BinaryInst::G, ZeroGroups);
        BasicBlock::insertBeforeTerminator(PreHeader, HasMain);
        if (!BasicBlock::replaceTerminator(
                PreHeader,
                new BrInst(HasMain, Core->Headers.front(), TailEntry))) {
            return LoopUnrollResult::Unmodified;
        }
    } else {
        TailEntry->push_back(new BrInst(Core->Headers[MainCount]));
        if (HeaderExiting) {
            std::unordered_map<Value *, Value *> TailInputValues;
            if (!buildHeaderTailInputValueMap(HeaderStates, PreHeader,
                                              TailInputValues)) {
                return LoopUnrollResult::Unmodified;
            }
            auto TailScanStopBlocks = PreCloneLoopBlocks;
            TailScanStopBlocks.insert(Exit);
            if (!replaceDataOperandsInReachableRegionFrom(
                    TailEntry, TailScanStopBlocks, TailInputValues)) {
                return LoopUnrollResult::Unmodified;
            }
        }
    }

    SE.clear();
    return LoopUnrollResult::PartiallyUnrolled;
}

LoopUnrollPass::LoopUnrollPass(UnrollConfig ConfigValue)
    : Config(std::move(ConfigValue)) {}

PreservationStatus LoopUnrollPass::run(Function &F,
                                       FunctionAnalysisPassManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    if (LI.getTopLevelLoops().empty()) {
        return PreservationStatus::all();
    }

    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);

    for (Loop *L : LI.getLoopsInPostOrder(true)) {
        LoopUnrollResult Result = tryToUnrollLoop(F, L, LI, DT, SE);
        if (Result != LoopUnrollResult::Unmodified) {
            SE.clear();
            return PreservationStatus::none();
        }
    }

    return PreservationStatus::all();
}

LoopUnrollResult LoopUnrollPass::tryToUnrollLoop(Function &F,
                                                 Loop *L,
                                                 LoopInfo &LI,
                                                 DominatorTree &DT,
                                                 SCEVResult &SE) {
    (void)F;

    if (!L) {
        return LoopUnrollResult::Unmodified;
    }

    if (!isCanonicalLoop(L)) {
        return LoopUnrollResult::Unmodified;
    }

    auto TripInfo = SE.analyzeLoopTripCount(L);
    if (!TripInfo || !TripInfo->Valid) {
        return LoopUnrollResult::Unmodified;
    }

    if (!canCloneLoopBody(L, LI)) {
        return LoopUnrollResult::Unmodified;
    }

    int BodyCost = estimateLoopBodyCost(L, LI);
    UnrollDecision Decision =
        computeUnrollDecision(L, *TripInfo, BodyCost, Config, LI, SE);

    if (!Decision.ShouldUnroll || Decision.Kind == UnrollKind::None) {
        return LoopUnrollResult::Unmodified;
    }


    switch (Decision.Kind) {
    case UnrollKind::Full:
        return executeFullUnroll(L, Decision, SE, LI, DT);
    case UnrollKind::Partial:
        return executePartialUnroll(L, Decision, SE, LI, DT);
    case UnrollKind::PartialWithRemainder:
        return executePartialWithRemainder(L, Decision, SE, LI, DT);
    case UnrollKind::Runtime:
        return executeRuntimeUnroll(L, Decision, SE, LI, DT);
    case UnrollKind::Peeling:
        return executePeeling(L, Decision, SE, LI, DT);
    case UnrollKind::None:
        break;
    }

    return LoopUnrollResult::Unmodified;
}

bool LoopUnrollPass::isCanonicalLoop(Loop *L) const {
    if (!L || !L->getHeader()) {
        return false;
    }

    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) || !PreHeader || !Latch) {
        return false;
    }

    if (!LCSSAPass::isLCSSAForm(L)) {
        return false;
    }

    std::vector<BasicBlock *> ExitBlocks = L->getExitBlocks();
    if (ExitBlocks.size() != 1 || !ExitBlocks.front()) {
        return false;
    }

    std::unordered_set<BasicBlock *> ExitingBlocks;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (L->isExitingBlock(BB)) {
            ExitingBlocks.insert(BB);
        }
    }

    return ExitingBlocks.size() == 1;
}

bool LoopUnrollPass::canCloneLoopBody(Loop *L, LoopInfo &LI) const {
    (void)LI;
    if (!L) return false;

    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (auto It = BB->begin(); It != BB->end(); ++It) {
            if (!isSupportedCloneInstruction(*It)) {
                return false;
            }
        }
    }

    return true;
}

int LoopUnrollPass::estimateLoopBodyCost(Loop *L, LoopInfo &LI) const {
    if (!L) return 0;

    int Cost = 0;
    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB || LI.getLoopFor(BB) != L) continue;

        for (auto It = BB->begin(); It != BB->end(); ++It) {
            Instruction *I = *It;
            if (!I) continue;

            switch (I->instType) {
            case Instruction::PHI:
                break;
            case Instruction::ADD:
            case Instruction::SUB:
            case Instruction::FADD:
            case Instruction::FSUB:
            case Instruction::ICMP:
            case Instruction::FCMP:
            case Instruction::TRUNC:
            case Instruction::ZEXT:
            case Instruction::SEXT:
            case Instruction::FPTOSI:
            case Instruction::SITOFP:
            case Instruction::BITCAST:
            case Instruction::AND:
            case Instruction::OR:
            case Instruction::XOR:
            case Instruction::SHL:
            case Instruction::LSHR:
            case Instruction::ASHR:
            case Instruction::SELECT:
                Cost += 1;
                break;
            case Instruction::MUL:
            case Instruction::FMUL:
                Cost += 3;
                break;
            case Instruction::SDIV:
            case Instruction::UDIV:
            case Instruction::SREM:
            case Instruction::UREM:
                Cost += 10;
                break;
            case Instruction::FDIV:
            case Instruction::FREM:
                Cost += 10;
                break;
            case Instruction::LOAD:
            case Instruction::STORE:
            case Instruction::GEP:
                Cost += 2;
                break;
            case Instruction::CALL:
                Cost += 20;
                break;
            case Instruction::BR:
            case Instruction::RET:
                Cost += 1;
                break;
            default:
                Cost += 1;
                break;
            }
        }
    }

    return Cost;
}

bool LoopUnrollPass::containsCall(Loop *L, LoopInfo &LI) const {
    (void)LI;
    if (!L) return false;

    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) continue;
        for (auto It = BB->begin(); It != BB->end(); ++It) {
            Instruction *I = *It;
            if (I && I->instType == Instruction::CALL) {
                return true;
            }
        }
    }

    return false;
}

int LoopUnrollPass::estimateSCEVCost(const SCEVExpr *E) const {
    if (!E) return 1000000;

    int Cost = localSCEVNodeCost(E);
    for (const SCEVExpr *Op : E->Operands) {
        Cost += estimateSCEVCost(Op);
    }
    return Cost;
}

bool LoopUnrollPass::isCheapSCEV(const SCEVExpr *E,
                                 const UnrollConfig &Config) const {
    if (!E) return false;
    if (Config.AllowExpensiveTripCount) return true;

    int Nodes = 0;
    int Cost = 0;
    std::vector<const SCEVExpr *> Stack;
    Stack.push_back(E);

    while (!Stack.empty()) {
        const SCEVExpr *Cur = Stack.back();
        Stack.pop_back();
        if (!Cur) return false;

        ++Nodes;
        Cost += localSCEVNodeCost(Cur);
        if (Nodes > Config.SCEVExpansionBudget ||
            Cost > Config.SCEVExpansionBudget) {
            return false;
        }

        for (const SCEVExpr *Op : Cur->Operands) {
            Stack.push_back(Op);
        }
    }

    return true;
}

int LoopUnrollPass::detectPeelCount(Loop *L,
                                    const InductionDescriptor &IV,
                                    const UnrollConfig &Config) const {
    if (!L || !IV.Valid || !IV.Phi) {
        return 0;
    }

    if (!IV.ConstantStart || !IV.ConstantStep ||
        *IV.ConstantStart != 0 || *IV.ConstantStep != 1) {
        return 0;
    }

    int BestPeelCount = 0;
    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB) continue;
        if (BB->Size() == 0) continue;
        auto *Br = dynamic_cast<BrInst *>(BB->back());
        if (!isInternalConditionalBranch(L, Br)) {
            continue;
        }

        auto *Cmp = dynamic_cast<BinaryInst *>(Br->getOperand(0));
        auto Match = matchCompareWithIV(Cmp, IV.Phi);
        if (!Match) {
            continue;
        }

        BestPeelCount = std::max(BestPeelCount, peelCountForPredicate(*Match));
        if (BestPeelCount >= Config.MaxPeelCount) {
            return Config.MaxPeelCount;
        }
    }

    return std::min(BestPeelCount, Config.MaxPeelCount);
}

UnrollDecision LoopUnrollPass::computeUnrollDecision(
    Loop *L,
    const LoopTripCountInfo &TCI,
    int BodyCost,
    const UnrollConfig &Config,
    LoopInfo &LI,
    SCEVResult &SE) const {
    if (!L || !TCI.Valid || BodyCost <= 0) {
        return makeNoUnrollDecision("not profitable");
    }

    bool HasCall = containsCall(L, LI);
    bool Nested = !L->isInnermost();

    if (Config.AllowFull && TCI.ConstantTripCount) {
        int64_t TripCount = *TCI.ConstantTripCount;
        int64_t MaxCount = (BodyCost <= 4)
                               ? Config.TinyLoopFullUnrollMaxCount
                               : Config.FullUnrollMaxCount;
        int64_t TotalCost = static_cast<int64_t>(BodyCost) * TripCount;
        bool Restricted = HasCall || Nested;
        int FullLimit = Restricted ? Config.FullThreshold / 2
                                   : Config.FullThreshold;

        if (TripCount > 1 && TripCount <= MaxCount &&
            (!Restricted || TripCount <= 4) &&
            withinThreshold(TotalCost, FullLimit)) {
            UnrollDecision Decision;
            Decision.Kind = UnrollKind::Full;
            Decision.Count = static_cast<int>(TripCount);
            Decision.ShouldUnroll = true;
            Decision.NeedRemainder = false;
            Decision.IsRuntime = false;
            Decision.IsPeeling = false;
            Decision.Reason = "full: small constant trip count";
            return Decision;
        }
    }

    if (HasCall || Nested) {
        return makeNoUnrollDecision("not profitable");
    }

    if (Config.AllowPeeling && TCI.ConstantTripCount &&
        *TCI.ConstantTripCount > 2 && BodyCost <= Config.PeelThreshold) {
        auto IV = SE.getCanonicalInduction(L);
        if (IV && IV->Valid) {
            int PeelCount = detectPeelCount(L, *IV, Config);
            if (PeelCount > 0 && PeelCount <= 2 &&
                PeelCount < *TCI.ConstantTripCount) {
                UnrollDecision Decision;
                Decision.Kind = UnrollKind::Peeling;
                Decision.Count = PeelCount;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = false;
                Decision.IsRuntime = false;
                Decision.IsPeeling = true;
                Decision.Reason = "peeling: initial special iterations";
                return Decision;
            }
        }
    }

    if (Config.AllowPartial && TCI.ConstantTripCount &&
        BodyCost <= Config.MaxLoopBodyCostForPartial) {
        int64_t TripCount = *TCI.ConstantTripCount;
        int Factor = chooseFactorBySize(BodyCost,
                                        Config.PartialThreshold,
                                        Config.PartialMaxCount);
        std::vector<int> Factors;
        if (Factor >= 4) {
            Factors.push_back(4);
            Factors.push_back(2);
        } else if (Factor >= 2) {
            Factors.push_back(2);
        }

        for (int Candidate : Factors) {
            if (TripCount >= Candidate && TripCount % Candidate == 0) {
                UnrollDecision Decision;
                Decision.Kind = UnrollKind::Partial;
                Decision.Count = Candidate;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = false;
                Decision.IsRuntime = false;
                Decision.IsPeeling = false;
                Decision.Reason =
                    "partial: constant trip count divisible by factor";
                return Decision;
            }
        }

        if (Config.AllowRemainder) {
            for (int Candidate : Factors) {
                if (TripCount % Candidate == 0 ||
                    TripCount <= Candidate) {
                    continue;
                }

                auto RI = SE.getRemainderInfo(L, Candidate);
                if (!RI || !RI->Valid || !RI->NeedRemainderLoop) {
                    continue;
                }

                UnrollDecision Decision;
                Decision.Kind = UnrollKind::PartialWithRemainder;
                Decision.Count = Candidate;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = true;
                Decision.IsRuntime = false;
                Decision.IsPeeling = false;
                Decision.Reason =
                    "partial-remainder: constant trip count with tail";
                return Decision;
            }
        }
    }

    if (Config.AllowRuntime && Config.AllowRemainder &&
        TCI.IsRuntime && BodyCost <= Config.MaxLoopBodyCostForRuntime &&
        isCheapSCEV(TCI.TripCount, Config)) {
        int RuntimeCountLimit = std::min(Config.RuntimeDefaultCount,
                                         Config.RuntimeMaxCount);
        int Factor = chooseFactorBySize(BodyCost,
                                        Config.RuntimeThreshold,
                                        RuntimeCountLimit);
        if (Factor >= 2) {
            auto RI = SE.getRemainderInfo(L, Factor);
            if (RI && RI->Valid && RI->NeedRemainderLoop) {
                UnrollDecision Decision;
                Decision.Kind = UnrollKind::Runtime;
                Decision.Count = Factor;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = true;
                Decision.IsRuntime = true;
                Decision.IsPeeling = false;
                Decision.Reason = "runtime: cheap runtime trip count";
                return Decision;
            }
        }
    }

    return makeNoUnrollDecision("not profitable");
}

LoopUnrollResult LoopUnrollPass::executeFullUnroll(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;
    (void)DT;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->ConstantTripCount ||
        *TCI->ConstantTripCount != Decision.Count ||
        TCI->ExitingBlock != TCI->Latch) {
        return LoopUnrollResult::Unmodified;
    }

    auto Core = cloneUnrolledBodyCore(L, Decision.Count, SE);
    if (!Core || !hasLatchExitShape(*Core)) {
        return LoopUnrollResult::Unmodified;
    }

    if (!makeStraightLineMain(*Core, Decision.Count, Core->Shape.Exit,
                              nullptr)) {
        return LoopUnrollResult::Unmodified;
    }
    if (!rewriteExitPhisForSingleExiting(*Core, Decision.Count - 1)) {
        return LoopUnrollResult::Unmodified;
    }
    if (!removeOriginalHeaderPhisForFull(*Core)) {
        return LoopUnrollResult::Unmodified;
    }

    SE.clear();
    return LoopUnrollResult::FullyUnrolled;
}

LoopUnrollResult LoopUnrollPass::executePartialUnroll(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;
    (void)DT;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->ConstantTripCount ||
        *TCI->ConstantTripCount % Decision.Count != 0 ||
        (TCI->ExitingBlock != TCI->Latch &&
         TCI->ExitingBlock != TCI->Header)) {
        return LoopUnrollResult::Unmodified;
    }

    auto Core = cloneUnrolledBodyCore(L, Decision.Count, SE);
    if (!Core) {
        return LoopUnrollResult::Unmodified;
    }

    if (hasLatchExitShape(*Core)) {
        BasicBlock *LastExiting = Core->Exitings.back();
        BasicBlock *ClonedHeader = Core->Headers.back();
        if (!LastExiting || !ClonedHeader ||
            !BasicBlock::redirectBranchTarget(LastExiting, ClonedHeader,
                                              Core->Headers.front())) {
            return LoopUnrollResult::Unmodified;
        }

        for (int Iter = 0; Iter + 1 < Decision.Count; ++Iter) {
            if (!BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Core->Headers[Iter + 1])) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!rewriteExitPhisForSingleExiting(*Core,
                                             Decision.Count - 1)) {
            return LoopUnrollResult::Unmodified;
        }
    } else if (Core->Shape.Exiting == Core->Shape.Header) {
        for (int Iter = 1; Iter < Decision.Count; ++Iter) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Exitings[Iter], Core->Shape.Exit);
            if (!Body ||
                !BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Body)) {
                return LoopUnrollResult::Unmodified;
            }
        }
        for (int Iter = 0; Iter + 1 < Decision.Count; ++Iter) {
            if (!BasicBlock::redirectBranchTarget(
                    Core->Latches[Iter], Core->Headers[Iter],
                    Core->Headers[Iter + 1])) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!BasicBlock::redirectBranchTarget(
                Core->Latches.back(), Core->Headers.back(),
                Core->Headers.front())) {
            return LoopUnrollResult::Unmodified;
        }
    } else {
        return LoopUnrollResult::Unmodified;
    }

    if (!rewriteOriginalHeaderPhisForPartial(*Core,
                                             Decision.Count - 1)) {
        return LoopUnrollResult::Unmodified;
    }

    SE.clear();
    return LoopUnrollResult::PartiallyUnrolled;
}

LoopUnrollResult LoopUnrollPass::executePartialWithRemainder(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;
    (void)DT;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }

    auto RI = SE.getRemainderInfo(L, Decision.Count);
    if (!RI || !RI->Valid || !RI->NeedRemainderLoop) {
        return LoopUnrollResult::Unmodified;
    }

    if (!RI->ConstantMainTripCount || !RI->ConstantRemainderCount ||
        *RI->ConstantMainTripCount < Decision.Count ||
        *RI->ConstantRemainderCount <= 0 ||
        *RI->ConstantRemainderCount >= Decision.Count) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || (TCI->ExitingBlock != TCI->Latch &&
                 TCI->ExitingBlock != TCI->Header)) {
        return LoopUnrollResult::Unmodified;
    }

    int64_t NumGroups =
        *RI->ConstantMainTripCount / Decision.Count;
    Value *GroupCount =
        makeTypedIntConstant(NumGroups, RI->TripCount->Ty);
    if (!GroupCount) return LoopUnrollResult::Unmodified;

    return executeUnrollWithTail(
        L, Decision.Count,
        static_cast<int>(*RI->ConstantRemainderCount), GroupCount, nullptr,
        false, SE);
}

LoopUnrollResult LoopUnrollPass::executeRuntimeUnroll(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;
    (void)DT;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }

    auto RI = SE.getRemainderInfo(L, Decision.Count);
    if (!RI || !RI->Valid || !RI->IsRuntimeRemainder) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || (TCI->ExitingBlock != TCI->Latch &&
                 TCI->ExitingBlock != TCI->Header) ||
        !TCI->PreHeader || !RI->TripCount ||
        !canMaterializeSCEV(RI->TripCount, L, TCI->PreHeader, DT)) {
        return LoopUnrollResult::Unmodified;
    }

    std::unordered_map<const SCEVExpr *, Value *> Cache;
    Value *TripCount =
        materializeSCEV(RI->TripCount, L, TCI->PreHeader, DT, Cache);
    Value *Factor =
        makeTypedIntConstant(Decision.Count, TripCount
                                                 ? TripCount->getIRType()
                                                 : nullptr);
    if (!TripCount || !Factor) return LoopUnrollResult::Unmodified;

    auto *Remainder =
        new BinaryInst(TripCount, BinaryInst::MOD, Factor);
    BasicBlock::insertBeforeTerminator(TCI->PreHeader, Remainder);
    auto *MainTripCount =
        new BinaryInst(TripCount, BinaryInst::SUB, Remainder);
    BasicBlock::insertBeforeTerminator(TCI->PreHeader, MainTripCount);
    auto *NumGroups =
        new BinaryInst(MainTripCount, BinaryInst::DIV, Factor);
    BasicBlock::insertBeforeTerminator(TCI->PreHeader, NumGroups);

    return executeUnrollWithTail(L, Decision.Count, Decision.Count - 1,
                                 NumGroups, Remainder, true, SE);
}

LoopUnrollResult LoopUnrollPass::executePeeling(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;
    (void)DT;

    if (!L || Decision.Count <= 0) {
        return LoopUnrollResult::Unmodified;
    }

    int PeelCount = Decision.Count;
    if (!L->isInnermost()) {
        return LoopUnrollResult::Unmodified;
    }

    std::vector<BasicBlock *> ExitBlocks = L->getExitBlocks();
    if (ExitBlocks.size() != 1 || !ExitBlocks.front()) {
        return LoopUnrollResult::Unmodified;
    }

    std::unordered_set<BasicBlock *> ExitingBlocks;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (L->isExitingBlock(BB)) {
            ExitingBlocks.insert(BB);
        }
    }
    if (ExitingBlocks.size() != 1) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->Valid || !TCI->ConstantTripCount ||
        *TCI->ConstantTripCount <= PeelCount) {
        return LoopUnrollResult::Unmodified;
    }

    auto ShapeOpt = getLoopExecutionShape(L, SE);
    if (!ShapeOpt || !ShapeOpt->PreHeader || !ShapeOpt->Header ||
        !ShapeOpt->Latch || !ShapeOpt->Exiting || !ShapeOpt->Exit) {
        return LoopUnrollResult::Unmodified;
    }

    bool LatchExiting = ShapeOpt->Exiting == ShapeOpt->Latch;
    bool HeaderExiting = ShapeOpt->Exiting == ShapeOpt->Header;
    if (!LatchExiting && !HeaderExiting) {
        return LoopUnrollResult::Unmodified;
    }

    std::vector<BasicBlock *> LoopBlocks = L->getBlocksInFunctionOrder();
    if (LoopBlocks.empty() || !canCloneAllInstructions(LoopBlocks)) {
        return LoopUnrollResult::Unmodified;
    }

    auto Core = clonePeeledIterations(L, PeelCount, SE);
    if (!Core) {
        return LoopUnrollResult::Unmodified;
    }

    if (LatchExiting) {
        for (int Iter = 0; Iter < PeelCount; ++Iter) {
            BasicBlock *Target =
                Iter + 1 < PeelCount ? Core->Headers[Iter + 1]
                                     : Core->Shape.Header;

            if (!Core->Exitings[Iter] || !Target ||
                !BasicBlock::replaceCondBrWithUncond(Core->Exitings[Iter],
                                                     Target)) {
                return LoopUnrollResult::Unmodified;
            }
        }
    } else if (HeaderExiting) {
        for (int Iter = 0; Iter < PeelCount; ++Iter) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Headers[Iter], Core->Shape.Exit);
            if (!Body ||
                !BasicBlock::replaceCondBrWithUncond(Core->Headers[Iter],
                                                     Body)) {
                return LoopUnrollResult::Unmodified;
            }

            BasicBlock *Target =
                Iter + 1 < PeelCount ? Core->Headers[Iter + 1]
                                     : Core->Shape.Header;

            if (!Core->Latches[Iter] || !Target ||
                !BasicBlock::redirectBranchTarget(Core->Latches[Iter],
                                                  Core->Headers[Iter],
                                                  Target)) {
                return LoopUnrollResult::Unmodified;
            }
        }
    }

    if (Core->Headers.empty() || !Core->Headers[0] ||
        !BasicBlock::redirectBranchTarget(Core->Shape.PreHeader,
                                          Core->Shape.Header,
                                          Core->Headers[0])) {
        return LoopUnrollResult::Unmodified;
    }

    if (!rewriteOriginalHeaderPhisAfterPeeling(*Core)) {
        return LoopUnrollResult::Unmodified;
    }

    if (!removeIncomingFromExitPhisForPeeledBlocks(*Core)) {
        return LoopUnrollResult::Unmodified;
    }

    SE.clear();
    return LoopUnrollResult::PartiallyUnrolled;
}
