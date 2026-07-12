#include "ir/opt/DSE.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <queue>

// =====================================================================
// Dead Store Elimination — Forward Kill Coverage 模型
//
// 对每个 store S1 to pointer P，从 S1 出发做前向 BFS：
//   KILL:        块内第一个 P 相关事件是 MustAlias 完整覆盖 store → 路径被挡住
//   CLOBBER:     块内第一个 P 相关事件无法继续证明死亡 → S1 alive
//   TRANSPARENT: 块内无 P 相关事件 → 继续探索后继
//
// S1 is dead ⟺ BFS 走完所有可达路径，全部被 KILL 挡住，
//              没有任何一条路径落到 CLOBBER 或非局部 exit。
//
// 分支联合覆盖：if-else 两边都写 %p，BFS 两条路径都被 KILL 挡住，
//              自然判定 S1 为 dead。不需要单个 store 后支配 S1。
// =====================================================================

// =====================================================================
// Phase 1: 块内 DSE
// =====================================================================

namespace {

bool storesHaveIdenticalCoverage(const StoreInst *First,
                                 const StoreInst *Second) {
    Value *FirstValue = First ? First->getValueOperand() : nullptr;
    Value *SecondValue = Second ? Second->getValueOperand() : nullptr;
    return FirstValue && SecondValue && FirstValue->getIRType() &&
           FirstValue->getIRType() == SecondValue->getIRType();
}

bool storeFullyCoversPointer(Value *Pointer, const StoreInst *Store) {
    std::shared_ptr<IRType> PointeeTy =
        Pointer ? Pointer->getPointeeType() : nullptr;
    Value *StoredValue = Store ? Store->getValueOperand() : nullptr;
    return PointeeTy && StoredValue && StoredValue->getIRType() &&
           PointeeTy == StoredValue->getIRType();
}

// A stack allocation is private at function exit only while none of its
// derived pointers escapes.  Merely tracing P back to an alloca is not enough:
// the address may have been stored elsewhere or retained by an earlier call.
bool isNonEscapingLocalStackPointer(Value *Pointer) {
    auto *Root = dynamic_cast<AllocaInst *>(
        Value::stripPointerCastsAndGEP(Pointer));
    if (!Root) return false;

    std::unordered_set<Value *> Visited;
    std::vector<Value *> Worklist{Root};
    while (!Worklist.empty()) {
        Value *Current = Worklist.back();
        Worklist.pop_back();
        if (!Current || !Visited.insert(Current).second) continue;

        for (auto It = Current->getUseList().begin();
             It != Current->getUseList().end(); ++It) {
            Use *U = *It;
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
                // Using Current as the destination is an ordinary access.
                // Storing Current as the value publishes the address.
                if (SI->getPointerOperand() != Current ||
                    SI->getValueOperand() == Current)
                    return false;
                continue;
            }

            // Calls, returns, PHIs/selects and every unmodelled pointer use are
            // conservatively considered captures.
            return false;
        }
    }
    return true;
}

// 块内逆向扫描时，PendingStores 表示“位于当前位置之后，可能
// 覆盖更早 store”的候选。候选只能跨过确定 NoAlias 的内存事件：
//
//   store 1, p
//   x = load q       ; q MayAlias p
//   store 2, p
//
// 如果 load q 没有清除 p 的候选，第一个 store 会被错删。同理，
// 一个仅 MayAlias 的 store 不能当作完整覆盖证明；这里也将它作为
// 保守的候选边界，避免未来引入不同访存宽度后误判。
bool eliminateInBlockWithAA(BasicBlock *BB, BasicAAResult &AA,
                            const LoopInfo &LoopInfo) {
    if (BB == nullptr) {
        return false;
    }

    std::vector<Instruction *> instructions;
    for (Instruction *I : *BB) {
        instructions.push_back(I);
    }

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

            for (auto PendingIt = pendingStores.begin();
                 PendingIt != pendingStores.end();) {
                Value *StorePtr = PendingIt->first;
                AliasInfo Alias = LoadPtr == StorePtr
                                      ? AliasInfo::MustAlias
                                      : AA.alias(LoadPtr, StorePtr, LoopInfo);
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
                    if (AA.alias(ptr, Pending.first, LoopInfo) ==
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

            // MayAlias store 不足以证明覆盖了 Pending store 的完整
            // 对象。把这些候选停在此处，只保留 NoAlias 候选跨过它。
            for (auto PendingIt = pendingStores.begin();
                 PendingIt != pendingStores.end();) {
                AliasInfo Alias = AA.alias(ptr, PendingIt->first, LoopInfo);
                if (Alias != AliasInfo::NoAlias) {
                    PendingIt = pendingStores.erase(PendingIt);
                } else {
                    ++PendingIt;
                }
            }
            pendingStores[ptr] = SI;
            continue;
        }

        if (dynamic_cast<CallInst *>(I) != nullptr) {
            pendingStores.clear();
        }
    }

    return changed;
}

} // namespace

// 保留类内原有 helper 的 ABI；真正的块内 DSE 需要 AA/LoopInfo，
// run() 会调用上面的 eliminateInBlockWithAA。没有别名信息时不做
// 一个可能不安全的快速删除。
bool DSEPass::eliminateInBlock(BasicBlock *BB) {
    (void)BB;
    return false;
}

// =====================================================================
// Phase 2: 全局 DSE — Forward Kill Coverage
// =====================================================================

// 从 First 开始扫描，确定对指针 P 的第一个相关事件。
//
// MustAlias 只能证明起始地址一致，不能单独证明覆盖宽度。
// KILL 还要求 store value 与 P 的 pointee 是同一 IRType，防止
// bitcast/不同宽度访存把部分覆盖误当成完整覆盖。
DSEBlockColor DSEPass::classifyFrom(
    Instruction *First, Value *P,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    for (Instruction *I = First; I; I = I->getNext()) {
        // 先检查 KILL：MustAlias store
        if (auto *SI = dynamic_cast<StoreInst *>(I)) {
            Value *StorePtr = SI->getPointerOperand();
            if (StorePtr &&
                AA.alias(P, StorePtr, LI) == AliasInfo::MustAlias &&
                storeFullyCoversPointer(P, SI)) {
                return DSEBlockColor::Kill;
            }
        }

        // 对 CallInst 做额外安全检查：
        // 如果 call 传递了任何可能 alias P 的指针参数 → CLOBBER
        // 这是对 getInstModRefInfo 的补充，因为 SideEffectAnalysis
        // 对自递归调用可能返回不完整的 ModRef 信息。
        if (auto *CI = dynamic_cast<CallInst *>(I)) {
            if (AA.callMayAliasPointer(CI, P, LI)) {
                return DSEBlockColor::Clobber;
            }
        }

        // 再检查 CLOBBER
        // CLOBBER 合并了两种失败：
        //   READ:    I 可能读 P（isRefSet）
        //   BARRIER: I 可能改 P 但无法证明完整覆盖（isModSet 且非 MustAlias store）
        // 后续若需区分，在此拆分。
        ModRefInfo MR = getInstModRefInfo(P, I, AA, LI, FAM);

        if (isRefSet(MR) || isModSet(MR)) return DSEBlockColor::Clobber;
    }

    return DSEBlockColor::Transparent;
}

// isStoreDead: 判断一个 store 是否为 dead store。
// 不使用全局早退 isReachableToExit —— 同块覆盖在任何块中都可以检测到。
// 只在 BFS 走到 exit 块时，根据指针是否 local 决定是否放行。
bool DSEPass::isStoreDead(
    StoreInst *S1,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    if (!S1) return false;
    Value *P = S1->getPointerOperand();
    BasicBlock *B1 = S1->getParent();
    if (!P) return false;
    if (!B1) return false;

    // Phase A: 检查 S1 所在块的剩余指令
    DSEBlockColor tailColor = classifyFrom(S1->getNext(), P, AA, LI, FAM);
    if (tailColor == DSEBlockColor::Kill) return true;   // 同块覆盖
    if (tailColor == DSEBlockColor::Clobber) return false;  // 同块就被读了

    // Phase A.5: B1 本身是 exit 块的情况
    // Phase A 返回 TRANSPARENT 说明 S1 之后到块末尾无 P 相关事件。
    // 如果 B1 没有后继（即 exit 块），则函数即将返回。
    // 非局部指针在返回后 caller 可能读 → S1 alive。
    {
        auto b1Succs = B1->getSuccessors();
        if (b1Succs.empty()) {
            if (isNonEscapingLocalStackPointer(P)) {
                return true;   // 局部变量，返回后无人读，store 可删
            } else {
                return false;  // 非局部指针，caller 可能读 → S1 alive
            }
        }
    }

    // Phase B: 前向 BFS
    // visited 的作用是图遍历终止，保证有环图（循环）中 BFS 不死循环。
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
            // 如果 BFS 遇到了 B1（存放被分析 store 的块），说明存在循环回边。
            // 循环回边意味着：store 的值在下一次迭代前可能被读到，
            // 而且下一次迭代的 store 写的可能是不同地址（动态索引）。
            // 保守处理：视为不可证明，S1 alive。
            if (BB == B1) {
                return false;
            }
            continue;
        }
        visited.insert(BB);

        if (--blockBudget < 0) {
            return false;  // 超出预算，保守放弃
        }

        DSEBlockColor color = classifyFrom(BB->front(), P, AA, LI, FAM);

        if (color == DSEBlockColor::Kill) {
            continue;  // 这条路径被覆盖 store 挡住，不再探索后继
        }

        if (color == DSEBlockColor::Clobber) {
            return false;  // 存在一条路径上 P 被读或被不可证明地修改 → S1 alive
        }

        // TRANSPARENT: 块内无 P 相关事件
        auto succs = BB->getSuccessors();
        if (succs.empty()) {
            // exit 块：函数即将返回。
            // 只有 P 是栈内存（返回后对 caller 不可见）时才可忽略此 exit。
            // 非局部指针（global/param）在函数返回后 caller 可能读 → S1 alive。
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

    // BFS 完成，所有路径都被 KILL 挡住或走到了安全 exit
    return true;
}

// =====================================================================
// DSEPass::run
// =====================================================================

PreservationStatus DSEPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    bool changed = false;

    // 取分析结果
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);

    // === Phase 1: 块内 DSE ===
    // 此阶段会直接 delete 死 store。所有跨内存事件的候选都必须
    // 经过 AA 证明 NoAlias，不再只按 Value* 相等性清障。
    for (BasicBlock *BB : F) {
        changed |= eliminateInBlockWithAA(BB, AA, LI);
    }

    // === Phase 2: 全局 DSE — Forward Kill Coverage ===
    // Phase 1 之后再收集存活 store，避免对已删指令的悬空指针
    std::vector<StoreInst *> allStores;
    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            if (auto *SI = dynamic_cast<StoreInst *>(I)) {
                allStores.push_back(SI);
            }
        }
    }

    // 先收集，再统一删除
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

    // DSE 只删 store，不改 CFG，不改指针关系
    // SideEffect 的 ModSet 可能变化（少了 store）
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();  // [DSE] 不改 CFG，后支配树仍有效
    PA.keep<LoopAnalysis>();
    PA.discard<SideEffectAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    return PA;
}
