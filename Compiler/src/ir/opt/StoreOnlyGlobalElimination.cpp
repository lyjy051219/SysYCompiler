#include "ir/opt/StoreOnlyGlobalElimination.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"

#include <unordered_set>
#include <unordered_map>
#include <vector>

// =====================================================================
// StoreOnlyGlobalElimination（索引精确 + SideEffect 集成版）
//
// 【整体思路】
//   Pass 1（读取收集）：扫全模块
//     - LoadInst  : 追对应全局 G，记 G 为"读"；若 load 的是 gep(G, 0, ...
//                   带常量索引)，精确记 `readIdx[G] += {idx...}`；变量索引
//                   则 `allIndex[G] = true`
//     - CallInst  : 对每个候选 G，查 callee 的 SideEffect 是否会读 G
//                   （复用 SideEffectAnalysis.RefSet + 参数映射）。不会读
//                   就不剔除。
//     - 其他无法解析的指针（PHI 可追，Load/Call 返回 / 参数等未知）：保守
//       标记 allIndex[G] = true（不 bail 整个 pass，只影响该 G）
//
//   Pass 2（删除）：对每个候选 store
//     - traceToBase 得到基址 G 和（若 GEP 全常量）索引路径 idx
//     - G 从未被读 → 删
//     - G 只被读于常量索引，且 store 的 idx 不在其中、也非变量索引 → 删
//     - 否则保守不删
//
// 【为什么比原版深】
//   原版粒度是 "整个 global"：任何一个读都让 G 全保。
//   新版对数组 G 按 ConstInt 索引路径细分：`arr[5]` 读，`arr[3]` 的 store 可删。
//   另外用 SideEffectAnalysis 消除了"任何指针参数都当 ModRef"的过保守假设。
// =====================================================================

// 从 ptr 向上追 GEP/BitCast 链到基址，并沿途记录常量索引序列。
// 返回 base（可能是全局 Variable / 其他）。如果 path 里遇到非常量索引，
// 把 hasVarIdx 置 true。path 只记录 "全部都是常量" 时才有用。
void StoreOnlyGlobalEliminationPass::collectReachableBases(Value *ptr,
                                  std::unordered_set<Value*> &bases,
                                  std::unordered_set<Value*> &visited,
                                  bool &escape,
                                  int depth) {
    if (escape || !ptr) return;
    if (!visited.insert(ptr).second) return;
    if (depth > 32) { escape = true; return; }

    if (auto *var = dynamic_cast<Variable*>(ptr)) {
        if (var->isGlobal()) bases.insert(var);
        return;
    }
    if (auto *gep = dynamic_cast<GetElementPtrInst*>(ptr)) {
        Value *Base = gep->getOperand(0);
        if (!Base) { escape = true; return; }
        collectReachableBases(Base,
                              bases, visited, escape, depth + 1);
        return;
    }
    if (auto *bc = dynamic_cast<BitCastInst*>(ptr)) {
        Value *Base = bc->getOperand(0);
        if (!Base) { escape = true; return; }
        collectReachableBases(Base,
                              bases, visited, escape, depth + 1);
        return;
    }
    if (auto *phi = dynamic_cast<PhiInst*>(ptr)) {
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
            collectReachableBases(phi->getIncomingValue(i),
                                  bases, visited, escape, depth + 1);
            if (escape) return;
        }
        return;
    }
    if (auto *sel = dynamic_cast<SelectInst*>(ptr)) {
        for (unsigned i = 1; i < sel->getNumOperands(); i++) {
            collectReachableBases(sel->getOperand(i),
                                  bases, visited, escape, depth + 1);
            if (escape) return;
        }
        return;
    }
    // Alloca / Param / Load / Call 返回值等：Alloca 安全（非 global），其他当 escape
    if (dynamic_cast<AllocaInst*>(ptr)) return;
    escape = true;
}

// 简化版：trace 单一 ptr，成功返回 base 和常量索引序列（若全常量）。
// 失败（PHI / 多 base / 未知）返回 base=nullptr 或 hasVarIdx/unresolved=true。
StoreOnlyGlobalTraceResult
StoreOnlyGlobalEliminationPass::traceBaseAndIndex(Value *ptr) {
    StoreOnlyGlobalTraceResult R;
    std::vector<int64_t> idx;
    Value *cur = ptr;
    int depth = 0;
    while (cur && depth < 32) {
        if (auto *var = dynamic_cast<Variable*>(cur)) {
            R.base = var;
            // 反转：from base 往 leaf，读 GEP 时我们是从 leaf 往 base
            for (auto it = idx.rbegin(); it != idx.rend(); ++it)
                R.constPath.push_back(*it);
            return R;
        }
        if (auto *gep = dynamic_cast<GetElementPtrInst*>(cur)) {
            if (gep->getNumOperands() == 0) { R.unresolved = true; return R; }
            // UL[1..] 是索引，累积（全常量才有意义）
            for (unsigned i = 1; i < gep->getNumOperands(); i++) {
                Value *vi = gep->getOperand(i);
                if (auto *ci = dynamic_cast<ConstInt*>(vi)) {
                    idx.push_back((int64_t)ci->getVal());
                } else {
                    R.hasVarIdx = true;
                }
            }
            cur = gep->getOperand(0);
            depth++;
            continue;
        }
        if (auto *bc = dynamic_cast<BitCastInst*>(cur)) {
            cur = bc->getOperand(0);
            if (!cur) { R.unresolved = true; return R; }
            depth++;
            continue;
        }
        // PHI/Select/其他：没法单一 base 地 trace
        R.unresolved = true;
        return R;
    }
    R.unresolved = true;
    return R;
}

// 每个 global 的读信息
void StoreOnlyGlobalEliminationPass::markGlobalRead(
                           Variable *G, const StoreOnlyGlobalTraceResult *tr,
                           std::unordered_map<Variable*, StoreOnlyGlobalReadInfo> &reads) {
    if (!G) return;
    auto &info = reads[G];
    if (info.anyIndexRead) return;
    if (!tr || tr->unresolved || tr->hasVarIdx) {
        info.anyIndexRead = true;
        info.constIdx.clear();
        return;
    }
    // tr->constPath 里第一个通常是 0（pointer decay），第二个才是"数组下标"
    // SysY 标量 global 的 load 是 gep(G, 0)，constPath={0}
    // SysY 数组 a[N] 的 load 是 gep(G, 0, i)，constPath={0, idx}
    if (tr->constPath.size() == 1 && tr->constPath[0] == 0) {
        // 标量读，整个 global 对应索引 0
        info.constIdx.insert(0);
    } else if (tr->constPath.size() >= 2 && tr->constPath[0] == 0) {
        info.constIdx.insert(tr->constPath[1]);   // 数组第一维下标
    } else {
        // 更复杂（多维 + 非首 0），保守
        info.anyIndexRead = true;
        info.constIdx.clear();
    }
}

PreservationStatus StoreOnlyGlobalEliminationPass::run(
    Module &M, ModuleAnalysisPassManager &MAM)
{
    // 初始候选：所有全局 Variable
    std::unordered_set<Variable *> candidates;
    for (auto &g : M.getGlobalVar()) {
        if (g && g->isGlobal()) candidates.insert(g.get());
    }
    if (candidates.empty()) return PreservationStatus::all();

    // 每 global 的读信息
    std::unordered_map<Variable*, StoreOnlyGlobalReadInfo> reads;

    // ---------- Pass 1：扫全模块，收集读 ----------
    for (auto &funcPtr : M.getFunctions()) {
        Function *F = funcPtr.get();
        if (!F) continue;

        for (BasicBlock *BB : *F) {
            for (Instruction *I : *BB) {
                // LoadInst → 读
                if (auto *LI = dynamic_cast<LoadInst*>(I)) {
                    Value *ptr = LI->getPointerOperand();
                    if (!ptr) continue;

                    // 先尝试单 base 精确路径
                    StoreOnlyGlobalTraceResult tr = traceBaseAndIndex(ptr);
                    if (tr.base) {
                        if (auto *v = dynamic_cast<Variable*>(tr.base)) {
                            if (v->isGlobal()) markGlobalRead(v, &tr, reads);
                        }
                        continue;
                    }
                    // 单 base 失败 → 用 PHI-aware 多 base 收集，任何一个都标 anyIndexRead
                    std::unordered_set<Value*> bases;
                    std::unordered_set<Value*> visited;
                    bool escape = false;
                    collectReachableBases(ptr, bases, visited, escape);
                    if (escape) {
                        // 指针可能指向任意 global → 全部 candidates 标 anyIndexRead
                        for (Variable *G : candidates) reads[G].anyIndexRead = true;
                    } else {
                        for (Value *b : bases) {
                            if (auto *v = dynamic_cast<Variable*>(b))
                                if (v->isGlobal()) markGlobalRead(v, nullptr, reads);
                        }
                    }
                    continue;
                }

                // CallInst → 仅当 callee 会读该 global 的时候才标
                if (auto *CI = dynamic_cast<CallInst*>(I)) {
                    Value *callee = CI->getCallee();
                    if (!callee) continue;

                    // 内置函数：按 FuncAttr 判定
                    if (auto *BIF = dynamic_cast<BuildInFunction*>(callee)) {
                        FuncAttr attr = classifyBuiltin(BIF);
                        // An untracked external routine may observe globals even
                        // when no pointer is passed explicitly.  It is therefore
                        // not legal to delete stores merely because its argument
                        // list contains no traceable global pointer.
                        if (attr == FuncAttr::Untracked) {
                            for (Variable *G : candidates)
                                reads[G].anyIndexRead = true;
                            continue;
                        }
                        bool maybeRead =
                            attr == FuncAttr::Sylib ||
                            attr == FuncAttr::MemReadOnly ||
                            attr == FuncAttr::MemReadWrite;
                        if (maybeRead) {
                            // 实参里若有指针指向 global，保守标该 global anyIndexRead
                            for (unsigned k = 0; k < CI->getArgCount(); k++) {
                                Value *arg = CI->getArg(k);
                                if (!arg || !arg->getIRType()) continue;
                                if (arg->getIRType()->getEnum() != IR_POINTER) continue;
                                std::unordered_set<Value*> bases;
                                std::unordered_set<Value*> visited;
                                bool escape = false;
                                collectReachableBases(arg, bases, visited, escape);
                                if (escape) {
                                    for (Variable *G : candidates) reads[G].anyIndexRead = true;
                                } else {
                                    for (Value *b : bases) {
                                        if (auto *v = dynamic_cast<Variable*>(b))
                                            if (v->isGlobal()) markGlobalRead(v, nullptr, reads);
                                    }
                                }
                            }
                        }
                        continue;
                    }

                    // 用户函数：查 callee 的 SideEffectInfo，只在 RefSet 里才算读
                    if (auto *calleeF = dynamic_cast<Function*>(callee)) {
                        auto &SEI = FAM.getResult<SideEffectAnalysis>(*calleeF);
                        // A transitive untracked call can access a global without
                        // receiving its address as a formal argument.
                        if (SEI.hasBuiltin) {
                            for (Variable *G : candidates)
                                reads[G].anyIndexRead = true;
                            continue;
                        }
                        // 1) 直接读的 global（callee 的 RefSet 里若有 Variable 类型）
                        for (Value *refV : SEI.RefSet) {
                            if (auto *v = dynamic_cast<Variable*>(refV))
                                if (v->isGlobal()) markGlobalRead(v, nullptr, reads);
                        }
                        // 2) 通过 param 传入的指针实参：对应 param 在 RefSet 里 → 实参基址被读
                        auto &params = calleeF->getParams();
                        for (unsigned i = 0; i < params.size() && i < CI->getArgCount(); i++) {
                            Value *p = params[i].get();
                            if (!p) continue;
                            if (!SEI.RefSet.count(p)) continue;  // callee 没读此 param
                            Value *arg = CI->getArg(i);
                            if (!arg) continue;
                            std::unordered_set<Value*> bases;
                            std::unordered_set<Value*> visited;
                            bool escape = false;
                            collectReachableBases(arg, bases, visited, escape);
                            if (escape) {
                                for (Variable *G : candidates) reads[G].anyIndexRead = true;
                            } else {
                                for (Value *b : bases) {
                                    if (auto *v = dynamic_cast<Variable*>(b))
                                        if (v->isGlobal()) markGlobalRead(v, nullptr, reads);
                                }
                            }
                        }
                        continue;
                    }

                    // 未知 callee 类型 → 保守
                    for (Variable *G : candidates) reads[G].anyIndexRead = true;
                    continue;
                }
            }
        }
    }

    // ---------- Pass 2：扫 store，按粒度决定是否删 ----------
    bool changed = false;
    for (auto &funcPtr : M.getFunctions()) {
        Function *F = funcPtr.get();
        if (!F) continue;

        for (BasicBlock *BB : *F) {
            std::vector<StoreInst*> toDelete;
            for (Instruction *I : *BB) {
                auto *SI = dynamic_cast<StoreInst*>(I);
                if (!SI) continue;
                Value *ptr = SI->getPointerOperand();
                if (!ptr) continue;

                StoreOnlyGlobalTraceResult tr = traceBaseAndIndex(ptr);
                if (!tr.base) continue;
                auto *G = dynamic_cast<Variable*>(tr.base);
                if (!G || !G->isGlobal()) continue;
                if (!candidates.count(G)) continue;

                auto it = reads.find(G);
                // 情形 A：从没人读 G —— 全删
                if (it == reads.end()) {
                    toDelete.push_back(SI);
                    continue;
                }
                const StoreOnlyGlobalReadInfo &info = it->second;
                // 情形 B：G 被"任意索引"读过 → 保守不删
                if (info.anyIndexRead) continue;
                // 情形 C：G 只被常量索引读过，检查此 store 是否落在读集合之外
                if (tr.hasVarIdx || tr.unresolved) continue;  // store 用变量索引 → 不知道影响哪个常量索引，保守
                int64_t key = 0;
                if (tr.constPath.size() == 1 && tr.constPath[0] == 0) {
                    key = 0;   // 标量
                } else if (tr.constPath.size() >= 2 && tr.constPath[0] == 0) {
                    key = tr.constPath[1];
                } else {
                    continue;
                }
                if (!info.constIdx.count(key)) {
                    toDelete.push_back(SI);
                }
            }
            for (StoreInst *SI : toDelete) {
                delete SI;
                changed = true;
            }
        }
    }

    // ---------- Pass 3：常量初值传播（LLVM GlobalOpt 经典模式） ----------
    // 对每个标量 global：如果经过 Pass 2 后已经没有任何 store 写它，
    // 且它的初值是 Const（或零初始化），把所有 load 替换为该常量。
    // 限定在 Variable 是 "单值标量" (IR_INT / IR_FLOAT 指针) 的情况，
    // 数组 / 多维留给 Global2Local 之类的 pass。
    for (auto &g : M.getGlobalVar()) {
        if (!g) continue;
        Variable *G = g.get();
        if (!G || !G->isGlobal()) continue;

        // 类型：必须是 pointer-to-scalar (i32 / float)
        auto ptrTy = std::dynamic_pointer_cast<POINTERType>(G->getIRType());
        if (!ptrTy) continue;
        auto inner = ptrTy->getPointerType();
        if (!inner) continue;
        auto innerEnum = inner->getEnum();
        if (innerEnum != IR_INT && innerEnum != IR_FLOAT) continue;

        // Scan the whole module for any instruction that may modify G.  Looking
        // only for syntactically direct stores is unsound: a store through a
        // PHI/select/parameter, memcpy/memset, or an unknown call may all write
        // the same object.  BasicAA + the call summaries provide a conservative
        // ModRef proof; a MayAlias result deliberately blocks propagation.
        bool anyWrite = false;
        // 同时收集所有 load
        std::vector<LoadInst*> loads;
        for (auto &funcPtr : M.getFunctions()) {
            Function *F = funcPtr.get();
            if (!F) continue;
            auto &AA = FAM.getResult<BasicAliasAnalysis>(*F);
            auto &LI = FAM.getResult<LoopAnalysis>(*F);
            for (BasicBlock *BB : *F) {
                for (Instruction *I : *BB) {
                    if (auto *SI2 = dynamic_cast<StoreInst*>(I)) {
                        if (isModSet(getInstModRefInfo(G, SI2, AA, LI, FAM))) {
                            anyWrite = true;
                            break;
                        }
                    } else if (auto *CI = dynamic_cast<CallInst*>(I)) {
                        if (isModSet(getInstModRefInfo(G, CI, AA, LI, FAM))) {
                            anyWrite = true;
                            break;
                        }
                    } else if (auto *LI = dynamic_cast<LoadInst*>(I)) {
                        Value *ptr = LI->getPointerOperand();
                        if (!ptr) continue;
                        StoreOnlyGlobalTraceResult tr = traceBaseAndIndex(ptr);
                        if (tr.base == G) {
                            // 只处理标量 load：GEP 路径必须是 {0}（即直接取值）
                            if (!tr.hasVarIdx && !tr.unresolved &&
                                tr.constPath.size() == 1 && tr.constPath[0] == 0)
                                loads.push_back(LI);
                            else if (tr.constPath.empty())
                                loads.push_back(LI);  // 直接 load Variable 无 GEP
                        }
                    }
                }
                if (anyWrite) break;
            }
            if (anyWrite) break;
        }
        if (anyWrite || loads.empty()) continue;

        // 取显式初值；没有初始化 operand 时使用 zero-initializer。
        Value *initVal = G->getUsee();
        Value *constRepl = nullptr;
        if (initVal) {
            if (auto *ci = dynamic_cast<ConstInt*>(initVal)) constRepl = ci;
            else if (auto *cf = dynamic_cast<ConstFloat*>(initVal)) constRepl = cf;
            // 若 init 是 Initializer（数组）则不是标量，略
        } else {
            // zero-initializer
            if (innerEnum == IR_INT)        constRepl = ConstInt::newConstInt(0);
            else if (innerEnum == IR_FLOAT) constRepl = ConstFloat::newConstFloat(0);
        }
        if (!constRepl) continue;

        for (LoadInst *LI : loads) {
            LI->replaceAllUsesWith(constRepl);
            delete LI;
            changed = true;
        }
    }

    // ---------- Pass 4：dead global removal ----------
    // 删完 store / 传播完 init 之后，有些 global 已经没任何 use 了。
    // 直接从 Module.globalvars 里 erase（unique_ptr 会析构 Variable）。
    // 安全性：只在 useList 为空时删，Variable 析构不会级联摧毁任何 user。
    {
        auto &gvars = M.getGlobalVar();
        for (auto it = gvars.begin(); it != gvars.end(); ) {
            Variable *G = it->get();
            if (G && G->isGlobal() && G->getUseList().is_empty()) {
                it = gvars.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }

    if (!changed) return PreservationStatus::all();
    return PreservationStatus::none();
}
