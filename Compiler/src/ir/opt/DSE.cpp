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

namespace {

// =====================================================================
// 块染色结果
// =====================================================================
enum class BlockColor {
    KILL,        // 块内存在 MustAlias 完整覆盖 store
    CLOBBER,     // 块内存在无法继续证明的事件 (READ 或 BARRIER)
    TRANSPARENT  // 块内无 P 相关事件
};

// =====================================================================
// Phase 1: 块内 DSE（保留现有逻辑）
// =====================================================================

bool eliminateInBlock(BasicBlock *BB) {
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
            if (LI->getUserList().size() >= 1) {
                Value *ptr = LI->getUserList()[0]->getValue();
                pendingStores.erase(ptr);
            } else {
                pendingStores.clear();
            }
            continue;
        }

        if (auto *SI = dynamic_cast<StoreInst *>(I)) {
            if (SI->getUserList().size() < 2) {
                pendingStores.clear();
                continue;
            }

            Value *ptr = SI->getUserList()[1]->getValue();
            auto pendingIt = pendingStores.find(ptr);
            if (pendingIt != pendingStores.end()) {
                delete SI;
                changed = true;
                continue;
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

// =====================================================================
// Phase 2: 全局 DSE — Forward Kill Coverage
// =====================================================================

// isLocalPointer: P 指向的内存在函数返回后对 caller 不可见。
// 只追溯到 AllocaInst 返回 true。
// 不是通用 escape analysis，不用于证明"call 不会读到"。
// 仅用于 exit 可见性判断。
static bool isLocalPointer(Value *P) {
    Value *base = P;
    while (base) {
        if (dynamic_cast<AllocaInst *>(base)) {
            return true;
        }
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(base)) {
            if (GEP->getUserList().empty()) return false;
            base = GEP->getUserList()[0]->getValue();
            continue;
        }
        if (auto *BC = dynamic_cast<BitCastInst *>(base)) {
            if (BC->getUserList().empty()) return false;
            base = BC->getUserList()[0]->getValue();
            continue;
        }
        // Global, Param, or anything else → not local
        return false;
    }
    return false;
}

// callMayAliasPointer: 保守检查 — 如果 CallInst 传递了任何可能 alias P 的指针参数，
// 返回 true。这是对 getInstModRefInfo 的补充安全网。
// 原因：SideEffectAnalysis 对自递归调用会跳过 (SideEffect.cpp:277)，
// 导致 getInstModRefInfo 可能对递归调用返回 NoModRef，但实际上调用会修改 P。
static bool callMayAliasPointer(
    CallInst *CI, Value *P,
    BasicAAResult &AA, const LoopInfo &LI)
{
    auto &UL = CI->getUserList();
    // UL[0] = callee, UL[1..] = arguments
    for (size_t i = 1; i < UL.size(); ++i) {
        Value *Arg = UL[i] ? UL[i]->getValue() : nullptr;
        if (!Arg) continue;
        if (!Arg->getIRType() || Arg->getIRType()->getEnum() != IR_POINTER) continue;
        if (AA.alias(P, Arg, LI) != AliasInfo::NoAlias) {
            return true;
        }
    }
    return false;
}

// classifyBlock: 扫描整个块，确定该块对指针 P 的第一个相关事件。
//
// 【假设】当前 IR 中 MustAlias 隐含完整覆盖（同类型同宽度）。
// 若后续 IR 引入不同宽度/切片的 store 到同基地址，
// 此处 KILL 判定需要额外加大小比较。
static BlockColor classifyBlock(
    BasicBlock *BB, Value *P,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    for (Instruction *I : *BB) {
        // 先检查 KILL：MustAlias store
        if (auto *SI = dynamic_cast<StoreInst *>(I)) {
            if (SI->getUserList().size() >= 2) {
                Value *StorePtr = SI->getUserList()[1]->getValue();
                if (AA.alias(P, StorePtr, LI) == AliasInfo::MustAlias) {
                    return BlockColor::KILL;
                }
            }
        }

        // 对 CallInst 做额外安全检查：
        // 如果 call 传递了任何可能 alias P 的指针参数 → CLOBBER
        // 这是对 getInstModRefInfo 的补充，因为 SideEffectAnalysis
        // 对自递归调用可能返回不完整的 ModRef 信息。
        if (auto *CI = dynamic_cast<CallInst *>(I)) {
            if (callMayAliasPointer(CI, P, AA, LI)) {
                return BlockColor::CLOBBER;
            }
        }

        // 再检查 CLOBBER
        // CLOBBER 合并了两种失败：
        //   READ:    I 可能读 P（isRefSet）
        //   BARRIER: I 可能改 P 但无法证明完整覆盖（isModSet 且非 MustAlias store）
        // 后续若需区分，在此拆分。
        ModRefInfo MR = getInstModRefInfo(P, I, AA, LI, FAM);

        if (isRefSet(MR)) {
            return BlockColor::CLOBBER;
        }

        if (isModSet(MR)) {
            // I 可能写 P，但不是 MustAlias store
            // 可能是 MayAlias store 或 call with Mod
            // 无法证明完整覆盖 → BARRIER
            return BlockColor::CLOBBER;
        }
    }

    return BlockColor::TRANSPARENT;
}

// classifyBlockPartial: 同 classifyBlock，但只扫描 startAfter 之后的指令。
// 用于 S1 所在块的尾部扫描。
static BlockColor classifyBlockPartial(
    BasicBlock *BB, Value *P, Instruction *startAfter,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    // 从 startAfter 的下一条指令开始
    Instruction *I = startAfter->getNext();
    while (I) {
        // 先检查 KILL
        if (auto *SI = dynamic_cast<StoreInst *>(I)) {
            if (SI->getUserList().size() >= 2) {
                Value *StorePtr = SI->getUserList()[1]->getValue();
                if (AA.alias(P, StorePtr, LI) == AliasInfo::MustAlias) {
                    return BlockColor::KILL;
                }
            }
        }

        // CallInst 安全网（同 classifyBlock 中的逻辑）
        if (auto *CI = dynamic_cast<CallInst *>(I)) {
            if (callMayAliasPointer(CI, P, AA, LI)) {
                return BlockColor::CLOBBER;
            }
        }

        ModRefInfo MR = getInstModRefInfo(P, I, AA, LI, FAM);

        if (isRefSet(MR)) {
            return BlockColor::CLOBBER;
        }

        if (isModSet(MR)) {
            return BlockColor::CLOBBER;
        }

        I = I->getNext();
    }

    return BlockColor::TRANSPARENT;
}

// isStoreDead: 判断一个 store 是否为 dead store。
// 不使用全局早退 isReachableToExit —— 同块覆盖在任何块中都可以检测到。
// 只在 BFS 走到 exit 块时，根据指针是否 local 决定是否放行。
static bool isStoreDead(
    StoreInst *S1,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    if (S1->getUserList().size() < 2) return false;
    Value *P = S1->getUserList()[1]->getValue();
    BasicBlock *B1 = dynamic_cast<BasicBlock *>(
        static_cast<Instruction *>(S1)->ChainNode<BasicBlock, Instruction>::getParent());
    if (!B1) return false;

    // Phase A: 检查 S1 所在块的剩余指令
    BlockColor tailColor = classifyBlockPartial(B1, P, S1, AA, LI, FAM);
    if (tailColor == BlockColor::KILL)    return true;   // 同块覆盖
    if (tailColor == BlockColor::CLOBBER) return false;  // 同块就被读了

    // Phase A.5: B1 本身是 exit 块的情况
    // Phase A 返回 TRANSPARENT 说明 S1 之后到块末尾无 P 相关事件。
    // 如果 B1 没有后继（即 exit 块），则函数即将返回。
    // 非局部指针在返回后 caller 可能读 → S1 alive。
    {
        auto b1Succs = B1->getSuccessors();
        if (b1Succs.empty()) {
            if (isLocalPointer(P)) {
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

        BlockColor color = classifyBlock(BB, P, AA, LI, FAM);

        if (color == BlockColor::KILL) {
            continue;  // 这条路径被覆盖 store 挡住，不再探索后继
        }

        if (color == BlockColor::CLOBBER) {
            return false;  // 存在一条路径上 P 被读或被不可证明地修改 → S1 alive
        }

        // TRANSPARENT: 块内无 P 相关事件
        auto succs = BB->getSuccessors();
        if (succs.empty()) {
            // exit 块：函数即将返回。
            // 只有 P 是栈内存（返回后对 caller 不可见）时才可忽略此 exit。
            // 非局部指针（global/param）在函数返回后 caller 可能读 → S1 alive。
            if (isLocalPointer(P)) {
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

} // namespace

// =====================================================================
// DSEPass::run
// =====================================================================

PreservationStatus DSEPass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    bool changed = false;

    // 取分析结果
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);

    // === Phase 1: 块内 DSE ===
    // 保留现有 eliminateInBlock 逻辑
    // 此阶段会直接 delete 死 store
    for (BasicBlock *BB : F) {
        changed |= eliminateInBlock(BB);
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
