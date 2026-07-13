#include "ir/analysis/MemorySSA.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <unordered_set>

namespace {

bool isInvariantInLoop(Value *V, const Loop *L) {
    if (!V || !L) return true;
    if (V->isConst()) return true;
    if (auto *Var = dynamic_cast<Variable *>(V))
        return Var->isParam() || Var->isGlobal();
    auto *I = dynamic_cast<Instruction *>(V);
    BasicBlock *BB = I ? I->getParent() : nullptr;
    return !BB || !L->contains(BB);
}

bool storeFullyCoversItsPointer(const StoreInst *SI) {
    Value *P = SI ? SI->getPointerOperand() : nullptr;
    Value *V = SI ? SI->getValueOperand() : nullptr;
    std::shared_ptr<IRType> Pointee = P ? P->getPointeeType() : nullptr;
    return Pointee && V && V->getIRType() &&
           Pointee->hasSameShape(*V->getIRType());
}

} // namespace

MemoryAccess *MemorySSAResult::createAccess(MemoryAccess::Kind K,
                                            BasicBlock *BB,
                                            Instruction *I) {
    const unsigned ID = static_cast<unsigned>(OwnedAccesses.size());
    OwnedAccesses.emplace_back(new MemoryAccess(K, ID, BB, I));
    return OwnedAccesses.back().get();
}

MemoryAccess *MemorySSAResult::getMemoryAccess(Instruction *I) const {
    auto It = InstructionAccesses.find(I);
    return It == InstructionAccesses.end() ? nullptr : It->second;
}

MemoryAccess *MemorySSAResult::getBlockEntry(BasicBlock *BB) const {
    auto It = BlockEntries.find(BB);
    return It == BlockEntries.end() ? nullptr : It->second;
}

MemoryAccess *MemorySSAResult::getBlockExit(BasicBlock *BB) const {
    auto It = BlockExits.find(BB);
    return It == BlockExits.end() ? nullptr : It->second;
}

bool MemorySSAResult::mayClobber(
    MemoryAccess *Access, Value *Ptr, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) const {
    if (!Access || !Access->isDef() || !Ptr) return true;
    Instruction *I = Access->getInstruction();
    if (!I) return true;

    if (auto *SI = dynamic_cast<StoreInst *>(I)) {
        Value *StorePtr = SI->getPointerOperand();
        return !StorePtr ||
               AA.aliasWithIndexRange(Ptr, StorePtr, LI) !=
                   AliasInfo::NoAlias;
    }

    if (auto *CI = dynamic_cast<CallInst *>(I)) {
        ModRefInfo MRI = getInstModRefInfo(Ptr, I, AA, LI, FAM);
        if (isModSet(MRI)) return true;

        // Side-effect summaries can be temporarily incomplete for a call
        // graph SCC.  Do not let a false NoModRef result make an aliasing
        // pointer argument transparent.  A proven readonly call remains
        // transparent because its result is Ref rather than NoModRef.
        return isNoModRef(MRI) && AA.callMayAliasPointer(CI, Ptr, LI);
    }

    // The builder currently creates Defs only for stores and calls.  Keep the
    // fallback conservative if the IR grows another memory-writing opcode.
    return true;
}

MemoryAccess *MemorySSAResult::getClobberingAccess(
    Instruction *At, Value *Ptr, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    MemoryAccess *MA = getMemoryAccess(At);
    if (!MA) return nullptr;
    return getClobberingAccess(MA->getDefiningAccess(), Ptr, AA, LI, FAM,
                               MaxVisitedAccesses);
}

MemoryAccess *MemorySSAResult::getClobberingAccess(
    MemoryAccess *Start, Value *Ptr, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    if (!Start || !Ptr || MaxVisitedAccesses == 0) return nullptr;

    std::vector<MemoryAccess *> Worklist{Start};
    std::unordered_set<MemoryAccess *> Visited;
    std::unordered_set<MemoryAccess *> FirstClobbers;
    bool BudgetExhausted = false;

    while (!Worklist.empty()) {
        MemoryAccess *Current = Worklist.back();
        Worklist.pop_back();
        if (!Current || !Visited.insert(Current).second) continue;
        if (Visited.size() > MaxVisitedAccesses) {
            BudgetExhausted = true;
            break;
        }

        if (Current->isLiveOnEntry()) {
            FirstClobbers.insert(Current);
            continue;
        }

        if (Current->isDef()) {
            if (mayClobber(Current, Ptr, AA, LI, FAM)) {
                FirstClobbers.insert(Current);
                continue;
            }
            Worklist.push_back(Current->getDefiningAccess());
            continue;
        }

        if (Current->isUse()) {
            Worklist.push_back(Current->getDefiningAccess());
            continue;
        }

        for (const auto &Incoming : Current->getIncomingAccesses())
            Worklist.push_back(Incoming.second);
    }

    if (BudgetExhausted) return nullptr;
    if (FirstClobbers.size() == 1)
        return *FirstClobbers.begin();

    // Start itself may be a Def/Use when every predecessor walk diverges.
    // Prefer its defining Phi when available so clients cannot mistake an
    // unrelated transparent Def for the clobber.
    if (Start->isPhi()) return Start;
    MemoryAccess *Defining = Start->getDefiningAccess();
    return Defining && Defining->isPhi() ? Defining : nullptr;
}

bool MemorySSAResult::isStoreValueRedundant(
    StoreInst *Store, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    if (!Store || MaxVisitedAccesses == 0 ||
        !storeFullyCoversItsPointer(Store))
        return false;

    MemoryAccess *Candidate = getMemoryAccess(Store);
    if (!Candidate || !Candidate->isDef()) return false;
    Value *Ptr = Store->getPointerOperand();
    Value *StoredValue = Store->getValueOperand();
    if (!Ptr || !StoredValue || Ptr->isUndef() || StoredValue->isUndef())
        return false;
    Value *PointerRoot = Value::stripPointerCastsAndGEP(Ptr);
    if (!PointerRoot || PointerRoot->isUndef()) return false;

    // MemorySSA is not iteration-sensitive.  Consequently, recognizing the
    // candidate on a loop backedge as an inductive same-value definition is
    // legal only when both operands are stable across that loop's iterations.
    Loop *InnermostLoop = LI.getLoopFor(Store->getParent());
    for (Loop *L = InnermostLoop; L;
         L = L->getParentLoop()) {
        if (!isInvariantInLoop(Ptr, L) ||
            !isInvariantInLoop(StoredValue, L))
            return false;
    }

    std::vector<MemoryAccess *> Worklist{Candidate->getDefiningAccess()};
    std::unordered_set<MemoryAccess *> Visited;
    bool SawConcreteSameValueStore = false;

    while (!Worklist.empty()) {
        MemoryAccess *Current = Worklist.back();
        Worklist.pop_back();
        if (!Current || !Visited.insert(Current).second) continue;
        if (Visited.size() > MaxVisitedAccesses) return false;

        if (Current->isLiveOnEntry()) return false;

        if (Current == Candidate) {
            // Only a cycle represented by LoopInfo may be treated as an
            // induction edge.  In an irreducible/unrecognised CFG cycle there
            // is no loop-invariance proof for the dynamic pointer/value, so a
            // self-supported MemoryDef must fail conservatively.
            if (!InnermostLoop) return false;
            // This is the induction edge.  It is not a base proof by itself.
            continue;
        }

        if (Current->isPhi()) {
            if (Current->getIncomingAccesses().empty()) return false;
            for (const auto &Incoming : Current->getIncomingAccesses()) {
                if (!Incoming.second) return false;
                Worklist.push_back(Incoming.second);
            }
            continue;
        }

        if (Current->isUse()) {
            Worklist.push_back(Current->getDefiningAccess());
            continue;
        }

        Instruction *I = Current->getInstruction();
        if (auto *PreviousStore = dynamic_cast<StoreInst *>(I)) {
            Value *PreviousPtr = PreviousStore->getPointerOperand();
            AliasInfo AI = PreviousPtr
                               ? AA.aliasWithIndexRange(Ptr, PreviousPtr, LI)
                               : AliasInfo::MayAlias;
            if (AI == AliasInfo::NoAlias) {
                Worklist.push_back(Current->getDefiningAccess());
                continue;
            }

            // MayAlias is not an overwrite proof.  MustAlias still needs an
            // exact full-width store and the identical SSA value.
            if (AI != AliasInfo::MustAlias ||
                !storeFullyCoversItsPointer(PreviousStore) ||
                PreviousStore->getValueOperand() != StoredValue)
                return false;

            SawConcreteSameValueStore = true;
            continue;
        }

        if (auto *CI = dynamic_cast<CallInst *>(I)) {
            ModRefInfo MRI = getInstModRefInfo(Ptr, I, AA, LI, FAM);
            if (isModSet(MRI) ||
                (isNoModRef(MRI) && AA.callMayAliasPointer(CI, Ptr, LI)))
                return false;
            // Ref-only calls do not change the value being proved.
            Worklist.push_back(Current->getDefiningAccess());
            continue;
        }

        // Unknown MemoryDef kind.
        return false;
    }

    // Reject a self-supported loop with no concrete entry definition.
    return SawConcreteSameValueStore;
}

bool MemorySSAResult::invalidate(Function &F,
                                 const PreservationStatus &PA) {
    (void)F;
    return !PA.isKept(MemorySSAAnalysis::ID());
}

PassID MemorySSAAnalysis::Key;

MemorySSAResult MemorySSAAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    (void)FAM;
    MemorySSAResult Result;
    Result.LiveOnEntry =
        Result.createAccess(MemoryAccess::Kind::LiveOnEntry);

    // A block-entry Phi for every block avoids iterative construction and is
    // still linear in the CFG size.  Trivial one-input Phis are intentionally
    // retained: they are cheap and let clients use one uniform graph API.
    for (BasicBlock *BB : F) {
        if (!BB) continue;
        Result.BlockEntries[BB] =
            Result.createAccess(MemoryAccess::Kind::Phi, BB);
    }

    for (BasicBlock *BB : F) {
        if (!BB) continue;
        MemoryAccess *Current = Result.BlockEntries[BB];
        for (Instruction *I : *BB) {
            if (!I) continue;
            if (dynamic_cast<LoadInst *>(I)) {
                MemoryAccess *Use =
                    Result.createAccess(MemoryAccess::Kind::Use, BB, I);
                Use->DefiningAccess = Current;
                Result.InstructionAccesses[I] = Use;
                continue;
            }
            if (dynamic_cast<StoreInst *>(I) || dynamic_cast<CallInst *>(I)) {
                MemoryAccess *Def =
                    Result.createAccess(MemoryAccess::Kind::Def, BB, I);
                Def->DefiningAccess = Current;
                Result.InstructionAccesses[I] = Def;
                Current = Def;
            }
        }
        Result.BlockExits[BB] = Current;
    }

    // All exits now exist, so CFG order (including backedges and irreducible
    // SCCs) does not matter when wiring block-entry states.
    for (BasicBlock *BB : F) {
        if (!BB) continue;
        MemoryAccess *Entry = Result.BlockEntries[BB];
        std::vector<BasicBlock *> Preds = BB->getPredecessors();
        // The function entry has an implicit predecessor representing the
        // memory state at each invocation even when a CFG backedge targets
        // the entry block.  Omitting that edge lets loop-carried stores
        // incorrectly use one another as the only base proof.  Unreachable
        // predecessor-free blocks likewise start from an unknown state.
        if (BB == F.front() || Preds.empty()) {
            Entry->IncomingAccesses.push_back({nullptr, Result.LiveOnEntry});
        }
        for (BasicBlock *Pred : Preds) {
            MemoryAccess *PredExit = Result.getBlockExit(Pred);
            Entry->IncomingAccesses.push_back(
                {Pred, PredExit ? PredExit : Result.LiveOnEntry});
        }
    }

    return Result;
}
