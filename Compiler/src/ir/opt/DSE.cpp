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
//判断两条store是否写入完全相同宽度的类型。
bool DSEPass::storesHaveIdenticalCoverage(const StoreInst *First,
                                          const StoreInst *Second) {
    Value *FirstValue = First ? First->getValueOperand() : nullptr;
    Value *SecondValue = Second ? Second->getValueOperand() : nullptr;
    return FirstValue && SecondValue && FirstValue->getIRType() &&
           FirstValue->getIRType() == SecondValue->getIRType();
}
//判断store写入类型是否完整覆盖指针所指对象。
bool DSEPass::storeFullyCoversPointer(
    Value *Pointer, const StoreInst *Store) {
    std::shared_ptr<IRType> PointeeTy =
        Pointer ? Pointer->getPointeeType() : nullptr;
    Value *StoredValue = Store ? Store->getValueOperand() : nullptr;
    return PointeeTy && StoredValue && StoredValue->getIRType() &&
           PointeeTy == StoredValue->getIRType();
}
//沿安全地址变换闭包检查指针是否源自未逃逸的局部栈对象。
bool DSEPass::isNonEscapingLocalStackPointer(Value *Pointer) {
    //2.1 从 alloca 沿 GEP/bitcast use 闭包追踪；只允许作为 load/store 地址使用，
    //一旦被存入内存或交给其他用户就视为逃逸，函数出口也不能据此删除最后写入。
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
//判断store是否只是把同一基本块内先前读取的值无变化写回原地址。
bool DSEPass::isReloadStoreBackRedundant(
    StoreInst *Store, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    //2.2 候选必须把一条普通load的结果完整写回完全相同的SSA地址；这种限制
    //避免把数值相等误当成内存状态相等，也不需要猜测动态别名关系。
    if (!Store || !Store->fullyCoversItsPointer()) return false;
    auto *SourceLoad = dynamic_cast<LoadInst *>(Store->getValueOperand());
    if (!SourceLoad || SourceLoad->getParent() != Store->getParent() ||
        !SourceLoad->getPointerOperand() || !SourceLoad->getIRType() ||
        SourceLoad->getIRType() !=
            SourceLoad->getPointerOperand()->getPointeeType() ||
        SourceLoad->getPointerOperand() != Store->getPointerOperand()) {
        return false;
    }
    Value *TargetPointer = SourceLoad->getPointerOperand();
    Value *StoredValue = Store->getValueOperand();
    if (!TargetPointer || !StoredValue || StoredValue->isUndef()) return false;
    std::shared_ptr<IRType> StoredType = StoredValue->getIRType();
    if (!StoredType || StoredType->getSize() == 0) return false;
    MemoryLocation TargetLocation = MemoryLocation::get(SourceLoad);

    //2.3 load与回写之间允许任意读取、纯计算和不修改P的调用。中间store只有
    //在确定不别名，或按同类型自然对齐写入同一个V时才不会破坏P原有的V。
    bool ReachedStore = false;
    for (Instruction *I = SourceLoad->getNext(); I; I = I->getNext()) {
        if (I == Store) {
            ReachedStore = true;
            break;
        }
        if (auto *MiddleStore = dynamic_cast<StoreInst *>(I)) {
            MemoryLocation MiddleLocation = MemoryLocation::get(MiddleStore);
            AliasInfo Alias = AA.aliasWithIndexRange(
                TargetLocation, MiddleLocation, LI);
            if (Alias == AliasInfo::NoAlias) continue;
            if (MiddleStore->getValueOperand() != StoredValue ||
                !MiddleStore->fullyCoversItsPointer()) {
                return false;
            }
            if (Alias == AliasInfo::MustAlias) continue;

            //同型标量地址若都至少按访问宽度对齐，则两次完整访问只能完全重合
            //或完全分离，不会出现部分覆盖；写入同一个SSA值时两种情况都安全。
            Value *MiddlePointer = MiddleStore->getPointerOperand();
            Value *MiddleValue = MiddleStore->getValueOperand();
            if (!MiddlePointer || !MiddleValue ||
                MiddleValue->getIRType() != StoredType ||
                AA.getKnownAlignment(TargetPointer) < StoredType->getSize() ||
                AA.getKnownAlignment(MiddlePointer) < StoredType->getSize()) {
                return false;
            }
            continue;
        }
        ModRefInfo Effect = getInstModRefInfo(
            TargetLocation, I, AA, LI, FAM);
        if (isModSet(Effect)) return false;
    }
    return ReachedStore;
}
//结合AA与调用ModRef信息逆向删除块内被后续写完整覆盖的store。
bool DSEPass::eliminateInBlockWithAA(BasicBlock *BB, BasicAAResult &AA,
                                     const LoopInfo &LoopInfo,
                                     FunctionAnalysisPassManager &FAM) {
    //1.从块尾逆向维护“尚未被观察的后继 store”。load 会清除所有可能别名项；
    //当前 store 若被同覆盖范围的 MustAlias 后继 store 覆盖即可删除。调用只按
    //程序内存 ModRef 清除冲突项；不访问程序内存的 timer 对该状态透明。
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
            //当前 load 会观察所有可能别名地址，因此对应的后继 store 不能再用于
            //证明更早写入无效，需要从待定集合中清除。
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
//保留无别名信息块内扫描接口的兼容占位实现。
bool DSEPass::eliminateInBlock(BasicBlock *BB) {
    (void)BB;
    return false;
}
//分类从指定指令到块尾对目标位置的首次相关内存事件。
DSEBlockColor DSEPass::classifyFrom(
    Instruction *First, const MemoryLocation &Location,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    //2.2 顺序扫描一段路径：完整 MustAlias 覆盖记为 Kill；任何可能读/写目标的
    //指令记为 Clobber；扫描到底仍未观察目标则为 Transparent。
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
//证明store在所有可达路径上均于首次观察前被完整覆盖。
bool DSEPass::isStoreDead(
    StoreInst *S1,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    //2.3 先分类 store 所在块的尾部；同块已 Kill 可立即删除，已 Clobber 则立即保留。
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
    //2.4 若控制流直接退出，只有未逃逸栈对象的最终值不可观察，其他内存必须保留。
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
    //2.5 对所有后继做有界 BFS。每条路径都必须在首次观察前被完整覆盖；透明环、
    //预算耗尽、任一 clobber 或可观察出口都会让证明失败。
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
//分层运行MemorySSA覆盖、冗余值、块内逆扫和CFG路径无效写消除。
PreservationStatus DSEPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    //1.获取AA与LoopInfo，为局部语义证明、索引范围和后续内存图查询提供依据。
    bool changed = false;
    BasicAAResult *AA = &FAM.getResult<BasicAliasAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    //1.1 先删除把刚读取值无变化写回同一地址的store。该证明允许中间存在
    //同值别名写，因此能覆盖通用的“复制后再自写回”数据流。
    std::vector<StoreInst *> reloadStoreBacks;
    for (StoreInst *SI : collectInstructions<StoreInst>(F)) {
        if (isReloadStoreBackRedundant(SI, *AA, LI, FAM))
            reloadStoreBacks.push_back(SI);
    }
    for (StoreInst *SI : reloadStoreBacks) {
        delete SI;
        changed = true;
    }
    if (!reloadStoreBacks.empty()) {
        PreservationStatus RebuildPA;
        RebuildPA.keep<DominantAnalysis>();
        RebuildPA.keep<PostDominantAnalysis>();
        RebuildPA.keep<LoopAnalysis>();
        RebuildPA.discard<SideEffectAnalysis>();
        RebuildPA.discard<BasicAliasAnalysis>();
        RebuildPA.discard<MemorySSAAnalysis>();
        FAM.invalidate(F, std::move(RebuildPA));
        AA = &FAM.getResult<BasicAliasAnalysis>(F);
    }
    MemorySSAResult *MSSA = &FAM.getResult<MemorySSAAnalysis>(F);
    //2.第一层由MemorySSA统一内存语义并沿CFG证明候选写在所有路径首次读取前
    //均被完整覆盖。先收集后删除保持图稳定；不同值的分支覆盖也能删除汇合前旧写。
    constexpr std::size_t MaxMemorySSAOverwriteQueries = 256;
    constexpr std::size_t MaxVisitedForwardAccesses = 512;
    std::vector<StoreInst *> overwrittenStores;
    std::size_t overwriteQueries = 0;
    for (StoreInst *SI : collectInstructions<StoreInst>(F)) {
        if (overwriteQueries >= MaxMemorySSAOverwriteQueries) break;
        ++overwriteQueries;
        if (MSSA->isStoreOverwrittenOnAllPaths(
                SI, *AA, LI, FAM, MaxVisitedForwardAccesses)) {
            overwrittenStores.push_back(SI);
        }
    }
    for (StoreInst *SI : overwrittenStores) {
        delete SI;
        changed = true;
    }
    //2.1 删除MemoryDef后旧图不再可查询；在Pass内部精确失效并重建AA/MemorySSA，
    //避免同时删除“前写被后写覆盖”和“后写依赖前写同值”的相互证明。
    if (!overwrittenStores.empty()) {
        PreservationStatus RebuildPA;
        RebuildPA.keep<DominantAnalysis>();
        RebuildPA.keep<PostDominantAnalysis>();
        RebuildPA.keep<LoopAnalysis>();
        RebuildPA.discard<SideEffectAnalysis>();
        RebuildPA.discard<BasicAliasAnalysis>();
        RebuildPA.discard<MemorySSAAnalysis>();
        FAM.invalidate(F, std::move(RebuildPA));
        AA = &FAM.getResult<BasicAliasAnalysis>(F);
        MSSA = &FAM.getResult<MemorySSAAnalysis>(F);
    }
    //3.第二层反向删除“写入值本身已冗余”的store；timer不访问SysY程序内存，
    //walker会越过其调用节点。该阶段同样先收集后删除以保持查询快照稳定。
    constexpr std::size_t MaxMemorySSAStoreQueries = 256;
    constexpr std::size_t MaxVisitedMemoryAccesses = 256;
    std::vector<StoreInst *> redundantValueStores;
    std::size_t queriedStores = 0;
    for (StoreInst *SI : collectInstructions<StoreInst>(F)) {
        if (queriedStores >= MaxMemorySSAStoreQueries) break;
        ++queriedStores;
        if (MSSA->isStoreValueRedundant(
                SI, *AA, LI, FAM, MaxVisitedMemoryAccesses)) {
            redundantValueStores.push_back(SI);
        }
    }
    for (StoreInst *SI : redundantValueStores) {
        delete SI;
        changed = true;
    }
    //3.1 后备扫描仍要使用与当前IR一致的AA；重复值删除后只需重建AA，
    //后续不再消费MemorySSA。
    if (!redundantValueStores.empty()) {
        PreservationStatus RebuildPA;
        RebuildPA.keep<DominantAnalysis>();
        RebuildPA.keep<PostDominantAnalysis>();
        RebuildPA.keep<LoopAnalysis>();
        RebuildPA.discard<SideEffectAnalysis>();
        RebuildPA.discard<BasicAliasAnalysis>();
        RebuildPA.discard<MemorySSAAnalysis>();
        FAM.invalidate(F, std::move(RebuildPA));
        AA = &FAM.getResult<BasicAliasAnalysis>(F);
    }
    //4.第三层逐块逆向消除被块内后继写入完整覆盖的store，作为预算耗尽、
    //不可观察栈对象和尚未纳入正向图证明场景的保守后备。
    for (BasicBlock *BB : F) {
        changed |= eliminateInBlockWithAA(BB, *AA, LI, FAM);
    }
    //5.第四层快照剩余store，保留原有CFG遍历后备，再统一删除。
    auto allStores = collectInstructions<StoreInst>(F);
    std::vector<StoreInst *> toDelete;
    for (StoreInst *SI : allStores) {
        if (isStoreDead(SI, *AA, LI, FAM)) {
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
    //6.删除store不改变CFG；内存版本、别名和副作用结论必须重新计算。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<SideEffectAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<MemorySSAAnalysis>();
    return PA;
}
