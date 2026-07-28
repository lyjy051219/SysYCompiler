#include "ir/opt/StoreOnlyGlobalElimination.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "lib/IRUtils.hpp"

#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

StoreOnlyGlobalEliminationPass::StoreOnlyGlobalEliminationPass(
    FunctionAnalysisPassManager &FAM)
    : FAM(FAM) {}

 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 

 
 
 
void StoreOnlyGlobalEliminationPass::collectReachableBases(Value *ptr,
                                  std::unordered_set<Value*> &bases,
                                  std::unordered_set<Value*> &visited,
                                  bool &escape,
                                  int depth) {
    // 1.1 沿 GEP/bitcast/PHI/select 递归收集一个指针可能来自的全部全局根；
    // 深度超限或遇到无法解释的产生者即置 escape，要求调用方退化为“可能读任意全局”。
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
        forEachPhiIncoming(
            phi, [&](unsigned, Value *Incoming, BasicBlock *) {
                collectReachableBases(Incoming, bases, visited,
                                      escape, depth + 1);
                return !escape;
            });
        return;
    }
    if (auto *sel = dynamic_cast<SelectInst*>(ptr)) {
        forEachOperand(
            sel, [&](size_t I, Use *, Value *Operand) {
                if (I == 0) return true;
                collectReachableBases(Operand, bases, visited,
                                      escape, depth + 1);
                return !escape;
            });
        return;
    }
     
    if (dynamic_cast<AllocaInst*>(ptr)) return;
    escape = true;
}

 
 
 
StoreOnlyGlobalTraceResult
StoreOnlyGlobalEliminationPass::traceBaseAndIndex(Value *ptr) {
    // 1.2 对单一路径从叶到根剥离同形 bitcast 与 GEP，分段记录常量索引；
    // 返回时按根到叶重排 GEP 段，保留每条 GEP 内的维度顺序。
    StoreOnlyGlobalTraceResult R;
    std::vector<std::vector<int64_t>> PathSegments;
    Value *cur = ptr;
    int depth = 0;
    while (cur && depth < 32) {
        if (auto *var = dynamic_cast<Variable*>(cur)) {
            R.base = var;
             
            for (auto Segment = PathSegments.rbegin();
                 Segment != PathSegments.rend(); ++Segment)
                R.constPath.insert(
                    R.constPath.end(), Segment->begin(), Segment->end());
            return R;
        }
        if (auto *gep = dynamic_cast<GetElementPtrInst*>(cur)) {
            Value *Base = gep->getBasePointer();
            if (!Base || gep->getNumIndices() == 0) {
                R.unresolved = true;
                return R;
            }

            std::vector<int64_t> Segment;
            for (size_t I = 0; I < gep->getNumIndices(); ++I) {
                auto *CI = dynamic_cast<ConstInt *>(gep->getIndex(I));
                if (!CI) {
                    R.hasVarIdx = true;
                    continue;
                }
                int64_t Index = static_cast<int64_t>(CI->getVal());
                Segment.push_back(Index);
            }
            PathSegments.push_back(std::move(Segment));

            cur = Base;
            depth++;
            continue;
        }
        if (auto *bc = dynamic_cast<BitCastInst*>(cur)) {
            Value *Source = bc->getUnaryOperand();
            auto SourceTy = Value::getPointeeType(Source);
            auto DestinationTy = Value::getPointeeType(bc);
            if (!Source ||
                !IRType::hasSameShape(SourceTy, DestinationTy)) {
                R.unresolved = true;
                return R;
            }
            cur = Source;
            if (!cur) { R.unresolved = true; return R; }
            depth++;
            continue;
        }
         
        R.unresolved = true;
        return R;
    }
    R.unresolved = true;
    return R;
}

 
void StoreOnlyGlobalEliminationPass::markGlobalRead(
                           Variable *G, const StoreOnlyGlobalTraceResult *tr,
                           std::unordered_map<Variable*, StoreOnlyGlobalReadInfo> &reads) {
    // 1.3 可精确追踪时记录标量槽或数组第一维索引；变量索引、复杂形状及未解析
    // 地址都会把该全局提升为 anyIndexRead，后续不再进行索引级删除。
    if (!G) return;
    auto &info = reads[G];
    if (info.anyIndexRead) return;
    if (!tr || tr->unresolved || tr->hasVarIdx) {
        info.anyIndexRead = true;
        info.constIdx.clear();
        return;
    }
     
     
     
    if (tr->constPath.size() == 1 && tr->constPath[0] == 0) {
         
        info.constIdx.insert(0);
    } else if (tr->constPath.size() >= 2 && tr->constPath[0] == 0) {
        info.constIdx.insert(tr->constPath[1]);    
    } else {
         
        info.anyIndexRead = true;
        info.constIdx.clear();
    }
}

PreservationStatus StoreOnlyGlobalEliminationPass::run(
    Module &M, ModuleAnalysisPassManager &MAM)
{
    // 1. 建立所有全局量候选集和逐全局读取摘要。
    std::unordered_set<Variable *> candidates;
    for (auto &g : M.getGlobalVar()) {
        if (g && g->isGlobal()) candidates.insert(g.get());
    }
    if (candidates.empty()) return PreservationStatus::all();

     
    std::unordered_map<Variable*, StoreOnlyGlobalReadInfo> reads;

     
    // 2. 扫描全模块的读取来源：直接 load 尽量精确到索引；无法单根追踪的地址
    // 合并所有可能基址，若地址逃逸则保守标记全部候选。
    for (auto &funcPtr : M.getFunctions()) {
        Function *F = funcPtr.get();
        if (!F) continue;

        forEachInstruction(*F, [&](Instruction *I) {
                 
                if (auto *LI = dynamic_cast<LoadInst*>(I)) {
                    Value *ptr = LI->getPointerOperand();
                    if (!ptr) return;

                     
                    StoreOnlyGlobalTraceResult tr = traceBaseAndIndex(ptr);
                    if (tr.base) {
                        if (auto *v = dynamic_cast<Variable*>(tr.base)) {
                            if (v->isGlobal()) markGlobalRead(v, &tr, reads);
                        }
                        return;
                    }
                     
                    std::unordered_set<Value*> bases;
                    std::unordered_set<Value*> visited;
                    bool escape = false;
                    collectReachableBases(ptr, bases, visited, escape);
                    if (escape) {
                         
                        for (Variable *G : candidates) reads[G].anyIndexRead = true;
                    } else {
                        for (Value *b : bases) {
                            if (auto *v = dynamic_cast<Variable*>(b))
                                if (v->isGlobal()) markGlobalRead(v, nullptr, reads);
                        }
                    }
                    return;
                }

                 
                // 2.1 调用按三类汇总：builtin 使用属性，已定义函数使用传递副作用摘要，
                // 未知调用读取任意全局；参数读取只映射被 callee RefSet 标记的形参。
                if (auto *CI = dynamic_cast<CallInst*>(I)) {
                    Value *callee = CI->getCallee();
                    if (!callee) return;

                     
                    if (auto *BIF = dynamic_cast<BuildInFunction*>(callee)) {
                        FuncAttr attr = classifyBuiltin(BIF);
                         
                         
                         
                         
                        if (attr == FuncAttr::Untracked) {
                            for (Variable *G : candidates)
                                reads[G].anyIndexRead = true;
                            return;
                        }
                        bool maybeRead =
                            attr == FuncAttr::Sylib ||
                            attr == FuncAttr::MemReadOnly ||
                            attr == FuncAttr::MemReadWrite;
                        if (maybeRead) {
                             
                            for (unsigned k = 0; k < CI->getArgCount(); k++) {
                                Value *arg = CI->getArg(k);
                                if (!arg || !arg->getIRType()) continue;
                                if (!arg->getIRType()->isPointer()) continue;
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
                        return;
                    }

                     
                    if (auto *calleeF = dynamic_cast<Function*>(callee)) {
                        auto &SEI = FAM.getResult<SideEffectAnalysis>(*calleeF);
                         
                         
                        if (SEI.hasBuiltin) {
                            for (Variable *G : candidates)
                                reads[G].anyIndexRead = true;
                            return;
                        }
                         
                        for (Value *refV : SEI.RefSet) {
                            if (auto *v = dynamic_cast<Variable*>(refV))
                                if (v->isGlobal()) markGlobalRead(v, nullptr, reads);
                        }
                         
                        auto &params = calleeF->getParams();
                        for (unsigned i = 0; i < params.size() && i < CI->getArgCount(); i++) {
                            Value *p = params[i].get();
                            if (!p) continue;
                            if (!SEI.RefSet.count(p)) continue;   
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
                        return;
                    }

                     
                    for (Variable *G : candidates) reads[G].anyIndexRead = true;
                    return;
                }
        });
    }

     
    // 3. 第二次扫描所有 store：全局从未读取则删除全部写；仅有常量索引读取时，
    // 删除落在未读索引上的精确写入；任何模糊地址都保留。
    bool changed = false;
    for (auto &funcPtr : M.getFunctions()) {
        Function *F = funcPtr.get();
        if (!F) continue;

        std::vector<StoreInst*> toDelete;
        for (StoreInst *SI : collectInstructions<StoreInst>(*F)) {
            Value *ptr = SI->getPointerOperand();
            if (!ptr) continue;

            StoreOnlyGlobalTraceResult tr = traceBaseAndIndex(ptr);
            if (!tr.base) continue;
            auto *G = dynamic_cast<Variable*>(tr.base);
            if (!G || !G->isGlobal() || !candidates.count(G)) continue;

            auto it = reads.find(G);
             
            if (it == reads.end()) {
                toDelete.push_back(SI);
                continue;
            }
            const StoreOnlyGlobalReadInfo &info = it->second;
             
            if (info.anyIndexRead) continue;
             
            if (tr.hasVarIdx || tr.unresolved) continue;
            int64_t key = 0;
            if (tr.constPath.size() == 1 && tr.constPath[0] == 0) {
                key = 0;
            } else if (tr.constPath.size() >= 2 && tr.constPath[0] == 0) {
                key = tr.constPath[1];
            } else {
                continue;
            }
            if (!info.constIdx.count(key)) toDelete.push_back(SI);
        }
        for (StoreInst *SI : toDelete) {
            delete SI;
            changed = true;
        }
    }

     
     
     
     
     
    // 4. 对删除写入后只读的标量全局做常量传播。用每个函数的 AA/LoopInfo/ModRef
    // 证明整个模块没有间接写，再把完整对象 load 替换为显式或零初始化常量。
    for (auto &g : M.getGlobalVar()) {
        if (!g) continue;
        Variable *G = g.get();
        if (!G || !G->isGlobal()) continue;

         
        auto inner = IRType::getPointeeType(G->getIRType());
        if (!inner) continue;
        if (!inner->isScalar()) continue;

         
         
         
         
         
        bool anyWrite = false;
         
        std::vector<LoadInst*> loads;
        for (auto &funcPtr : M.getFunctions()) {
            Function *F = funcPtr.get();
            if (!F) continue;
            auto &AA = FAM.getResult<BasicAliasAnalysis>(*F);
            auto &LI = FAM.getResult<LoopAnalysis>(*F);
            forEachInstruction(*F, [&](Instruction *I) {
                if (auto *SI2 = dynamic_cast<StoreInst*>(I)) {
                    if (isModSet(getInstModRefInfo(G, SI2, AA, LI, FAM))) {
                        anyWrite = true;
                        return false;
                    }
                } else if (auto *CI = dynamic_cast<CallInst*>(I)) {
                    if (isModSet(getInstModRefInfo(G, CI, AA, LI, FAM))) {
                        anyWrite = true;
                        return false;
                    }
                } else if (auto *LI = dynamic_cast<LoadInst*>(I)) {
                    Value *ptr = LI->getPointerOperand();
                    if (!ptr) return true;
                    StoreOnlyGlobalTraceResult tr = traceBaseAndIndex(ptr);
                    if (tr.base == G &&
                        IRType::hasSameShape(
                            LI->getIRType(), inner)) {
                         
                        if (!tr.hasVarIdx && !tr.unresolved &&
                            tr.constPath.size() == 1 && tr.constPath[0] == 0)
                            loads.push_back(LI);
                        else if (tr.constPath.empty())
                            loads.push_back(LI);   
                    }
                }
                return true;
            });
            if (anyWrite) break;
        }
        if (anyWrite || loads.empty()) continue;

         
        Value *initVal = G->getUsee();
        Value *constRepl = nullptr;
        if (initVal) {
            if (auto *ci = dynamic_cast<ConstInt*>(initVal)) constRepl = ci;
            else if (auto *cf = dynamic_cast<ConstFloat*>(initVal)) constRepl = cf;
             
        } else {
             
            constRepl = Value::makeZeroForType(inner);
        }
        if (!constRepl) continue;

        for (LoadInst *LI : loads) {
            LI->replaceAllUsesWith(constRepl);
            delete LI;
            changed = true;
        }
    }

     
     
     
     
    // 5. store/load 清理后统一移除没有任何 use 的全局量，最后失效全部模块相关分析。
    {
        auto &gvars = M.getGlobalVar();
        for (auto it = gvars.begin(); it != gvars.end(); ) {
            Variable *G = it->get();
            if (G && G->isGlobal() && !G->hasUses()) {
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
