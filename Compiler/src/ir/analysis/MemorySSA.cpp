#include "ir/analysis/MemorySSA.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <unordered_set>
MemoryAccess::MemoryAccess(Kind K, unsigned AccessID, BasicBlock *BB,
                           Instruction *I)
    : AccessKind(K), ID(AccessID), Block(BB), Inst(I) {}

MemoryAccess::Kind MemoryAccess::getKind() const {
    return AccessKind;
}

bool MemoryAccess::isLiveOnEntry() const {
    return AccessKind == Kind::LiveOnEntry;
}

bool MemoryAccess::isPhi() const {
    return AccessKind == Kind::Phi;
}

bool MemoryAccess::isUse() const {
    return AccessKind == Kind::Use;
}

bool MemoryAccess::isDef() const {
    return AccessKind == Kind::Def;
}

unsigned MemoryAccess::getID() const {
    return ID;
}

BasicBlock *MemoryAccess::getBlock() const {
    return Block;
}

Instruction *MemoryAccess::getInstruction() const {
    return Inst;
}

MemoryAccess *MemoryAccess::getDefiningAccess() const {
    return DefiningAccess;
}

const std::vector<std::pair<BasicBlock *, MemoryAccess *>> &
MemoryAccess::getIncomingAccesses() const {
    return IncomingAccesses;
}

MemorySSAResult::MemorySSAResult() = default;

MemorySSAResult::MemorySSAResult(MemorySSAResult &&) noexcept = default;

MemorySSAResult &MemorySSAResult::operator=(MemorySSAResult &&) noexcept = default;

MemoryAccess *MemorySSAResult::getLiveOnEntry() const {
    return LiveOnEntry;
}

std::size_t MemorySSAResult::size() const {
    return OwnedAccesses.size();
}

bool MemorySSAResult::isInvariantInLoop(Value *V, const Loop *L) {
    if (!V || !L) return true;
    if (V->isConst()) return true;
    if (V->isParam() || V->isGlobal()) return true;
    auto *I = dynamic_cast<Instruction *>(V);
    BasicBlock *BB = I ? I->getParent() : nullptr;
    return !BB || !L->contains(BB);
}

bool MemorySSAResult::storeFullyCoversItsPointer(const StoreInst *SI) {
    Value *P = SI ? SI->getPointerOperand() : nullptr;
    Value *V = SI ? SI->getValueOperand() : nullptr;
    std::shared_ptr<IRType> Pointee = P ? P->getPointeeType() : nullptr;
    return Pointee && V && V->getIRType() &&
           Pointee->hasSameShape(*V->getIRType());
}

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

// Walker 的单步判定：Store 交给 BasicAA，Call 结合 ModRef 摘要；未知 Def 一律视为写。
bool MemorySSAResult::mayClobber(
    MemoryAccess *Access, const MemoryLocation &Location,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) const {
    Value *Ptr = Location.Pointer;
    if (!Access || !Access->isDef() || !Ptr) return true;
    Instruction *I = Access->getInstruction();
    if (!I) return true;

    // 1. Store 使用带访问宽度的别名查询，只有明确 NoAlias 才可跳过。
    if (auto *SI = dynamic_cast<StoreInst *>(I)) {
        Value *StorePtr = SI->getPointerOperand();
        return !StorePtr ||
               AA.aliasWithIndexRange(Location, MemoryLocation::get(SI), LI) !=
                   AliasInfo::NoAlias;
    }

    // 2. Call 使用跨函数固定点 ModRef 摘要；未知指针来源已在摘要中保守表示。
    if (dynamic_cast<CallInst *>(I)) {
        ModRefInfo MRI = getInstModRefInfo(Location, I, AA, LI, FAM);
        return isModSet(MRI);
    }

    // 3. 构建器尚未识别的新 MemoryDef 种类一律视作潜在写入。
    return true;
}

MemoryAccess *MemorySSAResult::getClobberingAccess(
    Instruction *At, Value *Ptr, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    MemoryAccess *MA = getMemoryAccess(At);
    if (!MA) return nullptr;
    return getClobberingAccess(
        MA->getDefiningAccess(), MemoryLocation(Ptr, std::nullopt), AA, LI,
        FAM, MaxVisitedAccesses);
}

MemoryAccess *MemorySSAResult::getClobberingAccess(
    Instruction *At, const MemoryLocation &Location, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    MemoryAccess *MA = getMemoryAccess(At);
    if (!MA) return nullptr;
    return getClobberingAccess(MA->getDefiningAccess(), Location, AA, LI,
                               FAM, MaxVisitedAccesses);
}

MemoryAccess *MemorySSAResult::getClobberingAccess(
    MemoryAccess *Start, Value *Ptr, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    return getClobberingAccess(
        Start, MemoryLocation(Ptr, std::nullopt), AA, LI, FAM,
        MaxVisitedAccesses);
}

// 最近写查询从给定内存状态反向遍历 Def/Phi 图，并汇合各路径的首个潜在写。
MemoryAccess *MemorySSAResult::getClobberingAccess(
    MemoryAccess *Start, const MemoryLocation &Location, BasicAAResult &AA,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    if (!Start || !Location.Pointer || MaxVisitedAccesses == 0)
        return nullptr;

    std::vector<MemoryAccess *> Worklist{Start};
    // 1. visited 同时截断图环；访问数超过预算时不发布部分证明。
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
            if (mayClobber(Current, Location, AA, LI, FAM)) {
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

    // 2. 所有路径汇到同一首写时返回该访问，否则以 Phi 表示路径分歧。
    if (BudgetExhausted) return nullptr;
    if (FirstClobbers.size() == 1)
        return *FirstClobbers.begin();

    if (Start->isPhi()) return Start;
    MemoryAccess *Defining = Start->getDefiningAccess();
    return Defining && Defining->isPhi() ? Defining : nullptr;
}

// 冗余写查询把候选 Store 当作归纳边，要求每条非递归路径都落到相同值的完整覆盖写。
bool MemorySSAResult::isStoreValueRedundant(
    StoreInst *Store, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM,
    std::size_t MaxVisitedAccesses) const {
    // 1. 验证候选写入的指针、值和覆盖宽度，拒绝 undef 与缺失的 MemoryDef。
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

    // 2. 若候选位于循环，指针和值必须对所有包含它的循环保持不变。
    Loop *InnermostLoop = LI.getLoopFor(Store->getParent());
    for (Loop *L = InnermostLoop; L;
         L = L->getParentLoop()) {
        if (!isInvariantInLoop(Ptr, L) ||
            !isInvariantInLoop(StoredValue, L))
            return false;
    }

    // 3. 沿 MemorySSA 反向展开 Phi；读和不相交写透明，未知写或别名不确定立即失败。
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

            if (!InnermostLoop) return false;

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

            if (AI != AliasInfo::MustAlias ||
                !storeFullyCoversItsPointer(PreviousStore) ||
                PreviousStore->getValueOperand() != StoredValue)
                return false;

            SawConcreteSameValueStore = true;
            continue;
        }

        if (dynamic_cast<CallInst *>(I)) {
            ModRefInfo MRI = getInstModRefInfo(
                MemoryLocation::get(Store), I, AA, LI, FAM);
            if (isModSet(MRI)) return false;

            Worklist.push_back(Current->getDefiningAccess());
            continue;
        }

        return false;
    }

    // 4. 自支撑回边不能充当基础事实，至少需要一条具体的同值前驱写。
    return SawConcreteSameValueStore;
}

bool MemorySSAResult::invalidate(Function &F,
                                 const PreservationStatus &PA) {
    (void)F;
    return !PA.isKept(MemorySSAAnalysis::ID());
}

PassID MemorySSAAnalysis::Key;

// MemorySSA 主流程以 O(B+E+I) 构建统一内存状态图，不需要 CFG 固定点。
MemorySSAResult MemorySSAAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    (void)FAM;
    MemorySSAResult Result;
    // 1. 创建 LiveOnEntry，并为每个基本块预建入口 MemoryPhi。
    Result.LiveOnEntry =
        Result.createAccess(MemoryAccess::Kind::LiveOnEntry);

    for (BasicBlock *BB : F) {
        if (!BB) continue;
        Result.BlockEntries[BB] =
            Result.createAccess(MemoryAccess::Kind::Phi, BB);
    }

    // 2. 按块内顺序连接 Load Use 与 Store/Call Def。Call 节点保留调用顺序，
    // walker 会通过精确 ModRef 将不访问程序内存的 timer 视为透明。
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

    // 3. 所有出口就绪后回填入口 Phi；入口块和无前驱块额外接入 LiveOnEntry。
    for (BasicBlock *BB : F) {
        if (!BB) continue;
        MemoryAccess *Entry = Result.BlockEntries[BB];
        std::vector<BasicBlock *> Preds = BB->getPredecessors();

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
