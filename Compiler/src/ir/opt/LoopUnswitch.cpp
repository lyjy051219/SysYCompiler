// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/LoopUnswitch.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
//判断给定值是否为非常量布尔值 
bool LoopUnswitchPass::isBooleanNonConstant(Value *V) {
    if (!V || Value::isUndef(V) || V->isConst() || !V->getIRType())
        return false;
    return V->getIRType()->isBool();
}
//判断给定值是否能在指定循环内使用点获得且支配该使用
bool LoopUnswitchPass::isAvailableAt(Value *V, Instruction *UsePoint, const Loop &L,
                   const DominatorTree &DT) {
    if (!V || !UsePoint || Value::isUndef(V)) return false;
    if (V->isConst() || dynamic_cast<Function *>(V) ||
        dynamic_cast<BuildInFunction *>(V)) {
        return true;
    }
    if (V->isParam() || V->isGlobal()) return true;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || L.contains(I->getParent())) return false;
    return DT.dominatesValue(I, UsePoint);
}
//检查循环是否满足规范形状并填充头、预头和回边块
bool LoopUnswitchPass::getCanonicalLoopShape(Loop *L, CanonicalLoopShape &Shape) {
    if (!L || !L->getHeader() || !LCSSAPass::isLCSSAForm(L)) return false;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch) {
        return false;
    }
    auto *PreBr = PreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(PreBr) ||
        PreBr->getTrueSuccessor() != L->getHeader()) {
        return false;
    }
    Shape = {L->getHeader(), PreHeader, Latch};
    return true;
}
//判断基本块是否直接属于给定循环而非其子循环
bool LoopUnswitchPass::isDirectLoopBlock(const LoopInfo &LI, Loop *L, BasicBlock *BB) {
    return L && BB && LI.getLoopFor(BB) == L;
}
//收集给定循环中所有能够直接离环的基本块
std::vector<BasicBlock *> LoopUnswitchPass::collectExitingBlocks(Loop &L) {
    std::vector<BasicBlock *> Result;
    for (BasicBlock *BB : L.getBlocksIncludingSubLoops()) {
        if (BB && L.isExitingBlock(BB)) Result.push_back(BB);
    }
    return Result;
}
//判断候选分支块是否在循环取得进展前必定执行
bool LoopUnswitchPass::isMustExecuteBeforeLoopProgress(BasicBlock *Parent, Loop &L,
                                     const DominatorTree &DT) {
    if (!Parent) return false;
    for (BasicBlock *Backedge : L.getBackedgeSrcs()) {
        if (!Backedge || !DT.dominates(Parent, Backedge)) return false;
    }
    for (BasicBlock *Exiting : collectExitingBlocks(L)) {
        if (!DT.dominates(Parent, Exiting)) return false;
    }
    return true;
}
//将指定块集合内对条件的使用替换为已知布尔常量
bool LoopUnswitchPass::replaceConditionUsesInBlocks(Value *Condition, const BlockSet &Blocks,
                                  bool KnownValue) {
    if (!Condition || Blocks.empty()) return false;
    auto Uses = Condition->getUses();
    Value *Replacement = ConstBool::newConstBool(KnownValue);
    bool Changed = false;
    for (Use *U : Uses) {
        auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
        BasicBlock *UserBB = UserI ? UserI->getParent() : nullptr;
        if (UserBB && Blocks.count(UserBB))
            Changed |= U->replaceUseeWith(Replacement);
    }
    return Changed;
}
//把循环Header PHI中旧预头对应的前驱替换为新预头
void LoopUnswitchPass::setHeaderPredecessor(BasicBlock *Header, BasicBlock *OldPreHeader,
                          BasicBlock *NewPreHeader) {
    if (!Header || !OldPreHeader || !NewPreHeader) return;
    for (PhiInst *Phi : Header->collectPhiNodes()) {
        forEachPhiIncoming(
            Phi, [Phi, OldPreHeader, NewPreHeader](
                     unsigned Index, Value *, BasicBlock *Pred) {
                if (Pred == OldPreHeader)
                    Phi->setIncomingBlock(Index, NewPreHeader);
            });
    }
}
//收集选择指定分支后仍在循环内可达的特化闭包
LoopUnswitchPass::BlockSet
LoopUnswitchPass::collectSpecializedReachable(Loop &L, BasicBlock *Parent,
                                     BasicBlock *ChosenSuccessor) {
    BlockSet Reachable;
    BasicBlock *Header = L.getHeader();
    if (!Header || !Parent || !ChosenSuccessor) return Reachable;
    if (!L.contains(Header)) return Reachable;
    std::vector<BasicBlock *> Roots{Header};
    return collectReachableFromRootsCustomSucc<BasicBlock>(
        Roots, [&L, Parent, ChosenSuccessor](BasicBlock *BB) {
            std::vector<BasicBlock *> Successors;
            if (!BB || !L.contains(BB)) return Successors;
            if (BB == Parent) {
                if (L.contains(ChosenSuccessor))
                    Successors.push_back(ChosenSuccessor);
                return Successors;
            }
            for (BasicBlock *Succ : BB->getSuccessors()) {
                if (Succ && L.contains(Succ)) Successors.push_back(Succ);
            }
            std::reverse(Successors.begin(), Successors.end());
            return Successors;
        });
}
//判断给定CFG边是否在指定分支特化后仍然有效
bool LoopUnswitchPass::isActiveSpecializedEdge(BasicBlock *Pred, BasicBlock *Succ,
                             BasicBlock *Parent,
                             BasicBlock *ChosenSuccessor) {
    if (!Pred || !Succ) return false;
    if (Pred == Parent) return Succ == ChosenSuccessor;
    return Pred->countEdgesToSucc(Succ) > 0;
}
//统计给定块列表或过滤集合中的指令数量
std::size_t LoopUnswitchPass::countInstructions(const std::vector<BasicBlock *> &Blocks,
                                                const BlockSet *Filter) {
    std::size_t Count = 0;
    const std::size_t MaxSize = std::numeric_limits<std::size_t>::max();
    for (BasicBlock *BB : Blocks) {
        if (!BB || (Filter && !Filter->count(BB))) continue;
        const std::size_t BlockSize = static_cast<std::size_t>(BB->size());
        if (BlockSize > MaxSize - Count) return MaxSize;
        Count += BlockSize;
    }
    return Count;
}
//统计循环内由候选条件控制且必经的动态使用数量
unsigned LoopUnswitchPass::countMustExecuteControlUses(
    Value *Condition, Loop &L, const DominatorTree &DT) {
    if (!Condition) return 0;
    std::unordered_set<Instruction *> Seen;
    for (Use *U : Condition->getUses()) {
        auto *I = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
        if (!I || !I->getParent() || !L.contains(I->getParent())) continue;
        //5.3收益只计每迭代在进展或退出前必经的 branch/select use，不用偶发路径使用猜测热度。
        if (!isMustExecuteBeforeLoopProgress(I->getParent(), L, DT)) continue;
        if (auto *Br = dynamic_cast<BrInst *>(I)) {
            if (Br->getCondition() == Condition) Seen.insert(I);
        } else if (auto *Sel = dynamic_cast<SelectInst *>(I)) {
            if (Sel->getCondition() == Condition) Seen.insert(I);
        }
    }
    return static_cast<unsigned>(Seen.size());
}
//剥离布尔取反链并记录最终条件是否反相
Value *LoopUnswitchPass::stripBooleanNot(Value *V, bool &Inverted) {
    Inverted = false;
    for (unsigned Depth = 0; V && Depth != 8; ++Depth) {
        auto *BI = dynamic_cast<BinaryInst *>(V);
        if (!BI || BI->getOp() != BinaryInst::XOR ||
            !IRType::isBool(BI->getIRType())) {
            break;
        }
        Value *L = BI->getOperand(0);
        Value *R = BI->getOperand(1);
        auto *LC = dynamic_cast<ConstBool *>(L);
        auto *RC = dynamic_cast<ConstBool *>(R);
        if (LC && LC->getVal()) {
            V = R;
            Inverted = !Inverted;
        } else if (RC && RC->getVal()) {
            V = L;
            Inverted = !Inverted;
        } else {
            break;
        }
    }
    return V;
}
//尝试把给定条件求值为编译期布尔常量
std::optional<bool> LoopUnswitchPass::evaluateConstantBoolean(Value *Condition) {
    if (!Condition || Value::isUndef(Condition)) return std::nullopt;
    if (auto *CB = dynamic_cast<ConstBool *>(Condition))
        return CB->getVal();
    bool Inverted = false;
    Value *Base = stripBooleanNot(Condition, Inverted);
    if (auto *CB = dynamic_cast<ConstBool *>(Base))
        return CB->getVal() != Inverted;
    auto *BI = dynamic_cast<BinaryInst *>(Base);
    if (!BI || BI->getNumOperands() < 2) return std::nullopt;
    Value *Folded = BinaryInst::foldConstants(
        BI->getOp(), BI->getOperand(0), BI->getOperand(1), true);
    auto *FoldedBool = dynamic_cast<ConstBool *>(Folded);
    if (!FoldedBool) return std::nullopt;
    return FoldedBool->getVal() != Inverted;
}
//将整数比较谓词映射为小于、等于和大于关系掩码
unsigned LoopUnswitchPass::predicateRelationMask(BinaryInst::Operation Op) {
    switch (Op) {
    case BinaryInst::L:  return RelationLessMask;
    case BinaryInst::LE: return RelationLessMask | RelationEqualMask;
    case BinaryInst::E:  return RelationEqualMask;
    case BinaryInst::NE: return RelationLessMask | RelationGreaterMask;
    case BinaryInst::GE: return RelationEqualMask | RelationGreaterMask;
    case BinaryInst::G:  return RelationGreaterMask;
    default:             return 0;
    }
}
//返回整数比较谓词成立时变量允许落入的闭区间集合
std::vector<LoopUnswitchPass::ClosedInterval>
LoopUnswitchPass::predicateIntervals(BinaryInst::Operation Op,
                                               std::int64_t Bound) {
    std::vector<ClosedInterval> Result;
    auto Add = [&](std::int64_t L, std::int64_t R) {
        L = std::max(L, PredicateMinimum);
        R = std::min(R, PredicateMaximum);
        if (L <= R) Result.emplace_back(L, R);
    };
    switch (Op) {
    case BinaryInst::L:  Add(PredicateMinimum, Bound - 1); break;
    case BinaryInst::LE: Add(PredicateMinimum, Bound); break;
    case BinaryInst::E:  Add(Bound, Bound); break;
    case BinaryInst::NE:
        Add(PredicateMinimum, Bound - 1);
        Add(Bound + 1, PredicateMaximum);
        break;
    case BinaryInst::GE: Add(Bound, PredicateMaximum); break;
    case BinaryInst::G:  Add(Bound + 1, PredicateMaximum); break;
    default: break;
    }
    return Result;
}
//判断闭区间集合A是否完全包含于闭区间集合B
bool LoopUnswitchPass::intervalsAreSubset(const std::vector<ClosedInterval> &A,
                        const std::vector<ClosedInterval> &B) {
    for (const ClosedInterval &Need : A) {
        std::int64_t CoveredThrough = Need.first - 1;
        for (const ClosedInterval &Have : B) {
            if (Have.second < Need.first || Have.first > Need.second)
                continue;
            if (Have.first > CoveredThrough + 1) break;
            CoveredThrough = std::max(CoveredThrough, Have.second);
            if (CoveredThrough >= Need.second) break;
        }
        if (CoveredThrough < Need.second) return false;
    }
    return true;
}
//尝试将整数比较规范化为变量与i32常量边界的谓词
std::optional<LoopUnswitchPass::IntegerBoundPredicate>
LoopUnswitchPass::normalizeIntegerBound(Value *V) {
    auto *BI = dynamic_cast<BinaryInst *>(V);
    if (!BI || !BinaryInst::isCompareOp(BI->getOp())) return std::nullopt;
    Value *L = BI->getOperand(0);
    Value *R = BI->getOperand(1);
    if (!L || !R) return std::nullopt;
    Value *Variable = L;
    auto *Bound = dynamic_cast<ConstInt *>(R);
    BinaryInst::Operation Predicate = BI->getOp();
    if (!Bound) {
        Bound = dynamic_cast<ConstInt *>(L);
        Variable = R;
        Predicate = BinaryInst::reversePredicate(Predicate);
    }
    auto Ty = Variable ? Variable->getIRType() : nullptr;
    if (!Bound || !IRType::hasIntegerBitWidth(Ty, 32))
        return std::nullopt;
    return IntegerBoundPredicate{
        Variable, Predicate, static_cast<std::int32_t>(Bound->getVal())};
}
//根据已知守卫结果证明基础目标条件的布尔值
std::optional<bool> LoopUnswitchPass::proveBaseConditionFromGuard(Value *Target,
                                                Value *Guard,
                                                bool GuardKnown) {
    if (!Target || !Guard) return std::nullopt;
    if (Target == Guard) return GuardKnown;
    auto *TargetCmp = dynamic_cast<BinaryInst *>(Target);
    auto *GuardCmp = dynamic_cast<BinaryInst *>(Guard);
    if (!TargetCmp || !GuardCmp ||
        !BinaryInst::isCompareOp(TargetCmp->getOp()) ||
        !BinaryInst::isCompareOp(GuardCmp->getOp())) {
        return std::nullopt;
    }
    Value *TL = TargetCmp->getOperand(0);
    Value *TR = TargetCmp->getOperand(1);
    Value *GL = GuardCmp->getOperand(0);
    Value *GR = GuardCmp->getOperand(1);
    BinaryInst::Operation GuardOp = GuardCmp->getOp();
    if (TL == GR && TR == GL) {
        std::swap(GL, GR);
        GuardOp = BinaryInst::reversePredicate(GuardOp);
    }
    const bool IntegerOperands = TL && TR && TL->getIRType() &&
                                 TR->getIRType() &&
                                 TL->getIRType()->isInt() &&
                                 TR->getIRType()->isInt();
    if (TL == GL && TR == GR) {
        if (!IntegerOperands) {
            if (TargetCmp->getOp() == GuardOp) return GuardKnown;
            return std::nullopt;
        }
        if (!GuardKnown) GuardOp = BinaryInst::negatePredicate(GuardOp);
        const unsigned Known = predicateRelationMask(GuardOp);
        const unsigned Target = predicateRelationMask(TargetCmp->getOp());
        if (Known && (Known & ~Target) == 0) return true;
        if (Known && (Known & Target) == 0) return false;
    }
    if (!IntegerOperands) return std::nullopt;
    auto TargetBound = normalizeIntegerBound(Target);
    auto GuardBound = normalizeIntegerBound(Guard);
    if (!TargetBound || !GuardBound ||
        TargetBound->Variable != GuardBound->Variable) {
        return std::nullopt;
    }
    BinaryInst::Operation KnownPredicate = GuardBound->Predicate;
    if (!GuardKnown)
        KnownPredicate = BinaryInst::negatePredicate(KnownPredicate);
    const auto Known = predicateIntervals(KnownPredicate, GuardBound->Bound);
    const auto TargetTrue = predicateIntervals(TargetBound->Predicate,
                                               TargetBound->Bound);
    if (intervalsAreSubset(Known, TargetTrue)) return true;
    const auto TargetFalse = predicateIntervals(
        BinaryInst::negatePredicate(TargetBound->Predicate),
        TargetBound->Bound);
    if (intervalsAreSubset(Known, TargetFalse)) return false;
    return std::nullopt;
}
//剥离布尔取反后根据已知守卫结果证明目标条件
std::optional<bool> LoopUnswitchPass::proveConditionFromGuard(Value *Target, Value *Guard,
                                            bool GuardKnown) {
    bool TargetInverted = false;
    bool GuardInverted = false;
    Target = stripBooleanNot(Target, TargetInverted);
    Guard = stripBooleanNot(Guard, GuardInverted);
    GuardKnown = GuardKnown != GuardInverted;
    auto Result = proveBaseConditionFromGuard(Target, Guard, GuardKnown);
    if (!Result) return std::nullopt;
    return *Result != TargetInverted;
}
//判断条件在守卫块到循环预头之间是否保持稳定
bool LoopUnswitchPass::conditionIsStableAcrossGuardLoop(Value *Condition, BasicBlock *GuardBB,
                                      BasicBlock *PreHeader,
                                      const LoopInfo &LI,
                                      const DominatorTree &DT) {
    Loop *GuardLoop = LI.getLoopFor(GuardBB);
    if (!GuardLoop || !GuardLoop->contains(PreHeader)) return true;
    CanonicalLoopShape GuardShape;
    return getCanonicalLoopShape(GuardLoop, GuardShape) &&
           GuardShape.PreHeader && GuardShape.PreHeader->getTerminator() &&
           isAvailableAt(Condition, GuardShape.PreHeader->getTerminator(),
                         *GuardLoop, DT);
}
//沿循环入口控制链和支配守卫推导条件的已知值
std::optional<bool> LoopUnswitchPass::getEntryKnownValue(
    Value *Condition, BasicBlock *PreHeader, const LoopInfo &LI,
    const DominatorTree &DT) {
    if (!Condition || !PreHeader) return std::nullopt;
    if (auto Constant = evaluateConstantBoolean(Condition))
        return Constant;
    //4.1先沿 preheader 的唯一前驱链回溯真实动态入口边，只使用确实选中当前链条的 guard 事实。
    BasicBlock *Current = PreHeader;
    BlockSet Visited;
    for (unsigned Depth = 0; Current && Depth != 32; ++Depth) {
        if (!Visited.insert(Current).second) break;
        auto Preds = Current->getPredecessors();
        if (Preds.size() != 1) break;
        BasicBlock *Pred = Preds.front();
        if (!Pred || Pred->countEdgesToSucc(Current) != 1 ||
            !DT.dominates(Pred, PreHeader)) {
            break;
        }
        BrInst *Guard = Pred->getBranchTerminator();
        if (BasicBlock::isCondBr(Guard)) {
            BasicBlock *T = Guard->getTrueSuccessor();
            BasicBlock *F = Guard->getFalseSuccessor();
            if (!T || !F || T == F || (T != Current && F != Current))
                break;
            const bool GuardKnown = T == Current;
            if (auto Proven = proveConditionFromGuard(
                    Condition, Guard->getCondition(), GuardKnown)) {
                return Proven;
            }
            Current = Pred;
            continue;
        }
        if (!BasicBlock::isUncondBr(Guard) ||
            Guard->getTrueSuccessor() != Current) {
            break;
        }
        Current = Pred;
    }
    //4.2再沿支配树回溯 guard：只有 guard 某后继的唯一入边支配 preheader 时，块支配才能升级为边事实。
    //4.2.1guard 位于包围循环时，还须证明目标条件对该循环不变，禁止把早先外迭代的事实泄漏到当前迭代。
    Current = PreHeader;
    Visited.clear();
    for (unsigned Depth = 0; Current && Depth != 32; ++Depth) {
        Current = DT.getIDom(Current);
        if (!Current || !Visited.insert(Current).second) break;
        BrInst *Guard = Current->getBranchTerminator();
        if (!BasicBlock::isCondBr(Guard) ||
            !conditionIsStableAcrossGuardLoop(Condition, Current, PreHeader,
                                              LI, DT)) {
            continue;
        }
        BasicBlock *Successors[2] = {Guard->getTrueSuccessor(),
                                     Guard->getFalseSuccessor()};
        if (!Successors[0] || !Successors[1] ||
            Successors[0] == Successors[1]) {
            continue;
        }
        for (unsigned Arm = 0; Arm != 2; ++Arm) {
            BasicBlock *S = Successors[Arm];
            auto Preds = S->getPredecessors();
            if (Preds.size() != 1 || Preds.front() != Current ||
                Current->countEdgesToSucc(S) != 1 ||
                !DT.dominates(S, PreHeader)) {
                continue;
            }
            if (auto Proven = proveConditionFromGuard(
                    Condition, Guard->getCondition(), Arm == 0)) {
                return Proven;
            }
        }
    }
    return std::nullopt;
}
//检查平凡外提前缀是否无内存定义、故障和可观察副作用
bool LoopUnswitchPass::isSafeTrivialPrefix(
    const TrivialCandidate &C, const LoopInfo &LI,
    const DominatorTree &DT, const MemorySSAResult &MSSA) {
    if (!C.L || !C.Parent || !C.Branch || !C.Shape.Header) return false;
    if (!isMustExecuteBeforeLoopProgress(C.Parent, *C.L, DT)) return false;
    //1.前向收集首迭代从Header到候选分支的前缀，遇到回边或退出边保守拒绝
    BlockSet Forward;
    std::vector<BasicBlock *> ForwardRoots{C.Shape.Header};
    bool ForwardComplete =
        forEachReachablePreOrderCustomSucc<BasicBlock>(
            ForwardRoots,
            [](BasicBlock *BB) {
                auto Successors = BB ? BB->getSuccessors()
                                     : std::vector<BasicBlock *>{};
                std::reverse(Successors.begin(), Successors.end());
                return Successors;
            },
            [&C, &Forward](BasicBlock *BB) {
                if (!BB || !C.L->contains(BB))
                    return TraversalControl::SkipSuccessors;
                Forward.insert(BB);
                if (BB == C.Parent)
                    return TraversalControl::SkipSuccessors;
                for (BasicBlock *Succ : BB->getSuccessors()) {
                    if (!Succ || !C.L->contains(Succ) ||
                        Succ == C.Shape.Header) {
                        return TraversalControl::Stop;
                    }
                }
                return TraversalControl::Continue;
            });
    if (!ForwardComplete || !Forward.count(C.Parent)) return false;
    //2.反向证明所有前向前缀路径都必经Parent，避免外提原本可能不执行的条件
    BlockSet CanReachParent;
    std::vector<BasicBlock *> ReverseRoots{C.Parent};
    forEachReachablePreOrderCustomSucc<BasicBlock>(
        ReverseRoots,
        [&C](BasicBlock *BB) {
            std::vector<BasicBlock *> Predecessors;
            if (!BB || BB == C.Shape.Header) return Predecessors;
            for (BasicBlock *Pred : BB->getPredecessors()) {
                if (Pred && C.L->contains(Pred))
                    Predecessors.push_back(Pred);
            }
            std::reverse(Predecessors.begin(), Predecessors.end());
            return Predecessors;
        },
        [&C, &CanReachParent](BasicBlock *BB) {
            if (!BB || !C.L->contains(BB))
                return TraversalControl::SkipSuccessors;
            CanReachParent.insert(BB);
            if (BB == C.Shape.Header)
                return TraversalControl::SkipSuccessors;
            return TraversalControl::Continue;
        });
    //3.前缀必须直属当前循环且不含内存定义、故障、poison或可观察操作
    for (BasicBlock *BB : Forward) {
        if (!CanReachParent.count(BB) || !isDirectLoopBlock(LI, C.L, BB))
            return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            if (I == C.Branch) break;
            // 平凡外提会让退出路径跳过这段前缀；调用是否可观察需要依赖
            // 跨函数分析，因此此处对所有调用采用统一的结构门禁。
            if (dynamic_cast<CallInst *>(I)) return false;
            MemoryAccess *MA = MSSA.getMemoryAccess(I);
            if ((MA && MA->isDef()) ||
                instructionMayFaultOrProducePoison(I) ||
                dceInstructionMustBePreserved(I)) {
                return false;
            }
        }
    }
    return true;
}
//检查平凡外提后出口PHI是否能够保持合法SSA输入
bool LoopUnswitchPass::validateTrivialExitPhis(
    const TrivialCandidate &C, const DominatorTree &DT) {
    if (!C.Exit || !C.Shape.PreHeader || !C.Shape.PreHeader->getTerminator())
        return false;
    const bool SplitExit = C.Exit->getPredecessors().size() != 1 ||
                           C.Exit->getPredecessors().front() != C.Parent;
    //1.每个出口PHI必须恰有一个来自候选分支的值且该值在预头可用
    for (PhiInst *Phi : C.Exit->collectPhiNodes()) {
        if (!Phi) return false;
        Value *DirectValue = nullptr;
        unsigned DirectCount = 0;
        unsigned RemainingCount = 0;
        forEachPhiIncoming(
            Phi, [&C, &DirectValue, &DirectCount, &RemainingCount](
                     unsigned, Value *Incoming, BasicBlock *Pred) {
            if (Pred == C.Parent) {
                DirectValue = Incoming;
                ++DirectCount;
            } else {
                ++RemainingCount;
            }
            });
        if (DirectCount != 1 ||
            !isAvailableAt(DirectValue, C.Shape.PreHeader->getTerminator(),
                           *C.L, DT)) {
            return false;
        }
        if (SplitExit && RemainingCount == 0) return false;
        //2.拒绝依赖同一出口其他PHI的输入，避免拆分后形成反向SSA环
        if (!forEachPhiIncoming(
                Phi, [&C](unsigned, Value *Incoming, BasicBlock *) {
                    auto *InI = dynamic_cast<Instruction *>(Incoming);
                    return !InI || InI->getParent() != C.Exit;
                })) {
            return false;
        }
    }
    return true;
}
//扫描循环森林并返回首个合法平凡非交换候选
std::optional<LoopUnswitchPass::TrivialCandidate>
LoopUnswitchPass::findTrivialCandidate(
    Function &F, LoopInfo &LI, const DominatorTree &DT,
    const MemorySSAResult &MSSA) {
    //1.按照支配树前序稳定扫描块，不可用时保守退回函数布局顺序
    std::vector<BasicBlock *> DomOrder = DT.getBlocksInPreOrder();
    if (DomOrder.empty()) {
        for (BasicBlock *BB : F) DomOrder.push_back(BB);
    }
    //2.从内向外选择规范且条件在预头可用的循环
    for (Loop *L : LI.getLoopsInPostOrder()) {
        CanonicalLoopShape Shape;
        if (!getCanonicalLoopShape(L, Shape)) continue;
        //3.候选分支必须一侧继续、一侧退出，并通过前缀和出口PHI合法性证明
        for (BasicBlock *BB : DomOrder) {
            if (!isDirectLoopBlock(LI, L, BB)) continue;
            BrInst *Br = BB->getBranchTerminator();
            if (!BasicBlock::isCondBr(Br)) continue;
            Value *Cond = Br->getCondition();
            if (!isBooleanNonConstant(Cond) ||
                !isAvailableAt(Cond, Shape.PreHeader->getTerminator(),
                               *L, DT)) {
                continue;
            }
            BasicBlock *TrueSucc = Br->getTrueSuccessor();
            BasicBlock *FalseSucc = Br->getFalseSuccessor();
            if (!TrueSucc || !FalseSucc || TrueSucc == FalseSucc) continue;
            const bool TrueInside = L->contains(TrueSucc);
            const bool FalseInside = L->contains(FalseSucc);
            if (TrueInside == FalseInside) continue;
            TrivialCandidate C;
            C.L = L;
            C.Shape = Shape;
            C.Parent = BB;
            C.Branch = Br;
            C.Condition = Cond;
            C.ExitOnTrue = !TrueInside;
            C.Exit = C.ExitOnTrue ? TrueSucc : FalseSucc;
            C.Continue = C.ExitOnTrue ? FalseSucc : TrueSucc;
            if (!isSafeTrivialPrefix(C, LI, DT, MSSA) ||
                !validateTrivialExitPhis(C, DT)) {
                continue;
            }
            return C;
        }
    }
    return std::nullopt;
}
//检查克隆闭包在CFG和SSA使用上是否保持封闭合法
bool LoopUnswitchPass::cloneClosureIsLegal(const NonTrivialCandidate &C) {
    //1.克隆闭包必须同时具备循环、特化分支和两个有效后继，缺失信息时保守拒绝
    if (!C.L || !C.Parent || !C.CloneSucc || !C.OriginalSucc)
        return false;
    //2.PHI只检查特化后仍活跃的输入边，其定义不得来自被剪掉的循环内块
    for (BasicBlock *BB : C.AllBlocks) {
        if (!BB || !C.CloneReachable.count(BB)) continue;
        for (Instruction *I : *BB) {
            if (!I) return false;
            if (auto *Phi = dynamic_cast<PhiInst *>(I)) {
                const bool Legal = forEachPhiIncoming(
                    Phi, [&C, BB](unsigned, Value *Incoming,
                                  BasicBlock *Pred) {
                    bool Active = false;
                    if (BB == C.Shape.Header && Pred == C.Shape.PreHeader) {
                        Active = true;
                    } else if (C.CloneReachable.count(Pred)) {
                        Active = isActiveSpecializedEdge(
                            Pred, BB, C.Parent, C.CloneSucc);
                    }
                    if (!Active) return true;
                    auto *Def = dynamic_cast<Instruction *>(Incoming);
                    if (Def && Def->getParent() &&
                        C.L->contains(Def->getParent()) &&
                        !C.CloneReachable.count(Def->getParent())) {
                        return false;
                    }
                    return true;
                    });
                if (!Legal) return false;
                continue;
            }
            //3.普通操作数和CFG目标都必须在闭包内，唯一例外是将被改写掉的原分支另一侧
            const bool Legal = forEachOperand(
                I, [&C, BB, I](size_t, Use *, Value *Operand) {
                if (auto *Target = dynamic_cast<BasicBlock *>(Operand)) {
                    if (C.L->contains(Target) &&
                        !C.CloneReachable.count(Target)) {
                        auto *Br = dynamic_cast<BrInst *>(I);
                        if (!(BB == C.Parent && Br &&
                              Target == C.OriginalSucc)) {
                            return false;
                        }
                    }
                    return true;
                }
                auto *Def = dynamic_cast<Instruction *>(Operand);
                if (Def && Def->getParent() &&
                    C.L->contains(Def->getParent()) &&
                    !C.CloneReachable.count(Def->getParent())) {
                    return false;
                }
                return true;
                });
            if (!Legal) return false;
        }
    }
    return true;
}
//判断给定基本块集合中是否包含任意函数调用
bool LoopUnswitchPass::hasAnyCall(const std::vector<BasicBlock *> &Blocks) {
    for (BasicBlock *BB : Blocks) {
        if (!BB) return true;
        for (Instruction *I : *BB)
            if (dynamic_cast<CallInst *>(I)) return true;
    }
    return false;
}
//估算循环所有出口PHI版本化产生的抽象成本
std::size_t LoopUnswitchPass::estimateExitPhiCost(Loop &L) {
    std::size_t Count = 0;
    const std::size_t MaxSize = std::numeric_limits<std::size_t>::max();
    for (BasicBlock *Exit : L.getExitBlocks()) {
        if (!Exit) continue;
        const std::size_t PhiCount = Exit->collectPhiNodes().size();
        if (PhiCount > MaxSize - Count) return MaxSize;
        Count += PhiCount;
    }
    return Count;
}
//检查非平凡版本化后所有出口PHI是否能够合法修复
bool LoopUnswitchPass::validateVersionedExitPhis(const NonTrivialCandidate &C) {
    if (!C.L) return false;
    //1.合并两个特化闭包的活跃出口前驱，并拒绝当前修复模型无法表达的并行出口边
    for (BasicBlock *Exit : C.L->getExitBlocks()) {
        if (!Exit) return false;
        BlockSet LivePreds = C.TrueReachable;
        LivePreds.insert(C.FalseReachable.begin(), C.FalseReachable.end());
        for (BasicBlock *Pred : LivePreds) {
            if (!Pred || Pred->countEdgesToSucc(Exit) == 0) continue;
            //1.1每个版本的活跃前驱必须恰好有一条出口边
            if (Pred->countEdgesToSucc(Exit) != 1) return false;
        }
        //2.每个出口PHI必须为两个版本的每条活跃前驱各提供唯一输入
        for (PhiInst *Phi : Exit->collectPhiNodes()) {
            if (!Phi) return false;
            //2.1拒绝同一出口内部PHI依赖，避免代理块重建时形成逆向SSA环
            if (!forEachPhiIncoming(
                    Phi, [Exit](unsigned, Value *Incoming, BasicBlock *) {
                        auto *InI = dynamic_cast<Instruction *>(Incoming);
                        return !InI || InI->getParent() != Exit;
                    })) {
                return false;
            }
            for (BasicBlock *Pred : C.TrueReachable) {
                if (!Pred || Pred->countEdgesToSucc(Exit) == 0) continue;
                unsigned Hits = 0;
                forEachPhiIncoming(
                    Phi, [Pred, &Hits](unsigned, Value *,
                                       BasicBlock *IncomingPred) {
                        Hits += IncomingPred == Pred;
                    });
                if (Hits != 1) return false;
            }
            for (BasicBlock *Pred : C.FalseReachable) {
                if (!Pred || Pred->countEdgesToSucc(Exit) == 0) continue;
                unsigned Hits = 0;
                forEachPhiIncoming(
                    Phi, [Pred, &Hits](unsigned, Value *,
                                       BasicBlock *IncomingPred) {
                        Hits += IncomingPred == Pred;
                    });
                if (Hits != 1) return false;
            }
        }
    }
    return true;
}
//扫描循环森林并收集入口条件已经确定的无克隆候选
std::vector<LoopUnswitchPass::EntryKnownCandidate>
LoopUnswitchPass::findEntryKnownCandidates(
    LoopInfo &LI, const DominatorTree &DT) {
    std::vector<EntryKnownCandidate> Result;
    for (Loop *L : LI.getLoopsInPostOrder()) {
        //4.从内向外收集 entry-known 候选；同一循环内相同条件只记录一次。
        CanonicalLoopShape Shape;
        if (!getCanonicalLoopShape(L, Shape) ||
            !Shape.PreHeader->getTerminator()) {
            continue;
        }
        std::vector<BasicBlock *> AllBlocks = L->getBlocksInFunctionOrder();
        for (BasicBlock *BB : AllBlocks) {
            if (!isDirectLoopBlock(LI, L, BB)) continue;
            BrInst *Br = BB->getBranchTerminator();
            if (!BasicBlock::isCondBr(Br)) continue;
            Value *Condition = Br->getCondition();
            if (!isBooleanNonConstant(Condition) ||
                !isAvailableAt(Condition, Shape.PreHeader->getTerminator(),
                               *L, DT)) {
                continue;
            }
            auto Proof = getEntryKnownValue(Condition, Shape.PreHeader,
                                            LI, DT);
            if (!Proof) continue;
            bool Duplicate = false;
            for (const EntryKnownCandidate &Existing : Result) {
                if (Existing.L == L && Existing.Condition == Condition) {
                    Duplicate = true;
                    break;
                }
            }
            if (Duplicate) continue;
            Result.push_back(EntryKnownCandidate{
                L, Condition, AllBlocks, *Proof});
        }
    }
    return Result;
}
//按照函数布局顺序收集全部基本块
std::vector<BasicBlock *> LoopUnswitchPass::collectFunctionBlocks(Function &F) {
    std::vector<BasicBlock *> Blocks;
    for (BasicBlock *BB : F)
        if (BB) Blocks.push_back(BB);
    return Blocks;
}
//计算给定块列表或过滤区域的加权静态规模
std::size_t LoopUnswitchPass::weightedRegionSize(const std::vector<BasicBlock *> &Blocks,
                                                 const BlockSet *Filter) {
    std::size_t BlockCount = 0;
    const std::size_t MaxSize = std::numeric_limits<std::size_t>::max();
    for (BasicBlock *BB : Blocks) {
        if (!BB || (Filter && !Filter->count(BB))) continue;
        if (BlockCount == MaxSize) return MaxSize;
        ++BlockCount;
    }
    const std::size_t InstructionCount =
        countInstructions(Blocks, Filter);
    const std::size_t BlockUnit =
        static_cast<std::size_t>(ClonedBlockCost);
    if (InstructionCount == MaxSize ||
        BlockCount > (MaxSize - InstructionCount) / BlockUnit) {
        return MaxSize;
    }
    return InstructionCount + BlockUnit * BlockCount;
}
//选择保留较大区域并克隆较小区域的方向性版本方案
bool LoopUnswitchPass::chooseDirectionalClonePlan(NonTrivialCandidate &C) {
    const std::size_t TrueWeight =
        weightedRegionSize(C.AllBlocks, &C.TrueReachable);
    const std::size_t FalseWeight =
        weightedRegionSize(C.AllBlocks, &C.FalseReachable);
    //5.2无 profile 时不选热分支，而是按“指令+块布局”权重保留较大闭包、克隆较小闭包。
    C.OriginalValue = TrueWeight >= FalseWeight;
    C.OriginalSucc = C.OriginalValue ? C.TrueSucc : C.FalseSucc;
    C.CloneSucc = C.OriginalValue ? C.FalseSucc : C.TrueSucc;
    C.OriginalReachable = C.OriginalValue ? C.TrueReachable
                                          : C.FalseReachable;
    C.CloneReachable = C.OriginalValue ? C.FalseReachable
                                       : C.TrueReachable;
    return C.OriginalSucc && C.CloneSucc;
}
//扫描并返回收益严格覆盖静态成本的最佳非平凡候选
std::optional<LoopUnswitchPass::NonTrivialCandidate>
LoopUnswitchPass::findBestNonTrivialCandidate(
    Function &F, LoopInfo &LI, const DominatorTree &DT, SCEVResult &SE) {
    std::optional<NonTrivialCandidate> Best;
    auto Consider = [&](NonTrivialCandidate C) {
        const std::int64_t Net = C.Benefit - C.Cost;
        const std::int64_t BestNet =
            Best ? Best->Benefit - Best->Cost
                 : std::numeric_limits<std::int64_t>::min();
        if (!Best || Net > BestNet ||
            (Net == BestNet && C.ControlUses > Best->ControlUses) ||
            (Net == BestNet && C.ControlUses == Best->ControlUses &&
             C.CloneWeight < Best->CloneWeight)) {
            Best = std::move(C);
        }
    };
    const std::vector<BasicBlock *> FunctionBlocks =
        collectFunctionBlocks(F);
    const std::size_t FunctionWeight = weightedRegionSize(FunctionBlocks);
    if (FunctionWeight == 0) return std::nullopt;
    for (Loop *L : LI.getLoopsInPostOrder()) {
        //5.枚举 non-trivial 候选：分支两侧都必须留在循环内，条件在 preheader 可用，Parent 在每迭代进展前必经。
        CanonicalLoopShape Shape;
        if (!getCanonicalLoopShape(L, Shape)) continue;
        std::vector<BasicBlock *> AllBlocks = L->getBlocksInFunctionOrder();
        if (AllBlocks.empty()) continue;
        for (BasicBlock *BB : AllBlocks) {
            if (!isDirectLoopBlock(LI, L, BB)) continue;
            BrInst *Br = BB->getBranchTerminator();
            if (!BasicBlock::isCondBr(Br)) continue;
            BasicBlock *T = Br->getTrueSuccessor();
            BasicBlock *Fals = Br->getFalseSuccessor();
            if (!T || !Fals || T == Fals ||
                !L->contains(T) || !L->contains(Fals)) {
                continue;
            }
            Value *Cond = Br->getCondition();
            if (!isBooleanNonConstant(Cond) ||
                !isAvailableAt(Cond, Shape.PreHeader->getTerminator(),
                               *L, DT)) {
                continue;
            }
            NonTrivialCandidate C;
            C.L = L;
            C.Shape = Shape;
            C.Parent = BB;
            C.Branch = Br;
            C.Condition = Cond;
            C.TrueSucc = T;
            C.FalseSucc = Fals;
            C.AllBlocks = AllBlocks;
            C.ControlUses =
                countMustExecuteControlUses(Cond, *L, DT);
            if (C.ControlUses == 0) continue;
            if (!isMustExecuteBeforeLoopProgress(BB, *L, DT)) {
                continue;
            }
            C.TrueReachable = collectSpecializedReachable(*L, BB, T);
            C.FalseReachable = collectSpecializedReachable(*L, BB, Fals);
            //5.1把 Parent 的分支固定为 true/false 后分别求循环内可达闭包，两个版本都必须保留 Header 和 Parent。
            if (!C.TrueReachable.count(Shape.Header) ||
                !C.TrueReachable.count(BB) ||
                !C.FalseReachable.count(Shape.Header) ||
                !C.FalseReachable.count(BB) ||
                !chooseDirectionalClonePlan(C)) {
                continue;
            }
            if (C.CloneReachable.size() > MaxNonTrivialCloneBlocks) {
                continue;
            }
            if (!cloneClosureIsLegal(C)) {
                continue;
            }
            if (!validateVersionedExitPhis(C)) {
                continue;
            }
            C.CloneInstructions =
                countInstructions(AllBlocks, &C.CloneReachable);
            //5.4绝对预算限制克隆块/指令数，且克隆闭包中不得含任何调用。
            if (C.CloneInstructions > MaxNonTrivialCloneInstructions) {
                continue;
            }
            std::vector<BasicBlock *> CloneBlocks;
            for (BasicBlock *CloneBB : AllBlocks)
                if (CloneBB && C.CloneReachable.count(CloneBB))
                    CloneBlocks.push_back(CloneBB);
            if (hasAnyCall(CloneBlocks)) {
                continue;
            }
            const std::size_t ExitPhiCost = estimateExitPhiCost(*L);
            const std::size_t BaseCloneWeight =
                C.CloneInstructions +
                static_cast<std::size_t>(ClonedBlockCost) *
                    C.CloneReachable.size();
            const std::size_t MaxSize =
                std::numeric_limits<std::size_t>::max();
            if (ExitPhiCost >
                (MaxSize - BaseCloneWeight) /
                    static_cast<std::size_t>(ExitPhiVersioningCost)) {
                continue;
            }
            C.CloneWeight =
                BaseCloneWeight +
                static_cast<std::size_t>(ExitPhiVersioningCost) *
                    ExitPhiCost;
            //5.4.1相对预算用商和余数计算函数权重的百分比，避免先乘百分比导致 size_t 溢出。
            const std::size_t RelativeCloneLimit =
                (FunctionWeight / 100) * MaxClonePercentOfFunction +
                ((FunctionWeight % 100) * MaxClonePercentOfFunction) / 100;
            if (C.CloneWeight > RelativeCloneLimit) {
                continue;
            }
            if (C.CloneWeight >
                static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max())) {
                continue;
            }
            //5.4.2动态收益使用 SCEV 小常量 trip count 并在上限饱和；无法证明时只按一次迭代估计。
            std::int64_t TripEstimate = UnknownTripCountEstimate;
            if (auto TC = SE.getSmallConstantTripCount(L)) {
                TripEstimate = std::clamp<std::int64_t>(
                    *TC, 0, MaxTripCountForProfitability);
            }
            //5.4.3收益为 trip estimate 乘必经控制 use 数，并扣除新增的单次 preheader 版本分派。
            const std::int64_t EliminatedControlCost =
                TripEstimate *
                static_cast<std::int64_t>(C.ControlUses) *
                ControlUseCost;
            C.Benefit = std::max<std::int64_t>(
                0, EliminatedControlCost - EntryDispatchCost);
            C.Cost = static_cast<std::int64_t>(C.CloneWeight);
            //5.4.4只有动态控制收益严格大于含出口版本化在内的静态增长时，候选才进入最优比较。
            if (C.Benefit <= C.Cost) {
                continue;
            }
            Consider(std::move(C));
        }
    }
    return Best;
}
//通过值映射返回原值在克隆区域中的对应值
Value *LoopUnswitchPass::mapValue(Value *V, const ValueToValueMap &VMap) {
    auto It = VMap.find(V);
    return It == VMap.end() ? V : It->second;
}
//按照特化可达边剪枝游离克隆中的PHI并修复其使用
bool LoopUnswitchPass::pruneClonedPhis(ClonedBlockRegion &Region,
                     const NonTrivialCandidate &C,
                     BasicBlock *ClonedPreHeader) {
    //1.先离线收集每个克隆PHI在特化CFG中的活跃输入，不立即修改use链
    std::vector<PhiPrunePlan> Plans;
    for (const auto &Pair : Region.InstPairs) {
        auto *OldPhi = dynamic_cast<PhiInst *>(Pair.first);
        if (!OldPhi) continue;
        auto *NewPhi = dynamic_cast<PhiInst *>(Pair.second);
        if (!NewPhi) return false;
        BasicBlock *OldBB = OldPhi->getParent();
        PhiPrunePlan P;
        P.Original = OldPhi;
        P.Cloned = NewPhi;
        const bool Planned = forEachPhiIncoming(
            OldPhi, [&Region, &C, ClonedPreHeader, OldBB, &P](
                        unsigned, Value *OldValue, BasicBlock *OldPred) {
            BasicBlock *NewPred = nullptr;
            bool Active = false;
            if (OldBB == C.Shape.Header &&
                OldPred == C.Shape.PreHeader) {
                Active = true;
                NewPred = ClonedPreHeader;
            } else if (C.CloneReachable.count(OldPred) &&
                       isActiveSpecializedEdge(
                           OldPred, OldBB, C.Parent, C.CloneSucc)) {
                Active = true;
                NewPred = Region.getClonedBlock(OldPred);
            }
            if (!Active) return true;
            Value *NewValue = mapValue(OldValue, Region.VMap);
            if (!NewPred || !NewValue) return false;
            P.Incoming.push_back({NewValue, NewPred});
            return true;
            });
        if (!Planned) return false;
        Plans.push_back(std::move(P));
    }
    //2.零边折叠为undef、单边折叠为值、多边重建PHI；提交前拒绝单边替换环
    std::unordered_map<Value *, Value *> SingletonReplacements;
    for (PhiPrunePlan &P : Plans) {
        if (P.Incoming.size() != 1) continue;
        Value *Replacement = P.Incoming.front().V;
        if (Replacement == P.Cloned) {
            Replacement = UndefValue::NewUndefValue(P.Cloned->getIRType());
        }
        SingletonReplacements[P.Cloned] = Replacement;
    }
    for (const auto &Entry : SingletonReplacements) {
        Value *Current = Entry.first;
        std::unordered_set<Value *> Path;
        while (SingletonReplacements.count(Current)) {
            if (!Path.insert(Current).second) return false;
            Current = SingletonReplacements[Current];
        }
    }
    std::unordered_map<Value *, Value *> PhiReplacements;
    //3.为所有计划建立完整替换映射，确保跨PHI引用能够统一解析
    for (PhiPrunePlan &P : Plans) {
        if (P.Incoming.empty()) {
            P.Replacement = UndefValue::NewUndefValue(
                P.Cloned->getIRType());
        } else if (P.Incoming.size() == 1) {
            P.Replacement = SingletonReplacements[P.Cloned];
        } else {
            P.NewPhi = new PhiInst(P.Cloned->getIRType());
            P.NewPhi->setIdent(P.Cloned->getIdent());
            P.Replacement = P.NewPhi;
        }
        PhiReplacements[P.Cloned] = P.Replacement;
    }
    auto Resolve = [&](Value *V) {
        std::unordered_set<Value *> Seen;
        while (V && PhiReplacements.count(V) && Seen.insert(V).second) {
            Value *Next = PhiReplacements[V];
            if (Next == V) break;
            V = Next;
        }
        return V;
    };
    for (PhiPrunePlan &P : Plans) {
        if (P.NewPhi) {
            for (const auto &In : P.Incoming) {
                Value *Resolved = Resolve(In.V);
                if (!Resolved) return false;
                P.NewPhi->addIncoming(Resolved, In.Pred);
            }
        } else {
            P.Replacement = Resolve(P.Replacement);
            if (!P.Replacement) return false;
            PhiReplacements[P.Cloned] = P.Replacement;
        }
        Region.VMap[P.Original] = P.Replacement;
    }
    //4.全部解析成功后再集中迁移use并删除旧PHI，失败路径不会留下部分改写
    for (PhiPrunePlan &P : Plans) {
        Value *Replacement = P.NewPhi
                                 ? static_cast<Value *>(P.NewPhi)
                                 : P.Replacement;
        P.Cloned->replaceAllUsesWith(Replacement);
        if (P.NewPhi) P.Cloned->replaceWith(P.NewPhi);
        delete P.Cloned;
    }
    return true;
}
//销毁尚未提交到函数中的一组游离基本块
void LoopUnswitchPass::destroyBlocks(std::vector<BasicBlock *> Blocks) {
    //6.2回滚游离克隆时先清空所有 operand 打断跨块 use 链，再清空并销毁基本块。
    for (BasicBlock *BB : Blocks) {
        if (!BB) continue;
        for (Instruction *I : *BB)
            if (I) I->clearOperands();
    }
    for (BasicBlock *BB : Blocks)
        if (BB) BB->clear();
    for (BasicBlock *BB : Blocks) delete BB;
}
//在代理块中根据多条输入构建出口值或直接返回单一输入
PhiInst *LoopUnswitchPass::buildExitProxyValue(
    const std::shared_ptr<IRType> &Ty,
    const std::vector<PhiInst::IncomingEdge> &Incoming,
    BasicBlock *Proxy, const std::string &Name) {
    if (Incoming.empty() || !Proxy) return nullptr;
    auto *Phi = new PhiInst(Ty);
    Phi->setIdent(Name);
    for (const auto &In : Incoming) Phi->addIncoming(In.V, In.Pred);
    Proxy->pushFront(Phi);
    return Phi;
}
//离线规划单个循环出口的双版本前驱和PHI修复
std::optional<LoopUnswitchPass::VersionedExitPlan>
LoopUnswitchPass::planVersionedExit(
    BasicBlock *Exit, const NonTrivialCandidate &C,
    ClonedBlockRegion &Region) {
    if (!Exit) return std::nullopt;
    //1.离线收集原版本与克隆版本的出口前驱，任何边数或映射异常都保守回退
    VersionedExitPlan Plan;
    Plan.Exit = Exit;
    std::vector<BasicBlock *> CloneSourcePreds;
    for (BasicBlock *BB : C.AllBlocks) {
        if (!BB || BB->countEdgesToSucc(Exit) == 0) continue;
        if (C.OriginalReachable.count(BB)) {
            if (BB->countEdgesToSucc(Exit) != 1) return std::nullopt;
            Plan.OriginalPreds.push_back(BB);
        }
        if (C.CloneReachable.count(BB)) CloneSourcePreds.push_back(BB);
    }
    for (BasicBlock *SourcePred : CloneSourcePreds) {
        BasicBlock *Pred = Region.getClonedBlock(SourcePred);
        if (!Pred || Pred->countEdgesToSucc(Exit) != 1)
            return std::nullopt;
        Plan.ClonePreds.push_back(Pred);
    }
    //2.为每个出口PHI分别映射两个版本的输入值与前驱
    for (PhiInst *OldPhi : Exit->collectPhiNodes()) {
        if (!OldPhi) return std::nullopt;
        VersionedExitPhiPlan PhiPlan;
        PhiPlan.OldPhi = OldPhi;
        for (const auto &In : OldPhi->collectIncomingEdges()) {
            if (!In.V || !In.Pred) return std::nullopt;
            if (C.OriginalReachable.count(In.Pred) &&
                In.Pred->countEdgesToSucc(Exit) > 0) {
                PhiPlan.OriginalIncoming.push_back(In);
            }
            if (C.CloneReachable.count(In.Pred)) {
                BasicBlock *MappedPred = Region.getClonedBlock(In.Pred);
                Value *MappedValue = mapValue(In.V, Region.VMap);
                if (MappedPred && MappedPred->countEdgesToSucc(Exit) > 0 &&
                    MappedValue) {
                    PhiPlan.CloneIncoming.push_back(
                        {MappedValue, MappedPred});
                }
            }
        }
        //3.每侧PHI输入数必须与该版本活跃出口前驱数精确一致
        if ((!Plan.OriginalPreds.empty() &&
             PhiPlan.OriginalIncoming.size() !=
                 Plan.OriginalPreds.size()) ||
            (!Plan.ClonePreds.empty() &&
             PhiPlan.CloneIncoming.size() != Plan.ClonePreds.size())) {
            return std::nullopt;
        }
        Plan.PhiPlans.push_back(std::move(PhiPlan));
    }
    return Plan;
}
//提交一个出口版本化计划并重建出口前驱与PHI
void LoopUnswitchPass::applyVersionedExitPlan(Function &F, VersionedExitPlan &Plan) {
    BasicBlock *Exit = Plan.Exit;
    if (!Exit || (Plan.OriginalPreds.empty() && Plan.ClonePreds.empty()))
        return;
    //1.为每个存活版本创建独立代理块，先在版本边界收集LCSSA值
    BasicBlock *OriginalProxy = nullptr;
    BasicBlock *CloneProxy = nullptr;
    if (!Plan.OriginalPreds.empty()) {
        OriginalProxy = new BasicBlock();
        F.pushBlock(OriginalProxy);
        OriginalProxy->setIdent(F.makeUniqueBlockIdent(
            Exit->getIdent() + ".unswitch.original.exit"));
    }
    if (!Plan.ClonePreds.empty()) {
        CloneProxy = new BasicBlock();
        F.pushBlock(CloneProxy);
        CloneProxy->setIdent(F.makeUniqueBlockIdent(
            Exit->getIdent() + ".unswitch.clone.exit"));
    }
    //2.将两个版本的出口边分别重定向到代理块，预检保证这些边必然存在
    for (BasicBlock *Pred : Plan.OriginalPreds) {
        const bool Redirected =
            BasicBlock::redirectBranchTarget(Pred, Exit, OriginalProxy);
        assert(Redirected && "preflighted original exit edge disappeared");
        (void)Redirected;
    }
    for (BasicBlock *Pred : Plan.ClonePreds) {
        const bool Redirected =
            BasicBlock::redirectBranchTarget(Pred, Exit, CloneProxy);
        assert(Redirected && "preflighted cloned exit edge disappeared");
        (void)Redirected;
    }
    //3.每个旧出口PHI先在代理块内汇总，再在原出口合并两个版本并迁移全部use
    for (VersionedExitPhiPlan &PhiPlan : Plan.PhiPlans) {
        PhiInst *OldPhi = PhiPlan.OldPhi;
        std::vector<PhiInst::IncomingEdge> MergeIncoming;
        if (OriginalProxy) {
            PhiInst *V = buildExitProxyValue(
                OldPhi->getIRType(), PhiPlan.OriginalIncoming, OriginalProxy,
                OldPhi->getIdent() + ".unswitch.original");
            assert(V && "preflighted original exit PHI has no incoming");
            MergeIncoming.push_back({V, OriginalProxy});
        }
        if (CloneProxy) {
            PhiInst *V = buildExitProxyValue(
                OldPhi->getIRType(), PhiPlan.CloneIncoming, CloneProxy,
                OldPhi->getIdent() + ".unswitch.clone");
            assert(V && "preflighted cloned exit PHI has no incoming");
            MergeIncoming.push_back({V, CloneProxy});
        }
        assert(!MergeIncoming.empty() && "versioned exit lost every version");
        if (MergeIncoming.size() == 1) {
            OldPhi->replaceAllUsesWith(MergeIncoming.front().V);
            delete OldPhi;
        } else {
            auto *MergePhi = new PhiInst(OldPhi->getIRType());
            MergePhi->setIdent(OldPhi->getIdent() + ".unswitch.merge");
            for (const auto &In : MergeIncoming)
                MergePhi->addIncoming(In.V, In.Pred);
            OldPhi->replaceAllUsesWith(MergePhi);
            OldPhi->replaceWith(MergePhi);
            delete OldPhi;
        }
    }
    if (OriginalProxy) OriginalProxy->genUnCondInst(Exit);
    if (CloneProxy) CloneProxy->genUnCondInst(Exit);
}
//删除原循环版本中特化后不可达且无外部前驱的死块
void LoopUnswitchPass::eraseDeadOriginalBlocks(const std::vector<BasicBlock *> &AllBlocks,
                             const BlockSet &LiveBlocks) {
    BlockSet Dead;
    for (BasicBlock *BB : AllBlocks)
        if (BB && !LiveBlocks.count(BB)) Dead.insert(BB);
    if (Dead.empty()) return;
    for (BasicBlock *BB : Dead) {
        for (BasicBlock *Succ : BB->getSuccessors()) {
            if (!Succ || Dead.count(Succ)) continue;
            BasicBlock::removeEdgeFromSuccPhis(
                Succ, BB, BB->countEdgesToSucc(Succ));
        }
    }
    for (BasicBlock *BB : AllBlocks) {
        if (BB && Dead.count(BB)) BB->eraseBlockSafely();
    }
}
//提交平凡非交换并修复入口控制流与出口LCSSA值
bool LoopUnswitchPass::transformTrivial(
    Function &F, const TrivialCandidate &C) {
    if (!C.L || !C.Shape.Header || !C.Shape.PreHeader || !C.Parent ||
        !C.Branch || !C.Exit || !C.Continue) {
        return false;
    }
    //1.提交前重新验证预头边和原分支身份，候选过期时保守回退
    BasicBlock *OldPreHeader = C.Shape.PreHeader;
    BasicBlock *Header = C.Shape.Header;
    auto *OldPreBr = OldPreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(OldPreBr) ||
        OldPreBr->getTrueSuccessor() != Header ||
        C.Parent->getBranchTerminator() != C.Branch) {
        return false;
    }
    //2.创建新的继续预头，外提分支仍由原预头执行以保持条件求值位置
    auto *NewPreHeader = new BasicBlock();
    F.pushBlock(NewPreHeader);
    NewPreHeader->setIdent(
        F.makeUniqueBlockIdent(Header->getIdent() + ".unswitch.cont"));
    BasicBlock *ExitDestination = C.Exit;
    const auto ExitPreds = C.Exit->getPredecessors();
    const bool ReuseExit = ExitPreds.size() == 1 &&
                           ExitPreds.front() == C.Parent;
    if (ReuseExit) {
        //3.唯一出口前驱允许直接复用出口，只需把PHI前驱改为执行外提边的旧预头
        for (PhiInst *Phi : C.Exit->collectPhiNodes()) {
            forEachPhiIncoming(
                Phi, [Phi, &C, OldPreHeader](
                         unsigned Index, Value *, BasicBlock *Pred) {
                    if (Pred == C.Parent)
                        Phi->setIncomingBlock(Index, OldPreHeader);
                });
        }
    } else {
        //4.共享出口必须拆出合流块，使循环内值先满足LCSSA再与外提直达值合并
        auto *ExitMerge = new BasicBlock();
        F.pushBlock(ExitMerge);
        ExitMerge->setIdent(
            F.makeUniqueBlockIdent(C.Exit->getIdent() + ".unswitch.merge"));
        ExitDestination = ExitMerge;
        //4.1搬迁终结指令前快照旧后继，提交后统一修复其PHI前驱
        const std::vector<BasicBlock *> OldExitSuccessors =
            C.Exit->getSuccessors();
        std::vector<Instruction *> NonPhis;
        for (Instruction *I : *C.Exit) {
            if (!dynamic_cast<PhiInst *>(I)) NonPhis.push_back(I);
        }
        for (Instruction *I : NonPhis) {
            I->eraseFromParent();
            ExitMerge->pushBack(I);
        }
        std::vector<PhiInst *> OldPhis = C.Exit->collectPhiNodes();
        std::vector<PhiInst *> NormalPhis;
        std::vector<PhiInst *> MergePhis;
        for (PhiInst *OldPhi : OldPhis) {
            Value *DirectValue = nullptr;
            std::vector<PhiInst::IncomingEdge> Remaining;
            for (const auto &In : OldPhi->collectIncomingEdges()) {
                if (In.Pred == C.Parent) DirectValue = In.V;
                else Remaining.push_back(In);
            }
            //4.2即使只剩一条循环前驱也保留NormalPhi，维护显式LCSSA边界
            auto *NormalPhi = new PhiInst(OldPhi->getIRType());
            NormalPhi->setIdent(OldPhi->getIdent() + ".unswitch.normal");
            for (const auto &In : Remaining)
                NormalPhi->addIncoming(In.V, In.Pred);
            NormalPhis.push_back(NormalPhi);
            Value *NormalValue = NormalPhi;
            auto *MergePhi = new PhiInst(OldPhi->getIRType());
            MergePhi->setIdent(OldPhi->getIdent() + ".unswitch.merge");
            MergePhi->addIncoming(DirectValue, OldPreHeader);
            MergePhi->addIncoming(NormalValue, C.Exit);
            MergePhis.push_back(MergePhi);
            OldPhi->replaceAllUsesWith(MergePhi);
            delete OldPhi;
        }
        for (auto It = NormalPhis.rbegin(); It != NormalPhis.rend(); ++It)
            C.Exit->pushFront(*It);
        for (auto It = MergePhis.rbegin(); It != MergePhis.rend(); ++It)
            ExitMerge->pushFront(*It);
        C.Exit->genUnCondInst(ExitMerge);
        BlockSet SeenSuccessors;
        for (BasicBlock *Succ : OldExitSuccessors) {
            if (!Succ || !SeenSuccessors.insert(Succ).second) continue;
            BasicBlock::replaceIncomingPredInSuccPhis(
                Succ, C.Exit, ExitMerge);
        }
    }
    //5.修复Header入口PHI并把原条件分支整体搬到旧预头，保留分支属性和求值语义
    setHeaderPredecessor(Header, OldPreHeader, NewPreHeader);
    NewPreHeader->genUnCondInst(Header);
    const unsigned ExitOperand = C.ExitOnTrue ? 1U : 2U;
    const unsigned ContinueOperand = C.ExitOnTrue ? 2U : 1U;
    C.Branch->replaceOperand(ExitOperand, ExitDestination);
    C.Branch->replaceOperand(ContinueOperand, NewPreHeader);
    C.Branch->eraseFromParent();
    delete OldPreBr;
    OldPreHeader->pushBack(C.Branch);
    C.Parent->genUnCondInst(C.Continue);
    //6.循环内原条件使用在继续版本恒定，替换为对应常量交给后续CFG化简
    BlockSet OriginalLoopBlocks;
    for (BasicBlock *BB : C.L->getBlocksIncludingSubLoops())
        if (BB) OriginalLoopBlocks.insert(BB);
    replaceConditionUsesInBlocks(C.Condition, OriginalLoopBlocks,
                                 !C.ExitOnTrue);
    return true;
}
//将入口已知条件替换到循环全部基本块而不克隆CFG
bool LoopUnswitchPass::transformEntryKnown(const EntryKnownCandidate &C) {
    //4.3entry-known 提交仅把选中循环内的条件 use 替换为已证明布尔常量，后续简化 pass 负责折叠 CFG。
    if (!C.Condition || !C.L) return false;
    BlockSet Blocks;
    for (BasicBlock *BB : C.AllBlocks)
        if (BB) Blocks.insert(BB);
    return replaceConditionUsesInBlocks(C.Condition, Blocks, C.KnownValue);
}
//事务式克隆一侧特化闭包并修复入口、PHI和所有出口
bool LoopUnswitchPass::transformNonTrivial(Function &F, NonTrivialCandidate &C) {
    if (!C.L || !C.Shape.Header || !C.Shape.PreHeader || !C.Parent ||
        !C.Branch || !C.TrueSucc || !C.FalseSucc || !C.OriginalSucc ||
        !C.CloneSucc) {
        return false;
    }
    //6.提交前重新验证 preheader 和候选分支全部仍与发现阶段一致。
    BrInst *OldPreBr = C.Shape.PreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(OldPreBr) ||
        OldPreBr->getTrueSuccessor() != C.Shape.Header ||
        C.Parent->getBranchTerminator() != C.Branch ||
        !BasicBlock::isCondBr(C.Branch) ||
        C.Branch->getCondition() != C.Condition ||
        C.Branch->getTrueSuccessor() != C.TrueSucc ||
        C.Branch->getFalseSuccessor() != C.FalseSucc) {
        return false;
    }
    //6.1先创建两个游离版本 preheader，再只克隆 CloneReachable 闭包；原 CFG 在此阶段尚未改写。
    auto *OriginalPreHeader = new BasicBlock();
    F.pushBlock(OriginalPreHeader);
    OriginalPreHeader->setIdent(F.makeUniqueBlockIdent(
        C.Shape.Header->getIdent() + ".unswitch.original.preheader"));
    auto *ClonedPreHeader = new BasicBlock();
    F.pushBlock(ClonedPreHeader);
    ClonedPreHeader->setIdent(F.makeUniqueBlockIdent(
        C.Shape.Header->getIdent() +
        (C.OriginalValue ? ".unswitch.false.preheader"
                         : ".unswitch.true.preheader")));
    std::vector<BasicBlock *> CloneBlocks;
    for (BasicBlock *BB : C.AllBlocks)
        if (BB && C.CloneReachable.count(BB)) CloneBlocks.push_back(BB);
    ValueToValueMap InitialMap;
    InitialMap[C.Shape.PreHeader] = ClonedPreHeader;
    CloneBlockRegionOptions Options;
    Options.MakeBlockIdent = [&F, &C](const BasicBlock *BB, std::size_t) {
        const std::string Base = BB ? BB->getIdent() : "bb";
        return F.makeUniqueBlockIdent(
            Base + (C.OriginalValue ? ".unswitch.false"
                                    : ".unswitch.true"));
    };
    auto Region = cloneBasicBlockRegion(F, CloneBlocks, InitialMap, Options);
    if (!Region) {
        delete OriginalPreHeader;
        delete ClonedPreHeader;
        return false;
    }
    auto RollbackClones = [&]() {
        //6.1.1PHI 剪枝、特化分支或出口计划失败时，销毁完整克隆区域和两个未连入的 preheader。
        destroyBlocks(Region->ClonedBlocks);
        delete OriginalPreHeader;
        delete ClonedPreHeader;
    };
    BasicBlock *ClonedHeader = Region->getClonedBlock(C.Shape.Header);
    BasicBlock *ClonedParent = Region->getClonedBlock(C.Parent);
    BasicBlock *ClonedChosenSucc = Region->getClonedBlock(C.CloneSucc);
    if (!ClonedHeader || !ClonedParent || !ClonedChosenSucc) {
        RollbackClones();
        return false;
    }
    BrInst *ClonedBranch = ClonedParent->getBranchTerminator();
    if (!BasicBlock::isCondBr(ClonedBranch) ||
        !BasicBlock::replaceCondBrWithUncond(
            ClonedParent, ClonedChosenSucc) ||
        !pruneClonedPhis(*Region, C, ClonedPreHeader)) {
        RollbackClones();
        return false;
    }
    std::vector<VersionedExitPlan> ExitPlans;
    for (BasicBlock *Exit : C.L->getExitBlocks()) {
        auto Plan = planVersionedExit(Exit, C, *Region);
        if (!Plan) {
            RollbackClones();
            return false;
        }
        ExitPlans.push_back(std::move(*Plan));
    }
    //6.5所有可失败的克隆、PHI 剪枝和出口计划已在原 CFG 首次变更前完成；从此处开始集中提交。
    setHeaderPredecessor(C.Shape.Header, C.Shape.PreHeader,
                         OriginalPreHeader);
    OriginalPreHeader->genUnCondInst(C.Shape.Header);
    ClonedPreHeader->genUnCondInst(ClonedHeader);
    BasicBlock::removeEdgeFromSuccPhis(C.CloneSucc, C.Parent, 1);
    const bool SpecializedOriginal =
        BasicBlock::replaceCondBrWithUncond(C.Parent, C.OriginalSucc);
    assert(SpecializedOriginal &&
           "preflighted original unswitch branch disappeared");
    (void)SpecializedOriginal;
    //6.5.1原版本和克隆版本内的条件 use 分别替换为互补常量，两侧 Parent 都已变为无条件分支。
    BlockSet OriginalVersionBlocks = C.OriginalReachable;
    BlockSet ClonedVersionBlocks;
    for (BasicBlock *BB : C.CloneReachable) {
        if (BasicBlock *Mapped = Region->getClonedBlock(BB))
            ClonedVersionBlocks.insert(Mapped);
    }
    replaceConditionUsesInBlocks(C.Condition, OriginalVersionBlocks,
                                 C.OriginalValue);
    replaceConditionUsesInBlocks(C.Condition, ClonedVersionBlocks,
                                 !C.OriginalValue);
    for (VersionedExitPlan &Plan : ExitPlans)
        applyVersionedExitPlan(F, Plan);
    //6.5.2原 preheader 改为按循环不变条件选择版本，最后删除原版本中特化后不可达的块。
    BasicBlock *TruePreHeader =
        C.OriginalValue ? OriginalPreHeader : ClonedPreHeader;
    BasicBlock *FalsePreHeader =
        C.OriginalValue ? ClonedPreHeader : OriginalPreHeader;
    const bool InstalledSelector = C.Shape.PreHeader->replaceTerminator(
        new BrInst(C.Condition, TruePreHeader, FalsePreHeader));
    assert(InstalledSelector && "preflighted loop preheader lost terminator");
    (void)InstalledSelector;
    eraseDeadOriginalBlocks(C.AllBlocks, C.OriginalReachable);
    return true;
}
//运行循环非交换变换并返回IR和CFG修改状态
PassResult LoopUnswitchPass::run(Function &F,
                                 FunctionAnalysisPassManager &FAM) {
    FunctionState &State = States[&F];
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    //1.优先尝试无代码增长的 trivial 外提；成功后记录互斥模式并使全部分析失效。
    if (State.Kind != SelectedKind::NonTrivial) {
        auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F);
        if (auto Candidate = findTrivialCandidate(F, LI, DT, MSSA)) {
            if (transformTrivial(F, *Candidate)) {
                State.Kind = SelectedKind::Trivial;
                return PassResult::changed(PreservationStatus::none());
            }
        }
    }
    //1.1函数一旦选择 trivial 模式，后续固定点轮次仍可继续处理 trivial 候选，但绝不转入代码克隆模式。
    if (State.Kind == SelectedKind::Trivial)
        return PassResult::unchanged();
    //4.entry-known 由 guard 事实驱动且不增长代码，因此一次批量提交所有已证明条件，不占用完整克隆额度。
    bool EntryChanged = false;
    for (const EntryKnownCandidate &Candidate :
         findEntryKnownCandidates(LI, DT)) {
        if (!transformEntryKnown(Candidate)) continue;
        EntryChanged = true;
    }
    if (EntryChanged) {
        State.Kind = SelectedKind::NonTrivial;
        return PassResult::changed(PreservationStatus::none());
    }
    //5.5每个函数最多提交一次完整版本克隆；额度耗尽后的后续轮次仍可使用不克隆的 entry-known 路径。
    if (State.FullCloneCount != 0)
        return PassResult::unchanged();
    auto Candidate = findBestNonTrivialCandidate(F, LI, DT, SE);
    if (!Candidate) return PassResult::unchanged();
    if (!transformNonTrivial(F, *Candidate))
        return PassResult::unchanged();
    State.Kind = SelectedKind::NonTrivial;
    ++State.FullCloneCount;
    return PassResult::changed(PreservationStatus::none());
}
