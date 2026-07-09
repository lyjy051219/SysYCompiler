#include "ir/opt/GepOpt.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"

#include <vector>
#include <unordered_map>
#include <unordered_set>

// =====================================================================
// GepCombine — 跨 GEP 优化
//
// 三大类优化：链式 GEP 合并、PHI 中 GEP 提取、循环内相邻 GEP 合并。
//
// 【链式 GEP 合并】
//
// 当一个 GEP 的基址是另一个 GEP 的结果时，两个 GEP 的索引序列可以拼接
// 成一个 GEP，减少一条指令，也让后续别名分析更容易看到真实基址。
//
// 具体来说有三种场景可以合并：
//
// 首先，如果当前 GEP 的首索引是常量 0，说明它只做类型穿越不产生偏移，
// 可以把 base GEP 的全部索引前置到当前 GEP 前面，去掉首索引 0。
// 例如 gep(gep(arr, i1, i2), 0, i3) → gep(arr, i1, i2, i3)。
//
// 其次，如果 base GEP 的末索引是常量 0，说明 base GEP 最后一层只进入子类型，
// 可以去掉 base 末索引后把两边索引拼起来。
// 例如 gep(gep(arr, i1, 0), i2, i3) → gep(arr, i1, i2, i3)。
//
// 第三，如果交界处的两个索引都是常量（base 末索引和当前首索引），
// 可以在编译期把它们加在一起，减少一个索引层级。
// 例如 gep(gep(arr, i1, 3), 2, i3) → gep(arr, i1, 5, i3)。
// 合并后如果非零索引数超过 1（首部零索引不算），不合并，
// 防止合并后 GEP 比合并前更复杂。
//
// 以上三种场景都只在 base GEP 只有一个用户时才做，
// 否则 base GEP 不能删，合并不减少指令数。
//
// 【PHI 中 GEP 提取】
//
// 如果 PHI 的所有 incoming value 都是结构相同的 GEP（共享同一个 base，
// 只在一个索引位置不同），可以把 GEP 提到 merge 块里，
// 不同的那个索引位置换成新的 PHI。
// 例如 phi(gep(base, 0, i1), gep(base, 0, i2)) → gep(base, 0, phi(i1, i2))。
// 如果所有 incoming GEP 完全相同，就 clone 一份到当前块
// （不能直接用某个 incoming，因为它定义在前驱块不支配当前块的所有 uses）。
// 只允许一个索引位置不同，多了不值得。PHI 必须只有一个用户才做。
// 不折叠通过循环回边指向自身的 GEP，避免寄存器压力增大。
//
// 【循环内相邻 GEP 合并】
//
// 同一个块内如果有两个 GEP 除末索引外完全相同，末索引差一个常量，
// 后一个可以改为从前一个结果出发加上常量偏移。
// 例如 gep(arr, 0, i) 和 gep(arr, 0, i+1) → 后者改为 gep(前者, 1)。
// 也处理两个索引都是 add(base, const) 形式、差值是常量的情况。
// =====================================================================

static bool isConstZero(Value *V) {
    return V && V->isConstIntZero();
}

static ConstInt *asConstInt(Value *V) {
    return dynamic_cast<ConstInt *>(V);
}

// =====================================================================
// Part 1: 链式 GEP 合并
// =====================================================================
static bool combineChainedGEPs(Function &F) {
    bool changed = false;

    std::vector<GetElementPtrInst *> geps;
    for (BasicBlock *BB : F)
        for (Instruction *I : *BB)
            if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I))
                geps.push_back(GEP);

    // 延迟删除集合，避免遍历中 delete 导致悬空指针
    std::unordered_set<GetElementPtrInst *> deleted;
    // 收集待删除的 base GEP
    std::vector<GetElementPtrInst *> toDelete;

    for (GetElementPtrInst *GEP : geps) {
        // 跳过已删除的 GEP
        if (deleted.count(GEP)) continue;

        auto &UL = GEP->getUserList();
        if (UL.size() < 2) continue;

        Value *Base = UL[0]->getValue();
        auto *BaseGEP = dynamic_cast<GetElementPtrInst *>(Base);
        if (!BaseGEP) continue;
        if (deleted.count(BaseGEP)) continue;

        // 单 use 检查：base GEP 必须只被当前 GEP 使用
        if (BaseGEP->countUses() > 1) continue;

        auto &BaseUL = BaseGEP->getUserList();
        if (BaseUL.size() < 2) continue;

        Value *firstIdx = UL[1]->getValue();
        Value *baseLastIdx = BaseUL.back()->getValue();

        bool merged = false;

        // 首索引为 0 → 把 base 的全部索引前置
        if (isConstZero(firstIdx)) {
            std::vector<Value *> newIndices;
            for (size_t i = 1; i < BaseUL.size(); i++)
                newIndices.push_back(BaseUL[i]->getValue());
            for (size_t i = 2; i < UL.size(); i++)
                newIndices.push_back(UL[i]->getValue());

            Value *origBase = BaseUL[0]->getValue();
            while (UL.size() > 0) UL.pop_back();
            GEP->addUse(origBase);
            for (Value *idx : newIndices) GEP->addUse(idx);
            GEP->updateType();
            deleted.insert(BaseGEP);
            toDelete.push_back(BaseGEP);
            merged = true;
        }
        // base 末索引为 0 → 合并（去掉 base 末索引）
        else if (isConstZero(baseLastIdx)) {
            std::vector<Value *> newIndices;
            for (size_t i = 1; i + 1 < BaseUL.size(); i++)
                newIndices.push_back(BaseUL[i]->getValue());
            for (size_t i = 1; i < UL.size(); i++)
                newIndices.push_back(UL[i]->getValue());

            Value *origBase = BaseUL[0]->getValue();
            while (UL.size() > 0) UL.pop_back();
            GEP->addUse(origBase);
            for (Value *idx : newIndices) GEP->addUse(idx);
            GEP->updateType();
            deleted.insert(BaseGEP);
            toDelete.push_back(BaseGEP);
            merged = true;
        }
        // 交界处两个常量索引相加
        else {
            auto *CI1 = asConstInt(firstIdx);
            auto *CI2 = asConstInt(baseLastIdx);
            if (CI1 && CI2) {
                int sumVal = CI1->getVal() + CI2->getVal();
                std::vector<Value *> newIndices;
                for (size_t i = 1; i + 1 < BaseUL.size(); i++)
                    newIndices.push_back(BaseUL[i]->getValue());
                newIndices.push_back(ConstInt::newConstInt(sumVal));
                for (size_t i = 2; i < UL.size(); i++)
                    newIndices.push_back(UL[i]->getValue());

                // 合并后非零索引数检查：超过 1 个非零索引就不合并
                int nonZeroCount = 0;
                for (size_t i = 0; i < newIndices.size(); i++) {
                    if (i == 0 && isConstZero(newIndices[i])) continue;
                    if (!isConstZero(newIndices[i])) nonZeroCount++;
                }
                if (nonZeroCount <= 1) {
                    Value *origBase = BaseUL[0]->getValue();
                    while (UL.size() > 0) UL.pop_back();
                    GEP->addUse(origBase);
                    for (Value *idx : newIndices) GEP->addUse(idx);
                    GEP->updateType();
                    deleted.insert(BaseGEP);
                    toDelete.push_back(BaseGEP);
                    merged = true;
                }
            }
        }

        if (merged) changed = true;
    }

    // 统一删除
    for (auto *GEP : toDelete) {
        delete GEP;
    }

    return changed;
}

// =====================================================================
// Part 2: PHI 中 GEP 提取
// =====================================================================
static bool foldGEPOfPhi(Function &F) {
    bool changed = false;

    for (BasicBlock *BB : F) {
        std::vector<PhiInst *> phis;
        for (Instruction *I : *BB) {
            auto *Phi = dynamic_cast<PhiInst *>(I);
            if (!Phi) break;
            phis.push_back(Phi);
        }

        for (PhiInst *Phi : phis) {
            if (Phi->getNumIncomingValues() < 2) continue;
            if (!Phi->getIRType() || Phi->getIRType()->getEnum() != IR_POINTER) continue;

            // PHI 单 use 检查
            if (Phi->countUses() > 1) continue;

            auto *FirstGEP = dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(0));
            if (!FirstGEP) continue;

            // 不折叠自引用（循环回边的 GEP）
            bool selfRef = false;
            for (unsigned i = 0; i < Phi->getNumIncomingValues(); i++) {
                if (Phi->getIncomingValue(i) == Phi) { selfRef = true; break; }
            }
            if (selfRef) continue;

            auto &FirstUL = FirstGEP->getUserList();
            if (FirstUL.size() < 2) continue;

            bool valid = true;
            int diffIdx = -1;

            for (unsigned i = 1; i < Phi->getNumIncomingValues(); i++) {
                auto *OtherGEP = dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(i));
                if (!OtherGEP) { valid = false; break; }

                auto &OtherUL = OtherGEP->getUserList();
                if (OtherUL.size() != FirstUL.size()) { valid = false; break; }

                if (OtherUL[0]->getValue() != FirstUL[0]->getValue()) {
                    valid = false; break;
                }

                for (size_t j = 1; j < FirstUL.size(); j++) {
                    if (OtherUL[j]->getValue() != FirstUL[j]->getValue()) {
                        if (diffIdx == -1) {
                            diffIdx = (int)j;
                        } else if (diffIdx != (int)j) {
                            valid = false;
                            break;
                        }
                    }
                }
                if (!valid) break;
            }

            if (!valid) continue;

            if (diffIdx == -1) {
                // 所有 incoming GEP 完全相同。
                // 不能直接用 FirstGEP 替换，因为它在前驱块，不支配当前块的 uses。
                // 正确做法：clone 一份到当前块。
                std::vector<Value *> cloneIndices;
                for (size_t j = 1; j < FirstUL.size(); j++)
                    cloneIndices.push_back(FirstUL[j]->getValue());
                Value *base = FirstUL[0]->getValue();
                auto *clonedGEP = new GetElementPtrInst(base, cloneIndices);
                clonedGEP->updateType();
                for (Instruction *I : *BB) {
                    if (!dynamic_cast<PhiInst *>(I)) {
                        auto it = BB->begin();
                        while (*it != I) ++it;
                        it.insertBefore(clonedGEP);
                        break;
                    }
                }
                Phi->replaceAllUsesWith(clonedGEP);
                delete Phi;
                changed = true;
            } else {
                // 在不同的索引位置创建新 PHI
                Value *diffVal = FirstUL[diffIdx]->getValue();
                if (!diffVal->getIRType()) continue;

                PhiInst *newPhi = new PhiInst(diffVal->getIRType());
                for (unsigned i = 0; i < Phi->getNumIncomingValues(); i++) {
                    auto *G = dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(i));
                    newPhi->addIncoming(G->getUserList()[diffIdx]->getValue(),
                                        Phi->getIncomingBlock(i));
                }
                BB->push_front(newPhi);

                std::vector<Value *> newIndices;
                for (size_t j = 1; j < FirstUL.size(); j++) {
                    if ((int)j == diffIdx)
                        newIndices.push_back(newPhi);
                    else
                        newIndices.push_back(FirstUL[j]->getValue());
                }

                Value *base = FirstUL[0]->getValue();
                auto *newGEP = new GetElementPtrInst(base, newIndices);
                newGEP->updateType();

                for (Instruction *I : *BB) {
                    if (!dynamic_cast<PhiInst *>(I)) {
                        auto it = BB->begin();
                        while (*it != I) ++it;
                        it.insertBefore(newGEP);
                        break;
                    }
                }

                Phi->replaceAllUsesWith(newGEP);
                delete Phi;
                changed = true;
            }
        }
    }

    return changed;
}

// =====================================================================
// Part 3: 循环内相邻 GEP 合并
// =====================================================================
static bool combineLoopGEPs(Function &F) {
    bool changed = false;

    for (BasicBlock *BB : F) {
        struct GEPGroup {
            std::vector<GetElementPtrInst *> geps;
        };
        std::vector<GEPGroup> groups;
        std::unordered_map<size_t, std::vector<size_t>> hashToGroups;

        for (Instruction *I : *BB) {
            auto *GEP = dynamic_cast<GetElementPtrInst *>(I);
            if (!GEP) continue;
            auto &UL = GEP->getUserList();
            if (UL.size() < 3) continue;

            size_t hash = 0;
            for (size_t i = 0; i + 1 < UL.size(); i++)
                hash = hash * 131 + std::hash<Value *>{}(UL[i]->getValue());
            hash = hash * 131 + UL.size();

            bool inserted = false;
            if (hashToGroups.count(hash)) {
                for (size_t gIdx : hashToGroups[hash]) {
                    auto &grp = groups[gIdx];
                    auto *first = grp.geps[0];
                    auto &firstUL = first->getUserList();
                    if (firstUL.size() != UL.size()) continue;
                    bool match = true;
                    for (size_t i = 0; i + 1 < UL.size(); i++) {
                        if (UL[i]->getValue() != firstUL[i]->getValue()) {
                            match = false; break;
                        }
                    }
                    if (match) {
                        grp.geps.push_back(GEP);
                        inserted = true;
                        break;
                    }
                }
            }
            if (!inserted) {
                size_t idx = groups.size();
                groups.push_back({{GEP}});
                hashToGroups[hash].push_back(idx);
            }
        }

        for (auto &grp : groups) {
            if (grp.geps.size() < 2) continue;

            for (size_t i = 1; i < grp.geps.size(); i++) {
                auto *prevGEP = grp.geps[i - 1];
                auto *curGEP = grp.geps[i];
                auto &prevUL = prevGEP->getUserList();
                auto &curUL = curGEP->getUserList();

                Value *prevLast = prevUL.back()->getValue();
                Value *curLast = curUL.back()->getValue();

                // 检查 curLast = prevLast + C（直接 add 常量偏移）
                if (auto *Bin = dynamic_cast<BinaryInst *>(curLast)) {
                    if (Bin->getOp() == BinaryInst::ADD && Bin->getUserList().size() == 2) {
                        Value *lhs = Bin->getUserList()[0]->getValue();
                        Value *rhs = Bin->getUserList()[1]->getValue();
                        ConstInt *offset = nullptr;
                        if (lhs == prevLast) offset = asConstInt(rhs);
                        else if (rhs == prevLast) offset = asConstInt(lhs);

                        if (offset) {
                            std::vector<Value *> newIndices;
                            newIndices.push_back(ConstInt::newConstInt(offset->getVal()));
                            while (curUL.size() > 0) curUL.pop_back();
                            curGEP->addUse(prevGEP);
                            for (Value *idx : newIndices) curGEP->addUse(idx);
                            curGEP->updateType();
                            changed = true;
                            continue;
                        }
                    }
                }

                // 检查两个都是 add(base, const) 形式，差值是常量
                if (auto *PrevBin = dynamic_cast<BinaryInst *>(prevLast)) {
                    if (auto *CurBin = dynamic_cast<BinaryInst *>(curLast)) {
                        if (PrevBin->getOp() == BinaryInst::ADD &&
                            CurBin->getOp() == BinaryInst::ADD &&
                            PrevBin->getUserList().size() == 2 &&
                            CurBin->getUserList().size() == 2) {

                            Value *prevBase = PrevBin->getUserList()[0]->getValue();
                            Value *curBase = CurBin->getUserList()[0]->getValue();
                            auto *prevOff = asConstInt(PrevBin->getUserList()[1]->getValue());
                            auto *curOff = asConstInt(CurBin->getUserList()[1]->getValue());

                            if (prevBase == curBase && prevOff && curOff) {
                                int diff = curOff->getVal() - prevOff->getVal();
                                std::vector<Value *> newIndices;
                                newIndices.push_back(ConstInt::newConstInt(diff));
                                while (curUL.size() > 0) curUL.pop_back();
                                curGEP->addUse(prevGEP);
                                for (Value *idx : newIndices) curGEP->addUse(idx);
                                curGEP->updateType();
                                changed = true;
                                continue;
                            }
                        }
                    }
                }
            }
        }
    }

    return changed;
}

PreservationStatus GepCombinePass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    bool changed = false;

    for (int iter = 0; iter < 4; iter++) {
        if (!combineChainedGEPs(F)) break;
        changed = true;
    }

    changed |= foldGEPOfPhi(F);
    changed |= combineLoopGEPs(F);

    if (!changed) return PreservationStatus::all();

    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
