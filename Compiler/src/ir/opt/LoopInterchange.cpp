#include "ir/opt/LoopInterchange.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopDependence.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/MemorySSA.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/CFG.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

// 2.2/3.1 递归检查循环域控制切片：规范 PHI 是回边边界，其起点、更新和退出比较仍须逐项排除 undef。
bool controlSliceContainsUndef(
    Value *Root, const std::unordered_set<Value *> &CanonicalPhis,
    std::unordered_set<Value *> &Active,
    std::unordered_set<Value *> &KnownSafe, unsigned Depth = 0) {
    if (!Root || Depth >= 64) return true;
    if (CanonicalPhis.count(Root)) return false;
    if (Root->isUndef()) return true;
    if (dynamic_cast<BasicBlock *>(Root)) return false;

    auto *I = dynamic_cast<Instruction *>(Root);
    if (!I) return false;
    if (KnownSafe.count(Root)) return false;
    if (!Active.insert(Root).second) return true;

    bool ContainsUndef = false;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        Value *Operand = I->getOperand(Index);
        if (dynamic_cast<BasicBlock *>(Operand)) continue;
        if (controlSliceContainsUndef(
                Operand, CanonicalPhis, Active, KnownSafe, Depth + 1)) {
            ContainsUndef = true;
            break;
        }
    }
    Active.erase(Root);
    if (!ContainsUndef) KnownSafe.insert(Root);
    return ContainsUndef;
}

bool loopDomainContainsUndef(Loop *L, SCEVResult &SE) {
    struct DomainControl {
        PhiInst *Phi = nullptr;
        Value *Start = nullptr;
        Value *Update = nullptr;
        Value *ExitCmp = nullptr;
    };

    std::vector<DomainControl> Domains;
    std::unordered_set<Value *> CanonicalPhis;
    for (Loop *Current = L; Current; Current = Current->getParentLoop()) {
        auto Desc = SE.getCanonicalInduction(Current);
        auto Trip = SE.analyzeLoopTripCount(Current);
        if (!Desc || !Trip || !Desc->Valid || !Trip->Valid ||
            !Desc->Phi || !Desc->StartValue || !Desc->UpdateValue ||
            !Trip->ExitCmp) {
            return true;
        }
        Domains.push_back(
            {Desc->Phi, Desc->StartValue, Desc->UpdateValue,
             Trip->ExitCmp});
        CanonicalPhis.insert(Desc->Phi);
    }

    std::unordered_set<Value *> Active;
    std::unordered_set<Value *> KnownSafe;
    for (const DomainControl &Domain : Domains) {
        if (controlSliceContainsUndef(
                Domain.Start, CanonicalPhis, Active, KnownSafe) ||
            controlSliceContainsUndef(
                Domain.Update, CanonicalPhis, Active, KnownSafe) ||
            controlSliceContainsUndef(
                Domain.ExitCmp, CanonicalPhis, Active, KnownSafe)) {
            return true;
        }
    }
    return false;
}

}

bool LoopInterchangePass::expressionKeysEqual(
    const SCEVExpr *A, const SCEVExpr *B) {
    return A && B && !A->Key.empty() && A->Key == B->Key;
}

unsigned LoopInterchangePass::stridePenalty(int64_t Stride) {
    uint64_t Magnitude = Stride < 0
        ? static_cast<uint64_t>(-(Stride + 1)) + 1
        : static_cast<uint64_t>(Stride);
    unsigned Penalty = 0;
    while (Magnitude != 0) {
        ++Penalty;
        Magnitude >>= 1;
    }
    return Penalty;
}

bool LoopInterchangePass::isInstructionInBlocks(
    Instruction *I, const std::unordered_set<BasicBlock *> &Blocks) {
    return I && I->getParent() && Blocks.count(I->getParent()) != 0;
}

void LoopInterchangePass::destroyDetachedInstructions(
    std::vector<Instruction *> &Instructions) {
    for (Instruction *I : Instructions)
        if (I) I->clearOperands();
    for (Instruction *I : Instructions)
        delete I;
    Instructions.clear();
}

std::size_t LoopInterchangePass::HeaderPairHash::operator()(
    const HeaderPair &Pair) const {
    std::size_t A = std::hash<BasicBlock *>{}(Pair.Outer);
    std::size_t B = std::hash<BasicBlock *>{}(Pair.Inner);
    return A ^ (B + 0x9e3779b97f4a7c15ULL + (A << 6) + (A >> 2));
}

bool LoopInterchangePass::sameIterationDomain(
    Loop *Outer, Loop *Inner, SCEVResult &SE,
    PhiInst *&OuterIV, PhiInst *&InnerIV) {
    OuterIV = nullptr;
    InnerIV = nullptr;
    if (!Outer || !Inner) return false;

    auto OuterDesc = SE.getCanonicalInduction(Outer);
    auto InnerDesc = SE.getCanonicalInduction(Inner);
    auto OuterTrip = SE.analyzeLoopTripCount(Outer);
    auto InnerTrip = SE.analyzeLoopTripCount(Inner);
    if (!OuterDesc || !InnerDesc || !OuterDesc->Valid || !InnerDesc->Valid ||
        !OuterTrip || !InnerTrip || !OuterTrip->Valid || !InnerTrip->Valid ||
        !OuterTrip->TripCount || !InnerTrip->TripCount ||
        !OuterDesc->Phi || !InnerDesc->Phi ||
        !OuterDesc->ConstantStep || !InnerDesc->ConstantStep ||
        *OuterDesc->ConstantStep == 0 ||
        *OuterDesc->ConstantStep != *InnerDesc->ConstantStep) {
        return false;
    }
    if (!SE.isKnownFiniteLoop(Outer) ||
        !SE.isKnownFiniteLoop(Inner) ||
        loopDomainContainsUndef(Outer, SE) ||
        loopDomainContainsUndef(Inner, SE)) {
        return false;
    }

    // 3.1 不只比较 trip count：还必须比较 SCEV 规范化后的起点和步长，防止长度相同但坐标平移的循环被错交换。
    if (!expressionKeysEqual(OuterDesc->Start, InnerDesc->Start) ||
        !expressionKeysEqual(OuterDesc->Step, InnerDesc->Step) ||
        !expressionKeysEqual(OuterTrip->TripCount, InnerTrip->TripCount) ||
        OuterTrip->MayBeZeroTrip != InnerTrip->MayBeZeroTrip) {
        return false;
    }

    if (!OuterDesc->Phi->getIRType() || !InnerDesc->Phi->getIRType() ||
        !OuterDesc->Phi->getIRType()->hasSameShape(
            *InnerDesc->Phi->getIRType())) {
        return false;
    }

    OuterIV = OuterDesc->Phi;
    InnerIV = InnerDesc->Phi;
    return true;
}

bool LoopInterchangePass::hasOnlyCanonicalHeaderPhi(Loop *L, PhiInst *IV) {
    if (!L || !L->getHeader() || !IV || IV->getParent() != L->getHeader())
        return false;
    const auto Phis = L->getHeader()->collectPhiNodes();
    return Phis.size() == 1 && Phis.front() == IV;
}

bool LoopInterchangePass::hasUnsupportedDescendantDomain(
    Loop *Inner, SCEVResult &SE) {
    if (!Inner) return true;
    for (Loop *Sub : Inner->getSubLoops()) {
        if (!Sub || !SE.isKnownFiniteLoop(Sub)) return true;
        auto Desc = SE.getCanonicalInduction(Sub);
        auto Trip = SE.analyzeLoopTripCount(Sub);
        if (!Desc || !Trip || !Desc->Valid || !Trip->Valid ||
            !Desc->Start || !Desc->Step || !Trip->TripCount ||
            !Trip->Bound || loopDomainContainsUndef(Sub, SE)) {
            return true;
        }
        // 3.1 后代循环可以依赖待交换的轴；本阶段只要求其域能由 SCEV 完整表达，真正的次序合法性留给依赖分析。
        if (hasUnsupportedDescendantDomain(Sub, SE)) {
            return true;
        }
    }
    return false;
}

bool LoopInterchangePass::isPureSinkInstruction(Instruction *I) {
    if (!I || dynamic_cast<PhiInst *>(I) || dynamic_cast<LoadInst *>(I) ||
        dynamic_cast<StoreInst *>(I) || dynamic_cast<CallInst *>(I) ||
        dynamic_cast<AllocaInst *>(I) || dynamic_cast<BrInst *>(I) ||
        dynamic_cast<RetInst *>(I) || dceInstructionMustBePreserved(I) ||
        instructionMayFaultOrProducePoison(I)) {
        return false;
    }

    return dynamic_cast<GetElementPtrInst *>(I) ||
           dynamic_cast<BinaryInst *>(I) ||
           dynamic_cast<BitCastInst *>(I) ||
           dynamic_cast<ZextInst *>(I) ||
           dynamic_cast<SextInst *>(I) ||
           dynamic_cast<TruncInst *>(I);
}

bool LoopInterchangePass::collectSinkablePrefix(Candidate &C) {
    if (!C.Outer || !C.Inner || !C.OuterIV || !C.InnerIV ||
        !C.InnerPreHeader) {
        return false;
    }

    std::unordered_set<Instruction *> Marked;
    for (Instruction *I : *C.InnerPreHeader) {
        if (!I || dynamic_cast<BrInst *>(I)) continue;
        bool DependsOnOuter = false;
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            Value *Operand = I->getOperand(Index);
            auto *OperandI = dynamic_cast<Instruction *>(Operand);
            if (Operand == C.OuterIV || (OperandI && Marked.count(OperandI))) {
                DependsOnOuter = true;
                break;
            }
        }
        if (!DependsOnOuter) continue;
        if (!isPureSinkInstruction(I) ||
            C.SinkablePrefix.size() >= MaxSinkInstructions) {
            return false;
        }
        Marked.insert(I);
        C.SinkablePrefix.push_back(I);
    }

    // 3.2 从内循环 preheader 收集依赖外 IV 的纯计算前缀，并限制克隆数量以控制代码增长。
    // 3.2.1 每个下沉值只能流向同一前缀或选中的内循环子树，以保证旧计算与新角色不会同时被外部观察。
    for (Instruction *I : C.SinkablePrefix) {
        for (Use *U : I->getUses()) {
            auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
            if (!UserI) return false;
            // 3.2.2 内循环头 PHI 在 preheader 边上消费值，插在头块正文的克隆无法支配该使用。
            if (dynamic_cast<PhiInst *>(UserI) &&
                UserI->getParent() == C.Inner->getHeader()) {
                return false;
            }
            if (Marked.count(UserI) || isInstructionInBlocks(UserI, C.InnerBlocks))
                continue;
            return false;
        }
    }
    return true;
}

bool LoopInterchangePass::collectRoleUses(Candidate &C) {
    if (!C.Outer || !C.Inner || !C.OuterIV || !C.InnerIV) return false;
    std::unordered_set<Instruction *> SinkSet(C.SinkablePrefix.begin(),
                                               C.SinkablePrefix.end());

    for (Use *U : C.OuterIV->getUses()) {
        auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
        if (!UserI || !UserI->getParent()) return false;
        if (C.OuterControl.count(UserI)) continue;
        if (SinkSet.count(UserI)) continue;
        if (!isInstructionInBlocks(UserI, C.InnerBlocks)) return false;
        if (C.InnerControl.count(UserI)) return false;
        // 3.3 外 IV 的负载使用不能落在内头 PHI 的入边上，因为交换后的内 IV 在该边尚不可用。
        if (dynamic_cast<PhiInst *>(UserI) &&
            UserI->getParent() == C.Inner->getHeader()) {
            return false;
        }
        C.OuterUses.push_back(U);
    }

    for (Use *U : C.InnerIV->getUses()) {
        auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
        if (!UserI || !UserI->getParent()) return false;
        if (C.InnerControl.count(UserI)) continue;
        if (!isInstructionInBlocks(UserI, C.InnerBlocks)) return false;
        C.InnerUses.push_back(U);
    }

    return !C.OuterUses.empty() && !C.InnerUses.empty();
}

bool LoopInterchangePass::collectControlSlice(
    Value *V, PhiInst *IV, Loop *L,
    std::unordered_set<Instruction *> &Control,
    std::unordered_set<Value *> &Active, unsigned Depth) {
    if (!V || !IV || !L || Depth > 64) return false;
    if (V == IV || V->isConst()) return true;

    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || !L->contains(I->getParent())) return true;
    if (!Active.insert(V).second) return false;
    if (!dynamic_cast<BinaryInst *>(I) && !dynamic_cast<PhiInst *>(I) &&
        !dynamic_cast<ZextInst *>(I) && !dynamic_cast<SextInst *>(I) &&
        !dynamic_cast<TruncInst *>(I)) {
        Active.erase(V);
        return false;
    }

    Control.insert(I);
    bool Valid = true;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        Value *Operand = I->getOperand(Index);
        if (dynamic_cast<BasicBlock *>(Operand)) continue;
        if (!collectControlSlice(Operand, IV, L, Control, Active,
                                 Depth + 1)) {
            Valid = false;
            break;
        }
    }
    Active.erase(V);
    return Valid;
}

long long LoopInterchangePass::computeLocalityBenefit(
    const Candidate &C, LoopDependenceInfo &LD) {
    long long Benefit = 0;
    for (const LoopMemoryAccess &Access : LD.getMemoryAccesses(C.Outer)) {
        if (!Access.Inst || !isInstructionInBlocks(Access.Inst, C.InnerBlocks))
            continue;
        std::optional<int64_t> OuterStride =
            LD.getByteStride(Access.Inst, C.Outer);
        std::optional<int64_t> InnerStride =
            LD.getByteStride(Access.Inst, C.Inner);
        if (!OuterStride || !InnerStride) continue;

        const long long Weight = Access.Writes ? 2 : 1;
        Benefit += Weight *
            (static_cast<long long>(stridePenalty(*InnerStride)) -
             static_cast<long long>(stridePenalty(*OuterStride)));
    }
    return Benefit;
}

bool LoopInterchangePass::validateNestShape(
    const Candidate &C, const LoopInfo &LI, const DominatorTree &DT,
    const PostDominatorTree &PDT) {
    if (!C.Outer || !C.Inner || !C.OuterIV || !C.InnerIV ||
        C.Inner->getParentLoop() != C.Outer ||
        C.Outer->getSubLoops().size() != 1 ||
        C.Outer->getSubLoops().front() != C.Inner ||
        !LCSSAPass::isLCSSAForm(C.Outer) ||
        !LCSSAPass::isLCSSAForm(C.Inner)) {
        return false;
    }

    BasicBlock *OuterPreHeader = nullptr;
    BasicBlock *OuterLatch = nullptr;
    BasicBlock *InnerPreHeader = nullptr;
    BasicBlock *InnerLatch = nullptr;
    if (!C.Outer->getLoopSimplifyForm(OuterPreHeader, OuterLatch) ||
        !C.Inner->getLoopSimplifyForm(InnerPreHeader, InnerLatch) ||
        !OuterPreHeader || !OuterLatch || !InnerPreHeader || !InnerLatch ||
        InnerPreHeader != C.InnerPreHeader ||
        !C.Outer->contains(InnerPreHeader) ||
        C.Inner->contains(InnerPreHeader)) {
        return false;
    }

    // 3.4 结构门禁：每个真正进展的外层迭代都必须经过内循环，但保留外循环零次执行的旁路。
    if (!DT.dominates(C.Outer->getHeader(), C.Inner->getHeader()) ||
        !DT.dominates(C.Inner->getHeader(), OuterLatch) ||
        !PDT.isReachableToExit(C.Outer->getHeader()) ||
        !PDT.isReachableToExit(C.Inner->getHeader())) {
        return false;
    }

    // 3.4.1 外层直属块不得有访存，整个嵌套不得有调用，避免交换改变不完美语句的执行次数或可观察顺序。
    for (BasicBlock *BB : C.Outer->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            if (dynamic_cast<CallInst *>(I)) return false;
            if (LI.getLoopFor(BB) == C.Outer &&
                (dynamic_cast<StoreInst *>(I) ||
                 dynamic_cast<LoadInst *>(I))) {
                return false;
            }
        }
    }
    return true;
}

bool LoopInterchangePass::applyCandidate(Function &F, Candidate &C) {
    if (!C.OuterIV || !C.InnerIV || !C.Inner ||
        !C.Inner->getHeader()) {
        return false;
    }

    std::unordered_map<Value *, Value *> VMap;
    VMap[C.OuterIV] = C.InnerIV;
    std::vector<Instruction *> Clones;
    Clones.reserve(C.SinkablePrefix.size());
    for (Instruction *OldI : C.SinkablePrefix) {
        Instruction *NewI = OldI ? OldI->clone(VMap) : nullptr;
        if (!NewI) {
            destroyDetachedInstructions(Clones);
            return false;
        }
        NewI->setIdent(OldI->getIdent() + ".interchange");
        Clones.push_back(NewI);
        VMap[OldI] = NewI;
    }

    // 3.5 预提交：克隆前缀保持游离，并重新核验值映射和 use 快照；任一检查失败都销毁全部游离指令。
    for (Instruction *OldI : C.SinkablePrefix) {
        auto It = VMap.find(OldI);
        if (It == VMap.end() ||
            !dynamic_cast<Instruction *>(It->second)) {
            destroyDetachedInstructions(Clones);
            return false;
        }
    }
    for (Use *U : C.OuterUses) {
        if (!U || U->getValue() != C.OuterIV || C.InnerIV == C.OuterIV) {
            destroyDetachedInstructions(Clones);
            return false;
        }
    }
    for (Use *U : C.InnerUses) {
        if (!U || U->getValue() != C.InnerIV || C.OuterIV == C.InnerIV) {
            destroyDetachedInstructions(Clones);
            return false;
        }
    }

    // 3.6 提交克隆：insertAfterPhiNodes 总是放在非 PHI 区间首部，因此逆序插入才能保持前缀内的定义顺序。
    for (auto It = Clones.rbegin(); It != Clones.rend(); ++It) {
        if (!C.Inner->getHeader()->insertAfterPhiNodes(*It)) {
            // 3.6.1 此时只可能挂接了纯克隆；先全部脱链再统一销毁，恢复到提交前状态。
            for (Instruction *Clone : Clones) {
                if (Clone && Clone->getParent()) Clone->eraseFromParent();
            }
            destroyDetachedInstructions(Clones);
            return false;
        }
    }

    for (Instruction *OldI : C.SinkablePrefix) {
        auto It = VMap.find(OldI);
        auto *Replacement = dynamic_cast<Instruction *>(It->second);
        for (Use *U : OldI->getUses()) {
            auto *UserI = dynamic_cast<Instruction *>(U ? U->getUser() : nullptr);
            if (isInstructionInBlocks(UserI, C.InnerBlocks))
                U->replaceUseeWith(Replacement);
        }
    }

    // 3.7 使用预先快照的 use 集合交换两轴，避免第一轮替换污染第二轮工作列表。
    for (Use *U : C.OuterUses)
        U->replaceUseeWith(C.InnerIV);
    for (Use *U : C.InnerUses)
        U->replaceUseeWith(C.OuterIV);

    // 3.8 提交后给内层负载块加确定性标记；即使无前缀需要克隆，也能记录角色交换已发生，标记不参与 CFG/SSA 语义。
    for (BasicBlock *BB : F) {
        if (!BB || !C.Inner->contains(BB)) continue;
        BB->setIdent(F.makeUniqueBlockIdent(
            BB->getIdent() + ".interchange"));
    }
    return true;
}

bool LoopInterchangePass::buildRotatedNestCandidate(
    Loop *Outer, Loop *Inner, LoopInfo &LI, SCEVResult &SE,
    DominatorTree &DT, PostDominatorTree &PDT,
    LoopDependenceInfo &LD, RotatedNestCandidate &C) {
    C = RotatedNestCandidate{};
    if (!Outer || !Inner || Inner->getParentLoop() != Outer ||
        Outer->getSubLoops().size() != 1 ||
        Outer->getSubLoops().front() != Inner || !Inner->isInnermost() ||
        !LCSSAPass::isLCSSAForm(Outer) ||
        !LCSSAPass::isLCSSAForm(Inner) ||
        !SE.isKnownFiniteLoop(Outer) ||
        !SE.isKnownFiniteLoop(Inner)) {
        return false;
    }

    BasicBlock *Guard = nullptr;
    BasicBlock *OuterLatch = nullptr;
    BasicBlock *Prefix = nullptr;
    BasicBlock *InnerBody = nullptr;
    // 2.1 允许 guard 直接跳过零次外循环，但外头仍必须恰好有一条入边和一条回边；随后再逐边匹配固定 CFG 骨架。
    auto getGuardedLoopForm = [](Loop *L, BasicBlock *&Entry,
                                 BasicBlock *&Latch) {
        Entry = nullptr;
        Latch = nullptr;
        if (!L || !L->getHeader()) return false;
        for (BasicBlock *Pred : L->getHeader()->getPredecessors()) {
            if (!Pred) continue;
            BasicBlock *&Slot = L->contains(Pred) ? Latch : Entry;
            if (Slot) return false;
            Slot = Pred;
        }
        return Entry && Latch &&
               Entry->countEdgesToSucc(L->getHeader()) == 1 &&
               Latch->countEdgesToSucc(L->getHeader()) == 1;
    };
    if (!getGuardedLoopForm(Outer, Guard, OuterLatch) ||
        !Inner->getLoopSimplifyForm(Prefix, InnerBody) ||
        !Guard || !OuterLatch || !Prefix || !InnerBody ||
        Prefix == Outer->getHeader() ||
        !Outer->contains(Prefix) || Inner->contains(Prefix)) {
        return false;
    }

    BasicBlock *OuterHeader = Outer->getHeader();
    BasicBlock *InnerHeader = Inner->getHeader();
    auto OuterDesc = SE.getCanonicalInduction(Outer);
    auto InnerDesc = SE.getCanonicalInduction(Inner);
    auto OuterTrip = SE.analyzeLoopTripCount(Outer);
    auto InnerTrip = SE.analyzeLoopTripCount(Inner);
    if (!OuterHeader || !InnerHeader ||
        !OuterDesc || !InnerDesc || !OuterTrip || !InnerTrip ||
        !OuterDesc->Valid || !InnerDesc->Valid ||
        !OuterTrip->Valid || !InnerTrip->Valid ||
        !OuterDesc->Phi || !InnerDesc->Phi ||
        !OuterDesc->Start || !OuterDesc->Step ||
        !InnerDesc->Start || !InnerDesc->Step ||
        !OuterTrip->Bound || !InnerTrip->Bound ||
        !OuterTrip->ExitCmp || !InnerTrip->ExitCmp ||
        !OuterDesc->StartValue || !InnerDesc->StartValue ||
        !OuterDesc->UpdateValue || !InnerDesc->UpdateValue ||
        OuterTrip->ExitingBlock != OuterHeader ||
        InnerTrip->ExitingBlock != InnerHeader ||
        InnerTrip->ExitBlock != OuterLatch ||
        !OuterTrip->ExitBlock ||
        loopDomainContainsUndef(Outer, SE) ||
        loopDomainContainsUndef(Inner, SE)) {
        return false;
    }
    auto *InnerUpdate = dynamic_cast<Instruction *>(
        InnerDesc->UpdateValue);
    BasicBlock *Exit = OuterTrip->ExitBlock;
    if (OuterDesc->Start->mentionsLoop(Inner) ||
        OuterDesc->Step->mentionsLoop(Inner) ||
        OuterTrip->Bound->mentionsLoop(Inner) ||
        InnerDesc->Start->mentionsLoop(Outer) ||
        InnerDesc->Step->mentionsLoop(Outer) ||
        InnerTrip->Bound->mentionsLoop(Outer)) {
        return false;
    }

    if (!hasOnlyCanonicalHeaderPhi(Outer, OuterDesc->Phi) ||
        !hasOnlyCanonicalHeaderPhi(Inner, InnerDesc->Phi) ||
        !Prefix->collectPhiNodes().empty() ||
        !InnerBody->collectPhiNodes().empty() ||
        !OuterLatch->collectPhiNodes().empty() ||
        !Exit->collectPhiNodes().empty()) {
        return false;
    }

    const auto OuterExits = Outer->getExitBlocks();
    const auto InnerExits = Inner->getExitBlocks();
    if (OuterExits.size() != 1 || OuterExits.front() != Exit ||
        InnerExits.size() != 1 || InnerExits.front() != OuterLatch) {
        return false;
    }

    const auto &OuterDirectBlocks = Outer->getBlocks();
    const auto InnerBlocks = Inner->getBlocksIncludingSubLoops();
    if (OuterDirectBlocks.size() != 3 || InnerBlocks.size() != 2)
        return false;
    std::unordered_set<BasicBlock *> OuterDirect(
        OuterDirectBlocks.begin(), OuterDirectBlocks.end());
    std::unordered_set<BasicBlock *> InnerBlockSet(
        InnerBlocks.begin(), InnerBlocks.end());
    if (!OuterDirect.count(OuterHeader) || !OuterDirect.count(Prefix) ||
        !OuterDirect.count(OuterLatch) ||
        !InnerBlockSet.count(InnerHeader) ||
        !InnerBlockSet.count(InnerBody) ||
        LI.getLoopFor(OuterHeader) != Outer ||
        LI.getLoopFor(Prefix) != Outer ||
        LI.getLoopFor(OuterLatch) != Outer ||
        LI.getLoopFor(InnerHeader) != Inner ||
        LI.getLoopFor(InnerBody) != Inner) {
        return false;
    }

    auto *GuardBr = Guard->getBranchTerminator();
    auto *OuterHeaderBr = OuterHeader->getBranchTerminator();
    auto *PrefixBr = Prefix->getBranchTerminator();
    auto *InnerHeaderBr = InnerHeader->getBranchTerminator();
    auto *InnerBodyBr = InnerBody->getBranchTerminator();
    auto *OuterLatchBr = OuterLatch->getBranchTerminator();
    if ((!BasicBlock::isUncondBr(GuardBr) &&
         !BasicBlock::isCondBr(GuardBr)) ||
        !BasicBlock::isCondBr(OuterHeaderBr) ||
        !BasicBlock::isUncondBr(PrefixBr) ||
        !BasicBlock::isCondBr(InnerHeaderBr) ||
        !BasicBlock::isUncondBr(InnerBodyBr) ||
        !BasicBlock::isUncondBr(OuterLatchBr) ||
        PrefixBr->getSuccessor(0) != InnerHeader ||
        InnerBodyBr->getSuccessor(0) != InnerHeader ||
        OuterLatchBr->getSuccessor(0) != OuterHeader ||
        Guard->countEdgesToSucc(OuterHeader) != 1 ||
        OuterHeader->countEdgesToSucc(Prefix) != 1 ||
        Prefix->countEdgesToSucc(InnerHeader) != 1 ||
        InnerBody->countEdgesToSucc(InnerHeader) != 1 ||
        OuterHeaderBr->getCondition() != OuterTrip->ExitCmp ||
        InnerHeaderBr->getCondition() != InnerTrip->ExitCmp ||
        !InnerUpdate || InnerUpdate->getParent() != InnerHeader) {
        return false;
    }
    if (GuardBr->isConditional()) {
        BasicBlock *Other = GuardBr->getSuccessor(0) == OuterHeader
            ? GuardBr->getSuccessor(1) : GuardBr->getSuccessor(0);
        if (Other != Exit) return false;
    } else if (GuardBr->getSuccessor(0) != OuterHeader) {
        return false;
    }

    const unsigned OuterContinueIndex =
        OuterTrip->ContinueOnTrue ? 0U : 1U;
    const unsigned OuterExitIndex = OuterContinueIndex ^ 1U;
    const unsigned InnerContinueIndex =
        InnerTrip->ContinueOnTrue ? 0U : 1U;
    const unsigned InnerExitIndex = InnerContinueIndex ^ 1U;
    if (OuterHeaderBr->getSuccessor(OuterContinueIndex) != Prefix ||
        OuterHeaderBr->getSuccessor(OuterExitIndex) != Exit ||
        InnerHeaderBr->getSuccessor(InnerContinueIndex) != InnerBody ||
        InnerHeaderBr->getSuccessor(InnerExitIndex) != OuterLatch ||
        OuterLatchBr->getSuccessor(0) != OuterHeader) {
        return false;
    }

    auto findIncoming = [](PhiInst *Phi, BasicBlock *Pred,
                           Value *Expected, unsigned &Index) {
        if (!Phi || !Pred || !Expected ||
            Phi->countIncomingFromPred(Pred) != 1) {
            return false;
        }
        for (unsigned I = 0; I < Phi->getNumIncomingValues(); ++I) {
            if (Phi->getIncomingBlock(I) != Pred) continue;
            if (Phi->getIncomingValue(I) != Expected) return false;
            Index = I;
            return true;
        }
        return false;
    };

    unsigned OuterBackedgeIndex = 0;
    if (OuterDesc->Phi->getNumIncomingValues() != 2 ||
        InnerDesc->Phi->getNumIncomingValues() != 2 ||
        !findIncoming(OuterDesc->Phi, Guard, OuterDesc->StartValue,
                      C.OuterStartIndex) ||
        !findIncoming(OuterDesc->Phi, OuterLatch,
                      OuterDesc->UpdateValue, OuterBackedgeIndex) ||
        !findIncoming(InnerDesc->Phi, Prefix,
                      InnerDesc->StartValue, C.InnerStartIndex) ||
        !findIncoming(InnerDesc->Phi, InnerBody,
                      InnerDesc->UpdateValue, C.InnerBackedgeIndex)) {
        return false;
    }

    auto availableAtGuard = [&](Value *V) {
        auto *I = dynamic_cast<Instruction *>(V);
        if (!I || !I->getParent()) return true;
        BasicBlock *DefBlock = I->getParent();
        if (DefBlock != Guard) return DT.dominates(DefBlock, Guard);
        Instruction *Terminator = Guard->getTerminator();
        return I != Terminator && I->comesBefore(Terminator);
    };

    // 2.2 交换后旧内条件会先执行，因此要保留原外循环的首次头部比较；只接受直接比较外 IV 的固定位宽谓词。
    Value *CmpLHS = OuterTrip->ExitCmp->getOperand(0);
    Value *CmpRHS = OuterTrip->ExitCmp->getOperand(1);
    const bool PhiOnLHS = CmpLHS == OuterDesc->Phi;
    const bool PhiOnRHS = CmpRHS == OuterDesc->Phi;
    Value *FirstBound = PhiOnLHS ? CmpRHS : CmpLHS;
    if (OuterTrip->ComparePhase != LoopExitComparePhase::HeaderPhi ||
        OuterTrip->IndVar != OuterDesc->Phi ||
        !BinaryInst::isCompareOp(OuterTrip->ExitCmp->getOp()) ||
        PhiOnLHS == PhiOnRHS ||
        !availableAtGuard(OuterDesc->StartValue) ||
        !availableAtGuard(FirstBound)) {
        return false;
    }

    if (!availableAtGuard(OuterDesc->StartValue) ||
        !availableAtGuard(InnerDesc->StartValue) ||
        !DT.dominates(OuterHeader, Prefix) ||
        !DT.dominates(Prefix, InnerHeader) ||
        !DT.dominates(OuterHeader, InnerHeader) ||
        !DT.dominates(InnerHeader, InnerBody) ||
        !DT.dominates(InnerHeader, OuterLatch) ||
        !PDT.isReachableToExit(OuterHeader) ||
        !PDT.isReachableToExit(InnerHeader)) {
        return false;
    }

    // 2.3 验证将成为新外头的控制计算：它们必须纯、不故障，且所有跨块定义在 guard 处可用，以维持支配不变量。
    for (Instruction *I : *InnerHeader) {
        if (!I || dynamic_cast<PhiInst *>(I) ||
            dynamic_cast<BrInst *>(I)) {
            continue;
        }
        if (!dynamic_cast<BinaryInst *>(I) &&
            !dynamic_cast<BitCastInst *>(I) &&
            !dynamic_cast<ZextInst *>(I) &&
            !dynamic_cast<SextInst *>(I) &&
            !dynamic_cast<TruncInst *>(I)) {
            return false;
        }
        if (dceInstructionMustBePreserved(I) ||
            instructionMayFaultOrProducePoison(I)) {
            return false;
        }
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            Value *Operand = I->getOperand(Index);
            if (Operand == OuterDesc->Phi) return false;
            auto *Def = dynamic_cast<Instruction *>(Operand);
            if (!Def || !Def->getParent()) continue;
            if (Def->getParent() == InnerHeader) {
                if (Def != InnerDesc->Phi && !Def->comesBefore(I))
                    return false;
                continue;
            }
            if (!DT.dominates(Def->getParent(), Guard))
                return false;
        }
    }

    // 2.4 语义门禁：除已建模的 load/store 外，任何分配、调用、返回或未知可观察指令都拒绝物理重排。
    for (BasicBlock *BB : Outer->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<AllocaInst *>(I) ||
                dynamic_cast<CallInst *>(I) ||
                dynamic_cast<RetInst *>(I)) {
                return false;
            }
            if (!dynamic_cast<LoadInst *>(I) &&
                !dynamic_cast<StoreInst *>(I) &&
                !dynamic_cast<PhiInst *>(I) &&
                !dynamic_cast<BrInst *>(I) &&
                dceInstructionMustBePreserved(I)) {
                return false;
            }
        }
    }

    std::vector<Instruction *> PrefixLoads;
    for (BasicBlock *BB : OuterDirectBlocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<CallInst *>(I) ||
                dynamic_cast<StoreInst *>(I) ||
                dynamic_cast<AllocaInst *>(I) ||
                dynamic_cast<RetInst *>(I)) {
                return false;
            }
            if (auto *Load = dynamic_cast<LoadInst *>(I)) {
                if (BB != Prefix) return false;
                PrefixLoads.push_back(Load);
                continue;
            }
            if (dynamic_cast<PhiInst *>(I) || dynamic_cast<BrInst *>(I))
                continue;
            if (dceInstructionMustBePreserved(I) ||
                instructionMayFaultOrProducePoison(I)) {
                return false;
            }
        }
    }
    if (PrefixLoads.empty() || LD.hasUnknownMemory(Outer)) return false;

    // 2.5 Prefix 会随新内轴重复：其操作数必须在 guard 处可用，结果只能流向仍受 Prefix 支配的内体或外 latch。
    for (Instruction *I : *Prefix) {
        if (!I || dynamic_cast<BrInst *>(I)) continue;
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            auto *Def = dynamic_cast<Instruction *>(I->getOperand(Index));
            if (!Def || !Def->getParent()) continue;
            if (Def->getParent() == Prefix) {
                if (!Def->comesBefore(I)) return false;
                continue;
            }
            if (Def == OuterDesc->Phi) continue;
            if (!DT.dominates(Def->getParent(), Guard)) return false;
        }
        for (Use *U : I->getUses()) {
            auto *User = dynamic_cast<Instruction *>(
                U ? U->getUser() : nullptr);
            if (!User || !User->getParent()) return false;
            if (User->getParent() == Prefix) {
                if (!I->comesBefore(User)) return false;
                continue;
            }
            if (User->getParent() != InnerBody &&
                User->getParent() != OuterLatch) {
                return false;
            }
        }
    }

    const auto &Accesses = LD.getMemoryAccesses(Outer);
    for (Instruction *Load : PrefixLoads) {
        for (const LoopMemoryAccess &Access : Accesses) {
            if (!Access.Writes || !Access.Inst ||
                !Access.Inst->getParent() ||
                !Inner->contains(Access.Inst->getParent())) {
                continue;
            }
            if (!LD.isInvariantAccessDisjointFromLoopWrite(
                    Load, Access.Inst, Outer, Inner)) {
                return false;
            }
        }
    }

    for (const LoopDependenceRecord &Record : LD.getDependences(Outer)) {
        if (!Record.Source || !Record.Sink ||
            !Record.Source->Inst || !Record.Sink->Inst) {
            return false;
        }

        bool OuterEqual = false;
        bool InnerEqual = false;
        for (std::size_t I = 0;
             I < Record.Loops.size() && I < Record.DirectionMasks.size();
             ++I) {
            if (Record.Loops[I] == Outer)
                OuterEqual =
                    Record.DirectionMasks[I] == LoopDepSameIteration;
            if (Record.Loops[I] == Inner)
                InnerEqual =
                    Record.DirectionMasks[I] == LoopDepSameIteration;
        }
        // 2.6 区间不相交只证明 Prefix 读取对固定外轴稳定；跨外轴仍要求依赖方向在外轴或内轴上为同次迭代。
        if (!OuterEqual && !InnerEqual) return false;
    }

    long long Benefit = 0;
    for (const LoopMemoryAccess &Access : Accesses) {
        if (!Access.Inst || !Access.Inst->getParent() ||
            !Inner->contains(Access.Inst->getParent())) {
            continue;
        }
        auto OuterStride = LD.getByteStride(Access.Inst, Outer);
        auto InnerStride = LD.getByteStride(Access.Inst, Inner);
        if (!OuterStride || !InnerStride) continue;
        const long long Weight = Access.Writes ? 2 : 1;
        Benefit += Weight *
            (static_cast<long long>(stridePenalty(*InnerStride)) -
             static_cast<long long>(stridePenalty(*OuterStride)));
    }
    if (Benefit < MinLocalityBenefit) return false;

    C.Outer = Outer;
    C.Inner = Inner;
    C.Guard = Guard;
    C.OuterHeader = OuterHeader;
    C.Prefix = Prefix;
    C.InnerHeader = InnerHeader;
    C.InnerBody = InnerBody;
    C.OuterLatch = OuterLatch;
    C.Exit = Exit;
    C.OuterIV = OuterDesc->Phi;
    C.InnerIV = InnerDesc->Phi;
    C.OuterFirstCmp = OuterTrip->ExitCmp;
    C.OuterStartValue = OuterDesc->StartValue;
    C.OuterContinueOnTrue = OuterTrip->ContinueOnTrue;
    C.InnerContinueOnTrue = InnerTrip->ContinueOnTrue;
    C.LocalityBenefit = Benefit;
    return true;
}

bool LoopInterchangePass::applyRotatedNestCandidate(
    Function &F, RotatedNestCandidate &C) {
    if (!C.Guard || !C.OuterHeader || !C.Prefix || !C.InnerHeader ||
        !C.InnerBody || !C.OuterLatch || !C.Exit ||
        !C.OuterIV || !C.InnerIV) {
        return false;
    }

    if (!C.OuterFirstCmp || !C.OuterStartValue) {
        return false;
    }

    auto *GuardBr = C.Guard->getBranchTerminator();
    auto *OuterHeaderBr = C.OuterHeader->getBranchTerminator();
    auto *PrefixBr = C.Prefix->getBranchTerminator();
    auto *InnerHeaderBr = C.InnerHeader->getBranchTerminator();
    auto *InnerBodyBr = C.InnerBody->getBranchTerminator();
    auto *OuterLatchBr = C.OuterLatch->getBranchTerminator();
    auto findSuccessor = [](BrInst *Br, BasicBlock *Target,
                            unsigned &Index) {
        if (!Br || !Target) return false;
        bool Found = false;
        for (unsigned I = 0; I < Br->getSuccessorCount(); ++I) {
            if (Br->getSuccessor(I) != Target) continue;
            if (Found) return false;
            Found = true;
            Index = I;
        }
        return Found;
    };

    unsigned GuardToOuter = 0;
    unsigned OuterToPrefix = 0;
    unsigned OuterToExit = 0;
    unsigned PrefixToInner = 0;
    unsigned InnerBodyToHeader = 0;
    unsigned OuterLatchToHeader = 0;
    const unsigned OuterContinue =
        C.OuterContinueOnTrue ? 0U : 1U;
    const unsigned OuterExit = OuterContinue ^ 1U;
    unsigned InnerContinue = C.InnerContinueOnTrue ? 0U : 1U;
    unsigned InnerExit = InnerContinue ^ 1U;
    if (!findSuccessor(GuardBr, C.OuterHeader, GuardToOuter) ||
        !findSuccessor(OuterHeaderBr, C.Prefix, OuterToPrefix) ||
        !findSuccessor(OuterHeaderBr, C.Exit, OuterToExit) ||
        !findSuccessor(PrefixBr, C.InnerHeader, PrefixToInner) ||
        !findSuccessor(InnerBodyBr, C.InnerHeader,
                       InnerBodyToHeader) ||
        !findSuccessor(OuterLatchBr, C.OuterHeader,
                       OuterLatchToHeader) ||
        !InnerHeaderBr ||
        OuterToPrefix != OuterContinue || OuterToExit != OuterExit ||
        InnerHeaderBr->getSuccessor(InnerContinue) != C.InnerBody ||
        InnerHeaderBr->getSuccessor(InnerExit) != C.OuterLatch ||
        C.OuterIV->getIncomingBlock(C.OuterStartIndex) != C.Guard ||
        C.InnerIV->getIncomingBlock(C.InnerStartIndex) != C.Prefix ||
        C.InnerIV->getIncomingBlock(C.InnerBackedgeIndex) != C.InnerBody) {
        return false;
    }

    Value *FirstLHS = C.OuterFirstCmp->getOperand(0);
    Value *FirstRHS = C.OuterFirstCmp->getOperand(1);
    const bool PhiOnLHS = FirstLHS == C.OuterIV;
    const bool PhiOnRHS = FirstRHS == C.OuterIV;
    if (PhiOnLHS == PhiOnRHS) return false;
    if (PhiOnLHS) FirstLHS = C.OuterStartValue;
    else FirstRHS = C.OuterStartValue;

    auto *FirstCheck = new BasicBlock();
    auto *JPre = new BasicBlock();
    auto *KPre = new BasicBlock();
    auto *JLatch = new BasicBlock();
    JPre->setIdent(F.makeUniqueBlockIdent(
        C.InnerHeader->getIdent() + ".interchange.preheader"));
    KPre->setIdent(F.makeUniqueBlockIdent(
        C.OuterHeader->getIdent() + ".interchange.preheader"));
    JLatch->setIdent(F.makeUniqueBlockIdent(
        C.InnerHeader->getIdent() + ".interchange.latch"));
    FirstCheck->setIdent(F.makeUniqueBlockIdent(
        C.OuterHeader->getIdent() + ".interchange.firstcheck"));
    auto *FirstCmp = new BinaryInst(
        FirstLHS, C.OuterFirstCmp->getOp(), FirstRHS);
    FirstCheck->pushBack(FirstCmp);
    if (C.OuterContinueOnTrue)
        FirstCheck->genCondInst(FirstCmp, JPre, C.Exit);
    else
        FirstCheck->genCondInst(FirstCmp, C.Exit, JPre);
    JPre->genUnCondInst(C.InnerHeader);
    KPre->genUnCondInst(C.OuterHeader);
    JLatch->genUnCondInst(C.InnerHeader);

    // 2.7 预提交已完成全部边索引与 PHI 入边校验；挂接四个辅助块后只执行已证明的边重定向。
    // 2.7.1 新 CFG 保留原外头首次检查，且只改 PHI 前驱，不改两轴的起点、步长和上界。
    F.pushBlock(FirstCheck);
    F.pushBlock(JPre);
    F.pushBlock(KPre);
    F.pushBlock(JLatch);

    auto rewriteEdge = [](BrInst *Br, unsigned Index,
                          BasicBlock *Target) {
        const bool Changed = Br && Br->setSuccessor(Index, Target);
        assert(Changed && "validated interchange edge rewrite failed");
        (void)Changed;
    };
    rewriteEdge(GuardBr, GuardToOuter, FirstCheck);
    rewriteEdge(InnerHeaderBr, InnerContinue, KPre);
    rewriteEdge(InnerHeaderBr, InnerExit, C.Exit);
    rewriteEdge(OuterHeaderBr, OuterExit, JLatch);
    rewriteEdge(PrefixBr, PrefixToInner, C.InnerBody);
    rewriteEdge(InnerBodyBr, InnerBodyToHeader, C.OuterLatch);

    const bool OuterIncomingChanged = C.OuterIV->setIncomingBlock(
        C.OuterStartIndex, KPre);
    const bool InnerStartChanged = C.InnerIV->setIncomingBlock(
        C.InnerStartIndex, JPre);
    const bool InnerBackedgeChanged = C.InnerIV->setIncomingBlock(
        C.InnerBackedgeIndex, JLatch);
    assert(OuterIncomingChanged && InnerStartChanged &&
           InnerBackedgeChanged &&
           "validated interchange PHI rewrite failed");
    (void)OuterIncomingChanged;
    (void)InnerStartChanged;
    (void)InnerBackedgeChanged;

    C.Prefix->setIdent(F.makeUniqueBlockIdent(
        C.Prefix->getIdent() + ".interchange.cfg"));
    C.InnerBody->setIdent(F.makeUniqueBlockIdent(
        C.InnerBody->getIdent() + ".interchange.cfg"));
    return true;
}

PassResult LoopInterchangePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    // 1. 函数级预算耗尽时直接退出；否则一次获取结构、标量和依赖分析。
    if (TransformCount[&F] >= MaxInterchangesPerFunction)
        return PassResult::unchanged();

    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &PDT = FAM.getResult<PostDominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &LD = FAM.getResult<LoopDependenceAnalysis>(F);

    for (Loop *Inner : LI.getLoopsInPostOrder()) {
        // 1.1 从内向外枚举直接父子循环，跳过已处理的头块对。
        Loop *Outer = Inner ? Inner->getParentLoop() : nullptr;
        if (!Outer || !Outer->getHeader() || !Inner->getHeader()) continue;
        HeaderPair Key{Outer->getHeader(), Inner->getHeader()};
        if (Transformed.count(Key)) continue;

        RotatedNestCandidate Rotated;
        // 2. 物理交换覆盖旋转后固定形态；它改写 CFG，成功后立即返回并使所有分析失效。
        if (buildRotatedNestCandidate(
                Outer, Inner, LI, SE, DT, PDT, LD, Rotated) &&
            applyRotatedNestCandidate(F, Rotated)) {
            Transformed.insert(Key);
            Transformed.insert(
                HeaderPair{Inner->getHeader(), Outer->getHeader()});
            ++TransformCount[&F];
            return PassResult::changed(PreservationStatus::none());
        }

        Candidate C;
        // 3. 角色交换路径依次证明同域、规范 PHI、可表达的后代域与 LoopSimplify 形态。
        C.Outer = Outer;
        C.Inner = Inner;
        if (!sameIterationDomain(Outer, Inner, SE, C.OuterIV, C.InnerIV))
            continue;
        if (!hasOnlyCanonicalHeaderPhi(Outer, C.OuterIV) ||
            !hasOnlyCanonicalHeaderPhi(Inner, C.InnerIV))
            continue;
        if (hasUnsupportedDescendantDomain(Inner, SE))
            continue;

        BasicBlock *InnerLatch = nullptr;
        if (!Inner->getLoopSimplifyForm(C.InnerPreHeader, InnerLatch) ||
            !C.InnerPreHeader || !InnerLatch) {
            continue;
        }
        for (BasicBlock *BB : Inner->getBlocksIncludingSubLoops())
            if (BB) C.InnerBlocks.insert(BB);

        auto OuterDesc = SE.getCanonicalInduction(Outer);
        auto InnerDesc = SE.getCanonicalInduction(Inner);
        auto OuterTrip = SE.analyzeLoopTripCount(Outer);
        auto InnerTrip = SE.analyzeLoopTripCount(Inner);
        if (!OuterDesc || !InnerDesc || !OuterTrip || !InnerTrip ||
            !OuterTrip->ExitCmp || !InnerTrip->ExitCmp) {
            continue;
        }
        C.OuterControl.insert(C.OuterIV);
        C.InnerControl.insert(C.InnerIV);
        std::unordered_set<Value *> OuterControlActive;
        std::unordered_set<Value *> InnerControlActive;
        if (!collectControlSlice(OuterDesc->UpdateValue, C.OuterIV, Outer,
                                 C.OuterControl, OuterControlActive) ||
            !collectControlSlice(InnerDesc->UpdateValue, C.InnerIV, Inner,
                                 C.InnerControl, InnerControlActive)) {
            continue;
        }
        C.OuterControl.insert(OuterTrip->ExitCmp);
        C.InnerControl.insert(InnerTrip->ExitCmp);
        if (OuterTrip->ExitingBlock && OuterTrip->ExitingBlock->getTerminator())
            C.OuterControl.insert(OuterTrip->ExitingBlock->getTerminator());
        if (InnerTrip->ExitingBlock && InnerTrip->ExitingBlock->getTerminator())
            C.InnerControl.insert(InnerTrip->ExitingBlock->getTerminator());

        if (!validateNestShape(C, LI, DT, PDT))
            continue;
        // 3.4 依赖和收益门禁：内存必须可完整建模且方向向量允许交换，局部性改善达到阈值后才提交。
        if (LD.hasUnknownMemory(Outer))
            continue;
        if (!LD.isInterchangeLegal(Outer, Inner))
            continue;
        if (!collectSinkablePrefix(C))
            continue;
        if (!collectRoleUses(C))
            continue;

        C.LocalityBenefit = computeLocalityBenefit(C, LD);
        if (C.LocalityBenefit < MinLocalityBenefit) continue;

        if (!applyCandidate(F, C)) continue;
        Transformed.insert(Key);
        ++TransformCount[&F];

        PreservationStatus PA;
        // 5. 角色交换不动 CFG，因此保留支配、后支配和循环结构；值语义、别名、MemorySSA 与依赖结果全部重算。
        PA.keep<DominantAnalysis>();
        PA.keep<PostDominantAnalysis>();
        PA.keep<LoopAnalysis>();
        PA.discard<SCEVAnalysis>();
        PA.discard<BasicAliasAnalysis>();
        PA.discard<MemorySSAAnalysis>();
        PA.discard<LoopDependenceAnalysis>();
        PA.discard<SideEffectAnalysis>();
        return PassResult::changed(std::move(PA));
    }

    return PassResult::unchanged();
}
