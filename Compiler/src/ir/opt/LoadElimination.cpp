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

// 查询结果的构造统一放在实现文件，头文件只暴露语义明确的工厂接口。
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

// =====================================================================
// LoadElimination++ — Available Memory Value Forwarding
//
// 对每个 load P，回答：所有到达此 load 的路径上，
// P 的最近相关内存事件各给出什么值？
//
// Known(V):       所有路径值相同 → load 替换为 V
// PhiNeeded:      各前驱已知但值不同 → 在当前块头插 PHI
// Clobbered:      某路径上出现无法证明的事件 → 保守不动
// Unknown:        预算耗尽 / 环依赖 → 保守不动
// NeedPredecessors: 块内没找到定义，要继续查前驱（仅内部使用）
//
// 和 DSE 的关键区别：纯 Ref（只读）不阻断 load forwarding。
// 只有 Mod（写操作）才破坏已知值。
//
// 比 cmmc 强在：
// - 递归回溯 predecessor（最多 8 层），不只看一层 last-value
// - AA MustAlias + 类型检查，不只做精确指针匹配
// - if-else PHI synthesis（分支两边不同值 → 自动造 PHI）
// - onStack + memo 正确处理钻石图和循环
// - 支配 load 跨透明回边复用（完整路径无 Mod 证明）
// =====================================================================

// =====================================================================
// 辅助函数
// =====================================================================

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
    Value *P = Location.Pointer;
    if (!P || !I) return true;

    // 计时调用及其包装调用是完整优化屏障，不能把内存值跨过它复用。
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

// Check the complete CFG region between Candidate and Target.  Block
// dominance alone is insufficient: a backedge may execute a store after the
// dominating load and before a later dynamic execution of Target.  Walking
// backwards over every predecessor (including loop backedges) and rejecting
// any possible Mod proves that Candidate's value is still available.
//
// Reaching Candidate cuts a path: an execution through Candidate refreshes
// the SSA value, so instructions before it are irrelevant.  The visited set
// makes transparent cycles converge.  DT dominance guarantees that no finite
// entry-to-Target path can bypass Candidate.
bool LoadEliminationPass::isUnmodifiedOnAllPaths(
    LoadInst *Candidate, LoadInst *Target, Value *P,
    DominatorTree &DT, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
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

        // The initial Target state scans only instructions before the load.
        // If a loop backedge reaches TargetBB again, it must be scanned as a
        // complete block so that a store *after* Target in the previous
        // iteration is not missed.
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
            // Candidate must physically belong to this block and precede all
            // instructions that were scanned above.
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

// The recursive predecessor solver intentionally treats a CFG cycle as
// Unknown.  That is correct for arbitrary reaching definitions, but misses a
// common nested-loop case:
//
//   invariant = load A[i][k]       // dominates the inner loop
//   while (j < n) use load A[i][k]
//
// Search the dominator chain for such a load and separately prove that the
// entire intervening CFG region contains no possible Mod of the address.
LoadInst *LoadEliminationPass::findAvailableDominatingLoad(
    LoadInst *Target, BasicBlock *BB, DominatorTree &DT,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    Value *P = Target ? Target->getPointerOperand() : nullptr;
    if (!Target || !BB || !P) return nullptr;

    // This fallback exists specifically for a predecessor query stopped by a
    // loop backedge.  Outside loops the normal dataflow is cheaper.  Reusing a
    // value across iterations also requires the address itself to be defined
    // outside the innermost loop.
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

// =====================================================================
// scanBlockBackward: 从块内某位置向前扫描，寻找 P 的最近可用值。
//
// stopBefore: 扫描到此指令前停止。nullptr = 从块末尾开始扫全块。
// targetLoad: 正在查询的 load 指令，用于类型检查和避免自引用。
// =====================================================================
LoadElimQueryResult LoadEliminationPass::scanBlockBackward(
    BasicBlock *BB, Value *P, Instruction *stopBefore, LoadInst *targetLoad,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    // 确定起始指令
    Instruction *cur = nullptr;
    if (stopBefore) {
        cur = stopBefore->getPrev();
    } else {
        cur = BB->back();
    }

    while (cur) {
        // store → load forwarding
        if (auto *SI = dynamic_cast<StoreInst *>(cur)) {
            Value *StorePtr = SI->getPointerOperand();
            Value *StoredVal = SI->getValueOperand();
            if (StorePtr && StoredVal) {
                AliasInfo ai = AA.aliasWithIndexRange(P, StorePtr, LI);
                if (ai == AliasInfo::MustAlias && isTypeCompatible(StoredVal, targetLoad)) {
                    return LoadElimQueryResult::known(StoredVal);
                }
                if (ai != AliasInfo::NoAlias) {
                    return LoadElimQueryResult::clobbered();  // MayAlias store → 值可能被改
                }
            }
        }
        // load → load 复用
        else if (auto *LI2 = dynamic_cast<LoadInst *>(cur)) {
            if (LI2 != targetLoad) {
                Value *LoadPtr = LI2->getPointerOperand();
                if (AA.aliasWithIndexRange(P, LoadPtr, LI) == AliasInfo::MustAlias
                    && isTypeCompatible(LI2, targetLoad)) {
                    return LoadElimQueryResult::known(LI2);
                }
            }
        }
        // CallInst: 先查 ModRef（主判据），helper 做 fallback
        else if (auto *CI = dynamic_cast<CallInst *>(cur)) {
            if (CI->mayReachTimingCall()) {
                return LoadElimQueryResult::clobbered();
            }
            ModRefInfo MR = getInstModRefInfo(
                MemoryLocation::get(targetLoad), cur, AA, LI, FAM);
            if (isModSet(MR)) {
                return LoadElimQueryResult::clobbered();
            }
            // 纯 Ref → 不阻断 load forwarding，继续
            // 安全网：ModRef 说 NoModRef 但传了 aliasing pointer → Clobbered
            if (isNoModRef(MR) && !CI->getBuiltinCallee() &&
                AA.callMayAliasPointer(CI, P, LI)) {
                return LoadElimQueryResult::clobbered();
            }
        }
        // 其他指令
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

// =====================================================================
// queryAvailableValue: 递归查询块末尾 P 的可用值。
//
// onStack: 当前递归栈上的块（检测真实 CFG 环）
// memo:    缓存块末尾查询结果（钻石图共享祖先只算一次）
// =====================================================================
LoadElimQueryResult LoadEliminationPass::queryAvailableValue(
    Value *P, BasicBlock *BB, LoadInst *targetLoad, int depth,
    std::unordered_set<BasicBlock*> &onStack,
    std::unordered_map<BasicBlock*, LoadElimQueryResult> &memo,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    if (depth > MaxQueryDepth) {
        return LoadElimQueryResult::unknown();
    }

    // 环检测：当前递归栈上的块
    ScopedActiveMark ActiveMark(onStack, BB);
    if (!ActiveMark.inserted()) return LoadElimQueryResult::unknown();

    // 缓存命中
    auto memoIt = memo.find(BB);
    if (memoIt != memo.end()) {
        return memoIt->second;
    }

    // 块内扫描（扫全块，从末尾开始）
    LoadElimQueryResult result = scanBlockBackward(BB, P, nullptr, targetLoad, AA, LI, FAM);

    if (result.kind == LoadElimQueryResult::KNOWN || result.kind == LoadElimQueryResult::CLOBBERED) {
        memo[BB] = result;
        return result;
    }

    // result.kind == NEED_PREDECESSORS
    auto preds = BB->getPredecessors();
    if (preds.empty()) {
        // 函数入口，P 的初始值未知
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
            // 前驱本身需要 PHI → 第一版不处理嵌套 PHI
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

// =====================================================================
// processLoad: 尝试消除一个 load 指令。
// =====================================================================
bool LoadEliminationPass::processLoad(
    LoadInst *loadInst, BasicBlock *BB,
    DominatorTree &DT, BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM, int &DominatingQueryBudget)
{
    Value *P = loadInst ? loadInst->getPointerOperand() : nullptr;
    if (!P) return false;

    // Step 1: 块内扫描（从 load 位置向前）
    LoadElimQueryResult result = scanBlockBackward(BB, P, loadInst, loadInst, AA, LI, FAM);

    if (result.kind == LoadElimQueryResult::KNOWN) {
        loadInst->replaceAllUsesWith(result.value);
        delete loadInst;
        return true;
    }

    if (result.kind != LoadElimQueryResult::NEED_PREDECESSORS) {
        return false;   // CLOBBERED 或其他
    }

    // The predecessor query below cannot solve transparent CFG cycles.  A
    // dominating MustAlias load is nevertheless reusable when every path
    // from it to this load is proven free of possible modifications.
    if (DominatingQueryBudget > 0) {
        --DominatingQueryBudget;
        if (LoadInst *available = findAvailableDominatingLoad(
                loadInst, BB, DT, AA, LI, FAM)) {
            loadInst->replaceAllUsesWith(available);
            delete loadInst;
            return true;
        }
    }

    // Step 2: 查前驱
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
            return false;   // 任何非 KNOWN → 放弃
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

    // Step 3: PHI synthesis
    PhiInst *phi = new PhiInst(loadInst->getIRType());
    for (auto &[pred, val] : predValues) {
        phi->addIncoming(val, pred);
    }
    BB->pushFront(phi);
    loadInst->replaceAllUsesWith(phi);
    delete loadInst;
    return true;
}

// =====================================================================
// processFunction: 支配树前序遍历处理所有 load。
// =====================================================================
bool LoadEliminationPass::processFunction(
    Function &F, DominatorTree &DT,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
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

        // 收集本块所有 load（先收集再处理，避免遍历时删指令）
        auto loads = collectInstructions<LoadInst>(*BB);

        for (LoadInst *LI2 : loads) {
            // 检查 load 是否还活着（可能被前一轮 processLoad 间接影响）
            if (LI2->getParent() != BB) continue;
            changed |= processLoad(LI2, BB, DT, AA, LI, FAM,
                                   DominatingQueryBudget);
        }

    }

    return changed;
}

// =====================================================================
// LoadEliminationPass::run
// =====================================================================
PreservationStatus LoadEliminationPass::run(Function &F,
                                            FunctionAnalysisPassManager &FAM) {
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);

    // MemorySSA can answer the common cross-block case without recursively
    // rebuilding a reaching-value lattice for each load.  Only forward from
    // one concrete, dominating, full-width store.  A Phi/LiveOnEntry result,
    // a MayAlias address, or a walker budget miss is deliberately ignored and
    // falls through to the existing path solver below.
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

    // A forwarded store value can itself be an earlier load that is also in
    // the replacement set.  Resolve that relation before mutating anything;
    // otherwise deleting the earlier load first can leave a later
    // replacement pointing at freed IR.  Cycles are not expected in valid
    // dominance order, but are rejected conservatively rather than relying on
    // that assumption.
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

    changed |= processFunction(F, DT, AA, LI, FAM);

    if (!changed) {
        return PreservationStatus::all();
    }

    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<MemorySSAAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
