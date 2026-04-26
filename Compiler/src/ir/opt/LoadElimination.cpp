#include "ir/opt/LoadElimination.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/analysis/PostDominant.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <stack>

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
// =====================================================================

namespace {

// =====================================================================
// QueryResult
// =====================================================================
struct QueryResult {
    enum Kind {
        KNOWN,              // 所有路径值相同 = value
        PHI_NEEDED,         // 各前驱已知但值不同，predMap 有效
        CLOBBERED,          // 某路径上出现无法证明的事件
        UNKNOWN,            // 预算耗尽 / 递归过深 / 环依赖
        NEED_PREDECESSORS   // 块内没找到定义，要继续查前驱（仅内部使用）
    };
    Kind kind;
    Value *value = nullptr;
    std::vector<std::pair<BasicBlock*, Value*>> predMap;

    static QueryResult known(Value *V)      { return {KNOWN, V, {}}; }
    static QueryResult clobbered()          { return {CLOBBERED, nullptr, {}}; }
    static QueryResult unknown()            { return {UNKNOWN, nullptr, {}}; }
    static QueryResult needPredecessors()   { return {NEED_PREDECESSORS, nullptr, {}}; }
    static QueryResult phiNeeded(std::vector<std::pair<BasicBlock*, Value*>> m)
                                            { return {PHI_NEEDED, nullptr, std::move(m)}; }
};

// =====================================================================
// 辅助函数
// =====================================================================

// 【假设】当前 IR 中同一 MustAlias 地址上的 load/store 类型一定一致。
// 若后续 IR 出现 bitcast / 不同宽度访问同址，此处需增加宽度检查。
static bool isTypeCompatible(Value *source, LoadInst *targetLoad) {
    if (!source || !targetLoad) return false;
    if (!source->getIRType() || !targetLoad->getIRType()) return false;
    return source->getIRType()->getEnum() == targetLoad->getIRType()->getEnum();
}

// callMayAliasPointer: 检查 CallInst 是否传递了任何可能 alias P 的指针参数。
// 仅用作 getInstModRefInfo 的 fallback 安全网（SideEffectAnalysis 对自递归不完整）。
// 不作为主判据。
static bool callMayAliasPointer(
    CallInst *CI, Value *P,
    BasicAAResult &AA, const LoopInfo &LI)
{
    auto &UL = CI->getUserList();
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

// =====================================================================
// scanBlockBackward: 从块内某位置向前扫描，寻找 P 的最近可用值。
//
// stopBefore: 扫描到此指令前停止。nullptr = 从块末尾开始扫全块。
// targetLoad: 正在查询的 load 指令，用于类型检查和避免自引用。
// =====================================================================
static QueryResult scanBlockBackward(
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
            if (SI->getUserList().size() >= 2) {
                Value *StorePtr = SI->getUserList()[1]->getValue();
                Value *StoredVal = SI->getUserList()[0]->getValue();
                AliasInfo ai = AA.alias(P, StorePtr, LI);
                if (ai == AliasInfo::MustAlias && isTypeCompatible(StoredVal, targetLoad)) {
                    return QueryResult::known(StoredVal);
                }
                if (ai != AliasInfo::NoAlias) {
                    return QueryResult::clobbered();  // MayAlias store → 值可能被改
                }
            }
        }
        // load → load 复用
        else if (auto *LI2 = dynamic_cast<LoadInst *>(cur)) {
            if (LI2 != targetLoad && LI2->getUserList().size() >= 1) {
                Value *LoadPtr = LI2->getUserList()[0]->getValue();
                if (AA.alias(P, LoadPtr, LI) == AliasInfo::MustAlias
                    && isTypeCompatible(LI2, targetLoad)) {
                    return QueryResult::known(LI2);
                }
            }
        }
        // CallInst: 先查 ModRef（主判据），helper 做 fallback
        else if (auto *CI = dynamic_cast<CallInst *>(cur)) {
            ModRefInfo MR = getInstModRefInfo(P, cur, AA, LI, FAM);
            if (isModSet(MR)) {
                return QueryResult::clobbered();
            }
            // 纯 Ref → 不阻断 load forwarding，继续
            // 安全网：ModRef 说 NoModRef 但传了 aliasing pointer → Clobbered
            if (isNoModRef(MR) && callMayAliasPointer(CI, P, AA, LI)) {
                return QueryResult::clobbered();
            }
        }
        // 其他指令
        else {
            ModRefInfo MR = getInstModRefInfo(P, cur, AA, LI, FAM);
            if (isModSet(MR)) {
                return QueryResult::clobbered();
            }
        }

        cur = cur->getPrev();
    }

    return QueryResult::needPredecessors();
}

// =====================================================================
// queryAvailableValue: 递归查询块末尾 P 的可用值。
//
// onStack: 当前递归栈上的块（检测真实 CFG 环）
// memo:    缓存块末尾查询结果（钻石图共享祖先只算一次）
// =====================================================================
static const int MAX_DEPTH = 8;

static QueryResult queryAvailableValue(
    Value *P, BasicBlock *BB, LoadInst *targetLoad, int depth,
    std::unordered_set<BasicBlock*> &onStack,
    std::unordered_map<BasicBlock*, QueryResult> &memo,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    if (depth > MAX_DEPTH) {
        return QueryResult::unknown();
    }

    // 环检测：当前递归栈上的块
    if (onStack.count(BB)) {
        return QueryResult::unknown();
    }

    // 缓存命中
    auto memoIt = memo.find(BB);
    if (memoIt != memo.end()) {
        return memoIt->second;
    }

    onStack.insert(BB);

    // 块内扫描（扫全块，从末尾开始）
    QueryResult result = scanBlockBackward(BB, P, nullptr, targetLoad, AA, LI, FAM);

    if (result.kind == QueryResult::KNOWN || result.kind == QueryResult::CLOBBERED) {
        onStack.erase(BB);
        memo[BB] = result;
        return result;
    }

    // result.kind == NEED_PREDECESSORS
    auto preds = BB->getPredecessors();
    if (preds.empty()) {
        // 函数入口，P 的初始值未知
        onStack.erase(BB);
        memo[BB] = QueryResult::clobbered();
        return memo[BB];
    }

    std::vector<std::pair<BasicBlock*, Value*>> predValues;
    bool allSame = true;
    Value *firstVal = nullptr;

    for (BasicBlock *pred : preds) {
        QueryResult predResult = queryAvailableValue(
            P, pred, targetLoad, depth + 1, onStack, memo, AA, LI, FAM);

        if (predResult.kind == QueryResult::CLOBBERED ||
            predResult.kind == QueryResult::UNKNOWN) {
            onStack.erase(BB);
            memo[BB] = QueryResult::clobbered();
            return memo[BB];
        }

        if (predResult.kind == QueryResult::KNOWN) {
            predValues.push_back({pred, predResult.value});
            if (firstVal == nullptr) {
                firstVal = predResult.value;
            } else if (predResult.value != firstVal) {
                allSame = false;
            }
        } else if (predResult.kind == QueryResult::PHI_NEEDED) {
            // 前驱本身需要 PHI → 第一版不处理嵌套 PHI
            onStack.erase(BB);
            memo[BB] = QueryResult::clobbered();
            return memo[BB];
        }
    }

    if ((int)predValues.size() != (int)preds.size()) {
        onStack.erase(BB);
        memo[BB] = QueryResult::clobbered();
        return memo[BB];
    }

    QueryResult finalResult;
    if (allSame) {
        finalResult = QueryResult::known(firstVal);
    } else {
        finalResult = QueryResult::phiNeeded(std::move(predValues));
    }

    onStack.erase(BB);
    memo[BB] = finalResult;
    return finalResult;
}

// =====================================================================
// processLoad: 尝试消除一个 load 指令。
// =====================================================================
static bool processLoad(
    LoadInst *loadInst, BasicBlock *BB,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    if (loadInst->getUserList().size() < 1) return false;
    Value *P = loadInst->getUserList()[0]->getValue();

    // Step 1: 块内扫描（从 load 位置向前）
    QueryResult result = scanBlockBackward(BB, P, loadInst, loadInst, AA, LI, FAM);

    if (result.kind == QueryResult::KNOWN) {
        loadInst->replaceAllUsesWith(result.value);
        delete loadInst;
        return true;
    }

    if (result.kind != QueryResult::NEED_PREDECESSORS) {
        return false;   // CLOBBERED 或其他
    }

    // Step 2: 查前驱
    auto preds = BB->getPredecessors();
    if (preds.empty()) return false;

    std::unordered_set<BasicBlock*> onStack;
    std::unordered_map<BasicBlock*, QueryResult> memo;

    std::vector<std::pair<BasicBlock*, Value*>> predValues;
    bool allSame = true;
    Value *firstVal = nullptr;

    for (BasicBlock *pred : preds) {
        QueryResult predResult = queryAvailableValue(
            P, pred, loadInst, 1, onStack, memo, AA, LI, FAM);

        if (predResult.kind != QueryResult::KNOWN) {
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
    BB->push_front(phi);
    loadInst->replaceAllUsesWith(phi);
    delete loadInst;
    return true;
}

// =====================================================================
// processFunction: 支配树前序遍历处理所有 load。
// =====================================================================
static bool processFunction(
    Function &F, DominatorTree &DT,
    BasicAAResult &AA, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM)
{
    bool changed = false;

    // DFS 前序遍历支配树
    std::stack<DominatorNode*> dtStack;
    if (DT.Root) dtStack.push(DT.Root);

    while (!dtStack.empty()) {
        DominatorNode *node = dtStack.top();
        dtStack.pop();
        BasicBlock *BB = node->Block;
        if (!BB) continue;

        // 收集本块所有 load（先收集再处理，避免遍历时删指令）
        std::vector<LoadInst*> loads;
        for (Instruction *I : *BB) {
            if (auto *LI2 = dynamic_cast<LoadInst*>(I)) {
                loads.push_back(LI2);
            }
        }

        for (LoadInst *LI2 : loads) {
            // 检查 load 是否还活着（可能被前一轮 processLoad 间接影响）
            if (LI2->ChainNode<BasicBlock, Instruction>::getParent() != BB) continue;
            changed |= processLoad(LI2, BB, AA, LI, FAM);
        }

        // 推入子节点（逆序推入保持前序遍历顺序）
        std::vector<DominatorNode*> children;
        for (DominatorNode *child = node->FirstChild; child; child = child->NextSibling) {
            children.push_back(child);
        }
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            dtStack.push(*it);
        }
    }

    return changed;
}

} // namespace

// =====================================================================
// LoadEliminationPass::run
// =====================================================================
PreservationStatus LoadEliminationPass::run(Function &F,
                                            FunctionAnalysisPassManager &FAM) {
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);

    bool changed = processFunction(F, DT, AA, LI, FAM);

    if (!changed) {
        return PreservationStatus::all();
    }

    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
