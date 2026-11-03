// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/LoopLoadElimination.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/LoopDependence.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <unordered_set>
#include <vector>
 
//判断两个load是否产生完全相同形状的标量值
bool LoopLoadEliminationPass::isTypeCompatible(
    LoadInst *First, LoadInst *Second) {
    if (!First || !Second || !First->getIRType() ||
        !Second->getIRType()) {
        return false;
    }
    return First->getIRType()->isScalar() &&
        First->getIRType()->getSize() <= 8 &&
        First->getIRType()->hasSameShape(*Second->getIRType());
}

//判断值能否在预头终结指令前直接使用
bool LoopLoadEliminationPass::isAvailableAtPreheader(
    Value *V, BasicBlock *PreHeader,
    const DominatorTree &DT) {
    if (!V || !PreHeader || !PreHeader->back()) return false;
    if (dynamic_cast<ConstantSystem *>(V) || V->isParam() ||
        V->isGlobal()) {
        return true;
    }
    auto *Definition = dynamic_cast<Instruction *>(V);
    return Definition && Definition->getParent() &&
        DT.dominatesValue(Definition, PreHeader->back());
}

//判断一条潜在写指令是否可证明不会修改候选位置
bool LoopLoadEliminationPass::mayModifyLocation(
    const MemoryLocation &Location, Instruction *LocationContext,
    Instruction *I, BasicAAResult &AA, SCEVResult &SE,
    RangeInfo &RI, const DominatorTree &DT, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    if (!Location.Pointer || !LocationContext || !I) return true;
    ModRefInfo MRI = getInstModRefInfo(Location, I, AA, LI, FAM);
    if (!isModSet(MRI)) return false;
    auto *Store = dynamic_cast<StoreInst *>(I);
    if (!Store) return true;
    const MemoryLocation StoreLocation = MemoryLocation::get(Store);
    if (AA.aliasWithIndexRange(Location, StoreLocation, LI) ==
        AliasInfo::NoAlias) {
        return false;
    }
    //不同对象只能在两次真实访问都已于各自程序点证明在界时升级为NoAlias；
    //这一步显式拒绝借助动态越界或测试输入范围获得性能。
    if (!AA.haveKnownDistinctUnderlyingObjects(
            Location.Pointer, StoreLocation.Pointer)) {
        return true;
    }
    if (!AA.isLocationProvablyInBoundsAt(
            Location, LocationContext, SE, RI, DT) ||
        !AA.isLocationProvablyInBoundsAt(
            StoreLocation, Store, SE, RI, DT)) {
        return true;
    }
    return false;
}

//反向收集仍可能到达目标块的CFG区域
bool LoopLoadEliminationPass::collectBlocksReachingTarget(
    BasicBlock *Target,
    std::unordered_set<BasicBlock *> &Blocks,
    BasicBlock *StopAt) {
    if (!Target) return false;
    std::vector<BasicBlock *> Worklist{Target};
    while (!Worklist.empty()) {
        BasicBlock *BB = Worklist.back();
        Worklist.pop_back();
        if (!BB || !Blocks.insert(BB).second) continue;
        if (Blocks.size() > MaxReachableBlocks) return false;
        //回到seed表示下一次动态seed已经刷新内存版本，不应把更早迭代
        //的慢路径写入算作当前seed到target之间的写。
        if (BB == StopAt) continue;
        for (BasicBlock *Pred : BB->getPredecessors())
            if (Pred && !Blocks.count(Pred)) Worklist.push_back(Pred);
    }
    return true;
}

//证明支配seed到目标load的每条可达路径都没有相关写入
bool LoopLoadEliminationPass::hasNoClobberOnPaths(
    LoadInst *Seed, LoadInst *Target, MemorySSAResult &MSSA,
    BasicAAResult &AA, SCEVResult &SE, RangeInfo &RI,
    const DominatorTree &DT, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    if (!Seed || !Target || !Seed->getParent() ||
        !Target->getParent() || !MSSA.getMemoryAccess(Seed) ||
        !MSSA.getMemoryAccess(Target)) {
        return false;
    }
    BasicBlock *SeedBlock = Seed->getParent();
    BasicBlock *TargetBlock = Target->getParent();
    if (!DT.dominatesValue(Seed, Target)) return false;
    const MemoryLocation Location = MemoryLocation::get(Target);
    //支配只保证第一次到达目标时经过seed。若目标位于seed之外的内层循环，
    //目标之后的写仍可能沿回边影响下一次执行；逐层检查整环，直到seed也属于
    //该循环，避免把一次前向透明路径误当成跨迭代不变内存版本。
    for (Loop *L = LI.getLoopFor(TargetBlock);
         L && !L->contains(SeedBlock); L = L->getParentLoop()) {
        if (!hasNoLoopClobber(
                Location, Target, *L, MSSA, AA, SE, RI,
                DT, LI, FAM)) {
            return false;
        }
    }
    std::unordered_set<BasicBlock *> CanReachTarget;
    if (!collectBlocksReachingTarget(
            TargetBlock, CanReachTarget, SeedBlock) ||
        !CanReachTarget.count(SeedBlock)) {
        return false;
    }
    std::vector<BasicBlock *> Worklist{SeedBlock};
    std::unordered_set<BasicBlock *> Visited;
    std::size_t AccessCount = 0;
    bool ReachedTarget = false;
    while (!Worklist.empty()) {
        BasicBlock *BB = Worklist.back();
        Worklist.pop_back();
        if (!BB || !Visited.insert(BB).second) continue;
        if (Visited.size() > MaxReachableBlocks) return false;
        for (MemoryAccess *Access : MSSA.getBlockAccesses(BB)) {
            Instruction *I = Access ? Access->getInstruction() : nullptr;
            if (!I || I == Seed || I == Target) continue;
            bool InInterval = true;
            if (BB == SeedBlock && !Instruction::comesBefore(Seed, I))
                InInterval = false;
            if (BB == TargetBlock && !Instruction::comesBefore(I, Target))
                InInterval = false;
            if (!InInterval) continue;
            if (++AccessCount > MaxMemoryAccesses ||
                mayModifyLocation(
                    Location, Target, I, AA, SE, RI, DT, LI, FAM)) {
                return false;
            }
        }
        if (BB == TargetBlock) {
            ReachedTarget = true;
            continue;
        }
        for (BasicBlock *Succ : BB->getSuccessors()) {
            if (Succ && CanReachTarget.count(Succ) &&
                !Visited.count(Succ)) {
                Worklist.push_back(Succ);
            }
        }
    }
    return ReachedTarget;
}

//寻找能够在循环路径上保持同一内存版本的支配load
LoadInst *LoopLoadEliminationPass::findReusableSeed(
    LoadInst *Target, const std::vector<LoadInst *> &Loads,
    MemorySSAResult &MSSA, BasicAAResult &AA, SCEVResult &SE,
    RangeInfo &RI, const DominatorTree &DT, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    if (!Target || !Target->getParent()) return nullptr;
    Value *Pointer = Target->getPointerOperand();
    //未定义值派生的地址每次执行都可选择不同位模式，不能把两次load
    //仅因SSA地址相同就冻结成同一个结果。
    if (!Pointer || valueMayDependOnUndef(Pointer)) return nullptr;
    std::size_t Compared = 0;
    for (auto It = Loads.rbegin(); It != Loads.rend(); ++It) {
        LoadInst *Seed = *It;
        if (!Seed || Seed == Target || !Seed->getParent() ||
            Seed->getParent() == Target->getParent() ||
            Seed->getPointerOperand() != Pointer ||
            !isTypeCompatible(Seed, Target) ||
            !DT.dominatesValue(Seed, Target)) {
            continue;
        }
        if (Compared++ >= MaxSeedComparisons) break;
        if (hasNoClobberOnPaths(
                Seed, Target, MSSA, AA, SE, RI, DT, LI, FAM)) {
            return Seed;
        }
    }
    return nullptr;
}

//提交一条不新增访存的跨循环load复用
bool LoopLoadEliminationPass::eliminateOneDominatedLoad(
    Function &F, MemorySSAResult &MSSA, BasicAAResult &AA,
    SCEVResult &SE, RangeInfo &RI, const DominatorTree &DT,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM) {
    std::vector<LoadInst *> Loads = collectInstructions<LoadInst>(F);
    for (LoadInst *Target : Loads) {
        if (!Target || !Target->getParent() ||
            !LI.getLoopFor(Target->getParent())) {
            continue;
        }
        LoadInst *Seed = findReusableSeed(
            Target, Loads, MSSA, AA, SE, RI, DT, LI, FAM);
        if (!Seed) continue;
        Target->replaceAllUsesWith(Seed);
        delete Target;
        return true;
    }
    return false;
}

//证明循环及其子循环不存在会修改候选位置的内存访问
bool LoopLoadEliminationPass::hasNoLoopClobber(
    const MemoryLocation &Location, Instruction *LocationContext,
    Loop &L, MemorySSAResult &MSSA, BasicAAResult &AA,
    SCEVResult &SE, RangeInfo &RI, const DominatorTree &DT,
    const LoopInfo &LI, FunctionAnalysisPassManager &FAM) {
    std::size_t AccessCount = 0;
    for (BasicBlock *BB : L.getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (MemoryAccess *Access : MSSA.getBlockAccesses(BB)) {
            Instruction *I = Access ? Access->getInstruction() : nullptr;
            if (!I || I == LocationContext) continue;
            if (++AccessCount > MaxMemoryAccesses ||
                mayModifyLocation(
                    Location, LocationContext, I, AA, SE, RI,
                    DT, LI, FAM)) {
                return false;
            }
        }
    }
    return true;
}

//判断循环是否已经属于本pass生成的快版本或慢版本
bool LoopLoadEliminationPass::isGeneratedVersionLoop(Loop &L) {
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (L.getLoopSimplifyForm(PreHeader, Latch) && PreHeader) {
        const std::string &Name = PreHeader->getIdent();
        if (Name.find(".lle.fast.preheader") != std::string::npos ||
            Name.find(".lle.slow.preheader") != std::string::npos) {
            return true;
        }
    }
    for (BasicBlock *BB : L.getBlocksIncludingSubLoops()) {
        if (!BB) continue;
        const std::string &Name = BB->getIdent();
        if (Name.find(".lle.fast") != std::string::npos ||
            Name.find(".lle.slow") != std::string::npos) {
            return true;
        }
    }
    return false;
}

//用带类型GEP、对象大小和访问宽度计算该位置允许的最大共同边界
bool LoopLoadEliminationPass::accumulateLocationSafeBound(
    const MemoryLocation &Location, Instruction *LocationContext,
    BasicAAResult &AA, const DominatorTree &DT,
    const LoopInfo &LI, SCEVResult &SE, Value *&SharedBound,
    std::int64_t &SafeBound) {
    return AA.accumulateLocationSafeUpperBound(
        Location, LocationContext, DT, LI, SE,
        SharedBound, SafeBound);
}

//在给定候选位置下判断指令是否为静态无关写或可由共同边界守卫的写
bool LoopLoadEliminationPass::acceptVersionedMemoryInstruction(
    const MemoryLocation &Location, Instruction *I,
    BasicAAResult &AA, SCEVResult &SE,
    const DominatorTree &DT, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM, Value *&SharedBound,
    std::int64_t &SafeBound, bool &NeedsGuard) {
    if (!I) return false;
    ModRefInfo MRI = getInstModRefInfo(Location, I, AA, LI, FAM);
    if (!isModSet(MRI)) return true;
    auto *Store = dynamic_cast<StoreInst *>(I);
    if (!Store) return false;
    MemoryLocation StoreLocation = MemoryLocation::get(Store);
    if (AA.aliasWithIndexRange(Location, StoreLocation, LI) ==
        AliasInfo::NoAlias) {
        return true;
    }
    if (!AA.haveKnownDistinctUnderlyingObjects(
            Location.Pointer, StoreLocation.Pointer) ||
        !accumulateLocationSafeBound(
            StoreLocation, Store, AA, DT, LI, SE,
            SharedBound, SafeBound)) {
        return false;
    }
    NeedsGuard = true;
    return true;
}

//寻找一个小型最内层、无调用且可由单一共同边界保护的版本化候选
std::optional<LoopLoadEliminationPass::VersionCandidate>
LoopLoadEliminationPass::findVersionCandidate(
    Function &F, BasicAAResult &AA, SCEVResult &SE,
    const DominatorTree &DT, const LoopInfo &LI,
    FunctionAnalysisPassManager &FAM) {
    std::size_t ExistingVersions = 0;
    for (BasicBlock *BB : F) {
        if (BB && BB->getIdent().find(".lle.slow.header") !=
                      std::string::npos) {
            ++ExistingVersions;
        }
    }
    if (ExistingVersions >= MaxVersionsPerFunction) return std::nullopt;
    std::vector<LoadInst *> Loads = collectInstructions<LoadInst>(F);
    for (Loop *L : LI.getLoopsInPostOrder()) {
        if (!L || !L->isInnermost() || isGeneratedVersionLoop(*L) ||
            !LCSSAPass::isLCSSAForm(L)) {
            continue;
        }
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Latch = nullptr;
        if (!L->getHeader()) {
            continue;
        }
        if (!L->getLoopSimplifyForm(PreHeader, Latch)) {
            PreHeader = nullptr;
            Latch = nullptr;
            for (BasicBlock *Pred : L->getHeader()->getPredecessors()) {
                if (!Pred) continue;
                if (L->contains(Pred)) {
                    if (Latch) {
                        Latch = nullptr;
                        break;
                    }
                    Latch = Pred;
                } else {
                    if (PreHeader) {
                        PreHeader = nullptr;
                        break;
                    }
                    PreHeader = Pred;
                }
            }
            if (!PreHeader || !Latch ||
                PreHeader->countEdgesToSucc(L->getHeader()) != 1 ||
                Latch->countEdgesToSucc(L->getHeader()) != 1) {
                continue;
            }
            bool DedicatedExits = true;
            for (BasicBlock *Exit : L->getExitBlocks())
                for (BasicBlock *Pred : Exit->getPredecessors())
                    if (!Pred || !L->contains(Pred))
                        DedicatedExits = false;
            if (!DedicatedExits) continue;
        }
        if (!PreHeader || !Latch || !PreHeader->back() ||
            PreHeader->countEdgesToSucc(L->getHeader()) != 1) {
            continue;
        }
        std::vector<BasicBlock *> Blocks = L->getBlocksInFunctionOrder();
        std::vector<BasicBlock *> Exits = L->getExitBlocks();
        if (Blocks.empty() || Blocks.size() > MaxVersionBlocks ||
            Exits.size() != 1 || !Exits.front()) {
            continue;
        }
        bool ShapeValid = true;
        std::size_t InstructionCount = 0;
        for (BasicBlock *BB : Blocks) {
            if (!BB || BB->countEdgesToSucc(PreHeader) != 0) {
                ShapeValid = false;
                break;
            }
            for (Instruction *I : *BB) {
                if (!I || dynamic_cast<CallInst *>(I)) {
                    ShapeValid = false;
                    break;
                }
                ++InstructionCount;
            }
            if (!ShapeValid ||
                InstructionCount > MaxVersionInstructions) {
                ShapeValid = false;
                break;
            }
        }
        if (!ShapeValid) continue;
        for (PhiInst *Phi : L->getHeader()->collectPhiNodes()) {
            if (!Phi || Phi->countIncomingFromPred(PreHeader) != 1) {
                ShapeValid = false;
                break;
            }
        }
        if (!ShapeValid) continue;
        for (LoadInst *Target : Loads) {
            if (!Target || !Target->getParent() ||
                !L->contains(Target->getParent()) ||
                !Target->hasUses() || !Target->getPointerOperand() ||
                !LoopInfo::isLoopInvariant(
                    Target->getPointerOperand(), L)) {
                continue;
            }
            LoadInst *Seed = nullptr;
            for (auto It = Loads.rbegin(); It != Loads.rend(); ++It) {
                LoadInst *Current = *It;
                if (!Current || Current == Target ||
                    !Current->getParent() ||
                    L->contains(Current->getParent()) ||
                    Current->getPointerOperand() !=
                        Target->getPointerOperand() ||
                    !isTypeCompatible(Current, Target) ||
                    !DT.dominatesValue(Current, Target)) {
                    continue;
                }
                BasicBlock *SeedBlock = Current->getParent();
                const auto Preds = PreHeader->getPredecessors();
                if (SeedBlock != PreHeader &&
                    !(Preds.size() == 1 && Preds.front() == SeedBlock &&
                      SeedBlock->countEdgesToSucc(PreHeader) == 1)) {
                    continue;
                }
                Seed = Current;
                break;
            }
            if (!Seed) continue;
            bool IntervalClean = true;
            for (Instruction *I : *Seed->getParent()) {
                if (!I || I == Seed ||
                    !Instruction::comesBefore(Seed, I)) {
                    continue;
                }
                if (dynamic_cast<CallInst *>(I) ||
                    isModSet(getInstModRefInfo(
                        MemoryLocation::get(Target), I, AA, LI, FAM))) {
                    IntervalClean = false;
                    break;
                }
            }
            if (!IntervalClean) continue;
            if (Seed->getParent() != PreHeader) {
                for (Instruction *I : *PreHeader) {
                    if (!I || dynamic_cast<CallInst *>(I) ||
                        isModSet(getInstModRefInfo(
                            MemoryLocation::get(Target), I,
                            AA, LI, FAM))) {
                        IntervalClean = false;
                        break;
                    }
                }
            }
            if (!IntervalClean) continue;
            MemoryLocation Location = MemoryLocation::get(Target);
            Value *SharedBound = nullptr;
            std::int64_t SafeBound =
                std::numeric_limits<int32_t>::max();
            if (!accumulateLocationSafeBound(
                    Location, Target, AA, DT, LI, SE,
                    SharedBound, SafeBound)) {
                continue;
            }
            bool NeedsGuard = true;
            for (BasicBlock *BB : Blocks) {
                for (Instruction *I : *BB) {
                    if (!acceptVersionedMemoryInstruction(
                            Location, I, AA, SE, DT, LI, FAM,
                            SharedBound, SafeBound, NeedsGuard)) {
                        ShapeValid = false;
                        break;
                    }
                }
                if (!ShapeValid) break;
            }
            if (!ShapeValid || !NeedsGuard || !SharedBound ||
                SafeBound < 1 ||
                !IRType::hasIntegerBitWidth(
                    SharedBound->getIRType(), 32) ||
                !isAvailableAtPreheader(SharedBound, PreHeader, DT)) {
                ShapeValid = true;
                continue;
            }
            VersionCandidate Candidate;
            Candidate.L = L;
            Candidate.PreHeader = PreHeader;
            Candidate.Header = L->getHeader();
            Candidate.Latch = Latch;
            Candidate.Seed = Seed;
            Candidate.Target = Target;
            Candidate.Bound = SharedBound;
            Candidate.SafeBound = SafeBound;
            Candidate.Blocks = std::move(Blocks);
            Candidate.Exits = std::move(Exits);
            return Candidate;
        }
    }
    return std::nullopt;
}

//销毁尚未接入原CFG的克隆块和入口块
void LoopLoadEliminationPass::destroyDetachedBlocks(
    std::vector<BasicBlock *> Blocks) {
    std::unordered_set<BasicBlock *> Seen;
    std::vector<BasicBlock *> Unique;
    for (BasicBlock *BB : Blocks)
        if (BB && Seen.insert(BB).second) Unique.push_back(BB);
    for (BasicBlock *BB : Unique)
        for (Instruction *I : *BB)
            if (I) I->clearOperands();
    for (BasicBlock *BB : Unique) BB->clear();
    for (BasicBlock *BB : Unique) delete BB;
}

//在出口代理块中建立显式LCSSA汇总PHI
PhiInst *LoopLoadEliminationPass::buildExitProxyPhi(
    const std::shared_ptr<IRType> &Ty,
    const std::vector<PhiInst::IncomingEdge> &Incoming,
    BasicBlock *Proxy, const std::string &Name) {
    if (!Ty || Incoming.empty() || !Proxy) return nullptr;
    auto *Phi = new PhiInst(Ty);
    Phi->setIdent(Name);
    for (const auto &Edge : Incoming)
        Phi->addIncoming(Edge.V, Edge.Pred);
    Proxy->pushFront(Phi);
    return Phi;
}

//在克隆后、提交前离线验证并规划一个共享出口
std::optional<LoopLoadEliminationPass::VersionedExitPlan>
LoopLoadEliminationPass::planVersionedExit(
    BasicBlock *Exit, const VersionCandidate &Candidate,
    ClonedBlockRegion &Region) {
    if (!Exit || !Candidate.L) return std::nullopt;
    VersionedExitPlan Plan;
    Plan.Exit = Exit;
    for (BasicBlock *Source : Candidate.Blocks) {
        if (!Source || Source->countEdgesToSucc(Exit) == 0) continue;
        if (Source->countEdgesToSucc(Exit) != 1)
            return std::nullopt;
        BasicBlock *Clone = Region.getClonedBlock(Source);
        if (!Clone || Clone->countEdgesToSucc(Exit) != 1)
            return std::nullopt;
        Plan.FastPreds.push_back(Source);
        Plan.SlowPreds.push_back(Clone);
    }
    if (Plan.FastPreds.empty() ||
        Plan.FastPreds.size() != Plan.SlowPreds.size()) {
        return std::nullopt;
    }
    for (PhiInst *OldPhi : Exit->collectPhiNodes()) {
        if (!OldPhi ||
            OldPhi->getNumIncomingValues() != Plan.FastPreds.size()) {
            return std::nullopt;
        }
        VersionedExitPhiPlan PhiPlan;
        PhiPlan.OldPhi = OldPhi;
        for (BasicBlock *FastPred : Plan.FastPreds) {
            if (!FastPred ||
                OldPhi->countIncomingFromPred(FastPred) != 1) {
                return std::nullopt;
            }
            Value *FastValue = OldPhi->getIncomingFromPred(FastPred);
            if (!FastValue) return std::nullopt;
            auto *Definition = dynamic_cast<Instruction *>(FastValue);
            if (Definition && Definition->getParent() == Exit)
                return std::nullopt;
            Value *SlowValue = FastValue;
            if (Definition && Definition->getParent() &&
                Candidate.L->contains(Definition->getParent())) {
                auto It = Region.VMap.find(FastValue);
                if (It == Region.VMap.end() || !It->second)
                    return std::nullopt;
                SlowValue = It->second;
            }
            if (!SlowValue->getIRType() || !FastValue->getIRType() ||
                !SlowValue->getIRType()->hasSameShape(
                    *FastValue->getIRType())) {
                return std::nullopt;
            }
            BasicBlock *SlowPred = Region.getClonedBlock(FastPred);
            if (!SlowPred) return std::nullopt;
            PhiPlan.FastIncoming.push_back({FastValue, FastPred});
            PhiPlan.SlowIncoming.push_back({SlowValue, SlowPred});
        }
        Plan.PhiPlans.push_back(std::move(PhiPlan));
    }
    return Plan;
}

//提交一个已完整预检的共享出口双代理计划
void LoopLoadEliminationPass::applyVersionedExitPlan(
    Function &F, VersionedExitPlan &Plan) {
    assert(Plan.Exit && !Plan.FastPreds.empty() &&
           !Plan.SlowPreds.empty());
    auto *FastProxy = new BasicBlock();
    auto *SlowProxy = new BasicBlock();
    F.pushBlock(FastProxy);
    F.pushBlock(SlowProxy);
    FastProxy->setIdent(F.makeUniqueBlockIdent(
        Plan.Exit->getIdent() + ".lle.fast.exit"));
    SlowProxy->setIdent(F.makeUniqueBlockIdent(
        Plan.Exit->getIdent() + ".lle.slow.exit"));
    for (BasicBlock *Pred : Plan.FastPreds) {
        const bool Redirected = BasicBlock::redirectBranchTarget(
            Pred, Plan.Exit, FastProxy);
        assert(Redirected);
        (void)Redirected;
    }
    for (BasicBlock *Pred : Plan.SlowPreds) {
        const bool Redirected = BasicBlock::redirectBranchTarget(
            Pred, Plan.Exit, SlowProxy);
        assert(Redirected);
        (void)Redirected;
    }
    for (VersionedExitPhiPlan &PhiPlan : Plan.PhiPlans) {
        PhiInst *OldPhi = PhiPlan.OldPhi;
        PhiInst *FastValue = buildExitProxyPhi(
            OldPhi->getIRType(), PhiPlan.FastIncoming, FastProxy,
            OldPhi->getIdent() + ".lle.fast");
        PhiInst *SlowValue = buildExitProxyPhi(
            OldPhi->getIRType(), PhiPlan.SlowIncoming, SlowProxy,
            OldPhi->getIdent() + ".lle.slow");
        assert(FastValue && SlowValue);
        auto *Merge = new PhiInst(OldPhi->getIRType());
        Merge->setIdent(OldPhi->getIdent() + ".lle.merge");
        Merge->addIncoming(FastValue, FastProxy);
        Merge->addIncoming(SlowValue, SlowProxy);
        OldPhi->replaceAllUsesWith(Merge);
        OldPhi->replaceWith(Merge);
        delete OldPhi;
    }
    FastProxy->genUnCondInst(Plan.Exit);
    SlowProxy->genUnCondInst(Plan.Exit);
}

//事务式建立快慢最内层循环版本和显式范围守卫
bool LoopLoadEliminationPass::versionOneGuardedLoop(
    Function &F, const VersionCandidate &Candidate) {
    if (!Candidate.L || !Candidate.PreHeader || !Candidate.Header ||
        !Candidate.Latch || !Candidate.Seed || !Candidate.Target ||
        !Candidate.Bound || Candidate.SafeBound < 1 ||
        Candidate.Blocks.empty() || Candidate.Exits.size() != 1) {
        return false;
    }
    BrInst *OldBranch = Candidate.PreHeader->getBranchTerminator();
    if (!OldBranch ||
        Candidate.PreHeader->countEdgesToSucc(Candidate.Header) != 1)
        return false;
    const bool DedicatedPreHeader =
        BasicBlock::isUncondBr(OldBranch) &&
        OldBranch->getTrueSuccessor() == Candidate.Header;
    for (PhiInst *Phi : Candidate.Header->collectPhiNodes())
        if (!Phi ||
            Phi->countIncomingFromPred(Candidate.PreHeader) != 1)
            return false;

    auto *UpperGuard = new BasicBlock();
    BasicBlock *PositiveGuard = DedicatedPreHeader
        ? nullptr : new BasicBlock();
    auto *FastPreHeader = new BasicBlock();
    auto *SlowPreHeader = new BasicBlock();
    F.pushBlock(UpperGuard);
    if (PositiveGuard) F.pushBlock(PositiveGuard);
    F.pushBlock(FastPreHeader);
    F.pushBlock(SlowPreHeader);
    UpperGuard->setIdent(F.makeUniqueBlockIdent(
        Candidate.Header->getIdent() + ".lle.upper.guard"));
    if (PositiveGuard) {
        PositiveGuard->setIdent(F.makeUniqueBlockIdent(
            Candidate.Header->getIdent() + ".lle.positive.guard"));
    }
    FastPreHeader->setIdent(F.makeUniqueBlockIdent(
        Candidate.Header->getIdent() + ".lle.fast.preheader"));
    SlowPreHeader->setIdent(F.makeUniqueBlockIdent(
        Candidate.Header->getIdent() + ".lle.slow.preheader"));

    ValueToValueMap InitialMap;
    InitialMap[Candidate.PreHeader] = SlowPreHeader;
    CloneBlockRegionOptions Options;
    Options.MakeBlockIdent = [&F, &Candidate](
                                 const BasicBlock *BB, std::size_t) {
        const char *Suffix = BB == Candidate.Header
            ? ".lle.slow.header" : ".lle.slow";
        return F.makeUniqueBlockIdent(
            (BB ? BB->getIdent() : "bb") + Suffix);
    };
    auto Region = cloneBasicBlockRegion(
        F, Candidate.Blocks, InitialMap, Options);
    if (!Region) {
        std::vector<BasicBlock *> Blocks{
            UpperGuard, FastPreHeader, SlowPreHeader};
        if (PositiveGuard) Blocks.push_back(PositiveGuard);
        destroyDetachedBlocks(std::move(Blocks));
        return false;
    }
    auto Rollback = [&]() {
        std::vector<BasicBlock *> Blocks = Region->ClonedBlocks;
        Blocks.push_back(UpperGuard);
        if (PositiveGuard) Blocks.push_back(PositiveGuard);
        Blocks.push_back(FastPreHeader);
        Blocks.push_back(SlowPreHeader);
        destroyDetachedBlocks(std::move(Blocks));
    };
    BasicBlock *SlowHeader = Region->getClonedBlock(Candidate.Header);
    auto *SlowTarget = dynamic_cast<LoadInst *>(
        Region->getClonedInstruction(Candidate.Target));
    if (!SlowHeader || !SlowTarget) {
        Rollback();
        return false;
    }
    for (PhiInst *Phi : SlowHeader->collectPhiNodes()) {
        if (!Phi || Phi->countIncomingFromPred(SlowPreHeader) != 1 ||
            Phi->countIncomingFromPred(Candidate.PreHeader) != 0) {
            Rollback();
            return false;
        }
    }
    std::vector<VersionedExitPlan> ExitPlans;
    for (BasicBlock *Exit : Candidate.Exits) {
        auto Plan = planVersionedExit(Exit, Candidate, *Region);
        if (!Plan) {
            Rollback();
            return false;
        }
        ExitPlans.push_back(std::move(*Plan));
    }
    Value *Zero = Value::makeIntegerLike(Candidate.Bound, 0);
    Value *Limit = Value::makeIntegerLike(
        Candidate.Bound, Candidate.SafeBound);
    if (!Zero || !Limit) {
        Rollback();
        return false;
    }
    auto *WithinBound = new BinaryInst(
        Candidate.Bound, BinaryInst::LE, Limit);
    UpperGuard->pushBack(WithinBound);
    UpperGuard->genCondInst(
        WithinBound, FastPreHeader, SlowPreHeader);
    FastPreHeader->genUnCondInst(Candidate.Header);
    SlowPreHeader->genUnCondInst(SlowHeader);
    auto *Positive = new BinaryInst(
        Candidate.Bound, BinaryInst::G, Zero);
    if (PositiveGuard) {
        PositiveGuard->pushBack(Positive);
        PositiveGuard->genCondInst(
            Positive, UpperGuard, SlowPreHeader);
    }

    //从这里开始只执行已经预检的不可失败提交，不再查询任何旧分析结果。
    for (PhiInst *Phi : Candidate.Header->collectPhiNodes()) {
        const bool Replaced = PhiInst::replaceIncomingPred(
            Phi, Candidate.PreHeader, FastPreHeader);
        assert(Replaced);
        (void)Replaced;
    }
    for (VersionedExitPlan &Plan : ExitPlans)
        applyVersionedExitPlan(F, Plan);
    if (DedicatedPreHeader) {
        const bool Inserted =
            Candidate.PreHeader->insertBeforeTerminator(Positive);
        assert(Inserted);
        (void)Inserted;
        const bool Selected = Candidate.PreHeader->replaceTerminator(
            new BrInst(Positive, UpperGuard, SlowPreHeader));
        assert(Selected);
        (void)Selected;
    } else {
        const bool Selected = BasicBlock::redirectBranchTarget(
            Candidate.PreHeader, Candidate.Header, PositiveGuard);
        assert(Selected);
        (void)Selected;
    }
    //出口代理已经消费完旧live-out快照，此时再把快版本目标load替换为支配
    //seed；慢版本保留克隆load，故边界守卫失败时仍执行完整原始访存序列。
    Candidate.Target->replaceAllUsesWith(Candidate.Seed);
    delete Candidate.Target;
    return true;
}

//构造只保留CFG结构分析的失效声明
PreservationStatus LoopLoadEliminationPass::changedPreservation() {
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<MemorySSAAnalysis>();
    PA.discard<SideEffectAnalysis>();
    PA.discard<SCEVAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<LoopDependenceAnalysis>();
    PA.discard<LoopAccessAnalysis>();
    return PA;
}

//在函数中提交至多一次具有稳定QEMU收益证据的load复用，供有界固定点安全重建分析
PreservationStatus LoopLoadEliminationPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    //先消费已有支配load；失败时才为可由对象大小推导边界的小循环建立快慢版本。
    if (eliminateOneDominatedLoad(
            F, MSSA, AA, SE, RI, DT, LI, FAM)) {
        return changedPreservation();
    }
    auto Candidate = findVersionCandidate(
        F, AA, SE, DT, LI, FAM);
    if (Candidate && versionOneGuardedLoop(F, *Candidate))
        return PreservationStatus::none();
    return PreservationStatus::all();
}
