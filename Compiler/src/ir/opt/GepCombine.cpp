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

 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 

 
 
 
bool pointerIsProvablyWithinKnownObject(
    Value *Pointer, RangeInfo &RI, const DominatorTree &DT) {
    // 1. 从目标指针反向收集同形 bitcast/GEP 链，并拒绝任何依赖 undef 的地址。
    if (!Pointer || valueMayDependOnUndef(Pointer)) return false;

    std::unordered_set<Value *> ActiveRanges;
    std::function<IntRange(Value *, BasicBlock *, unsigned)>
        queryDefinedIndexRange;
    // 1.1 RangeAnalysis 无直接结论时，有界递归解释纯整数索引表达式；每一步都检查
    // i32 运算不回绕，循环依赖、未知操作或预算耗尽均返回未知区间。
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

    // 1.2 链根必须是 alloca 或全局对象，未知参数/堆地址没有可用的对象边界。
    auto *RootGlobal = dynamic_cast<Variable *>(Current);
    if (!dynamic_cast<AllocaInst *>(Current) &&
        (!RootGlobal || !RootGlobal->isGlobal()))
        return false;

    // 1.3 逐级验证首索引恒为零、后续索引落入当前数组维度，并校验最终元素类型；
    // 只有整条链均在界内，调用方才能安全采用改变 GEP 分组方式的改写。
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
    // 2. 快照所有 GEP，寻找“当前 GEP 以单用户 GEP 为基址”的链式候选；
    // 计时屏障会阻止合并，以免改变其两侧地址计算的执行位置。
    bool changed = false;

    auto geps = collectInstructions<GetElementPtrInst>(F);

     
    std::unordered_set<GetElementPtrInst *> deleted;
     
    std::vector<GetElementPtrInst *> toDelete;

    for (GetElementPtrInst *GEP : geps) {
         
        if (deleted.count(GEP)) continue;

        if (GEP->getNumIndices() < 1) continue;

        Value *Base = GEP->getBasePointer();
        auto *BaseGEP = dynamic_cast<GetElementPtrInst *>(Base);
        if (!BaseGEP) continue;
        if (deleted.count(BaseGEP)) continue;

         
        if (BaseGEP->countUses() > 1) continue;
        if (F.hasTimingBarrierOnPath(BaseGEP, GEP)) continue;

        if (BaseGEP->getNumIndices() < 1) continue;
        Value *firstIdx = GEP->getIndex(0);
        Value *baseLastIdx = BaseGEP->getIndex(BaseGEP->getNumIndices() - 1);

        bool merged = false;

         
        // 2.1 交界处当前首索引为零时直接拼接；基址末索引为零时去掉该零再拼接。
        if (firstIdx && firstIdx->isConstIntZero()) {
            std::vector<Value *> newIndices = BaseGEP->getIndices();
            for (size_t i = 1; i < GEP->getNumIndices(); i++)
                newIndices.push_back(GEP->getIndex(i));

            GEP->reset(BaseGEP->getBasePointer(), newIndices);
            deleted.insert(BaseGEP);
            toDelete.push_back(BaseGEP);
            merged = true;
        }
         
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
         
        // 2.2 两侧交界索引均为常量时，在 i32 无回绕前提下相加；合并后若出现
        // 多个非零索引则放弃，避免得到比原地址更复杂的表达式。
        else {
            auto *CI1 = firstIdx ? firstIdx->as<ConstInt>() : nullptr;
            auto *CI2 = baseLastIdx ? baseLastIdx->as<ConstInt>() : nullptr;
            if (CI1 && CI2) {
                int64_t checkedSum = 0;
                 
                 
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

     
    // 2.3 所有用户都已原位重置后再统一删除旧基址 GEP，避免快照中悬空。
    for (auto *GEP : toDelete) {
        delete GEP;
    }

    return changed;
}

 
 
 
bool GepCombinePass::foldGEPOfPhi(Function &F) {
    // 3. 寻找单用户指针 PHI，其所有 incoming 都是同基址、同索引数的 GEP，
    // 且最多只有同一个索引位置不同；自引用和跨计时屏障候选被排除。
    bool changed = false;

    for (BasicBlock *BB : F) {
        auto phis = BB->collectPhiNodes();

        for (PhiInst *Phi : phis) {
            if (Phi->getNumIncomingValues() < 2) continue;
            if (!Phi->getIRType() || !Phi->getIRType()->isPointer()) continue;
            if (F.hasTimingBarrierOnPath(Phi, Phi)) continue;

             
            if (Phi->countUses() > 1) continue;

            auto *FirstGEP = dynamic_cast<GetElementPtrInst *>(Phi->getIncomingValue(0));
            if (!FirstGEP) continue;

             
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
                auto *OtherGEP = dynamic_cast<GetElementPtrInst *>(Incoming);
                if (!OtherGEP) return false;
                if (F.hasTimingBarrierOnPath(OtherGEP, Phi)) return false;
                if (I == 0) return true;
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

            // 3.1 incoming 地址完全相同时，在汇合块克隆一条 GEP；不能直接复用
            // 某个前驱定义，否则该定义未必支配 PHI 的全部用户。
            if (diffIdx == -1) {
                 
                 
                 
                std::vector<Value *> cloneIndices = FirstGEP->getIndices();
                Value *base = FirstGEP->getBasePointer();
                auto *clonedGEP = new GetElementPtrInst(base, cloneIndices);
                clonedGEP->updateType();
                BB->insertAfterPhiNodes(clonedGEP);
                Phi->replaceAllUsesWith(clonedGEP);
                delete Phi;
                changed = true;
            // 3.2 仅一个索引变化时，先为该索引创建 PHI，再在 PHI 区之后生成公共 GEP，
            // 用它替换原指针 PHI。
            } else {
                 
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

 
 
 
bool GepCombinePass::isKnownNoWrapAdd(
    BinaryInst *Inst, BasicBlock *ContextBlock, RangeInfo &RI,
    const DominatorTree &DT) {
    // 4.1 用操作数区间的两个端点证明 i32 加法不会回绕，这是相邻地址重基址的前提。
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
    // 4. 在每个基本块内按“除最后索引外的全部操作数”分组，保持程序顺序比较相邻 GEP。
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
                if (F.hasTimingBarrierOnPath(prevGEP, curGEP)) continue;
                Value *prevLast = prevGEP->getIndex(prevGEP->getNumIndices() - 1);
                Value *curLast = curGEP->getIndex(curGEP->getNumIndices() - 1);

                 
                // 4.2 若当前末索引等于前一末索引加常量且原加法无回绕，把当前 GEP
                // 改为以前一地址为基址的常量步进。
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
                             
                             
                            std::vector<Value *> newIndices;
                            newIndices.push_back(ConstInt::newConstInt(offset->getVal()));
                            curGEP->reset(prevGEP, newIndices);
                            changed = true;
                            continue;
                        }
                    }
                }

                 
                // 4.3 若两个末索引都是同一基值加常量，验证两次原加法及常量差均
                // 不回绕后，以偏移差进行同样的重基址。
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
    // 1. 获取范围与支配证明；二者只服务合法性判断，不在改写过程中增量更新。
    bool changed = false;
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);

    // 2. 链式合并最多迭代四轮，使一轮暴露出的更长链继续收敛且限制编译时间。
    for (int iter = 0; iter < 4; iter++) {
        if (!combineChainedGEPs(F)) break;
        changed = true;
    }

    // 3—4. 再做 PHI 外提与块内相邻地址重基址。
    changed |= foldGEPOfPhi(F);
    changed |= combineLoopGEPs(F, RI, DT);

    if (!changed) return PreservationStatus::all();

    // 5. 不改 CFG，保留支配与循环结构；其余依赖值表达式的分析按默认规则失效。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
