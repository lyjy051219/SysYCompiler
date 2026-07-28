#include "ir/opt/LoadElimination.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "lib/IRUtils.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

 
LoadElimQueryResult LoadElimQueryResult::known(Value *V) {
    return {KNOWN, V, {}};
}

LoadElimQueryResult LoadElimQueryResult::clobbered() {
    return {CLOBBERED, nullptr, {}};
}

LoadElimQueryResult LoadElimQueryResult::unknown() {
    return {UNKNOWN, nullptr, {}};
}

LoadElimQueryResult LoadElimQueryResult::needPredecessors() {
    return {NEED_PREDECESSORS, nullptr, {}};
}

LoadElimQueryResult LoadElimQueryResult::phiNeeded(
    std::vector<std::pair<BasicBlock *, Value *>> M) {
    return {PHI_NEEDED, nullptr, std::move(M)};
}

 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 

 
 
 

bool LoadEliminationPass::isTypeCompatible(
    Value *source, LoadInst *targetLoad) {
    if (!source || !targetLoad) return false;
    if (!source->getIRType() || !targetLoad->getIRType()) return false;
    return source->getIRType()->hasSameShape(*targetLoad->getIRType());
}

bool LoadEliminationPass::mayModifyPointer(
    const MemoryLocation &Location, Instruction *I, BasicAAResult &AA,
    const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    // 1.1 把计时调用视为完整屏障，再以 ModRef 为主判据；对摘要声称 NoModRef
    // 的普通调用追加实参与目标地址的别名兜底，任何不确定写入都终止转发。
    Value *P = Location.Pointer;
    if (!P || !I) return true;

     
    if (auto *CI = dynamic_cast<CallInst *>(I);
        CI && CI->mayReachTimingCall()) {
        return true;
    }

    ModRefInfo MR = getInstModRefInfo(Location, I, AA, LI, FAM);
    if (isModSet(MR)) return true;

    if (auto *CI = dynamic_cast<CallInst *>(I)) {
        if (isNoModRef(MR) && !CI->getBuiltinCallee() &&
            AA.callMayAliasPointer(CI, P, LI)) {
            return true;
        }
    }
    return false;
}

 
 
 
 
 
 
 
 
 
 
bool LoadEliminationPass::isUnmodifiedOnAllPaths(
    LoadInst *Candidate, LoadInst *Target, Value *P,
    DominatorTree &DT, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    // 2.1 候选 load 必须支配目标。随后从目标反向遍历全部前驱路径，直到每条路径
    // 都命中候选；期间一旦出现可能修改该地址的指令，证明即失败。
    if (!Candidate || !Target || !P) return false;
    BasicBlock *CandidateBB = Candidate->getParent();
    BasicBlock *TargetBB = Target->getParent();
    if (!CandidateBB || !TargetBB || CandidateBB == TargetBB) return false;
    if (!DT.dominates(CandidateBB, TargetBB)) return false;

    std::vector<PathScanState> worklist{{TargetBB, true}};
    std::unordered_set<BasicBlock *> visited;
    int scannedBlocks = 0;
    int scannedInstructions = 0;

    while (!worklist.empty()) {
        PathScanState State = worklist.back();
        worklist.pop_back();
        BasicBlock *BB = State.BB;
        if (!BB) return false;

         
         
         
         
        // 目标块首次只扫描 load 之前的前缀；经回边再次到达时必须扫描整块，
        // 才不会漏掉上一轮迭代中位于目标 load 之后的写入。
        if (!State.TargetPrefixOnly && !visited.insert(BB).second) continue;
        if (++scannedBlocks > MaxPathScanBlocks) return false;

        Instruction *I = State.TargetPrefixOnly ? Target->getPrev() : BB->back();
        bool reachedCandidate = false;
        for (; I; I = I->getPrev()) {
            if (++scannedInstructions > MaxPathScanInstructions) return false;
            if (BB == CandidateBB && I == Candidate) {
                reachedCandidate = true;
                break;
            }
            if (mayModifyPointer(MemoryLocation::get(Target), I, AA, LI,
                                 FAM))
                return false;
        }

        if (BB == CandidateBB) {
             
             
            if (!reachedCandidate) return false;
            continue;
        }

        auto preds = BB->getPredecessors();
        if (preds.empty()) return false;
        for (BasicBlock *Pred : preds) {
            if (!Pred) return false;
            worklist.push_back({Pred, false});
        }
    }
    return true;
}

 
 
 
 
 
 
 
 
 
LoadInst *LoadEliminationPass::findAvailableDominatingLoad(
    LoadInst *Target, BasicBlock *BB, DominatorTree &DT,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    // 2.2 该兜底只服务循环：地址必须在目标循环外定义，再沿支配链寻找循环外、
    // MustAlias 且类型一致的 load，并交给全路径扫描验证跨迭代没有写入。
    Value *P = Target ? Target->getPointerOperand() : nullptr;
    if (!Target || !BB || !P) return nullptr;

     
     
     
     
    Loop *TargetLoop = LI.getLoopFor(BB);
    if (!TargetLoop) return nullptr;
    if (auto *PtrI = dynamic_cast<Instruction *>(P)) {
        BasicBlock *PtrBB = PtrI->getParent();
        if (!PtrBB || TargetLoop->contains(PtrBB)) return nullptr;
    }

    BasicBlock *Ancestor = DT.getIDom(BB);
    int depth = 0;
    for (; Ancestor && depth < MaxDominatorSearchDepth;
         Ancestor = DT.getIDom(Ancestor), ++depth) {
        if (TargetLoop->contains(Ancestor)) continue;
        if (!DT.dominates(Ancestor, TargetLoop->getHeader())) continue;

        for (Instruction *I = Ancestor->back(); I; I = I->getPrev()) {
            auto *Candidate = dynamic_cast<LoadInst *>(I);
            if (!Candidate || !isTypeCompatible(Candidate, Target)) continue;

            Value *CandidateP = Candidate->getPointerOperand();
            if (!CandidateP ||
                AA.aliasWithIndexRange(P, CandidateP, LI) !=
                    AliasInfo::MustAlias) {
                continue;
            }

            if (isUnmodifiedOnAllPaths(Candidate, Target, P, DT, AA, LI,
                                       FAM)) {
                return Candidate;
            }
        }
    }
    return nullptr;
}

 
 
 
 
 
 
LoadElimQueryResult LoadEliminationPass::scanBlockBackward(
    BasicBlock *BB, Value *P, Instruction *stopBefore, LoadInst *targetLoad,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    // 3.1 从指定位置向块首逆向寻找最近内存事件：MustAlias store/load 给出已知值，
    // MayAlias 写入或有写效应调用返回 Clobbered，纯读事件可以继续穿越。
    Instruction *cur = nullptr;
    if (stopBefore) {
        cur = stopBefore->getPrev();
    } else {
        cur = BB->back();
    }

    while (cur) {
         
        if (auto *SI = dynamic_cast<StoreInst *>(cur)) {
            Value *StorePtr = SI->getPointerOperand();
            Value *StoredVal = SI->getValueOperand();
            if (StorePtr && StoredVal) {
                AliasInfo ai = AA.aliasWithIndexRange(P, StorePtr, LI);
                if (ai == AliasInfo::MustAlias && isTypeCompatible(StoredVal, targetLoad)) {
                    return LoadElimQueryResult::known(StoredVal);
                }
                if (ai != AliasInfo::NoAlias) {
                    return LoadElimQueryResult::clobbered();   
                }
            }
        }
         
        else if (auto *LI2 = dynamic_cast<LoadInst *>(cur)) {
            if (LI2 != targetLoad) {
                Value *LoadPtr = LI2->getPointerOperand();
                if (AA.aliasWithIndexRange(P, LoadPtr, LI) == AliasInfo::MustAlias
                    && isTypeCompatible(LI2, targetLoad)) {
                    return LoadElimQueryResult::known(LI2);
                }
            }
        }
         
        else if (auto *CI = dynamic_cast<CallInst *>(cur)) {
            if (CI->mayReachTimingCall()) {
                return LoadElimQueryResult::clobbered();
            }
            ModRefInfo MR = getInstModRefInfo(
                MemoryLocation::get(targetLoad), cur, AA, LI, FAM);
            if (isModSet(MR)) {
                return LoadElimQueryResult::clobbered();
            }
             
             
            if (isNoModRef(MR) && !CI->getBuiltinCallee() &&
                AA.callMayAliasPointer(CI, P, LI)) {
                return LoadElimQueryResult::clobbered();
            }
        }
         
        else {
            ModRefInfo MR = getInstModRefInfo(
                MemoryLocation::get(targetLoad), cur, AA, LI, FAM);
            if (isModSet(MR)) {
                return LoadElimQueryResult::clobbered();
            }
        }

        cur = cur->getPrev();
    }

    return LoadElimQueryResult::needPredecessors();
}

 
 
 
 
 
 
LoadElimQueryResult LoadEliminationPass::queryAvailableValue(
    Value *P, BasicBlock *BB, LoadInst *targetLoad, int depth,
    std::unordered_set<BasicBlock*> &onStack,
    std::unordered_map<BasicBlock*, LoadElimQueryResult> &memo,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    // 3.2 递归求解块入口的可用值。深度预算、递归栈环或无法解释的前驱都保守失败；
    // memo 让钻石 CFG 的公共祖先只求解一次。
    if (depth > MaxQueryDepth) {
        return LoadElimQueryResult::unknown();
    }

     
    ScopedActiveMark ActiveMark(onStack, BB);
    if (!ActiveMark.inserted()) return LoadElimQueryResult::unknown();

     
    auto memoIt = memo.find(BB);
    if (memoIt != memo.end()) {
        return memoIt->second;
    }

     
    LoadElimQueryResult result = scanBlockBackward(BB, P, nullptr, targetLoad, AA, LI, FAM);

    if (result.kind == LoadElimQueryResult::KNOWN || result.kind == LoadElimQueryResult::CLOBBERED) {
        memo[BB] = result;
        return result;
    }

     
    // 3.3 汇总全部前驱：值相同则直接可用，值不同则返回需在当前汇合点建 PHI；
    // 任一路径被 clobber 或还需要嵌套 PHI 时，本层不继续猜测。
    auto preds = BB->getPredecessors();
    if (preds.empty()) {
         
        memo[BB] = LoadElimQueryResult::clobbered();
        return memo[BB];
    }

    std::vector<std::pair<BasicBlock*, Value*>> predValues;
    bool allSame = true;
    Value *firstVal = nullptr;

    for (BasicBlock *pred : preds) {
        LoadElimQueryResult predResult = queryAvailableValue(
            P, pred, targetLoad, depth + 1, onStack, memo, AA, LI, FAM);

        if (predResult.kind == LoadElimQueryResult::CLOBBERED ||
            predResult.kind == LoadElimQueryResult::UNKNOWN) {
            memo[BB] = LoadElimQueryResult::clobbered();
            return memo[BB];
        }

        if (predResult.kind == LoadElimQueryResult::KNOWN) {
            predValues.push_back({pred, predResult.value});
            if (firstVal == nullptr) {
                firstVal = predResult.value;
            } else if (predResult.value != firstVal) {
                allSame = false;
            }
        } else if (predResult.kind == LoadElimQueryResult::PHI_NEEDED) {
             
            memo[BB] = LoadElimQueryResult::clobbered();
            return memo[BB];
        }
    }

    if ((int)predValues.size() != (int)preds.size()) {
        memo[BB] = LoadElimQueryResult::clobbered();
        return memo[BB];
    }

    LoadElimQueryResult finalResult;
    if (allSame) {
        finalResult = LoadElimQueryResult::known(firstVal);
    } else {
        finalResult = LoadElimQueryResult::phiNeeded(std::move(predValues));
    }

    memo[BB] = finalResult;
    return finalResult;
}

 
 
 
bool LoadEliminationPass::processLoad(
    LoadInst *loadInst, BasicBlock *BB,
    DominatorTree &DT, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM, int &DominatingQueryBudget)
{
    // 4. 单条 load 先查询块内最近定义，这是成本最低且无需修改 SSA 的路径。
    Value *P = loadInst ? loadInst->getPointerOperand() : nullptr;
    if (!P) return false;

     
    LoadElimQueryResult result = scanBlockBackward(BB, P, loadInst, loadInst, AA, LI, FAM);

    if (result.kind == LoadElimQueryResult::KNOWN) {
        loadInst->replaceAllUsesWith(result.value);
        delete loadInst;
        return true;
    }

    if (result.kind != LoadElimQueryResult::NEED_PREDECESSORS) {
        return false;    
    }

     
     
     
    // 4.1 块内无定义时，优先尝试有总预算限制的跨回边支配 load 复用。
    if (DominatingQueryBudget > 0) {
        --DominatingQueryBudget;
        if (LoadInst *available = findAvailableDominatingLoad(
                loadInst, BB, DT, AA, LI, FAM)) {
            loadInst->replaceAllUsesWith(available);
            delete loadInst;
            return true;
        }
    }

     
    // 4.2 常规前驱求解要求每条入边均得到已知值；全相同直接替换，否则在块首
    // 合成 PHI，再用该 PHI 替换 load。
    auto preds = BB->getPredecessors();
    if (preds.empty()) return false;

    std::unordered_set<BasicBlock*> onStack;
    std::unordered_map<BasicBlock*, LoadElimQueryResult> memo;

    std::vector<std::pair<BasicBlock*, Value*>> predValues;
    bool allSame = true;
    Value *firstVal = nullptr;

    for (BasicBlock *pred : preds) {
        LoadElimQueryResult predResult = queryAvailableValue(
            P, pred, loadInst, 1, onStack, memo, AA, LI, FAM);

        if (predResult.kind != LoadElimQueryResult::KNOWN) {
            return false;    
        }

        predValues.push_back({pred, predResult.value});
        if (firstVal == nullptr) {
            firstVal = predResult.value;
        } else if (predResult.value != firstVal) {
            allSame = false;
        }
    }

    if ((int)predValues.size() != (int)preds.size()) {
        return false;
    }

    if (allSame) {
        loadInst->replaceAllUsesWith(firstVal);
        delete loadInst;
        return true;
    }

     
    PhiInst *phi = new PhiInst(loadInst->getIRType());
    for (auto &[pred, val] : predValues) {
        phi->addIncoming(val, pred);
    }
    BB->pushFront(phi);
    loadInst->replaceAllUsesWith(phi);
    delete loadInst;
    return true;
}

 
 
 
bool LoadEliminationPass::processFunction(
    Function &F, DominatorTree &DT,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    // 5. 按支配树前序处理基本块，使支配侧先完成替换；每块先快照 load，避免删除
    // 当前指令破坏遍历，并共享函数级跨回边查询预算。
    bool changed = false;
    int DominatingQueryBudget = MaxDominatingQueriesPerFunction;

    std::vector<DominatorNode *> Roots;
    if (DT.Root) Roots.push_back(DT.Root);
    auto DomOrder =
        buildPreOrderFromRootsCustomSucc<DominatorNode>(
            Roots, [&DT](DominatorNode *Node) {
                if (!Node || !Node->Block)
                    return std::vector<DominatorNode *>{};
                return DT.getChildNodes(Node);
            });

    for (DominatorNode *node : DomOrder) {
        if (!node) continue;
        BasicBlock *BB = node->Block;
        if (!BB) continue;

         
        auto loads = collectInstructions<LoadInst>(*BB);

        for (LoadInst *LI2 : loads) {
             
            if (LI2->getParent() != BB) continue;
            changed |= processLoad(LI2, BB, DT, AA, LI, FAM,
                                   DominatingQueryBudget);
        }

    }

    return changed;
}

 
 
 
PreservationStatus LoadEliminationPass::run(Function &F,
                                            FunctionAnalysisPassManager &FAM) {
    // 1. 获取别名、循环、支配和 MemorySSA，分别服务内存冲突、回边与定义链证明。
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);

     
     
     
     
     
    // 2. 快速层：在查询预算内让 MemorySSA 找每个 load 的最近 clobber；只有该定义
    // 是支配目标且无计时屏障的 MustAlias store，才计划用其存储值转发。
    auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F);
    constexpr std::size_t MaxMSSAQueries = 512;
    constexpr std::size_t MaxMSSAWalk = 256;
    std::vector<std::pair<LoadInst *, Value *>> MSSAReplacements;
    std::size_t QueryCount = 0;
    for (LoadInst *Load : collectInstructions<LoadInst>(F)) {
            if (QueryCount++ >= MaxMSSAQueries) break;
            BasicBlock *BB = Load->getParent();
            if (!BB) continue;

            MemoryAccess *Clobber = MSSA.getClobberingAccess(
                Load, MemoryLocation::get(Load), AA, LI, FAM,
                MaxMSSAWalk);
            auto *Store = Clobber && Clobber->isDef()
                              ? dynamic_cast<StoreInst *>(
                                    Clobber->getInstruction())
                              : nullptr;
            if (!Store || !Store->getParent()) continue;
            if (!DT.dominates(Store->getParent(), BB)) continue;
            if (Store->getParent() == BB &&
                !Instruction::comesBefore(Store, Load))
                continue;
            if (F.hasTimingBarrierOnPath(Store, Load)) continue;
            if (AA.aliasWithIndexRange(MemoryLocation::get(Load),
                                       MemoryLocation::get(Store), LI) !=
                AliasInfo::MustAlias)
                continue;
            Value *Stored = Store->getValueOperand();
            if (!isTypeCompatible(Stored, Load)) continue;
            MSSAReplacements.push_back({Load, Stored});
    }

     
     
     
     
     
     
    // 2.1 先解析计划之间的传递依赖并检测环，再批量替换 use、批量删除 load，
    // 避免先删一个 load 后让另一条计划持有悬空替换值。
    std::unordered_map<LoadInst *, Value *> Planned;
    for (const auto &Replacement : MSSAReplacements)
        Planned.emplace(Replacement.first, Replacement.second);

    std::function<Value *(Value *, std::unordered_set<LoadInst *> &)>
        ResolveReplacement = [&](Value *V,
                                 std::unordered_set<LoadInst *> &Active)
            -> Value * {
        auto *AsLoad = dynamic_cast<LoadInst *>(V);
        auto It = AsLoad ? Planned.find(AsLoad) : Planned.end();
        if (It == Planned.end()) return V;
        ScopedActiveMark ActiveMark(Active, AsLoad);
        if (!ActiveMark.inserted()) return nullptr;
        return ResolveReplacement(It->second, Active);
    };

    std::vector<std::pair<LoadInst *, Value *>> ResolvedReplacements;
    ResolvedReplacements.reserve(MSSAReplacements.size());
    for (const auto &Replacement : MSSAReplacements) {
        LoadInst *Load = Replacement.first;
        if (!Load || !Load->getParent()) continue;
        std::unordered_set<LoadInst *> Active;
        Value *Resolved = ResolveReplacement(Replacement.second, Active);
        if (!Resolved || Resolved == Load ||
            !isTypeCompatible(Resolved, Load))
            continue;
        ResolvedReplacements.push_back({Load, Resolved});
    }

    bool changed = false;
    for (const auto &Replacement : ResolvedReplacements)
        Replacement.first->replaceAllUsesWith(Replacement.second);
    for (const auto &Replacement : ResolvedReplacements) {
        LoadInst *Load = Replacement.first;
        if (!Load || !Load->getParent()) continue;
        delete Load;
        changed = true;
    }

    // 3—5. 对快速层未覆盖的 load 运行块内、前驱数据流与跨回边支配证明。
    changed |= processFunction(F, DT, AA, LI, FAM);

    if (!changed) {
        return PreservationStatus::all();
    }

    // 6. CFG 未变，结构分析可保留；load 消失改变别名缓存、MemorySSA 和副作用摘要。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<MemorySSAAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
