#include "ir/opt/DSE.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>

bool DSEPass::storesHaveIdenticalCoverage(const StoreInst *First,
                                          const StoreInst *Second) {
    Value *FirstValue = First ? First->getValueOperand() : nullptr;
    Value *SecondValue = Second ? Second->getValueOperand() : nullptr;
    return FirstValue && SecondValue && FirstValue->getIRType() &&
           FirstValue->getIRType() == SecondValue->getIRType();
}

bool DSEPass::storeFullyCoversPointer(
    Value *Pointer, const StoreInst *Store) {
    std::shared_ptr<IRType> PointeeTy =
        Pointer ? Pointer->getPointeeType() : nullptr;
    Value *StoredValue = Store ? Store->getValueOperand() : nullptr;
    return PointeeTy && StoredValue && StoredValue->getIRType() &&
           PointeeTy == StoredValue->getIRType();
}

bool DSEPass::isNonEscapingLocalStackPointer(Value *Pointer) {
    // 2.1 从 alloca 沿 GEP/bitcast use 闭包追踪；只允许作为 load/store 地址使用，
    // 一旦被存入内存或交给其他用户就视为逃逸，函数出口也不能据此删除最后写入。
    auto *Root = dynamic_cast<AllocaInst *>(
        Value::stripPointerCastsAndGEP(Pointer));
    if (!Root) return false;

    std::unordered_set<Value *> Visited;
    std::vector<Value *> Worklist{Root};
    while (!Worklist.empty()) {
        Value *Current = Worklist.back();
        Worklist.pop_back();
        if (!Current || !Visited.insert(Current).second) continue;

        for (Use *U : Current->getUses()) {
            if (!U || U->getValue() != Current) continue;
            User *Usr = U->getUser();
            if (!Usr) return false;

            if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Usr)) {
                if (GEP->getBasePointer() != Current) return false;
                Worklist.push_back(GEP);
                continue;
            }
            if (auto *BC = dynamic_cast<BitCastInst *>(Usr)) {
                if (BC->getUnaryOperand() != Current) return false;
                Worklist.push_back(BC);
                continue;
            }
            if (auto *LI = dynamic_cast<LoadInst *>(Usr)) {
                if (LI->getPointerOperand() != Current) return false;
                continue;
            }
            if (auto *SI = dynamic_cast<StoreInst *>(Usr)) {

                if (SI->getPointerOperand() != Current ||
                    SI->getValueOperand() == Current)
                    return false;
                continue;
            }

            return false;
        }
    }
    return true;
}

bool DSEPass::eliminateInBlockWithAA(BasicBlock *BB, BasicAAResult &AA,
                                     const LoopInfo &LoopInfo,
                                     FunctionAnalysisPassManager &FAM) {
    // 1. 从块尾逆向维护“尚未被观察的后继 store”。load 会清除所有可能别名项；
    // 当前 store 若被同覆盖范围的 MustAlias 后继 store 覆盖即可删除。调用只按
    // 程序内存 ModRef 清除冲突项；不访问程序内存的 timer 对该状态透明。
    if (BB == nullptr) {
        return false;
    }

    auto instructions = collectInstructions(*BB);

    bool changed = false;
    std::unordered_map<Value *, StoreInst *> pendingStores;

    for (auto it = instructions.rbegin(); it != instructions.rend(); ++it) {
        Instruction *I = *it;

        if (auto *LI = dynamic_cast<LoadInst *>(I)) {
            Value *LoadPtr = LI->getPointerOperand();
            if (!LoadPtr) {
                pendingStores.clear();
                continue;
            }

            // 当前 load 会观察所有可能别名地址，因此对应的后继 store 不能再用于
            // 证明更早写入无效，需要从待定集合中清除。
            for (auto PendingIt = pendingStores.begin();
                 PendingIt != pendingStores.end();) {
                Value *StorePtr = PendingIt->first;
                AliasInfo Alias = LoadPtr == StorePtr
                                      ? AliasInfo::MustAlias
                                      : AA.alias(
                                            MemoryLocation::get(LI),
                                            MemoryLocation::get(
                                                PendingIt->second),
                                            LoopInfo);
                if (Alias != AliasInfo::NoAlias) {
                    PendingIt = pendingStores.erase(PendingIt);
                } else {
                    ++PendingIt;
                }
            }
            continue;
        }

        if (auto *SI = dynamic_cast<StoreInst *>(I)) {
            Value *ptr = SI->getPointerOperand();
            if (!ptr) {
                pendingStores.clear();
                continue;
            }

            auto ExactIt = pendingStores.find(ptr);
            bool FullyOverwritten =
                ExactIt != pendingStores.end() &&
                storesHaveIdenticalCoverage(SI, ExactIt->second);
            if (!FullyOverwritten) {
                for (const auto &Pending : pendingStores) {
                    if (AA.alias(MemoryLocation::get(SI),
                                 MemoryLocation::get(Pending.second),
                                 LoopInfo) ==
                            AliasInfo::MustAlias &&
                        storesHaveIdenticalCoverage(SI, Pending.second)) {
                        FullyOverwritten = true;
                        break;
                    }
                }
            }

            if (FullyOverwritten) {
                delete SI;
                changed = true;
                continue;
            }

            for (auto PendingIt = pendingStores.begin();
                 PendingIt != pendingStores.end();) {
                AliasInfo Alias = AA.alias(
                    MemoryLocation::get(SI),
                    MemoryLocation::get(PendingIt->second), LoopInfo);
                if (Alias != AliasInfo::NoAlias) {
                    PendingIt = pendingStores.erase(PendingIt);
                } else {
                    ++PendingIt;
                }
            }
            pendingStores[ptr] = SI;
            continue;
        }

        if (auto *CI = dynamic_cast<CallInst *>(I)) {
            for (auto PendingIt = pendingStores.begin();
                 PendingIt != pendingStores.end();) {
                StoreInst *PendingStore = PendingIt->second;
                ModRefInfo Effect = getInstModRefInfo(
                    MemoryLocation::get(PendingStore), CI, AA, LoopInfo, FAM);
                if (isRefSet(Effect) || isModSet(Effect)) {
                    PendingIt = pendingStores.erase(PendingIt);
                } else {
                    ++PendingIt;
                }
            }
        }
    }

    return changed;
}

bool DSEPass::eliminateInBlock(BasicBlock *BB) {
    (void)BB;
    return false;
}

DSEBlockColor DSEPass::classifyFrom(
    Instruction *First, const MemoryLocation &Location,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    // 2.2 顺序扫描一段路径：完整 MustAlias 覆盖记为 Kill；任何可能读/写目标的
    // 指令记为 Clobber；扫描到底仍未观察目标则为 Transparent。
    Value *P = Location.Pointer;
    if (!P) return DSEBlockColor::Clobber;
    for (Instruction *I = First; I; I = I->getNext()) {

        if (auto *SI = dynamic_cast<StoreInst *>(I)) {
            Value *StorePtr = SI->getPointerOperand();
            if (StorePtr &&
                AA.aliasWithIndexRange(
                    Location, MemoryLocation::get(SI), LI) ==
                    AliasInfo::MustAlias &&
                storeFullyCoversPointer(P, SI)) {
                return DSEBlockColor::Kill;
            }
        }

        ModRefInfo MR = getInstModRefInfo(Location, I, AA, LI, FAM);

        if (isRefSet(MR) || isModSet(MR)) return DSEBlockColor::Clobber;
    }

    return DSEBlockColor::Transparent;
}

bool DSEPass::isStoreDead(
    StoreInst *S1,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    // 2.3 先分类 store 所在块的尾部；同块已 Kill 可立即删除，已 Clobber 则立即保留。
    if (!S1) return false;
    Value *P = S1->getPointerOperand();
    BasicBlock *B1 = S1->getParent();
    if (!P) return false;
    if (!B1) return false;

    MemoryLocation StoreLocation = MemoryLocation::get(S1);
    DSEBlockColor tailColor = classifyFrom(
        S1->getNext(), StoreLocation, AA, LI, FAM);
    if (tailColor == DSEBlockColor::Kill) return true;
    if (tailColor == DSEBlockColor::Clobber) return false;

    // 2.4 若控制流直接退出，只有未逃逸栈对象的最终值不可观察，其他内存必须保留。
    {
        auto b1Succs = B1->getSuccessors();
        if (b1Succs.empty()) {
            if (isNonEscapingLocalStackPointer(P)) {
                return true;
            } else {
                return false;
            }
        }
    }

    // 2.5 对所有后继做有界 BFS。每条路径都必须在首次观察前被完整覆盖；透明环、
    // 预算耗尽、任一 clobber 或可观察出口都会让证明失败。
    std::unordered_set<BasicBlock *> visited;
    std::queue<BasicBlock *> worklist;
    visited.insert(B1);

    for (BasicBlock *succ : B1->getSuccessors()) {
        if (succ) worklist.push(succ);
    }

    int blockBudget = 100;

    while (!worklist.empty()) {
        BasicBlock *BB = worklist.front();
        worklist.pop();

        if (visited.count(BB)) {

            if (BB == B1) {
                return false;
            }
            continue;
        }
        visited.insert(BB);

        if (--blockBudget < 0) {
            return false;
        }

        DSEBlockColor color = classifyFrom(
            BB->front(), StoreLocation, AA, LI, FAM);

        if (color == DSEBlockColor::Kill) {
            continue;
        }

        if (color == DSEBlockColor::Clobber) {
            return false;
        }

        auto succs = BB->getSuccessors();
        if (succs.empty()) {

            if (isNonEscapingLocalStackPointer(P)) {
                continue;
            } else {
                return false;
            }
        }

        for (BasicBlock *succ : succs) {
            if (succ) worklist.push(succ);
        }
    }

    return true;
}

PreservationStatus DSEPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    // 1. 获取 AA、LoopInfo 与 MemorySSA，为覆盖判定、索引范围和到达定义提供依据。
    bool changed = false;

    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);

    // 2. 第一层用 MemorySSA 有界查询删除“写入值本身已冗余”的 store；timer
    // 不访问 SysY 程序内存，walker 会越过其调用节点。先收集后删除以保持图稳定。
    auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F);
    constexpr std::size_t MaxMemorySSAStoreQueries = 256;
    constexpr std::size_t MaxVisitedMemoryAccesses = 256;
    std::vector<StoreInst *> redundantValueStores;
    std::size_t queriedStores = 0;

    for (StoreInst *SI : collectInstructions<StoreInst>(F)) {
        if (queriedStores >= MaxMemorySSAStoreQueries) break;
        ++queriedStores;
        if (MSSA.isStoreValueRedundant(
                SI, AA, LI, FAM, MaxVisitedMemoryAccesses)) {
            redundantValueStores.push_back(SI);
        }
    }
    for (StoreInst *SI : redundantValueStores) {
        delete SI;
        changed = true;
    }

    // 3. 第二层逐块逆向消除被块内后继写入完整覆盖的 store。
    for (BasicBlock *BB : F) {
        changed |= eliminateInBlockWithAA(BB, AA, LI, FAM);
    }

    // 4. 第三层快照剩余 store，跨 CFG 证明所有路径在观察前均被覆盖，再统一删除。
    auto allStores = collectInstructions<StoreInst>(F);

    std::vector<StoreInst *> toDelete;
    for (StoreInst *SI : allStores) {
        if (isStoreDead(SI, AA, LI, FAM)) {
            toDelete.push_back(SI);
        }
    }

    for (StoreInst *SI : toDelete) {
        delete SI;
        changed = true;
    }

    if (!changed) {
        return PreservationStatus::all();
    }

    // 5. 删除 store 不改变 CFG；内存版本、别名和副作用结论必须重新计算。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<SideEffectAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<MemorySSAAnalysis>();
    return PA;
}
