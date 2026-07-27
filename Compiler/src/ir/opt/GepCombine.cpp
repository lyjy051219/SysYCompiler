#include "ir/opt/GepOpt.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/opt/DCE.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
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

// =====================================================================
// Part 1: 链式 GEP 合并
// =====================================================================
bool pointerIsProvablyWithinKnownObject(
    Value *Pointer, RangeInfo &RI, const DominatorTree &DT) {
    if (!Pointer || valueMayDependOnUndef(Pointer)) return false;

    std::unordered_set<Value *> ActiveRanges;
    std::function<IntRange(Value *, BasicBlock *, unsigned)>
        queryDefinedIndexRange;
    queryDefinedIndexRange =
        [&](Value *Index, BasicBlock *Context, unsigned Depth) -> IntRange {
        if (!Index || !Context || Depth > 8 ||
            valueMayDependOnUndef(Index))
            return IntRange();

        IntRange Known = RI.query(Index, Context, DT);
        if (!Known.isTop() && !Known.isBottom())
            return Known;

        auto *Binary = dynamic_cast<BinaryInst *>(Index);
        if (!Binary || !ActiveRanges.insert(Index).second)
            return Known;
        Value *LHS = nullptr;
        Value *RHS = nullptr;
        if (!Binary->getOperands(LHS, RHS)) {
            ActiveRanges.erase(Index);
            return IntRange();
        }

        IntRange Left =
            queryDefinedIndexRange(LHS, Context, Depth + 1);
        IntRange Right =
            queryDefinedIndexRange(RHS, Context, Depth + 1);
        IntRange Result;
        if (!Left.isBottom() && !Right.isBottom() &&
            !Left.isTop() && !Right.isTop()) {
            int64_t Minimum = 0;
            int64_t Maximum = 0;
            bool Defined = false;
            switch (Binary->getOp()) {
                case BinaryInst::ADD:
                    Defined =
                        IRMath::addI32(
                            Left.min, Right.min, Minimum) &&
                        IRMath::addI32(
                            Left.max, Right.max, Maximum);
                    break;
                case BinaryInst::SUB:
                    Defined =
                        IRMath::addI32(
                            Left.min, -Right.max, Minimum) &&
                        IRMath::addI32(
                            Left.max, -Right.min, Maximum);
                    break;
                case BinaryInst::MUL: {
                    int64_t Products[4] = {
                        Left.min * Right.min,
                        Left.min * Right.max,
                        Left.max * Right.min,
                        Left.max * Right.max};
                    auto Bounds = std::minmax_element(
                        std::begin(Products), std::end(Products));
                    Minimum = *Bounds.first;
                    Maximum = *Bounds.second;
                    Defined =
                        Minimum >= std::numeric_limits<int32_t>::min() &&
                        Maximum <= std::numeric_limits<int32_t>::max();
                    break;
                }
                case BinaryInst::SHL:
                    if (Right.isSingleValue() && Right.min >= 0 &&
                        Right.min < 31 && Left.min >= 0) {
                        int64_t Scale = int64_t{1} << Right.min;
                        Minimum = Left.min * Scale;
                        Maximum = Left.max * Scale;
                        Defined =
                            Maximum <=
                            std::numeric_limits<int32_t>::max();
                    }
                    break;
                case BinaryInst::LSHR:
                    if (Right.isSingleValue() && Right.min >= 0 &&
                        Right.min < 31 && Left.min >= 0) {
                        Minimum = Left.min >> Right.min;
                        Maximum = Left.max >> Right.min;
                        Defined = true;
                    }
                    break;
                default:
                    break;
            }
            if (Defined) Result = IntRange(Minimum, Maximum);
        }
        ActiveRanges.erase(Index);
        return Result;
    };

    std::vector<GetElementPtrInst *> Chain;
    std::unordered_set<Value *> Seen;
    Value *Current = Pointer;
    while (Current && Seen.insert(Current).second) {
        if (auto *Node = dynamic_cast<GetElementPtrInst *>(Current)) {
            Chain.push_back(Node);
            Current = Node->getBasePointer();
            continue;
        }
        if (auto *BC = dynamic_cast<BitCastInst *>(Current)) {
            Value *Source = BC->getUnaryOperand();
            auto SourceTy = Value::getPointeeType(Source);
            auto DestinationTy = Value::getPointeeType(BC);
            if (!Source ||
                !IRType::hasSameShape(SourceTy, DestinationTy))
                return false;
            Current = Source;
            continue;
        }
        break;
    }

    auto *RootGlobal = dynamic_cast<Variable *>(Current);
    if (!dynamic_cast<AllocaInst *>(Current) &&
        (!RootGlobal || !RootGlobal->isGlobal()))
        return false;

    for (GetElementPtrInst *Node : Chain) {
        Value *Base = Node->getBasePointer();
        BasicBlock *Context = Node->getParent();
        if (!Base || !Context || Node->getNumIndices() == 0)
            return false;

        auto indexIsWithin = [&](Value *Index, int64_t Minimum,
                                 int64_t Maximum) {
            if (!Index || Minimum > Maximum) return false;
            if (auto *CI = dynamic_cast<ConstInt *>(Index)) {
                int64_t Value = static_cast<int64_t>(CI->getVal());
                return Value >= Minimum && Value <= Maximum;
            }
            if (!Index->getIRType() ||
                !Index->getIRType()->isInt())
                return false;
            IntRange Range =
                queryDefinedIndexRange(Index, Context, 0);
            return !Range.isBottom() && Range.min >= Minimum &&
                   Range.max <= Maximum;
        };

        // 已知 global/alloca 在当前 IR 中只代表一个完整对象。首索引必须
        // 恒为 0；后续索引则由 TypeSystem 的数组层级逐维给出边界。
        if (!indexIsWithin(Node->getIndex(0), 0, 0))
            return false;

        std::shared_ptr<IRType> AddressedTy =
            Value::getPointeeType(Base);
        for (size_t I = 1; I < Node->getNumIndices(); ++I) {
            auto ArrayTy =
                std::dynamic_pointer_cast<ARRAYType>(AddressedTy);
            if (!ArrayTy || ArrayTy->getElementCount() <= 0 ||
                !indexIsWithin(
                    Node->getIndex(I), 0,
                    static_cast<int64_t>(
                        ArrayTy->getElementCount()) - 1))
                return false;
            AddressedTy = ArrayTy->getElementType();
        }

        auto ResultTy = Value::getPointeeType(Node);
        if (!AddressedTy || !ResultTy ||
            !IRType::hasSameShape(AddressedTy, ResultTy))
            return false;
    }
    return true;
}

bool gepIsProvablyWithinKnownObject(
    GetElementPtrInst *GEP, RangeInfo &RI, const DominatorTree &DT) {
    return GEP && pointerIsProvablyWithinKnownObject(GEP, RI, DT);
}

bool GepCombinePass::combineChainedGEPs(Function &F) {
    bool changed = false;

    auto geps = collectInstructions<GetElementPtrInst>(F);

    // 延迟删除集合，避免遍历中 delete 导致悬空指针
    std::unordered_set<GetElementPtrInst *> deleted;
    // 收集待删除的 base GEP
    std::vector<GetElementPtrInst *> toDelete;

    for (GetElementPtrInst *GEP : geps) {
        // 跳过已删除的 GEP
        if (deleted.count(GEP)) continue;

        if (GEP->getNumIndices() < 1) continue;

        Value *Base = GEP->getBasePointer();
        auto *BaseGEP = dynamic_cast<GetElementPtrInst *>(Base);
        if (!BaseGEP) continue;
        if (deleted.count(BaseGEP)) continue;

        // 单 use 检查：base GEP 必须只被当前 GEP 使用
        if (BaseGEP->countUses() > 1) continue;

        if (BaseGEP->getNumIndices() < 1) continue;
        Value *firstIdx = GEP->getIndex(0);
        Value *baseLastIdx = BaseGEP->getIndex(BaseGEP->getNumIndices() - 1);

        bool merged = false;

        // 首索引为 0 → 把 base 的全部索引前置
        if (firstIdx && firstIdx->isConstIntZero()) {
            std::vector<Value *> newIndices = BaseGEP->getIndices();
            for (size_t i = 1; i < GEP->getNumIndices(); i++)
                newIndices.push_back(GEP->getIndex(i));

            GEP->reset(BaseGEP->getBasePointer(), newIndices);
            deleted.insert(BaseGEP);
            toDelete.push_back(BaseGEP);
            merged = true;
        }
        // base 末索引为 0 → 合并（去掉 base 末索引）
        else if (baseLastIdx && baseLastIdx->isConstIntZero()) {
            std::vector<Value *> newIndices = BaseGEP->getIndices();
            newIndices.pop_back();
            auto CurrentIndices = GEP->getIndices();
            newIndices.insert(newIndices.end(), CurrentIndices.begin(),
                              CurrentIndices.end());

            GEP->reset(BaseGEP->getBasePointer(), newIndices);
            deleted.insert(BaseGEP);
            toDelete.push_back(BaseGEP);
            merged = true;
        }
        // 交界处两个常量索引相加
        else {
            auto *CI1 = firstIdx ? firstIdx->as<ConstInt>() : nullptr;
            auto *CI2 = baseLastIdx ? baseLastIdx->as<ConstInt>() : nullptr;
            if (CI1 && CI2) {
                int64_t checkedSum = 0;
                // GEP 索引是 i32。宿主 C++ 加法和目标 i32 加法都必须无溢出，
                // 否则不能把两级索引折叠为一个常量。
                if (!IRMath::addI32(
                        CI1->getVal(), CI2->getVal(), checkedSum)) {
                    continue;
                }
                std::vector<Value *> newIndices;
                for (size_t i = 0; i + 1 < BaseGEP->getNumIndices(); i++)
                    newIndices.push_back(BaseGEP->getIndex(i));
                newIndices.push_back(
                    ConstInt::newConstInt(static_cast<int>(checkedSum)));
                for (size_t i = 1; i < GEP->getNumIndices(); i++)
                    newIndices.push_back(GEP->getIndex(i));

                // 合并后非零索引数检查：超过 1 个非零索引就不合并
                int nonZeroCount = 0;
                for (size_t i = 0; i < newIndices.size(); i++) {
                    if (i == 0 && newIndices[i]->isConstIntZero()) continue;
                    if (!newIndices[i]->isConstIntZero()) nonZeroCount++;
                }
                if (nonZeroCount <= 1) {
                    GEP->reset(BaseGEP->getBasePointer(), newIndices);
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
bool GepCombinePass::foldGEPOfPhi(Function &F) {
    bool changed = false;

    for (BasicBlock *BB : F) {
        auto phis = BB->collectPhiNodes();

        for (PhiInst *Phi : phis) {
            if (Phi->getNumIncomingValues() < 2) continue;
            if (!Phi->getIRType() || !Phi->getIRType()->isPointer()) continue;

            // PHI 单 use 检查
            if (Phi->countUses() > 1) continue;

            auto *FirstGEP = dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(0));
            if (!FirstGEP) continue;

            // 不折叠自引用（循环回边的 GEP）
            bool selfRef = !forEachPhiIncoming(
                Phi, [Phi](unsigned, Value *Incoming, BasicBlock *) {
                    return Incoming != Phi;
                });
            if (selfRef) continue;

            if (FirstGEP->getNumIndices() < 1) continue;

            bool valid = true;
            int diffIdx = -1;

            valid = forEachPhiIncoming(
                Phi, [&](unsigned I, Value *Incoming, BasicBlock *) {
                if (I == 0) return true;
                auto *OtherGEP = dynamic_cast<GetElementPtrInst *>(Incoming);
                if (!OtherGEP) return false;
                if (OtherGEP->getNumIndices() != FirstGEP->getNumIndices()) {
                    return false;
                }

                if (OtherGEP->getBasePointer() != FirstGEP->getBasePointer()) {
                    return false;
                }

                for (size_t j = 0; j < FirstGEP->getNumIndices(); j++) {
                    if (OtherGEP->getIndex(j) != FirstGEP->getIndex(j)) {
                        if (diffIdx == -1) {
                            diffIdx = (int)j;
                        } else if (diffIdx != (int)j) {
                            return false;
                        }
                    }
                }
                return true;
            });

            if (!valid) continue;

            if (diffIdx == -1) {
                // 所有 incoming GEP 完全相同。
                // 不能直接用 FirstGEP 替换，因为它在前驱块，不支配当前块的 uses。
                // 正确做法：clone 一份到当前块。
                std::vector<Value *> cloneIndices = FirstGEP->getIndices();
                Value *base = FirstGEP->getBasePointer();
                auto *clonedGEP = new GetElementPtrInst(base, cloneIndices);
                clonedGEP->updateType();
                BB->insertAfterPhiNodes(clonedGEP);
                Phi->replaceAllUsesWith(clonedGEP);
                delete Phi;
                changed = true;
            } else {
                // 在不同的索引位置创建新 PHI
                Value *diffVal = FirstGEP->getIndex(static_cast<size_t>(diffIdx));
                if (!diffVal->getIRType()) continue;

                PhiInst *newPhi = new PhiInst(diffVal->getIRType());
                forEachPhiIncoming(
                    Phi, [&](unsigned, Value *Incoming,
                             BasicBlock *IncomingBlock) {
                    auto *G = dynamic_cast<GetElementPtrInst *>(Incoming);
                    newPhi->addIncoming(G->getIndex(static_cast<size_t>(diffIdx)),
                                        IncomingBlock);
                });
                BB->pushFront(newPhi);

                std::vector<Value *> newIndices;
                for (size_t j = 0; j < FirstGEP->getNumIndices(); j++) {
                    if (static_cast<int>(j) == diffIdx)
                        newIndices.push_back(newPhi);
                    else
                        newIndices.push_back(FirstGEP->getIndex(j));
                }

                Value *base = FirstGEP->getBasePointer();
                auto *newGEP = new GetElementPtrInst(base, newIndices);
                newGEP->updateType();

                BB->insertAfterPhiNodes(newGEP);

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
bool GepCombinePass::isKnownNoWrapAdd(
    BinaryInst *Inst, BasicBlock *ContextBlock, RangeInfo &RI,
    const DominatorTree &DT) {
    if (!Inst || !ContextBlock || Inst->getOp() != BinaryInst::ADD)
        return false;

    Value *LHS = nullptr;
    Value *RHS = nullptr;
    if (!Inst->getOperands(LHS, RHS) || !LHS || !RHS ||
        !IRType::hasIntegerBitWidth(LHS->getIRType(), 32) ||
        !IRType::hasIntegerBitWidth(RHS->getIRType(), 32)) {
        return false;
    }

    IntRange LRange = RI.query(LHS, ContextBlock, DT);
    IntRange RRange = RI.query(RHS, ContextBlock, DT);
    if (LRange.isBottom() || RRange.isBottom()) return false;

    int64_t CheckedMin = 0;
    int64_t CheckedMax = 0;
    return IRMath::addI32(
               LRange.min, RRange.min, CheckedMin) &&
           IRMath::addI32(
               LRange.max, RRange.max, CheckedMax);
}

bool GepCombinePass::combineLoopGEPs(
    Function &F, RangeInfo &RI, const DominatorTree &DT) {
    bool changed = false;

    for (BasicBlock *BB : F) {
        std::vector<GepCombineGroup> groups;
        std::unordered_map<size_t, std::vector<size_t>> hashToGroups;

        for (Instruction *I : *BB) {
            auto *GEP = dynamic_cast<GetElementPtrInst *>(I);
            if (!GEP) continue;
            if (GEP->getNumIndices() < 2) continue;

            size_t hash = 0;
            forEachOperand(
                GEP, [&](size_t I, Use *, Value *Operand) {
                    if (I + 1 < GEP->getNumOperands())
                        hash = hash * 131 + std::hash<Value *>{}(Operand);
                });
            hash = hash * 131 + GEP->getNumOperands();

            bool inserted = false;
            if (hashToGroups.count(hash)) {
                for (size_t gIdx : hashToGroups[hash]) {
                    auto &grp = groups[gIdx];
                    auto *first = grp.Geps[0];
                    if (first->getNumOperands() != GEP->getNumOperands()) continue;
                    bool match = forEachOperand(
                        GEP, [&](size_t I, Use *, Value *Operand) {
                            return I + 1 >= GEP->getNumOperands() ||
                                   Operand == first->getOperand(I);
                        });
                    if (match) {
                        grp.Geps.push_back(GEP);
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
            if (grp.Geps.size() < 2) continue;

            for (size_t i = 1; i < grp.Geps.size(); i++) {
                auto *prevGEP = grp.Geps[i - 1];
                auto *curGEP = grp.Geps[i];
                Value *prevLast = prevGEP->getIndex(prevGEP->getNumIndices() - 1);
                Value *curLast = curGEP->getIndex(curGEP->getNumIndices() - 1);

                // 检查 curLast = prevLast + C（直接 add 常量偏移）
                if (auto *Bin = dynamic_cast<BinaryInst *>(curLast)) {
                    Value *lhs = nullptr;
                    Value *rhs = nullptr;
                    if (Bin->getOp() == BinaryInst::ADD &&
                        Bin->getOperands(lhs, rhs)) {
                        ConstInt *offset = nullptr;
                        if (lhs == prevLast) offset = rhs ? rhs->as<ConstInt>() : nullptr;
                        else if (rhs == prevLast) offset = lhs ? lhs->as<ConstInt>() : nullptr;

                        if (offset &&
                            isKnownNoWrapAdd(Bin, BB, RI, DT)) {
                            // 只有证明原 i32 加法不回绕后，才能把
                            // gep(base, prev+C) 改写为 gep(gep(base, prev), C)。
                            std::vector<Value *> newIndices;
                            newIndices.push_back(ConstInt::newConstInt(offset->getVal()));
                            curGEP->reset(prevGEP, newIndices);
                            changed = true;
                            continue;
                        }
                    }
                }

                // 检查两个都是 add(base, const) 形式，差值是常量
                if (auto *PrevBin = dynamic_cast<BinaryInst *>(prevLast)) {
                    if (auto *CurBin = dynamic_cast<BinaryInst *>(curLast)) {
                        if (PrevBin->getOp() == BinaryInst::ADD &&
                            CurBin->getOp() == BinaryInst::ADD) {
                            Value *PrevLHS = nullptr;
                            Value *PrevRHS = nullptr;
                            Value *CurLHS = nullptr;
                            Value *CurRHS = nullptr;
                            if (!PrevBin->getOperands(PrevLHS, PrevRHS) ||
                                !CurBin->getOperands(CurLHS, CurRHS))
                                continue;

                            Value *PrevBase = PrevLHS;
                            Value *CurBase = CurLHS;
                            ConstInt *PrevOffset =
                                PrevRHS ? PrevRHS->as<ConstInt>() : nullptr;
                            ConstInt *CurOffset =
                                CurRHS ? CurRHS->as<ConstInt>() : nullptr;
                            if (!PrevOffset) {
                                PrevOffset =
                                    PrevLHS ? PrevLHS->as<ConstInt>() : nullptr;
                                PrevBase = PrevRHS;
                            }
                            if (!CurOffset) {
                                CurOffset =
                                    CurLHS ? CurLHS->as<ConstInt>() : nullptr;
                                CurBase = CurRHS;
                            }

                            int64_t CheckedDiff = 0;
                            if (PrevBase == CurBase && PrevOffset && CurOffset &&
                                isKnownNoWrapAdd(PrevBin, BB, RI, DT) &&
                                isKnownNoWrapAdd(CurBin, BB, RI, DT) &&
                                IRMath::addI32(
                                    CurOffset->getVal(),
                                    -static_cast<int64_t>(
                                        PrevOffset->getVal()),
                                    CheckedDiff)) {
                                // 两个原始加法和常量差都经过 i32 检查，重基址
                                // 不会把原来的回绕语义变成指针宽度算术。
                                std::vector<Value *> newIndices;
                                newIndices.push_back(
                                    ConstInt::newConstInt(
                                        static_cast<int>(CheckedDiff)));
                                curGEP->reset(prevGEP, newIndices);
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
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);

    for (int iter = 0; iter < 4; iter++) {
        if (!combineChainedGEPs(F)) break;
        changed = true;
    }

    changed |= foldGEPOfPhi(F);
    changed |= combineLoopGEPs(F, RI, DT);

    if (!changed) return PreservationStatus::all();

    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
