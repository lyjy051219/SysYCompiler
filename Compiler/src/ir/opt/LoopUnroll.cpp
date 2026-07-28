#include "ir/opt/LoopUnroll.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

bool LoopUnrollPass::isStructurallyLoopInvariant(
    Value *V, Loop *L, std::unordered_set<Value *> &Active) {
    // 3.2.1 peeling 收益只使用结构不变的纯值链：排除 undef、PHI、访存、调用、分配和可故障运算，并用 Active 拒绝环。
    if (!V || !L || Value::isUndef(V)) return false;
    std::unordered_set<Value *> UndefActive;
    if (containsUndef(V, UndefActive, 0)) return false;
    if (dynamic_cast<ConstantSystem *>(V) ||
        V->isParam() || V->isGlobal() ||
        dynamic_cast<Function *>(V) ||
        dynamic_cast<BuildInFunction *>(V)) {
        return true;
    }

    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || !L->contains(I->getParent())) return true;
    if (dynamic_cast<PhiInst *>(I) || dynamic_cast<LoadInst *>(I) ||
        dynamic_cast<CallInst *>(I) || dynamic_cast<AllocaInst *>(I)) {
        return false;
    }
	if (instructionMayFaultOrProducePoison(I)) return false;

    bool PureProducer = dynamic_cast<GetElementPtrInst *>(I) ||
                        dynamic_cast<SelectInst *>(I) ||
                        dynamic_cast<BinaryInst *>(I) ||
                        I->instType == Instruction::TRUNC ||
                        I->instType == Instruction::ZEXT ||
                        I->instType == Instruction::SEXT ||
                        I->instType == Instruction::BITCAST;
    if (!PureProducer) return false;
    ScopedActiveMark ActiveMark(Active, V);
    if (!ActiveMark.inserted()) return false;

    const bool OperandsInvariant = forEachOperand(
        I, [L, &Active](size_t, Use *, Value *Operand) {
            if (dynamic_cast<BasicBlock *>(Operand)) return true;
            return isStructurallyLoopInvariant(Operand, L, Active);
        });
    return OperandsInvariant;
}

std::vector<StoreInst *>
LoopUnrollPass::collectPeelRedundantStoreCandidates(
    Loop *L, LoopInfo &LI, BasicAAResult &AA, const DominatorTree &DT) {
    std::vector<StoreInst *> Result;
    if (!L) return Result;

    BasicBlock *PreHeader = nullptr;
    BasicBlock *OuterLatch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, OuterLatch) ||
        !PreHeader || !OuterLatch) {
        return Result;
    }

    // 3.2.2 不变 store 分析先要求所有非退出内部分支条件结构不变，且循环子树不含任何调用。
    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB) return {};
        auto *Br = BB->getBranchTerminator();
        if (!Br || !Br->isConditional() || L->isExitingBlock(BB))
            continue;
        std::unordered_set<Value *> Active;
        if (!isStructurallyLoopInvariant(Br->getCondition(), L, Active))
            return {};
    }

    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return {};
        for (Instruction *I : *BB)
            if (dynamic_cast<CallInst *>(I)) return {};
    }

    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB) return {};
        for (Instruction *I : *BB) {
            auto *SI = dynamic_cast<StoreInst *>(I);
            if (!SI) continue;

            // 3.2.3 候选 store 必须支配 latch，才能按“每个已完成迭代执行一次”计收益，不使用未知分支猜测频率。
            if (!DT.dominates(BB, OuterLatch)) continue;

            std::unordered_set<Value *> Active;
            if (!isStructurallyLoopInvariant(
                    SI->getPointerOperand(), L, Active)) {
                continue;
            }
            Active.clear();
            if (!isStructurallyLoopInvariant(
                    SI->getValueOperand(), L, Active)) {
                continue;
            }

            // 3.2.4 store 的地址和写入值均须结构不变，且循环内不存在与该地址可能别名的其他 store。
            bool HasCompetingMod = false;
            for (BasicBlock *OtherBB : L->getBlocksIncludingSubLoops()) {
                if (!OtherBB) {
                    HasCompetingMod = true;
                    break;
                }
                for (Instruction *OtherI : *OtherBB) {
                    auto *OtherStore = dynamic_cast<StoreInst *>(OtherI);
                    if (!OtherStore || OtherStore == SI) continue;
                    if (AA.alias(SI->getPointerOperand(),
                                 OtherStore->getPointerOperand(), LI) !=
                        AliasInfo::NoAlias) {
                        HasCompetingMod = true;
                        break;
                    }
                }
                if (HasCompetingMod) break;
            }
            if (!HasCompetingMod) Result.push_back(SI);
        }
    }
    return Result;
}

bool LoopUnrollPass::proveRuntimeTripCountAtPreheader(
    SCEVResult &SE, const LoopTripCountInfo &TCI, RangeInfo &RI,
    DominatorTree &DT) {
    return SE.proveRuntimeNoWrapAtPreheader(TCI, RI, DT);
}

bool LoopUnrollPass::containsUndef(
    Value *V, std::unordered_set<Value *> &Active, int Depth) {
    if (!V || Value::isUndef(V)) return true;
    if (Depth > 32) return true;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return false;
    ScopedActiveMark ActiveMark(Active, V);
    if (!ActiveMark.inserted()) return true;
    const bool NoOperandContainsUndef = forEachOperand(
        I, [&Active, Depth](size_t, Use *, Value *Operand) {
            if (dynamic_cast<BasicBlock *>(Operand)) return true;
            return !containsUndef(Operand, Active, Depth + 1);
        });
    return !NoOperandContainsUndef;
}

// 4.1 未提交克隆的回滚不依赖析构顺序：先将仍被外部引用的克隆值转成 undef，再清空指令和块。
void LoopUnrollPass::discardUncommittedPeelClones(PeeledBodyCore &Core) {
    std::vector<BasicBlock *> Blocks;
    std::unordered_set<BasicBlock *> Seen;
    for (const auto &Iteration : Core.IterBlocks)
        for (BasicBlock *BB : Iteration)
            if (BB && Seen.insert(BB).second) Blocks.push_back(BB);

    for (BasicBlock *BB : Blocks) {
        for (Instruction *I : *BB) {
            if (!I || !I->getIRType() ||
                I->getIRType()->isVoid()) {
                continue;
            }
            if (I->hasUses())
                I->replaceAllUsesWith(
                    UndefValue::NewUndefValue(I->getIRType()));
        }
    }
    for (BasicBlock *BB : Blocks) BB->clear();
    for (BasicBlock *BB : Blocks) delete BB;

    Core.IterBlocks.clear();
    Core.Headers.clear();
    Core.Latches.clear();
    Core.Exitings.clear();
    Core.ValueMaps.clear();
}

void LoopUnrollPass::discardUncommittedUnrollClones(
    UnrolledBodyCore &Core) {
    PeeledBodyCore Clones;
    if (Core.IterBlocks.size() > 1) {
        Clones.IterBlocks.assign(Core.IterBlocks.begin() + 1,
                                 Core.IterBlocks.end());
    }
    discardUncommittedPeelClones(Clones);

    Core.IterBlocks.clear();
    Core.Headers.clear();
    Core.Latches.clear();
    Core.Exitings.clear();
    Core.ValueMaps.clear();
}

int LoopUnrollPass::nextUnrollSerial() {
    static int Serial = 0;
    return ++Serial;
}

std::string LoopUnrollPass::makeUnrollBlockIdent(Function *F,
                                 const BasicBlock *BB,
                                 int Iter,
                                 int UnrollSerial) {
    std::string Base = BB ? BB->getIdent() : "bb";

    Base += ".unr";
    Base += std::to_string(UnrollSerial);
    Base += ".";
    Base += std::to_string(Iter);

    return F ? F->makeUniqueBlockIdent(Base) : Base;
}

std::string LoopUnrollPass::makePeelBlockIdent(Function *F,
                               const BasicBlock *BB,
                               int Iter,
                               int PeelSerial) {
    std::string Base = BB ? BB->getIdent() : "bb";

    Base += ".peel";
    Base += std::to_string(PeelSerial);
    Base += ".";
    Base += std::to_string(Iter);

    return F ? F->makeUniqueBlockIdent(Base) : Base;
}

bool LoopUnrollPass::isSupportedCloneInstruction(Instruction *I) {
    if (!I) return false;

    // 2.2 所有展开/剥离路径共享该克隆门禁：计时调用及封装、ret、alloca 和未知二元指令不得被复制。
    if (auto *CI = dynamic_cast<CallInst *>(I)) {
        if (CI->mayReachTimingCall()) return false;
    }

    switch (I->instType) {
    case Instruction::RET:
    case Instruction::ALLOCA:
    case Instruction::BinaryUnknown:
        return false;
    default:
        return true;
    }
}

Value *LoopUnrollPass::stripSimpleCast(Value *V) {
    while (auto *I = dynamic_cast<Instruction *>(V)) {
        switch (I->instType) {
        case Instruction::TRUNC:
        case Instruction::ZEXT:
        case Instruction::SEXT:
        case Instruction::BITCAST:
            V = I->getUnaryOperand();
            continue;
        default:
            return V;
        }
    }
    return V;
}

std::optional<PeelPredicate> LoopUnrollPass::matchCompareWithIV(BinaryInst *Cmp, Value *IV) {
    // 3.2.5 穿过简单整数 cast 将内部分支规范化为 IV op Constant，再将首轮特化谓词映射为 1 或 2 次 peeling。
    Value *RawLHS = nullptr;
    Value *RawRHS = nullptr;
    if (!Cmp || !IV || !Cmp->getOperands(RawLHS, RawRHS))
        return std::nullopt;

    Value *LHS = stripSimpleCast(RawLHS);
    Value *RHS = stripSimpleCast(RawRHS);
    IV = stripSimpleCast(IV);

    if (LHS == IV) {
        if (auto C = RHS ? RHS->getIntConstant() : std::nullopt) {
            return PeelPredicate{Cmp->getOp(), *C};
        }
    }

    if (RHS == IV) {
        if (auto C = LHS ? LHS->getIntConstant() : std::nullopt) {
            return PeelPredicate{BinaryInst::reversePredicate(Cmp->getOp()), *C};
        }
    }

    return std::nullopt;
}

int LoopUnrollPass::peelCountForPredicate(const PeelPredicate &P) {
    switch (P.Op) {
    case BinaryInst::E:
        if (P.Constant == 0) return 1;
        return 0;
    case BinaryInst::L:
        if (P.Constant == 1) return 1;
        if (P.Constant == 2) return 2;
        return 0;
    case BinaryInst::LE:
        if (P.Constant == 0) return 1;
        if (P.Constant == 1) return 2;
        return 0;
    default:
        return 0;
    }
}

bool LoopUnrollPass::isInternalConditionalBranch(Loop *L, BrInst *Br) {
    if (!L || !Br || !Br->isConditional()) {
        return false;
    }

    auto *TrueBB = Br->getTrueSuccessor();
    auto *FalseBB = Br->getFalseSuccessor();
    return TrueBB && FalseBB && L->contains(TrueBB) && L->contains(FalseBB);
}

UnrollDecision LoopUnrollPass::makeNoUnrollDecision() {
    UnrollDecision Decision;
    Decision.Kind = UnrollKind::None;
    Decision.Count = 0;
    Decision.ShouldUnroll = false;
    Decision.NeedRemainder = false;
    Decision.IsRuntime = false;
    Decision.IsPeeling = false;
    return Decision;
}

bool LoopUnrollPass::withinThreshold(int64_t Cost, int Threshold) {
    return Threshold >= 0 && Cost <= static_cast<int64_t>(Threshold);
}

int LoopUnrollPass::chooseFactorBySize(int BodyCost, int SizeLimit, int MaxCount) {
    if (BodyCost <= 0 || SizeLimit <= 0 || MaxCount < 2)
        return 0;

    int Estimated = SizeLimit / BodyCost;
    int Factor = std::min(Estimated, MaxCount);

    if (Factor >= 4)
        return 4;
    if (Factor >= 2)
        return 2;
    return 0;
}

int LoopUnrollPass::localSCEVNodeCost(const SCEVExpr *E) {
    // 3.5.1 runtime 候选用抽象节点成本约束 trip-count SCEV 的物化规模，递推节点和除/模成本更高。
    if (!E) return 1000000;

    switch (E->Kind) {
    case SCEVExprKind::Constant:
        return 0;
    case SCEVExprKind::Unknown:
        return 1;
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub:
    case SCEVExprKind::Cast:
    case SCEVExprKind::Compare:
    case SCEVExprKind::Min:
    case SCEVExprKind::Max:
        return 1;
    case SCEVExprKind::Mul:
    case SCEVExprKind::AddRec:
        return 2;
    case SCEVExprKind::Div:
    case SCEVExprKind::Mod:
    case SCEVExprKind::MulRec:
    case SCEVExprKind::DivRec:
        return 4;
    }

    return 1000000;
}

std::optional<LoopExecutionShape>
LoopUnrollPass::getLoopExecutionShape(Loop *L, SCEVResult &SE) {
    // 2.1 统一提取 PreHeader/Header/Latch/Exiting/Exit，后续克隆和五种执行路径都使用同一 CFG 视图。
    if (!L) return std::nullopt;

    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch)) return std::nullopt;

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->Valid) return std::nullopt;
    if (!TCI->Header || !TCI->ExitingBlock || !TCI->ExitBlock) {
        return std::nullopt;
    }

    LoopExecutionShape Shape;
    Shape.PreHeader = PreHeader;
    Shape.Header = TCI->Header;
    Shape.Latch = Latch;
    Shape.Exiting = TCI->ExitingBlock;
    Shape.Exit = TCI->ExitBlock;
    return Shape;
}

bool LoopUnrollPass::canCloneAllInstructions(const std::vector<BasicBlock *> &Blocks) {
    // 2.2.1 在创建任何副本前对全部指令做试克隆，保证块克隆不会因单条不支持指令中途失败。
    std::unordered_map<Value *, Value *> VMap;
    for (BasicBlock *BB : Blocks) {
        if (BB) VMap[BB] = BB;
    }

    for (BasicBlock *BB : Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!isSupportedCloneInstruction(I)) return false;
            Instruction *Probe = I->clone(VMap);
            if (!Probe) return false;
            delete Probe;
        }
    }

    return true;
}

Value *LoopUnrollPass::mapValueAtIteration(Value *V,
                           int Iter,
                           const UnrolledBodyCore &Core) {
    if (!V || Iter <= 0) return V;
    if (Iter >= static_cast<int>(Core.ValueMaps.size())) return V;

    auto It = Core.ValueMaps[Iter].find(V);
    return It == Core.ValueMaps[Iter].end() ? V : It->second;
}

bool LoopUnrollPass::isBlockValue(Value *V) {
    return dynamic_cast<BasicBlock *>(V) != nullptr;
}

bool LoopUnrollPass::isDataValue(Value *V) {
    return V && !isBlockValue(V) && V->getIRType();
}

bool LoopUnrollPass::isValueCompatibleWithPhi(Value *V, PhiInst *Phi) {
    if (!Phi || !isDataValue(V)) return false;
    // 4.2.1 所有重建 PHI 输入必须具有完整同形类型，不允许仅存储大小相等的异形值混入 SSA。
    return IRType::hasSameShape(Phi->getIRType(), V->getIRType());
}

bool LoopUnrollPass::isValueCompatibleWithType(Value *V, std::shared_ptr<IRType> Ty) {
    if (!isDataValue(V) || !Ty) return false;
    return IRType::hasSameShape(Ty, V->getIRType());
}

Value *LoopUnrollPass::mapDataValueAtIteration(Value *V,
                               int Iter,
                               const UnrolledBodyCore &Core) {
    if (!V) return nullptr;
    if (Iter > 0 && Iter < static_cast<int>(Core.ValueMaps.size())) {
        auto It = Core.ValueMaps[Iter].find(V);
        if (It != Core.ValueMaps[Iter].end()) {
            return isDataValue(It->second) ? It->second : nullptr;
        }
    }
    return isDataValue(V) ? V : nullptr;
}

Value *LoopUnrollPass::mapDataValueAtPeelIteration(Value *V,
                                   int Iter,
                                   const PeeledBodyCore &Core) {
    if (!V) return nullptr;
    if (Iter >= 0 && Iter < static_cast<int>(Core.ValueMaps.size())) {
        auto It = Core.ValueMaps[Iter].find(V);
        if (It != Core.ValueMaps[Iter].end()) {
            return isDataValue(It->second) ? It->second : nullptr;
        }
    }
    return isDataValue(V) ? V : nullptr;
}

Value *LoopUnrollPass::getCapturedDataValueMappedAtIteration(Value *V,
                                             int Iter,
                                             const UnrolledBodyCore &Core) {
    if (!V || Iter <= 0 ||
        Iter >= static_cast<int>(Core.ValueMaps.size())) {
        return nullptr;
    }

    auto It = Core.ValueMaps[Iter].find(V);
    if (It == Core.ValueMaps[Iter].end()) return nullptr;
    return isDataValue(It->second) ? It->second : nullptr;
}

bool LoopUnrollPass::addIncomingChecked(PhiInst *Phi, Value *V, BasicBlock *Pred) {
    if (!Phi || !Pred || !isValueCompatibleWithPhi(V, Phi)) {
        return false;
    }
    Phi->addIncoming(V, Pred);
    return true;
}

bool LoopUnrollPass::rebuildPhiFromIncomingChecked(
    PhiInst *Phi,
    const std::vector<PhiInst::IncomingEdge> &Incoming,
    Value **Replacement) {
    if (!Phi) return false;
    for (const auto &Edge : Incoming) {
        if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi)) {
            return false;
        }
    }

    // 4.2.2 校验全部输入后再一次性提交：零入边替换为 undef，单入边折叠，多入边重建新 PHI。
    Value *Result = nullptr;
    if (Incoming.empty()) {
        Result = UndefValue::NewUndefValue(Phi->getIRType());
        Phi->replaceAllUsesWith(Result);
        delete Phi;
    } else if (Incoming.size() == 1) {
        Result = Incoming.front().V;
        Phi->replaceAllUsesWith(Result);
        delete Phi;
    } else {
        auto *NewPhi = new PhiInst(Phi->getIRType());
        for (const auto &Edge : Incoming) {
            NewPhi->addIncoming(Edge.V, Edge.Pred);
        }
        Phi->replaceAllUsesWith(NewPhi);
        Phi->replaceWith(NewPhi);
        delete Phi;
        Result = NewPhi;
    }

    if (Replacement) *Replacement = Result;
    return true;
}

bool LoopUnrollPass::replaceDataOperandsUsingMap(
    Instruction *I,
    const std::unordered_map<Value *, Value *> &VMap) {
    if (!I) return false;

    forEachOperand(
        I, [&VMap](size_t, Use *U, Value *Old) {
        if (!U) return;
        if (isBlockValue(Old)) return;
        auto It = VMap.find(Old);
        if (It == VMap.end()) return;
        if (!isValueCompatibleWithType(It->second, Old->getIRType())) return;
        U->replaceUseeWith(It->second);
        });
    return true;
}

bool LoopUnrollPass::removeClonedHeaderPhis(UnrolledBodyCore &Core, int Iter) {
    // 4.1.1 展开副本中的 Header PHI 不再是循环入口，用前一迭代映射后的回边值替换并记入 ValueMap。
    if (Iter <= 0 || Iter >= Core.Count) return false;

    BasicBlock *OrigHeader = Core.Headers.front();
    BasicBlock *OrigLatch = Core.Latches.front();
    BasicBlock *ClonedHeader = Core.Headers[Iter];
    if (!OrigHeader || !OrigLatch || !ClonedHeader) return false;

    auto OrigPhis = OrigHeader->collectPhiNodes();
    auto ClonedPhis = ClonedHeader->collectPhiNodes();
    if (ClonedPhis.size() < OrigPhis.size()) return false;

    for (size_t I = 0; I < OrigPhis.size(); ++I) {
        PhiInst *OrigPhi = OrigPhis[I];
        PhiInst *ClonedPhi = ClonedPhis[I];
        if (!OrigPhi || !ClonedPhi) return false;

        Value *BackedgeValue = OrigPhi->getIncomingFromPred(OrigLatch);
        if (!BackedgeValue) return false;

        Value *Replacement =
            mapDataValueAtIteration(BackedgeValue, Iter - 1, Core);
        if (!Replacement) return false;

        ClonedPhi->replaceAllUsesWith(Replacement);
        Core.ValueMaps[Iter][OrigPhi] = Replacement;
        delete ClonedPhi;
    }

    return true;
}

bool LoopUnrollPass::removePeeledHeaderPhis(PeeledBodyCore &Core, int Iter) {
    // 4.1.2 剥离副本的第 0 次 Header PHI 取原 preheader 初值，后续副本取上一剥离迭代的回边值。
    if (Iter < 0 || Iter >= Core.PeelCount) return false;

    BasicBlock *OrigHeader = Core.Shape.Header;
    BasicBlock *OrigLatch = Core.Shape.Latch;
    BasicBlock *PreHeader = Core.Shape.PreHeader;
    BasicBlock *ClonedHeader = Core.Headers[Iter];
    if (!OrigHeader || !OrigLatch || !PreHeader || !ClonedHeader) {
        return false;
    }

    auto OrigPhis = OrigHeader->collectPhiNodes();
    auto ClonedPhis = ClonedHeader->collectPhiNodes();
    if (ClonedPhis.size() < OrigPhis.size()) return false;

    for (size_t I = 0; I < OrigPhis.size(); ++I) {
        PhiInst *OrigPhi = OrigPhis[I];
        PhiInst *ClonedPhi = ClonedPhis[I];
        if (!OrigPhi || !ClonedPhi) return false;

        Value *Replacement = nullptr;
        if (Iter == 0) {
            Replacement = OrigPhi->getIncomingFromPred(PreHeader);
        } else {
            Value *BackedgeValue = OrigPhi->getIncomingFromPred(OrigLatch);
            if (!BackedgeValue) return false;
            Replacement =
                mapDataValueAtPeelIteration(BackedgeValue, Iter - 1, Core);
        }

        if (!isValueCompatibleWithPhi(Replacement, ClonedPhi)) {
            return false;
        }

        ClonedPhi->replaceAllUsesWith(Replacement);
        Core.ValueMaps[Iter][OrigPhi] = Replacement;
        delete ClonedPhi;
    }

    return true;
}

std::optional<UnrolledBodyCore>
LoopUnrollPass::cloneUnrolledBodyCore(Loop *L, int Count, SCEVResult &SE) {
    // 4.1 展开核心保留第 0 迭代为原循环，为 1..Count-1 克隆完整最内层循环区域并保存逐迭代值映射。
    if (!L || Count < 2 || !L->isInnermost()) return std::nullopt;

    auto Shape = getLoopExecutionShape(L, SE);
    if (!Shape || !Shape->Header || !Shape->Header->getParent()) {
        return std::nullopt;
    }

    std::vector<BasicBlock *> LoopBlocks = L->getBlocksInFunctionOrder();
    if (LoopBlocks.empty() || !canCloneAllInstructions(LoopBlocks)) {
        return std::nullopt;
    }

    Function *F = Shape->Header->getParent();
    int UnrollSerial = nextUnrollSerial();
    UnrolledBodyCore Core;
    Core.Shape = *Shape;
    Core.Count = Count;
    Core.IterBlocks.resize(Count);
    Core.IterBlocks[0] = LoopBlocks;
    Core.Headers.assign(Count, nullptr);
    Core.Latches.assign(Count, nullptr);
    Core.Exitings.assign(Count, nullptr);
    Core.ValueMaps.resize(Count);
    Core.Headers[0] = Shape->Header;
    Core.Latches[0] = Shape->Latch;
    Core.Exitings[0] = Shape->Exiting;
    auto Fail = [&]() -> std::optional<UnrolledBodyCore> {
        // 4.1.3 任一副本缺失关键块或 Header PHI 无法折叠时，销毁所有已建的额外迭代，原循环保持不变。
        discardUncommittedUnrollClones(Core);
        return std::nullopt;
    };

    for (int Iter = 1; Iter < Count; ++Iter) {
        CloneBlockRegionOptions Options;
        Options.MakeBlockIdent =
            [F, Iter, UnrollSerial](const BasicBlock *BB, std::size_t) {
                return makeUnrollBlockIdent(F, BB, Iter, UnrollSerial);
            };
        auto Cloned = cloneBasicBlockRegion(*F, LoopBlocks, {}, Options);
        if (!Cloned) return Fail();

        Core.Headers[Iter] = Cloned->getClonedBlock(Shape->Header);
        Core.Latches[Iter] = Cloned->getClonedBlock(Shape->Latch);
        Core.Exitings[Iter] = Cloned->getClonedBlock(Shape->Exiting);
        Core.ValueMaps[Iter] = std::move(Cloned->VMap);
        Core.IterBlocks[Iter] = std::move(Cloned->ClonedBlocks);
        if (!Core.Headers[Iter] || !Core.Latches[Iter] || !Core.Exitings[Iter]) {
            return Fail();
        }

        if (!removeClonedHeaderPhis(Core, Iter)) {
            return Fail();
        }
    }

    return Core;
}

std::optional<PeeledBodyCore>
LoopUnrollPass::clonePeeledIterations(Loop *L, int PeelCount, SCEVResult &SE) {
    // 4.1.4 peeling 不复用原循环作为第 0 副本，而是克隆 PeelCount 组完整迭代，以便将它们接到残余原循环之前。
    if (!L || PeelCount <= 0) return std::nullopt;

    auto Shape = getLoopExecutionShape(L, SE);
    if (!Shape || !Shape->Header || !Shape->PreHeader || !Shape->Latch ||
        !Shape->Exiting || !Shape->Exit || !Shape->Header->getParent()) {
        return std::nullopt;
    }

    std::vector<BasicBlock *> LoopBlocks = L->getBlocksInFunctionOrder();
    if (LoopBlocks.empty() || !canCloneAllInstructions(LoopBlocks)) {
        return std::nullopt;
    }

    Function *F = Shape->Header->getParent();
    int PeelSerial = nextUnrollSerial();
    PeeledBodyCore Core;
    Core.Shape = *Shape;
    Core.PeelCount = PeelCount;
    Core.IterBlocks.resize(PeelCount);
    Core.Headers.resize(PeelCount);
    Core.Latches.resize(PeelCount);
    Core.Exitings.resize(PeelCount);
    Core.ValueMaps.resize(PeelCount);
    auto Fail = [&]() -> std::optional<PeeledBodyCore> {
        discardUncommittedPeelClones(Core);
        return std::nullopt;
    };

    for (int Iter = 0; Iter < PeelCount; ++Iter) {
        CloneBlockRegionOptions Options;
        Options.MakeBlockIdent =
            [F, Iter, PeelSerial](const BasicBlock *BB, std::size_t) {
                return makePeelBlockIdent(F, BB, Iter, PeelSerial);
            };
        auto Cloned = cloneBasicBlockRegion(*F, LoopBlocks, {}, Options);
        if (!Cloned) return Fail();

        Core.Headers[Iter] = Cloned->getClonedBlock(Shape->Header);
        Core.Latches[Iter] = Cloned->getClonedBlock(Shape->Latch);
        Core.Exitings[Iter] = Cloned->getClonedBlock(Shape->Exiting);
        Core.ValueMaps[Iter] = std::move(Cloned->VMap);
        Core.IterBlocks[Iter] = std::move(Cloned->ClonedBlocks);
        if (!Core.Headers[Iter] || !Core.Latches[Iter] ||
            !Core.Exitings[Iter]) {
            return Fail();
        }

        if (!removePeeledHeaderPhis(Core, Iter)) {
            return Fail();
        }
    }

    return Core;
}

bool LoopUnrollPass::rewriteOriginalHeaderPhisForPartial(UnrolledBodyCore &Core,
                                         int LastIter,
                                         std::unordered_map<PhiInst *, Value *>
                                             *Replacements) {
    BasicBlock *Header = Core.Headers.front();
    BasicBlock *OrigLatch = Core.Latches.front();
    BasicBlock *LastLatch =
        LastIter >= 0 && LastIter < Core.Count ? Core.Latches[LastIter]
                                               : nullptr;
    if (!Header || !OrigLatch || !LastLatch || LastIter <= 0) return false;

    // 4.2.3 部分展开后原 Header 的回边不再来自原 Latch，而来自末副本 Latch 与映射后的末迭代状态。
    auto HeaderPhis = Header->collectPhiNodes();
    std::vector<PhiRewritePlan> Plans;
    Plans.reserve(HeaderPhis.size());

    for (PhiInst *Phi : HeaderPhis) {
        if (!Phi) continue;

        auto Incoming = Phi->collectIncomingEdges();
        PhiRewritePlan Plan;
        Plan.OldPhi = Phi;
        Plan.Incoming.reserve(Incoming.size());

        Value *BackedgeValue = nullptr;
        for (const auto &Edge : Incoming) {
            if (Edge.Pred == OrigLatch) {
                BackedgeValue = Edge.V;
                continue;
            }
            Plan.Incoming.push_back(Edge);
        }

        if (!BackedgeValue) return false;
        Value *MappedBackedge =
            mapDataValueAtIteration(BackedgeValue, LastIter, Core);
        if (!MappedBackedge) return false;

        Plan.Incoming.push_back({MappedBackedge, LastLatch});
        for (const auto &Edge : Plan.Incoming) {
            if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi)) {
                return false;
            }
        }
        Plans.push_back(std::move(Plan));
    }

    for (PhiRewritePlan &Plan : Plans) {
        if (!Plan.OldPhi) return false;
        if (Plan.Incoming.empty()) {
            Plan.Replacement =
                UndefValue::NewUndefValue(Plan.OldPhi->getIRType());
        } else if (Plan.Incoming.size() == 1) {
            Plan.Replacement = Plan.Incoming.front().V;
        } else {
            Plan.NewPhi = new PhiInst(Plan.OldPhi->getIRType());
            for (const auto &Edge : Plan.Incoming) {
                Plan.NewPhi->addIncoming(Edge.V, Edge.Pred);
            }
            Plan.Replacement = Plan.NewPhi;
        }
        if (!isDataValue(Plan.Replacement)) return false;
    }

    if (Replacements) {
        for (const PhiRewritePlan &Plan : Plans) {
            (*Replacements)[Plan.OldPhi] = Plan.Replacement;
        }
    }

    for (const PhiRewritePlan &Plan : Plans) {
        Plan.OldPhi->replaceAllUsesWith(Plan.Replacement);
    }
    for (const PhiRewritePlan &Plan : Plans) {
        if (Plan.NewPhi) {
            Plan.OldPhi->replaceWith(Plan.NewPhi);
        }
    }
    for (const PhiRewritePlan &Plan : Plans) {
        delete Plan.OldPhi;
    }

    return true;
}

bool LoopUnrollPass::rewriteOriginalHeaderPhisAfterPeeling(
    PeeledBodyCore &Core, bool ValidateOnly,
    std::unordered_map<PhiInst *, Value *> *Replacements) {
    BasicBlock *Header = Core.Shape.Header;
    BasicBlock *PreHeader = Core.Shape.PreHeader;
    BasicBlock *OrigLatch = Core.Shape.Latch;
    int LastIter = Core.PeelCount - 1;
    BasicBlock *LastPeeledLatch =
        LastIter >= 0 && LastIter < static_cast<int>(Core.Latches.size())
            ? Core.Latches[LastIter]
            : nullptr;

    if (!Header || !PreHeader || !OrigLatch || !LastPeeledLatch) {
        return false;
    }

    // 4.2.4 peeling 后删除原 preheader 初值入边，用最后剥离 Latch 与其映射后回边值作为残余循环新初值。
    auto HeaderPhis = Header->collectPhiNodes();
    std::vector<PhiRewritePlan> Plans;
    Plans.reserve(HeaderPhis.size());
    for (PhiInst *Phi : HeaderPhis) {
        if (!Phi) continue;

        auto Incoming = Phi->collectIncomingEdges();
        PhiRewritePlan Plan;
        Plan.OldPhi = Phi;
        Plan.Incoming.reserve(Incoming.size());

        Value *BackedgeValue = nullptr;
        for (const auto &Edge : Incoming) {
            if (Edge.Pred == PreHeader) {
                continue;
            }

            if (Edge.Pred == OrigLatch) {
                BackedgeValue = Edge.V;
            }

            Plan.Incoming.push_back(Edge);
        }

        if (!BackedgeValue) return false;

        Value *NewInitial =
            mapDataValueAtPeelIteration(BackedgeValue, LastIter, Core);
        if (!isValueCompatibleWithPhi(NewInitial, Phi)) {
            return false;
        }

        Plan.Incoming.push_back({NewInitial, LastPeeledLatch});

        for (const auto &Edge : Plan.Incoming) {
            if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi))
                return false;
        }
        Plans.push_back(std::move(Plan));
    }

    if (ValidateOnly) return true;

    for (PhiRewritePlan &Plan : Plans) {
        if (Plan.Incoming.empty()) {
            Plan.Replacement =
                UndefValue::NewUndefValue(Plan.OldPhi->getIRType());
        } else if (Plan.Incoming.size() == 1) {
            Plan.Replacement = Plan.Incoming.front().V;
        } else {
            Plan.NewPhi = new PhiInst(Plan.OldPhi->getIRType());
            for (const auto &Edge : Plan.Incoming)
                Plan.NewPhi->addIncoming(Edge.V, Edge.Pred);
            Plan.Replacement = Plan.NewPhi;
        }
    }
    for (PhiRewritePlan &Plan : Plans)
        Plan.OldPhi->replaceAllUsesWith(Plan.Replacement);
    if (Replacements) {
        for (const PhiRewritePlan &Plan : Plans)
            (*Replacements)[Plan.OldPhi] = Plan.Replacement;
    }
    for (PhiRewritePlan &Plan : Plans)
        if (Plan.NewPhi) Plan.OldPhi->replaceWith(Plan.NewPhi);
    for (PhiRewritePlan &Plan : Plans) delete Plan.OldPhi;

    return true;
}

bool LoopUnrollPass::removeIncomingFromExitPhisForPeeledBlocks(
    PeeledBodyCore &Core, bool ValidateOnly) {
    BasicBlock *Exit = Core.Shape.Exit;
    if (!Exit) return false;

    // 6.3 已证明循环长度大于 PeelCount 时，克隆退出检查会被删除，对应的 Exit PHI 入边也必须移除。
    std::unordered_set<BasicBlock *> RemovedPreds;
    for (BasicBlock *BB : Core.Exitings) {
        if (BB) RemovedPreds.insert(BB);
    }

    std::vector<PhiRewritePlan> Plans;
    for (PhiInst *Phi : Exit->collectPhiNodes()) {
        if (!Phi) continue;

        PhiRewritePlan P;
        P.OldPhi = Phi;
        for (const auto &Edge : Phi->collectIncomingEdges()) {
            if (!RemovedPreds.count(Edge.Pred)) {
                if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi))
                    return false;
                P.Incoming.push_back(Edge);
            }
        }
        Plans.push_back(std::move(P));
    }

    if (ValidateOnly) return true;
    for (PhiRewritePlan &P : Plans)
        if (!rebuildPhiFromIncomingChecked(P.OldPhi, P.Incoming)) return false;

    return true;
}

bool LoopUnrollPass::addIncomingToExitPhisForPeeledBlocks(
    PeeledBodyCore &Core, bool ValidateOnly) {
    BasicBlock *Exit = Core.Shape.Exit;
    BasicBlock *OrigExiting = Core.Shape.Exiting;
    if (!Exit || !OrigExiting || Core.Exitings.size() !=
                                      static_cast<size_t>(Core.PeelCount)) {
        return false;
    }

    // 6.2 未证明长度时每个剥离副本都保留短路退出，因此为 Exit PHI 添加每个克隆 Exiting 与映射值。
    std::vector<PhiRewritePlan> Plans;
    for (PhiInst *Phi : Exit->collectPhiNodes()) {
        if (!Phi) return false;
        Value *OrigValue = Phi->getIncomingFromPred(OrigExiting);
        if (!OrigValue) return false;

        PhiRewritePlan P;
        P.OldPhi = Phi;
        P.Incoming = Phi->collectIncomingEdges();
        for (const auto &Edge : P.Incoming)
            if (!Edge.Pred || !isValueCompatibleWithPhi(Edge.V, Phi))
                return false;
        for (int Iter = 0; Iter < Core.PeelCount; ++Iter) {
            BasicBlock *Pred = Core.Exitings[Iter];
            Value *Mapped =
                mapDataValueAtPeelIteration(OrigValue, Iter, Core);
            if (!Pred || !isValueCompatibleWithPhi(Mapped, Phi))
                return false;
            P.Incoming.push_back({Mapped, Pred});
        }
        Plans.push_back(std::move(P));
    }

    if (ValidateOnly) return true;
    for (PhiRewritePlan &P : Plans)
        if (!rebuildPhiFromIncomingChecked(P.OldPhi, P.Incoming)) return false;
    return true;
}

bool LoopUnrollPass::hasLatchExitShape(const UnrolledBodyCore &Core) {
    return Core.Shape.Exiting && Core.Shape.Latch &&
           Core.Shape.Exiting == Core.Shape.Latch;
}

std::unordered_set<BasicBlock *>
LoopUnrollPass::collectCoreExitings(const UnrolledBodyCore &Core) {
    std::unordered_set<BasicBlock *> Result;
    for (BasicBlock *BB : Core.Exitings) {
        if (BB) Result.insert(BB);
    }
    return Result;
}

bool LoopUnrollPass::rewriteExitPhisForSingleExiting(UnrolledBodyCore &Core,
                                     int ActiveIter) {
    if (ActiveIter < 0 || ActiveIter >= Core.Count) return false;

    BasicBlock *Exit = Core.Shape.Exit;
    BasicBlock *OrigExiting = Core.Shape.Exiting;
    BasicBlock *ActiveExiting = Core.Exitings[ActiveIter];
    if (!Exit || !OrigExiting || !ActiveExiting) return false;

    // 5.1 完全展开或 latch-tested 部分展开只保留一个有效退出副本，出口 PHI 删除其余 Exiting 入边并映射到 ActiveIter。
    auto RemovedPreds = collectCoreExitings(Core);
    auto ExitPhis = Exit->collectPhiNodes();
    for (PhiInst *Phi : ExitPhis) {
        if (!Phi) continue;

        Value *OrigValue = Phi->getIncomingFromPred(OrigExiting);
        Value *MappedValue =
            OrigValue ? mapDataValueAtIteration(OrigValue, ActiveIter, Core)
                      : UndefValue::NewUndefValue(Phi->getIRType());
        if (!MappedValue) return false;

        std::vector<PhiInst::IncomingEdge> Incoming;
        for (const auto &Edge : Phi->collectIncomingEdges()) {
            if (!RemovedPreds.count(Edge.Pred)) Incoming.push_back(Edge);
        }
        Incoming.push_back({MappedValue, ActiveExiting});
        if (!rebuildPhiFromIncomingChecked(Phi, Incoming)) return false;
    }
    return true;
}

std::vector<BasicBlock *> LoopUnrollPass::collectExitRegionBlocksFrom(
    BasicBlock *Exit,
    const std::unordered_set<BasicBlock *> &StopBlocks,
    bool StopAtConditionalBranch) {
    std::vector<BasicBlock *> Region;
    if (!Exit) return Region;

    std::vector<BasicBlock *> Roots{Exit};
    forEachReachablePreOrderCustomSucc<BasicBlock>(
        Roots,
        [&StopBlocks](BasicBlock *BB) {
            std::vector<BasicBlock *> Successors;
            if (!BB) return Successors;
            for (BasicBlock *Succ : BB->getSuccessors()) {
                if (!Succ || StopBlocks.count(Succ)) continue;
                Successors.push_back(Succ);
            }
            std::reverse(Successors.begin(), Successors.end());
            return Successors;
        },
        [&Region, Exit, &StopBlocks,
         StopAtConditionalBranch](BasicBlock *BB) {
            if (BB != Exit && StopBlocks.count(BB))
                return TraversalControl::SkipSuccessors;

            Region.push_back(BB);
            if (StopAtConditionalBranch &&
                BasicBlock::isCondBr(BB->getBranchTerminator())) {
                return TraversalControl::SkipSuccessors;
            }
            return TraversalControl::Continue;
        });

    return Region;
}

std::vector<ExitRegionDataUse> LoopUnrollPass::collectExitRegionDataUsesFrom(
    BasicBlock *Exit,
    const std::unordered_set<BasicBlock *> &StopBlocks,
    BasicBlock *Exiting,
    bool StopAtConditionalBranch) {
    // 5.3.1 在 CFG 重连前快照出口可达区域中的非 PHI 数据 use，后续按真正退出迭代的 ValueMap 改写。
    std::vector<ExitRegionDataUse> Uses;

    for (BasicBlock *BB : collectExitRegionBlocksFrom(
             Exit, StopBlocks, StopAtConditionalBranch)) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<PhiInst *>(I)) continue;
            forEachOperand(
                I, [Exit, Exiting, &Uses](size_t, Use *U, Value *Old) {
                if (!U) return;
                if (!isDataValue(Old)) return;
                Value *Lookup = Old;
                if (auto *Phi = dynamic_cast<PhiInst *>(Old)) {
                    if (Phi->getParent() == Exit && Exiting) {
                        if (Value *Incoming = Phi->getIncomingFromPred(Exiting)) {
                            Lookup = Incoming;
                        }
                    }
                }
                Uses.push_back({U, Old, Lookup, Old->getIRType()});
                });
        }
    }

    return Uses;
}

bool LoopUnrollPass::rewriteExitRegionDataOperandsForIteration(
    UnrolledBodyCore &Core,
    int ActiveIter,
    const std::vector<ExitRegionDataUse> &CapturedUses,
    const std::unordered_map<Value *, Value *> *HeaderExitValues) {
    if (ActiveIter <= 0 ||
        ActiveIter >= static_cast<int>(Core.ValueMaps.size())) {
        return false;
    }

    for (const ExitRegionDataUse &Entry : CapturedUses) {
        if (!Entry.OperandUse || !Entry.OriginalValue) continue;
        Value *Lookup = Entry.LookupValue ? Entry.LookupValue
                                          : Entry.OriginalValue;
        if (HeaderExitValues) {
            auto It = HeaderExitValues->find(Lookup);
            if (It != HeaderExitValues->end()) {
                if (isValueCompatibleWithType(It->second,
                                              Entry.ExpectedType)) {
                    Entry.OperandUse->replaceUseeWith(It->second);
                    continue;
                }
            }
        }
        Value *Mapped = getCapturedDataValueMappedAtIteration(
            Lookup, ActiveIter, Core);
        if (!Mapped) continue;
        if (!isValueCompatibleWithType(Mapped, Entry.ExpectedType)) continue;
        Entry.OperandUse->replaceUseeWith(Mapped);
    }

    return true;
}

bool LoopUnrollPass::replaceDataOperandsInReachableRegionFrom(
    BasicBlock *Start,
    const std::unordered_set<BasicBlock *> &StopBlocks,
    const std::unordered_map<Value *, Value *> &Replacements) {
    for (BasicBlock *BB :
         collectExitRegionBlocksFrom(Start, StopBlocks,
                                     false)) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            if (!replaceDataOperandsUsingMap(I, Replacements)) {
                return false;
            }
        }
    }
    return true;
}

void LoopUnrollPass::refreshCapturedExitRegionUses(
    std::vector<ExitRegionDataUse> &Uses,
    const std::unordered_map<Value *, Value *> &Replacements) {
    for (ExitRegionDataUse &Entry : Uses) {
        auto OrigIt = Replacements.find(Entry.OriginalValue);
        if (OrigIt != Replacements.end()) {
            Entry.OriginalValue = OrigIt->second;
        }

        auto LookupIt = Replacements.find(Entry.LookupValue);
        if (LookupIt != Replacements.end()) {
            Entry.LookupValue = LookupIt->second;
        }
    }
}

bool LoopUnrollPass::removeOriginalHeaderPhisForFull(UnrolledBodyCore &Core) {
    BasicBlock *Header = Core.Headers.front();
    BasicBlock *PreHeader = Core.Shape.PreHeader;
    if (!Header || !PreHeader) return false;

    auto HeaderPhis = Header->collectPhiNodes();
    for (PhiInst *Phi : HeaderPhis) {
        if (!Phi) continue;
        Value *InitialValue = Phi->getIncomingFromPred(PreHeader);
        if (!InitialValue) return false;
        Phi->replaceAllUsesWith(InitialValue);
        delete Phi;
    }

    return true;
}

bool LoopUnrollPass::makeStraightLineMain(UnrolledBodyCore &Core, int MainCount,
                          BasicBlock *FinalTarget, Value *ContinueCondition) {
    if (MainCount < 2 || MainCount > Core.Count || !FinalTarget) return false;
    if (!hasLatchExitShape(Core)) return false;

    for (int Iter = 0; Iter + 1 < MainCount; ++Iter) {
        if (!BasicBlock::replaceCondBrWithUncond(Core.Exitings[Iter],
                                                Core.Headers[Iter + 1])) {
            return false;
        }
    }

    BasicBlock *LastExiting = Core.Exitings[MainCount - 1];
    if (!LastExiting) return false;
    if (!ContinueCondition) {
        return BasicBlock::replaceCondBrWithUncond(LastExiting, FinalTarget);
    }

    return BasicBlock::replaceTerminator(
        LastExiting,
        new BrInst(ContinueCondition, Core.Headers.front(), FinalTarget));
}

Value *LoopUnrollPass::makeTypedIntConstant(int64_t V, std::shared_ptr<IRType> Ty) {
    auto IntegerType = std::dynamic_pointer_cast<INTType>(Ty);
    if (!IntegerType || IntegerType->getBitWidth() > 64)
        return nullptr;
    if (IntegerType->getIsBool())
        return ConstBool::newConstBool(V != 0);
    return ConstInt::newConstIntForType(V, IntegerType);
}

bool LoopUnrollPass::isValueAvailableAtPreheader(Value *V, Loop *L, BasicBlock *PreHeader,
                                 const DominatorTree &DT) {
    if (!V || !L || !PreHeader) return false;
    if (V->isConst()) return true;
    if (V->isParam() || V->isGlobal()) return true;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I || !I->getParent() || L->contains(I->getParent())) return false;
    return I->getParent() == PreHeader ||
           DT.dominates(I->getParent(), PreHeader);
}

bool LoopUnrollPass::hasSafeSCEVDivisor(const SCEVExpr *E) {
    if (!E || E->Operands.size() != 2) return false;
    const SCEVExpr *Divisor = E->Operands[1];
    return Divisor && Divisor->Kind == SCEVExprKind::Constant &&
           Divisor->ConstValue != 0 && Divisor->ConstValue != -1;
}

bool LoopUnrollPass::canMaterializeSCEV(const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
                        const DominatorTree &DT) {
    // 5.3.2 runtime SCEV 只允许 preheader 可用的常量/未知值和可安全组合的算术、比较、Min/Max/Cast，不物化任何递推节点。
    if (!E) return false;
    switch (E->Kind) {
    case SCEVExprKind::Constant:
        return E->ConstValue >= std::numeric_limits<int>::min() &&
               E->ConstValue <= std::numeric_limits<int>::max();
    case SCEVExprKind::Unknown:
        return isValueAvailableAtPreheader(E->RawValue, L, PreHeader, DT);
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub:
    case SCEVExprKind::Mul:
    case SCEVExprKind::Min:
    case SCEVExprKind::Max:
    case SCEVExprKind::Compare:
        return E->Operands.size() == 2 &&
               canMaterializeSCEV(E->Operands[0], L, PreHeader, DT) &&
               canMaterializeSCEV(E->Operands[1], L, PreHeader, DT);
    case SCEVExprKind::Div:
    case SCEVExprKind::Mod:
        return hasSafeSCEVDivisor(E) &&
               canMaterializeSCEV(E->Operands[0], L, PreHeader, DT) &&
               canMaterializeSCEV(E->Operands[1], L, PreHeader, DT);
    case SCEVExprKind::Cast:
        return E->Ty && E->Operands.size() == 1 &&
               canMaterializeSCEV(E->Operands[0], L, PreHeader, DT);
    case SCEVExprKind::AddRec:
    case SCEVExprKind::MulRec:
    case SCEVExprKind::DivRec:
        return false;
    }
    return false;
}

Value *LoopUnrollPass::materializeSCEV(const SCEVExpr *E, Loop *L, BasicBlock *PreHeader,
                       const DominatorTree &DT,
                       std::unordered_map<const SCEVExpr *, Value *> &Cache) {
    if (!canMaterializeSCEV(E, L, PreHeader, DT)) return nullptr;
    auto Cached = Cache.find(E);
    if (Cached != Cache.end()) return Cached->second;

    // 5.3.3 递归物化时用 Cache 共享子表达式，所有新指令都插在选定 preheader 的终结指令之前。
    Value *Result = nullptr;
    if (E->Kind == SCEVExprKind::Constant) {
        Result = makeTypedIntConstant(E->ConstValue, E->Ty);
    } else if (E->Kind == SCEVExprKind::Unknown) {
        Result = E->RawValue;
    } else if (E->Kind == SCEVExprKind::Cast) {
        Value *Op =
            materializeSCEV(E->Operands[0], L, PreHeader, DT, Cache);
        if (!Op) return nullptr;

        Instruction *Cast = nullptr;
        auto IntegerType =
            std::dynamic_pointer_cast<INTType>(E->Ty);
        switch (E->CastKind) {
        case SCEVCastKind::Trunc:
            if (!IntegerType) return nullptr;
            Cast = new TruncInst(Op, IntegerType);
            break;
        case SCEVCastKind::ZExt:
            if (!IntegerType) return nullptr;
            Cast = new ZextInst(Op, IntegerType);
            break;
        case SCEVCastKind::SExt:
            if (!IntegerType) return nullptr;
            Cast = new SextInst(Op, IntegerType);
            break;
        case SCEVCastKind::BitCast:
            Cast = new BitCastInst(Op, E->Ty);
            break;
        }
        if (!Cast) return nullptr;
        BasicBlock::insertBeforeTerminator(PreHeader, Cast);
        Result = Cast;
    } else {
        Value *LHS =
            materializeSCEV(E->Operands[0], L, PreHeader, DT, Cache);
        Value *RHS =
            materializeSCEV(E->Operands[1], L, PreHeader, DT, Cache);
        if (!LHS || !RHS) return nullptr;

        if (E->Kind == SCEVExprKind::Min ||
            E->Kind == SCEVExprKind::Max) {
            BinaryInst::Operation Pred =
                E->Kind == SCEVExprKind::Min ? BinaryInst::L
                                             : BinaryInst::G;
            auto *Cmp = new BinaryInst(LHS, Pred, RHS);
            BasicBlock::insertBeforeTerminator(PreHeader, Cmp);
            auto *Select = new SelectInst(Cmp, LHS, RHS);
            BasicBlock::insertBeforeTerminator(PreHeader, Select);
            Result = Select;
        } else {
            BinaryInst::Operation Op = BinaryInst::ADD;
            switch (E->Kind) {
            case SCEVExprKind::Add: Op = BinaryInst::ADD; break;
            case SCEVExprKind::Sub: Op = BinaryInst::SUB; break;
            case SCEVExprKind::Mul: Op = BinaryInst::MUL; break;
            case SCEVExprKind::Div: Op = BinaryInst::DIV; break;
            case SCEVExprKind::Mod: Op = BinaryInst::MOD; break;
            case SCEVExprKind::Compare: Op = E->Predicate; break;
            default: return nullptr;
            }
            auto *I = new BinaryInst(LHS, Op, RHS);
            BasicBlock::insertBeforeTerminator(PreHeader, I);
            Result = I;
        }
    }

    if (Result) Cache[E] = Result;
    return Result;
}

bool LoopUnrollPass::replaceValuesInIterationBlocks(
    UnrolledBodyCore &Core, int FirstIter,
    const std::unordered_map<Value *, Value *> &Replacements) {
    for (int Iter = FirstIter; Iter < Core.Count; ++Iter) {
        if (Iter < 0 || Iter >= static_cast<int>(Core.IterBlocks.size())) {
            return false;
        }
        for (BasicBlock *NewBB : Core.IterBlocks[Iter]) {
            if (!NewBB) continue;
            for (Instruction *I : *NewBB) {
                if (!replaceDataOperandsUsingMap(I, Replacements)) {
                    return false;
                }
            }
        }
    }
    return true;
}

Value *LoopUnrollPass::getValueBeforeLoop(Value *V, const UnrolledBodyCore &Core,
                          std::shared_ptr<IRType> FallbackTy) {
    if (!V) return UndefValue::NewUndefValue(std::move(FallbackTy));
    if (!isDataValue(V)) return nullptr;
    if (auto *Phi = dynamic_cast<PhiInst *>(V)) {
        if (Phi->getParent() == Core.Shape.Header) {
            Value *Initial = Phi->getIncomingFromPred(Core.Shape.PreHeader);
            if (isDataValue(Initial)) return Initial;
            return nullptr;
        }
    }
    if (auto *I = dynamic_cast<Instruction *>(V)) {
        if (I->getParent() && I->getParent() != Core.Shape.PreHeader) {
            for (const auto &MapEntry : Core.ValueMaps) {
                if (MapEntry.count(I)) {
                    return UndefValue::NewUndefValue(std::move(FallbackTy));
                }
            }
        }
    }
    return isDataValue(V) ? V : nullptr;
}

BasicBlock *LoopUnrollPass::getNonExitSuccessor(BasicBlock *Exiting, BasicBlock *Exit) {
    if (!Exiting || !Exit) return nullptr;
    for (BasicBlock *Succ : Exiting->getSuccessors()) {
        if (Succ && Succ != Exit) return Succ;
    }
    return nullptr;
}

Value *LoopUnrollPass::mapValueAfterIteration(Value *V, int Iter,
                              const std::vector<HeaderState> &States,
                              const UnrolledBodyCore &Core) {
    if (!isDataValue(V)) return nullptr;
    for (const HeaderState &State : States) {
        if (V == State.CurrentValue) {
            Value *Mapped = State.BackedgeIsOriginalPhi
                                ? State.CurrentValue
                                : mapDataValueAtIteration(
                                      State.BackedgeValue, Iter, Core);
            if (Iter >= State.TailStartIter && State.TailStateValue &&
                Mapped == State.CurrentValue) {
                return State.TailStateValue;
            }
            return Mapped;
        }
    }
    return mapDataValueAtIteration(V, Iter, Core);
}

bool LoopUnrollPass::buildHeaderExitValueMap(
    const std::vector<HeaderState> &HeaderStates,
    int ExitIter,
    const UnrolledBodyCore &Core,
    BasicBlock *PreHeader,
    std::unordered_map<Value *, Value *> &HeaderExitValues) {
    HeaderExitValues.clear();

    for (const HeaderState &State : HeaderStates) {
        Value *ExitValue = mapValueAfterIteration(State.CurrentValue,
                                                  ExitIter,
                                                  HeaderStates,
                                                  Core);
        if (!isDataValue(ExitValue)) return false;

        HeaderExitValues[State.CurrentValue] = ExitValue;
        HeaderExitValues[State.MainValue] = ExitValue;
        HeaderExitValues[State.BackedgeValue] = ExitValue;
        if (State.CurrentPhi) {
            for (const auto &Edge : State.CurrentPhi->collectIncomingEdges()) {
                if (Edge.Pred != PreHeader && isDataValue(Edge.V)) {
                    HeaderExitValues[Edge.V] = ExitValue;
                }
            }
        }
    }

    return true;
}

bool LoopUnrollPass::buildHeaderTailInputValueMap(
    const std::vector<HeaderState> &HeaderStates,
    BasicBlock *PreHeader,
    std::unordered_map<Value *, Value *> &TailInputValues) {
    TailInputValues.clear();

    for (const HeaderState &State : HeaderStates) {
        Value *InputValue = State.TailStateValue ? State.TailStateValue
                                                 : State.CurrentValue;
        if (!isDataValue(InputValue)) return false;

        TailInputValues[State.CurrentValue] = InputValue;
        TailInputValues[State.MainValue] = InputValue;
        TailInputValues[State.BackedgeValue] = InputValue;
        if (State.CurrentPhi) {
            for (const auto &Edge : State.CurrentPhi->collectIncomingEdges()) {
                if (Edge.Pred != PreHeader && isDataValue(Edge.V)) {
                    TailInputValues[Edge.V] = InputValue;
                }
            }
        }
    }

    return true;
}

LoopUnrollResult LoopUnrollPass::executeUnrollWithTail(
    Loop *L, int MainCount, int TailCopies, Value *NumGroups,
    Value *RemainderValue, bool IsRuntime, SCEVResult &SE) {
    if (!L || MainCount < 2 || TailCopies < 1 || !NumGroups) {
        return LoopUnrollResult::Unmodified;
    }

    // 5.3 带 tail 的公共引擎同时服务常量余数和 runtime 展开：MainCount 个副本组成主循环，TailCopies 个副本处理余数。
    auto PreCloneShape = getLoopExecutionShape(L, SE);
    if (!PreCloneShape || !PreCloneShape->Exit) {
        return LoopUnrollResult::Unmodified;
    }
    std::unordered_set<BasicBlock *> PreCloneLoopBlocks;
    for (BasicBlock *BB : L->getBlocksInFunctionOrder()) {
        if (BB) PreCloneLoopBlocks.insert(BB);
    }
    const bool PreCloneLatchExiting =
        PreCloneShape->Exiting && PreCloneShape->Latch &&
        PreCloneShape->Exiting == PreCloneShape->Latch;
    // 5.3.1 退出形态互斥分类：单块循环虽然 Header==Latch==Exiting，但它先执行循环体，必须归入 latch-tested，不能同时按 header-tested 接 tail PHI。
    const bool PreCloneHeaderExiting =
        !PreCloneLatchExiting && PreCloneShape->Exiting &&
        PreCloneShape->Header &&
        PreCloneShape->Exiting == PreCloneShape->Header;
    std::vector<ExitRegionDataUse> ExitRegionUses =
        collectExitRegionDataUsesFrom(PreCloneShape->Exit,
                                      PreCloneLoopBlocks,
                                      PreCloneShape->Exiting,
                                      PreCloneHeaderExiting);

    // 5.3.2 先快照出口区域 use，再一次克隆主副本和 tail 副本；在原 SSA/CFG 提交前的形状失败可销毁全部额外副本。
    auto Core = cloneUnrolledBodyCore(L, MainCount + TailCopies, SE);
    if (!Core) {
        return LoopUnrollResult::Unmodified;
    }
    const bool LatchExiting = hasLatchExitShape(*Core);
    const bool HeaderExiting =
        !LatchExiting && Core->Shape.Exiting && Core->Shape.Header &&
        Core->Shape.Exiting == Core->Shape.Header;
    if (!LatchExiting && !HeaderExiting) {
        discardUncommittedUnrollClones(*Core);
        return LoopUnrollResult::Unmodified;
    }

    BasicBlock *Header = Core->Shape.Header;
    BasicBlock *PreHeader = Core->Shape.PreHeader;
    BasicBlock *MainLast = Core->Latches[MainCount - 1];
    BasicBlock *MainTailPred =
        HeaderExiting ? Header : Core->Exitings[MainCount - 1];
    BasicBlock *Exit = Core->Shape.Exit;
    Function *F = Header ? Header->getParent() : nullptr;
    if (!Header || !PreHeader || !MainLast || !MainTailPred || !Exit || !F) {
        discardUncommittedUnrollClones(*Core);
        return LoopUnrollResult::Unmodified;
    }

    // 5.3.3 为每个原 Header PHI 快照初值、回边值和主组末状态，然后将原回边统一改为主组最后副本。
    std::vector<HeaderState> HeaderStates;
    for (PhiInst *Phi : Header->collectPhiNodes()) {
        Value *Initial = Phi->getIncomingFromPred(PreHeader);
        Value *Backedge = Phi->getIncomingFromPred(Core->Shape.Latch);
        Value *MainValue =
            mapDataValueAtIteration(Backedge, MainCount - 1, *Core);
        if (!isDataValue(Initial) || !isDataValue(Backedge) ||
            !isDataValue(MainValue)) {
            discardUncommittedUnrollClones(*Core);
            return LoopUnrollResult::Unmodified;
        }
        HeaderStates.push_back(
            {Phi, nullptr, nullptr, Initial, Backedge, MainValue, nullptr,
             std::numeric_limits<int>::max(), MainValue == Phi,
             Backedge == Phi});
    }

    std::unordered_map<PhiInst *, Value *> HeaderPhiReplacements;
    if (!rewriteOriginalHeaderPhisForPartial(*Core, MainCount - 1,
                                             &HeaderPhiReplacements)) {
        discardUncommittedUnrollClones(*Core);
        return LoopUnrollResult::Unmodified;
    }

    for (HeaderState &State : HeaderStates) {
        auto It = HeaderPhiReplacements.find(State.OriginalPhi);
        if (It == HeaderPhiReplacements.end() ||
            !isDataValue(It->second)) {
            return LoopUnrollResult::Unmodified;
        }
        State.CurrentValue = It->second;
        State.CurrentPhi = dynamic_cast<PhiInst *>(State.CurrentValue);
        if (State.MainValueIsOriginalPhi) {
            State.MainValue = State.CurrentValue;
        }
    }
    std::unordered_map<Value *, Value *> HeaderValueReplacements;
    for (const auto &Entry : HeaderPhiReplacements) {
        HeaderValueReplacements[Entry.first] = Entry.second;
    }
    if (HeaderExiting) {
        refreshCapturedExitRegionUses(ExitRegionUses,
                                      HeaderValueReplacements);
    }

    // 5.3.4 TailEntry 是主循环与余数副本的分派/状态合流点；runtime 路径还在此合并“零个主组”和“完成主组”的 Header 状态。
    auto *TailEntry = new BasicBlock();
    std::string TailBase =
        Header->getIdent() +
        (IsRuntime ? ".unr.runtime.tail" : ".unr.constant.tail");
    TailEntry->setIdent(F->makeUniqueBlockIdent(TailBase));
    F->pushBlock(TailEntry);

    if (IsRuntime || HeaderExiting) {
        std::unordered_map<Value *, Value *> TailStateMap;
        for (HeaderState &State : HeaderStates) {
            Value *TailState = State.CurrentValue;
            if (IsRuntime) {
                auto *Merge = new PhiInst(State.InitialValue->getIRType());
                Value *MainIncoming =
                    HeaderExiting ? State.CurrentValue : State.MainValue;
                if (!addIncomingChecked(Merge, State.InitialValue,
                                        PreHeader) ||
                    !addIncomingChecked(Merge, MainIncoming,
                                        MainTailPred)) {
                    delete Merge;
                    return LoopUnrollResult::Unmodified;
                }
                TailEntry->pushBack(Merge);
                TailState = Merge;
            }
            if (!isDataValue(State.MainValue) || !isDataValue(TailState)) {
                return LoopUnrollResult::Unmodified;
            }
            State.TailStateValue = TailState;
            State.TailStartIter = MainCount;
            TailStateMap[State.MainValue] = TailState;
            if (HeaderExiting) {
                TailStateMap[State.CurrentValue] = TailState;
                TailStateMap[State.BackedgeValue] = TailState;
                if (State.CurrentPhi) {
                    for (const auto &Edge :
                         State.CurrentPhi->collectIncomingEdges()) {
                        if (Edge.Pred != PreHeader && isDataValue(Edge.V)) {
                            TailStateMap[Edge.V] = TailState;
                        }
                    }
                }
            }
        }
        if (!replaceValuesInIterationBlocks(*Core, MainCount, TailStateMap)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    // 5.3.5 GroupPhi 在原 Header 计数已完成的展开组，每组结尾的 NextGroup 决定继续主循环还是进入 TailEntry。
    auto *GroupPhi = new PhiInst(NumGroups->getIRType());
    Value *Zero = makeTypedIntConstant(0, NumGroups->getIRType());
    Value *One = makeTypedIntConstant(1, NumGroups->getIRType());
    if (!Zero || !One) {
        return LoopUnrollResult::Unmodified;
    }
    GroupPhi->addIncoming(Zero, PreHeader);
    Header->pushFront(GroupPhi);

    auto *NextGroup = new BinaryInst(GroupPhi, BinaryInst::ADD, One);
    BasicBlock::insertBeforeTerminator(MainLast, NextGroup);
    GroupPhi->addIncoming(NextGroup, MainLast);

    // 5.3.6 latch-tested 主组删除前 MainCount-1 个退出检查并直连下一副本；header-tested 主组则保留第一头检查作为组计数门禁。
    if (LatchExiting) {
        auto *MoreGroups =
            new BinaryInst(NextGroup, BinaryInst::L, NumGroups);
        BasicBlock::insertBeforeTerminator(MainLast, MoreGroups);
        if (!makeStraightLineMain(*Core, MainCount, TailEntry,
                                  MoreGroups)) {
            return LoopUnrollResult::Unmodified;
        }
    } else {
        BasicBlock *FirstBody = getNonExitSuccessor(Header, Exit);
        if (!FirstBody) {
            return LoopUnrollResult::Unmodified;
        }

        auto *HasGroup =
            new BinaryInst(GroupPhi, BinaryInst::L, NumGroups);
        BasicBlock::insertBeforeTerminator(Header, HasGroup);
        if (!BasicBlock::replaceTerminator(
                Header, new BrInst(HasGroup, FirstBody, TailEntry))) {
            return LoopUnrollResult::Unmodified;
        }

        for (int Iter = 1; Iter < MainCount; ++Iter) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Exitings[Iter], Exit);
            if (!Body ||
                !BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Body)) {
                return LoopUnrollResult::Unmodified;
            }
        }
        for (int Iter = 0; Iter + 1 < MainCount; ++Iter) {
            if (!BasicBlock::redirectBranchTarget(
                    Core->Latches[Iter], Core->Headers[Iter],
                    Core->Headers[Iter + 1])) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!BasicBlock::redirectBranchTarget(
                Core->Latches[MainCount - 1],
                Core->Headers[MainCount - 1], Header)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    // 5.3.7 常量余数将 tail 副本全部直线连接；runtime 则在每个非末副本后比较 Remainder 决定继续或退出。
    for (int Tail = 0; Tail < TailCopies; ++Tail) {
        int Iter = MainCount + Tail;
        BasicBlock *TailExiting =
            LatchExiting ? Core->Exitings[Iter] : Core->Latches[Iter];
        if (!TailExiting) {
            return LoopUnrollResult::Unmodified;
        }

        if (HeaderExiting) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Exitings[Iter], Exit);
            if (!Body ||
                !BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Body)) {
                return LoopUnrollResult::Unmodified;
            }
        }

        if (!IsRuntime) {
            BasicBlock *Target =
                Tail + 1 < TailCopies ? Core->Headers[Iter + 1] : Exit;
            if (!BasicBlock::replaceCondBrWithUncond(TailExiting, Target)) {
                return LoopUnrollResult::Unmodified;
            }
            continue;
        }

        if (Tail + 1 == TailCopies) {
            if (!BasicBlock::replaceCondBrWithUncond(TailExiting, Exit)) {
                return LoopUnrollResult::Unmodified;
            }
            continue;
        }

        Value *Threshold =
            makeTypedIntConstant(Tail + 1, RemainderValue->getIRType());
        if (!Threshold) {
            return LoopUnrollResult::Unmodified;
        }
        auto *HasMore =
            new BinaryInst(RemainderValue, BinaryInst::G, Threshold);
        BasicBlock::insertBeforeTerminator(TailExiting, HasMore);
        if (!BasicBlock::replaceTerminator(TailExiting,
                           new BrInst(HasMore, Core->Headers[Iter + 1],
                                      Exit))) {
            return LoopUnrollResult::Unmodified;
        }
    }

    // 5.3.8 重建 Exit PHI：runtime 同时合流零主组、主组无 tail 和每个 tail 退出；常量余数只保留最后 tail 副本。
    auto RemovedPreds = collectCoreExitings(*Core);
    auto ExitPhis = Exit->collectPhiNodes();
    for (PhiInst *Phi : ExitPhis) {
        Value *OrigValue = Phi->getIncomingFromPred(Core->Shape.Exiting);
        std::vector<PhiInst::IncomingEdge> Incoming;
        for (const auto &Edge : Phi->collectIncomingEdges()) {
            if (!RemovedPreds.count(Edge.Pred)) Incoming.push_back(Edge);
        }

        if (IsRuntime) {
            Value *InitialExit =
                getValueBeforeLoop(OrigValue, *Core, Phi->getIRType());
            Value *MainExit =
                OrigValue ? (HeaderExiting
                                 ? OrigValue
                                 : mapValueAfterIteration(
                                       OrigValue, MainCount - 1,
                                       HeaderStates, *Core))
                          : UndefValue::NewUndefValue(Phi->getIRType());
            if (!InitialExit || !MainExit) {
                return LoopUnrollResult::Unmodified;
            }
            auto *NoTailValue = new PhiInst(Phi->getIRType());
            if (!addIncomingChecked(NoTailValue, InitialExit, PreHeader) ||
                !addIncomingChecked(NoTailValue, MainExit, MainTailPred)) {
                delete NoTailValue;
                return LoopUnrollResult::Unmodified;
            }
            TailEntry->pushBack(NoTailValue);
            Incoming.push_back({NoTailValue, TailEntry});

            for (int Tail = 0; Tail < TailCopies; ++Tail) {
                int Iter = MainCount + Tail;
                Value *TailValue =
                    OrigValue ? mapValueAfterIteration(
                                    OrigValue, Iter, HeaderStates, *Core)
                              : UndefValue::NewUndefValue(Phi->getIRType());
                if (!TailValue) {
                    return LoopUnrollResult::Unmodified;
                }
                Incoming.push_back(
                    {TailValue, LatchExiting ? Core->Exitings[Iter]
                                             : Core->Latches[Iter]});
            }
        } else {
            int LastTailIter = MainCount + TailCopies - 1;
            Value *TailValue =
                OrigValue ? mapValueAfterIteration(
                                OrigValue, LastTailIter, HeaderStates, *Core)
                    : UndefValue::NewUndefValue(Phi->getIRType());
            if (!TailValue) {
                return LoopUnrollResult::Unmodified;
            }
            Incoming.push_back(
                {TailValue, LatchExiting ? Core->Exitings[LastTailIter]
                                         : Core->Latches[LastTailIter]});
        }

        if (!rebuildPhiFromIncomingChecked(Phi, Incoming)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    if (!IsRuntime) {
        int LastTailIter = MainCount + TailCopies - 1;
        std::unordered_map<Value *, Value *> HeaderExitValues;
        if (HeaderExiting) {
            if (!buildHeaderExitValueMap(HeaderStates, LastTailIter,
                                         *Core, PreHeader,
                                         HeaderExitValues)) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!rewriteExitRegionDataOperandsForIteration(
                *Core, LastTailIter, ExitRegionUses,
                HeaderExiting ? &HeaderExitValues : nullptr)) {
            return LoopUnrollResult::Unmodified;
        }
    }

    // 5.3.9 最后安装入口分派：runtime 先判断 NumGroups>0，TailEntry 再判断 Remainder!=0；常量余数则无条件进入第一 tail 副本。
    if (IsRuntime) {
        if (!RemainderValue) {
            return LoopUnrollResult::Unmodified;
        }
        Value *ZeroRemainder =
            makeTypedIntConstant(0, RemainderValue->getIRType());
        if (!ZeroRemainder) {
            return LoopUnrollResult::Unmodified;
        }
        auto *HasRemainder =
            new BinaryInst(RemainderValue, BinaryInst::NE, ZeroRemainder);
        TailEntry->pushBack(HasRemainder);
        TailEntry->pushBack(
            new BrInst(HasRemainder, Core->Headers[MainCount], Exit));

        Value *ZeroGroups =
            makeTypedIntConstant(0, NumGroups->getIRType());
        if (!ZeroGroups) {
            return LoopUnrollResult::Unmodified;
        }
        auto *HasMain =
            new BinaryInst(NumGroups, BinaryInst::G, ZeroGroups);
        BasicBlock::insertBeforeTerminator(PreHeader, HasMain);
        if (!BasicBlock::replaceTerminator(
                PreHeader,
                new BrInst(HasMain, Core->Headers.front(), TailEntry))) {
            return LoopUnrollResult::Unmodified;
        }
    } else {
        TailEntry->pushBack(new BrInst(Core->Headers[MainCount]));
        if (HeaderExiting) {
            std::unordered_map<Value *, Value *> TailInputValues;
            if (!buildHeaderTailInputValueMap(HeaderStates, PreHeader,
                                              TailInputValues)) {
                return LoopUnrollResult::Unmodified;
            }
            auto TailScanStopBlocks = PreCloneLoopBlocks;
            TailScanStopBlocks.insert(Exit);
            if (!replaceDataOperandsInReachableRegionFrom(
                    TailEntry, TailScanStopBlocks, TailInputValues)) {
                return LoopUnrollResult::Unmodified;
            }
        }
    }

    SE.clear();
    return LoopUnrollResult::PartiallyUnrolled;
}

LoopUnrollPass::LoopUnrollPass(UnrollConfig ConfigValue)
    : Config(std::move(ConfigValue)) {}

const UnrollConfig &LoopUnrollPass::getConfig() const {
    return Config;
}

PreservationStatus LoopUnrollPass::run(Function &F,
                                       FunctionAnalysisPassManager &FAM) {
    auto &AlreadyTransformed = TransformedLoopHeaders[&F];
    // 1. 只快照 pass 入口时存在且未记录的循环头；成功头跨调用保留，阻止 4x→16x 式重复倍增，新产生的残余头则留给后续有界轮次。
    std::vector<BasicBlock *> CandidateHeaders;
    {
        auto &InitialLI = FAM.getResult<LoopAnalysis>(F);
        for (Loop *L : InitialLI.getLoopsInPostOrder(true)) {
            if (L && L->getHeader() &&
                AlreadyTransformed.count(L->getHeader()->getIdent()) == 0)
                CandidateHeaders.push_back(L->getHeader());
        }
    }

    if (CandidateHeaders.empty()) return PreservationStatus::all();

    // 1.1 每次调用最多成功四个循环，统一约束函数级代码增长和反复重建分析的编译成本。
    static constexpr size_t MaxTransformsPerRun = 4;
    size_t TransformCount = 0;
    bool Changed = false;
    for (BasicBlock *Header : CandidateHeaders) {
        // 1.2 早先的外循环完全展开可能删除快照中的内头；在函数块列表中用指针身份确认存活前不解引它。
        bool HeaderIsLive = false;
        for (BasicBlock *BB : F) {
            if (BB == Header) {
                HeaderIsLive = true;
                break;
            }
        }
        if (!HeaderIsLive) continue;

        auto &LI = FAM.getResult<LoopAnalysis>(F);
        Loop *L = LI.getLoopFor(Header);
        if (!L || L->getHeader() != Header) continue;

        auto &DT = FAM.getResult<DominantAnalysis>(F);
        auto &SE = FAM.getResult<SCEVAnalysis>(F);
        auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
        auto &RI = FAM.getResult<RangeAnalysis>(F);
        const std::string HeaderIdent = Header->getIdent();
        LoopUnrollResult Result =
            tryToUnrollLoop(L, LI, DT, SE, AA, RI);
        if (Result == LoopUnrollResult::Unmodified) continue;

        AlreadyTransformed.insert(HeaderIdent);
        Changed = true;
        ++TransformCount;
        SE.clear();

        // 7. 克隆和 CFG/内存改写使当前 Loop/DT/SCEV/Range/AA/MemorySSA 引用全部过期；处理下一个入口快照头前失效所有函数分析。
        FAM.invalidate(F, PreservationStatus::none());

        if (TransformCount >= MaxTransformsPerRun) break;
    }

    return Changed ? PreservationStatus::none()
                   : PreservationStatus::all();
}

LoopUnrollResult LoopUnrollPass::tryToUnrollLoop(Loop *L,
                                                 LoopInfo &LI,
                                                 DominatorTree &DT,
                                                 SCEVResult &SE,
                                                 BasicAAResult &AA,
                                                 RangeInfo &RI) {
    if (!L) {
        return LoopUnrollResult::Unmodified;
    }

    // 2. 单候选先扫描整个循环子树，任何直接或经用户函数可达的计时调用都禁止复制或改变首迭代位置。
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return LoopUnrollResult::Unmodified;
        for (Instruction *I : *BB) {
            auto *CI = dynamic_cast<CallInst *>(I);
            if (CI && CI->mayReachTimingCall()) {
                return LoopUnrollResult::Unmodified;
            }
        }
    }

    // 2.1 依次证明规范 CFG/LCSSA、可用 trip-count 和全循环指令可克隆，再估算直属循环体成本。
    if (!isCanonicalLoop(L)) {
        return LoopUnrollResult::Unmodified;
    }

    auto TripInfo = SE.analyzeLoopTripCount(L);
    if (!TripInfo || !TripInfo->Valid) {
        return LoopUnrollResult::Unmodified;
    }

    if (!canCloneLoopBody(L, LI)) {
        return LoopUnrollResult::Unmodified;
    }

    int BodyCost = estimateLoopBodyCost(L, LI);
    UnrollDecision Decision =
        computeUnrollDecision(
            L, *TripInfo, BodyCost, Config, LI, SE, AA, RI, DT);

    if (!Decision.ShouldUnroll || Decision.Kind == UnrollKind::None) {
        return LoopUnrollResult::Unmodified;
    }

    // 3. 决策完成后只分派到一条执行路径，执行阶段仍会重新核验与该路径相关的 trip/CFG 条件。
    switch (Decision.Kind) {
    case UnrollKind::Full:
        return executeFullUnroll(L, Decision, SE, LI, DT);
    case UnrollKind::Partial:
        return executePartialUnroll(L, Decision, SE, LI, DT);
    case UnrollKind::PartialWithRemainder:
        return executePartialWithRemainder(L, Decision, SE, LI, DT);
    case UnrollKind::Runtime:
        return executeRuntimeUnroll(L, Decision, SE, LI, DT);
    case UnrollKind::Peeling:
        return executePeeling(L, Decision, SE, LI, DT, AA);
    case UnrollKind::None:
        break;
    }

    return LoopUnrollResult::Unmodified;
}

bool LoopUnrollPass::isCanonicalLoop(Loop *L) const {
    if (!L || !L->getHeader()) {
        return false;
    }

    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) || !PreHeader || !Latch) {
        return false;
    }

    if (!LCSSAPass::isLCSSAForm(L)) {
        return false;
    }

    std::vector<BasicBlock *> ExitBlocks = L->getExitBlocks();
    if (ExitBlocks.size() != 1 || !ExitBlocks.front()) {
        return false;
    }

    std::unordered_set<BasicBlock *> ExitingBlocks;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (L->isExitingBlock(BB)) {
            ExitingBlocks.insert(BB);
        }
    }

    return ExitingBlocks.size() == 1;
}

bool LoopUnrollPass::canCloneLoopBody(Loop *L, LoopInfo &LI) const {
    (void)LI;
    if (!L) return false;

    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (auto It = BB->begin(); It != BB->end(); ++It) {
            if (!isSupportedCloneInstruction(*It)) {
                return false;
            }
        }
    }

    return true;
}

int LoopUnrollPass::estimateLoopBodyCost(Loop *L, LoopInfo &LI) const {
    if (!L) return 0;

    int Cost = 0;
    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB || LI.getLoopFor(BB) != L) continue;

        for (auto It = BB->begin(); It != BB->end(); ++It) {
            Instruction *I = *It;
            if (!I) continue;

            int LocalCost = 0;
            switch (I->instType) {
            case Instruction::PHI:
                continue;
            case Instruction::ADD:
            case Instruction::SUB:
            case Instruction::FADD:
            case Instruction::FSUB:
            case Instruction::ICMP:
            case Instruction::FCMP:
            case Instruction::TRUNC:
            case Instruction::ZEXT:
            case Instruction::SEXT:
            case Instruction::FPTOSI:
            case Instruction::SITOFP:
            case Instruction::BITCAST:
            case Instruction::AND:
            case Instruction::OR:
            case Instruction::XOR:
            case Instruction::SHL:
            case Instruction::LSHR:
            case Instruction::ASHR:
            case Instruction::SELECT:
                LocalCost = 1;
                break;
            case Instruction::MUL:
            case Instruction::FMUL:
                LocalCost = 3;
                break;
            case Instruction::SDIV:
            case Instruction::UDIV:
            case Instruction::SREM:
            case Instruction::UREM:
                LocalCost = 10;
                break;
            case Instruction::FDIV:
            case Instruction::FREM:
                LocalCost = 10;
                break;
            case Instruction::LOAD:
            case Instruction::STORE:
            case Instruction::GEP:
                LocalCost = 2;
                break;
            case Instruction::CALL:
                LocalCost = 20;
                break;
            case Instruction::BR:
            case Instruction::RET:
                LocalCost = 1;
                break;
            default:
                LocalCost = 1;
                break;
            }
            // 2.3 代价以抽象指令单位饱和累加，防止超大循环的成本和发生有符号溢出。
            if (Cost > std::numeric_limits<int>::max() - LocalCost)
                return std::numeric_limits<int>::max();
            Cost += LocalCost;
        }
    }

    return Cost;
}

bool LoopUnrollPass::containsCall(Loop *L, LoopInfo &LI) const {
    (void)LI;
    if (!L) return false;

    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (!BB) continue;
        for (auto It = BB->begin(); It != BB->end(); ++It) {
            Instruction *I = *It;
            if (I && I->instType == Instruction::CALL) {
                return true;
            }
        }
    }

    return false;
}

int LoopUnrollPass::estimateSCEVCost(const SCEVExpr *E) const {
    if (!E) return 1000000;

    int Cost = localSCEVNodeCost(E);
    for (const SCEVExpr *Op : E->Operands) {
        Cost += estimateSCEVCost(Op);
    }
    return Cost;
}

bool LoopUnrollPass::isCheapSCEV(const SCEVExpr *E,
                                 const UnrollConfig &Config) const {
    if (!E) return false;
    if (Config.AllowExpensiveTripCount) return true;

    int Nodes = 0;
    int Cost = 0;
    std::vector<const SCEVExpr *> Stack;
    Stack.push_back(E);

    while (!Stack.empty()) {
        const SCEVExpr *Cur = Stack.back();
        Stack.pop_back();
        if (!Cur) return false;

        ++Nodes;
        Cost += localSCEVNodeCost(Cur);
        if (Nodes > Config.SCEVExpansionBudget ||
            Cost > Config.SCEVExpansionBudget) {
            return false;
        }

        for (const SCEVExpr *Op : Cur->Operands) {
            Stack.push_back(Op);
        }
    }

    return true;
}

int LoopUnrollPass::detectPeelCount(Loop *L,
                                    const InductionDescriptor &IV,
                                    const UnrollConfig &Config) const {
    if (!L || !IV.Valid || !IV.Phi) {
        return 0;
    }

    if (!IV.ConstantStart || !IV.ConstantStep ||
        *IV.ConstantStart != 0 || *IV.ConstantStep != 1) {
        return 0;
    }

    int BestPeelCount = 0;
    for (BasicBlock *BB : L->getBlocks()) {
        if (!BB) continue;
        if (BB->size() == 0) continue;
        auto *Br = dynamic_cast<BrInst *>(BB->back());
        if (!isInternalConditionalBranch(L, Br)) {
            continue;
        }

        auto *Cmp = dynamic_cast<BinaryInst *>(Br->getOperand(0));
        auto Match = matchCompareWithIV(Cmp, IV.Phi);
        if (!Match) {
            continue;
        }

        BestPeelCount = std::max(BestPeelCount, peelCountForPredicate(*Match));
        if (BestPeelCount >= Config.MaxPeelCount) {
            return Config.MaxPeelCount;
        }
    }

    return std::min(BestPeelCount, Config.MaxPeelCount);
}

UnrollDecision LoopUnrollPass::computeUnrollDecision(
    Loop *L,
    const LoopTripCountInfo &TCI,
    int BodyCost,
    const UnrollConfig &Config,
    LoopInfo &LI,
    SCEVResult &SE,
    BasicAAResult &AA,
    RangeInfo &RI,
    DominatorTree &DT) const {
    if (!L || !TCI.Valid || BodyCost <= 0) {
        return makeNoUnrollDecision();
    }

    // 3. 决策器按固定优先级返回首个合法且资源受控的方案；调用和嵌套循环会限制后续部分/runtime 路径。
    bool HasCall = containsCall(L, LI);
    bool Nested = !L->isInnermost();

    if (Config.AllowFull && TCI.ProvenNoWrap && TCI.ConstantTripCount) {
        // 3.1 完全展开优先：trip count 必须是已证 no-wrap 的小常量，展开总成本须在阈值内，有调用/嵌套时使用更严限制。
        int64_t TripCount = *TCI.ConstantTripCount;
        int64_t MaxCount = (BodyCost <= 4)
                               ? Config.TinyLoopFullUnrollMaxCount
                               : Config.FullUnrollMaxCount;
        bool Restricted = HasCall || Nested;
        int FullLimit = Restricted ? Config.FullThreshold / 2
                                   : Config.FullThreshold;

        // 3.1.1 先用展开次数上限夹住 trip count，然后才计算 BodyCost*TripCount，避免代价乘法溢出。
        if (TripCount > 1 && TripCount <= MaxCount) {
            int64_t TotalCost =
                static_cast<int64_t>(BodyCost) * TripCount;
            if ((!Restricted || TripCount <= 4) &&
                withinThreshold(TotalCost, FullLimit)) {
                UnrollDecision Decision;
                Decision.Kind = UnrollKind::Full;
                Decision.Count = static_cast<int>(TripCount);
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = false;
                Decision.IsRuntime = false;
                Decision.IsPeeling = false;
                return Decision;
            }
        }
    }

    if (Config.AllowPeeling &&
        Config.PeelThreshold > 0 &&
        Config.MaxPeelBlocks > 0) {
        // 3.2 peeling 次之：一方面匹配 IV==0/IV<1 等首轮特化谓词，另一方面统计剥离后可从残余循环删除的不变 store。
        auto IV = SE.getCanonicalInduction(L);
        int PeelCount =
            (IV && IV->Valid) ? detectPeelCount(L, *IV, Config) : 0;
        const std::size_t InvariantStoreCount =
            collectPeelRedundantStoreCandidates(L, LI, AA, DT).size();

        int64_t PeelCloneCost = 0;
        int PeelBlockCount = 0;
        for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
            if (!BB) {
                PeelCloneCost =
                    static_cast<int64_t>(Config.PeelThreshold) + 1;
                break;
            }
            ++PeelBlockCount;
            for (Instruction *I : *BB) {
                if (!I || dynamic_cast<PhiInst *>(I)) continue;
                int LocalCost = 1;
                switch (I->instType) {
                case Instruction::MUL:
                case Instruction::FMUL:
                    LocalCost = 3;
                    break;
                case Instruction::SDIV:
                case Instruction::UDIV:
                case Instruction::SREM:
                case Instruction::UREM:
                case Instruction::FDIV:
                case Instruction::FREM:
                    LocalCost = 10;
                    break;
                case Instruction::LOAD:
                case Instruction::STORE:
                case Instruction::GEP:
                    LocalCost = 2;
                    break;
                case Instruction::CALL:
                    LocalCost = 20;
                    break;
                default:
                    break;
                }
                if (PeelCloneCost >
                    Config.PeelThreshold - LocalCost) {
                    PeelCloneCost =
                        static_cast<int64_t>(Config.PeelThreshold) + 1;
                    break;
                }
                PeelCloneCost += LocalCost;
            }
        }

        if (IV && IV->Valid &&
            PeelBlockCount <= Config.MaxPeelBlocks &&
            PeelCloneCost > 0 &&
            PeelCloneCost <= Config.PeelThreshold) {
            // 3.2.1 若无首轮谓词需要 peeling，只有已证 no-wrap 的精确常量 trip count 才能用不变 store 动态收益触发一次剥离。
            if (PeelCount == 0 && TCI.ProvenNoWrap &&
                TCI.ConstantTripCount &&
                *TCI.ConstantTripCount > 1 &&
                InvariantStoreCount > 0 &&
                Config.PeelStoreCost > 0) {
                const int64_t ResidualIterations =
                    *TCI.ConstantTripCount - 1;
                const int64_t MaxBenefit =
                    std::numeric_limits<int64_t>::max();
                int64_t BenefitPerIteration = MaxBenefit;
                if (InvariantStoreCount <= static_cast<std::size_t>(
                        MaxBenefit / Config.PeelStoreCost)) {
                    BenefitPerIteration =
                        static_cast<int64_t>(InvariantStoreCount) *
                        Config.PeelStoreCost;
                }
                int64_t DynamicBenefit = MaxBenefit;
                if (BenefitPerIteration == 0) {
                    DynamicBenefit = 0;
                } else if (ResidualIterations <=
                           MaxBenefit / BenefitPerIteration) {
                    DynamicBenefit =
                        ResidualIterations * BenefitPerIteration;
                }

                // 3.2.2 静态克隆成本与残余迭代中确实可删的动态写收益使用同一成本单位，收益必须覆盖克隆并留出净余量。
                const int64_t RequiredBenefit =
                    PeelCloneCost +
                    std::max(0, Config.PeelMinNetBenefit);
                if (DynamicBenefit >= RequiredBenefit) {
                    PeelCount = 1;
                }
            }

            bool ConstantLongEnough =
                TCI.ProvenNoWrap && TCI.ConstantTripCount &&
                *TCI.ConstantTripCount > PeelCount;
            bool CanGuardRuntime =
                (TCI.IsRuntime || !TCI.ProvenNoWrap) && PeelCount > 0 &&
                PeelCount <= Config.MaxGuardedPeelCount;
            if (PeelCount > 0 &&
                PeelCount <= Config.MaxPeelCount &&
                (ConstantLongEnough || CanGuardRuntime) &&
                PeelCloneCost * PeelCount <=
                    Config.PeelThreshold) {
                UnrollDecision Decision;
                Decision.Kind = UnrollKind::Peeling;
                Decision.Count = PeelCount;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = false;
                Decision.IsRuntime = false;
                Decision.IsPeeling = true;
                return Decision;
            }
        }
    }

    // 3.3 完全展开和 peeling 都未选中后，含调用或子循环的候选不再进入部分/runtime 展开。
    if (HasCall || Nested) {
        return makeNoUnrollDecision();
    }

    if (Config.AllowPartial && TCI.ProvenNoWrap && TCI.ConstantTripCount &&
        BodyCost <= Config.MaxLoopBodyCostForPartial) {
        // 3.4 常量部分展开从代码大小选出 4/2 路候选，优先选能整除 trip count 的因子。
        int64_t TripCount = *TCI.ConstantTripCount;
        int Factor = chooseFactorBySize(BodyCost,
                                        Config.PartialThreshold,
                                        Config.PartialMaxCount);
        std::vector<int> Factors;
        if (Factor >= 4) {
            Factors.push_back(4);
            Factors.push_back(2);
        } else if (Factor >= 2) {
            Factors.push_back(2);
        }

        for (int Candidate : Factors) {
            if (TripCount >= Candidate && TripCount % Candidate == 0) {
                UnrollDecision Decision;
                Decision.Kind = UnrollKind::Partial;
                Decision.Count = Candidate;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = false;
                Decision.IsRuntime = false;
                Decision.IsPeeling = false;
                return Decision;
            }
        }

        if (Config.AllowRemainder) {
            // 3.4.1 无法整除时，只在 SCEV 给出有效主次数/余数分解时选择带常量 tail 的部分展开。
            for (int Candidate : Factors) {
                if (TripCount % Candidate == 0 ||
                    TripCount <= Candidate) {
                    continue;
                }

                auto RI = SE.getRemainderInfo(L, Candidate);
                if (!RI || !RI->Valid || !RI->NeedRemainderLoop) {
                    continue;
                }

                UnrollDecision Decision;
                Decision.Kind = UnrollKind::PartialWithRemainder;
                Decision.Count = Candidate;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = true;
                Decision.IsRuntime = false;
                Decision.IsPeeling = false;
                return Decision;
            }
        }
    }

    if (Config.AllowRuntime && Config.AllowRemainder &&
        TCI.IsRuntime &&
        BodyCost <= Config.MaxLoopBodyCostForRuntime &&
        isCheapSCEV(TCI.TripCount, Config)) {
        // 3.5 runtime 作为最后选项：trip-count SCEV 必须在扩展预算内，且已有 no-wrap 证明或能在 preheader 由 Range/SCEV 证明。
        const bool RuntimeTripCountIsProven =
            TCI.ProvenNoWrap ||
            proveRuntimeTripCountAtPreheader(SE, TCI, RI, DT);
        if (!RuntimeTripCountIsProven)
            return makeNoUnrollDecision();
        int RuntimeCountLimit = std::min(Config.RuntimeDefaultCount,
                                         Config.RuntimeMaxCount);
        int Factor = chooseFactorBySize(BodyCost,
                                        Config.RuntimeThreshold,
                                        RuntimeCountLimit);
        if (Factor >= 2) {
            auto RI = SE.getRemainderInfo(L, Factor);
            if (RI && RI->Valid && RI->NeedRemainderLoop) {
                UnrollDecision Decision;
                Decision.Kind = UnrollKind::Runtime;
                Decision.Count = Factor;
                Decision.ShouldUnroll = true;
                Decision.NeedRemainder = true;
                Decision.IsRuntime = true;
                Decision.IsPeeling = false;
                Decision.TripCountIsProven = true;
                return Decision;
            }
        }
    }

    return makeNoUnrollDecision();
}

LoopUnrollResult LoopUnrollPass::executeFullUnroll(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;
    (void)DT;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }
    // 5.1 完全展开执行前重新确认已证 no-wrap 的常量次数与 Decision 完全一致，且退出测试位于 latch。
    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->ProvenNoWrap || !TCI->ConstantTripCount ||
        *TCI->ConstantTripCount != Decision.Count ||
        TCI->ExitingBlock != TCI->Latch) {
        return LoopUnrollResult::Unmodified;
    }

    auto Core = cloneUnrolledBodyCore(L, Decision.Count, SE);
    if (!Core) {
        return LoopUnrollResult::Unmodified;
    }
    if (!hasLatchExitShape(*Core)) {
        discardUncommittedUnrollClones(*Core);
        return LoopUnrollResult::Unmodified;
    }

    // 5.1.1 将全部迭代副本串成直线 CFG，仅末副本到 Exit，并把 Exit PHI 与原 Header PHI 分别折叠到末值和初值。
    if (!makeStraightLineMain(*Core, Decision.Count, Core->Shape.Exit,
                              nullptr)) {
        return LoopUnrollResult::Unmodified;
    }
    if (!rewriteExitPhisForSingleExiting(*Core, Decision.Count - 1)) {
        return LoopUnrollResult::Unmodified;
    }
    if (!removeOriginalHeaderPhisForFull(*Core)) {
        return LoopUnrollResult::Unmodified;
    }

    SE.clear();
    return LoopUnrollResult::FullyUnrolled;
}

LoopUnrollResult LoopUnrollPass::executePartialUnroll(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;
    (void)DT;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }

    // 5.2 整除部分展开重新验证 no-wrap 常量 trip count 能被 Factor 整除，且退出仅位于 Header 或 Latch。
    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->ProvenNoWrap || !TCI->ConstantTripCount ||
        *TCI->ConstantTripCount % Decision.Count != 0 ||
        (TCI->ExitingBlock != TCI->Latch &&
         TCI->ExitingBlock != TCI->Header)) {
        return LoopUnrollResult::Unmodified;
    }

    auto Core = cloneUnrolledBodyCore(L, Decision.Count, SE);
    if (!Core) {
        return LoopUnrollResult::Unmodified;
    }

    // 5.2.1 latch-tested 循环将前 Factor-1 个退出改为直连下一副本，末 Latch 回到首 Header，只保留末副本退出 PHI 值。
    if (hasLatchExitShape(*Core)) {
        BasicBlock *LastExiting = Core->Exitings.back();
        BasicBlock *ClonedHeader = Core->Headers.back();
        if (!LastExiting || !ClonedHeader ||
            !BasicBlock::redirectBranchTarget(LastExiting, ClonedHeader,
                                              Core->Headers.front())) {
            discardUncommittedUnrollClones(*Core);
            return LoopUnrollResult::Unmodified;
        }

        for (int Iter = 0; Iter + 1 < Decision.Count; ++Iter) {
            if (!BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Core->Headers[Iter + 1])) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!rewriteExitPhisForSingleExiting(*Core,
                                             Decision.Count - 1)) {
            return LoopUnrollResult::Unmodified;
        }
    } else if (Core->Shape.Exiting == Core->Shape.Header) {
        // 5.2.2 header-tested 循环保留首 Header 的退出检查，删除后续副本头检查，并把各 Latch 依次连到下一 Header/首 Header。
        for (int Iter = 1; Iter < Decision.Count; ++Iter) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Exitings[Iter], Core->Shape.Exit);
            if (!Body ||
                !BasicBlock::replaceCondBrWithUncond(
                    Core->Exitings[Iter], Body)) {
                discardUncommittedUnrollClones(*Core);
                return LoopUnrollResult::Unmodified;
            }
        }
        for (int Iter = 0; Iter + 1 < Decision.Count; ++Iter) {
            if (!BasicBlock::redirectBranchTarget(
                    Core->Latches[Iter], Core->Headers[Iter],
                    Core->Headers[Iter + 1])) {
                return LoopUnrollResult::Unmodified;
            }
        }
        if (!BasicBlock::redirectBranchTarget(
                Core->Latches.back(), Core->Headers.back(),
                Core->Headers.front())) {
            return LoopUnrollResult::Unmodified;
        }
    } else {
        discardUncommittedUnrollClones(*Core);
        return LoopUnrollResult::Unmodified;
    }

    // 5.2.3 两种形态最后都将原 Header PHI 回边改为组内末副本状态。
    if (!rewriteOriginalHeaderPhisForPartial(*Core,
                                             Decision.Count - 1)) {
        return LoopUnrollResult::Unmodified;
    }

    SE.clear();
    return LoopUnrollResult::PartiallyUnrolled;
}

LoopUnrollResult LoopUnrollPass::executePartialWithRemainder(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;
    (void)DT;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }
    // 5.3 常量余数路径要求 SCEV 给出非空主段和 0<Remainder<Factor，再将主段换算为展开组数交给共享 tail 引擎。
    auto RI = SE.getRemainderInfo(L, Decision.Count);
    if (!RI || !RI->Valid || !RI->NeedRemainderLoop) {
        return LoopUnrollResult::Unmodified;
    }

    if (!RI->ConstantMainTripCount || !RI->ConstantRemainderCount ||
        *RI->ConstantMainTripCount < Decision.Count ||
        *RI->ConstantRemainderCount <= 0 ||
        *RI->ConstantRemainderCount >= Decision.Count) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || (TCI->ExitingBlock != TCI->Latch &&
                 TCI->ExitingBlock != TCI->Header)) {
        return LoopUnrollResult::Unmodified;
    }

    int64_t NumGroups =
        *RI->ConstantMainTripCount / Decision.Count;
    Value *GroupCount =
        makeTypedIntConstant(NumGroups, RI->TripCount->Ty);
    if (!GroupCount) return LoopUnrollResult::Unmodified;

    return executeUnrollWithTail(
        L, Decision.Count,
        static_cast<int>(*RI->ConstantRemainderCount), GroupCount, nullptr,
        false, SE);
}

LoopUnrollResult LoopUnrollPass::executeRuntimeUnroll(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT) {
    (void)LI;

    if (!L || Decision.Count < 2) {
        return LoopUnrollResult::Unmodified;
    }
    if (!Decision.TripCountIsProven)
        return LoopUnrollResult::Unmodified;

    // 5.4 runtime 执行再次要求 Decision 携带 no-wrap 证明、SCEV 余数信息有效，且 trip-count 表达式可在 preheader 安全物化。
    auto RI = SE.getRemainderInfo(L, Decision.Count);
    if (!RI || !RI->Valid || !RI->IsRuntimeRemainder) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || (TCI->ExitingBlock != TCI->Latch &&
                 TCI->ExitingBlock != TCI->Header) ||
        !TCI->PreHeader || !RI->TripCount ||
        !canMaterializeSCEV(RI->TripCount, L, TCI->PreHeader, DT)) {
        return LoopUnrollResult::Unmodified;
    }

    // 5.4.1 向外层循环试探 trip-count SCEV 仍可用的最早入口，将组数/余数计算上提到该处，避免外循环不变边界在每个外迭代重复物化。
    Loop *ExpansionLoop = L;
    BasicBlock *ExpansionBlock = TCI->PreHeader;
    for (Loop *Ancestor = TCI->ProvenNoWrap ? L->getParentLoop() : nullptr;
         Ancestor; Ancestor = Ancestor->getParentLoop()) {
        // 5.4.1.1 旋转后父循环可能由带零次旁路的 guard 唯一入边进入；即使它有两个后继，仍可作为无故障整数 SCEV 的上提点。
        BasicBlock *AncestorEntry = nullptr;
        bool AmbiguousEntry = false;
        for (BasicBlock *Pred : Ancestor->getHeader()->getPredecessors()) {
            if (!Pred || Ancestor->contains(Pred)) continue;
            if (!AncestorEntry) {
                AncestorEntry = Pred;
            } else if (AncestorEntry != Pred) {
                AmbiguousEntry = true;
                break;
            }
        }
        if (AmbiguousEntry || !AncestorEntry ||
            !canMaterializeSCEV(RI->TripCount, Ancestor,
                                AncestorEntry, DT)) {
            break;
        }
        ExpansionLoop = Ancestor;
        ExpansionBlock = AncestorEntry;
    }

    std::unordered_map<const SCEVExpr *, Value *> Cache;
    Value *TripCount =
        materializeSCEV(RI->TripCount, ExpansionLoop, ExpansionBlock,
                        DT, Cache);
    if (!TripCount || !TripCount->getIRType() ||
        !TripCount->getIRType()->isInt()) {
        return LoopUnrollResult::Unmodified;
    }

    Value *Remainder = nullptr;
    Value *NumGroups = nullptr;

    // 5.4.2 已证 no-wrap 的 header-test/update-test trip count 都非负；Factor 为二次幂时用掩码和逻辑右移直接计算 Remainder/NumGroups。
    const bool CanUsePowerOfTwoForm =
        Decision.TripCountIsProven &&
        IRMath::isPowerOf2(Decision.Count);
    if (CanUsePowerOfTwoForm) {
        Value *Mask = makeTypedIntConstant(
            Decision.Count - 1, TripCount->getIRType());
        Value *Shift = makeTypedIntConstant(
            IRMath::log2i(Decision.Count), TripCount->getIRType());
        if (!Mask || !Shift) return LoopUnrollResult::Unmodified;

        auto *RemainderInst =
            new BinaryInst(TripCount, BinaryInst::AND, Mask);
        BasicBlock::insertBeforeTerminator(ExpansionBlock, RemainderInst);
        auto *NumGroupsInst =
            new BinaryInst(TripCount, BinaryInst::LSHR, Shift);
        BasicBlock::insertBeforeTerminator(ExpansionBlock, NumGroupsInst);
        Remainder = RemainderInst;
        NumGroups = NumGroupsInst;
    } else {
        // 5.4.3 非二次幂 Factor 使用通用 MOD、SUB、DIV 链计算余数和完整展开组数。
        Value *Factor = makeTypedIntConstant(
            Decision.Count, TripCount->getIRType());
        if (!Factor) return LoopUnrollResult::Unmodified;

        auto *RemainderInst =
            new BinaryInst(TripCount, BinaryInst::MOD, Factor);
        BasicBlock::insertBeforeTerminator(ExpansionBlock, RemainderInst);
        auto *MainTripCount =
            new BinaryInst(TripCount, BinaryInst::SUB, RemainderInst);
        BasicBlock::insertBeforeTerminator(ExpansionBlock, MainTripCount);
        auto *NumGroupsInst =
            new BinaryInst(MainTripCount, BinaryInst::DIV, Factor);
        BasicBlock::insertBeforeTerminator(ExpansionBlock, NumGroupsInst);
        Remainder = RemainderInst;
        NumGroups = NumGroupsInst;
    }

    return executeUnrollWithTail(L, Decision.Count, Decision.Count - 1,
                                 NumGroups, Remainder, true, SE);
}

LoopUnrollResult LoopUnrollPass::executePeeling(
    Loop *L,
    const UnrollDecision &Decision,
    SCEVResult &SE,
    LoopInfo &LI,
    DominatorTree &DT,
    BasicAAResult &AA) {
    if (!L || Decision.Count <= 0) {
        return LoopUnrollResult::Unmodified;
    }

    // 6. peeling 执行先固定唯一 Exit/唯一 Exiting 和有效 trip-count 形状，再克隆 Decision.Count 个前置迭代。
    int PeelCount = Decision.Count;

    std::vector<BasicBlock *> ExitBlocks = L->getExitBlocks();
    if (ExitBlocks.size() != 1 || !ExitBlocks.front()) {
        return LoopUnrollResult::Unmodified;
    }

    std::unordered_set<BasicBlock *> ExitingBlocks;
    for (BasicBlock *BB : L->getBlocksIncludingSubLoops()) {
        if (L->isExitingBlock(BB)) {
            ExitingBlocks.insert(BB);
        }
    }
    if (ExitingBlocks.size() != 1) {
        return LoopUnrollResult::Unmodified;
    }

    auto TCI = SE.analyzeLoopTripCount(L);
    if (!TCI || !TCI->Valid) {
        return LoopUnrollResult::Unmodified;
    }
    // 6.1 只有已证 no-wrap 的常量次数才能证明前 PeelCount 次一定执行并删除克隆退出检查；否则只允许少量 guarded peeling。
    const bool GuardedPeeling =
        !TCI->ConstantTripCount.has_value() || !TCI->ProvenNoWrap;
    if (GuardedPeeling) {
        if ((!TCI->IsRuntime && TCI->ProvenNoWrap) ||
            PeelCount > Config.MaxGuardedPeelCount)
            return LoopUnrollResult::Unmodified;
    } else if (*TCI->ConstantTripCount <= PeelCount) {
        return LoopUnrollResult::Unmodified;
    }

    auto ShapeOpt = getLoopExecutionShape(L, SE);
    if (!ShapeOpt || !ShapeOpt->PreHeader || !ShapeOpt->Header ||
        !ShapeOpt->Latch || !ShapeOpt->Exiting || !ShapeOpt->Exit) {
        return LoopUnrollResult::Unmodified;
    }

    // 6.1.1 与 tail 引擎一样互斥分类 latch-tested/header-tested，单块循环只归入 latch-tested，避免克隆退出边与 PHI 前驱选取不同副本。
    const bool LatchExiting = ShapeOpt->Exiting == ShapeOpt->Latch;
    const bool HeaderExiting =
        !LatchExiting && ShapeOpt->Exiting == ShapeOpt->Header;
    if (!LatchExiting && !HeaderExiting) {
        return LoopUnrollResult::Unmodified;
    }

    std::vector<BasicBlock *> LoopBlocks = L->getBlocksInFunctionOrder();
    if (LoopBlocks.empty() || !canCloneAllInstructions(LoopBlocks)) {
        return LoopUnrollResult::Unmodified;
    }

    std::vector<StoreInst *> RedundantResidualStores =
        collectPeelRedundantStoreCandidates(L, LI, AA, DT);

    // 6.2 克隆完成后在任何 CFG 提交前，以 ValidateOnly 模式预检原 Header PHI 与 guarded/已证长度两种 Exit PHI 方案。
    auto Core = clonePeeledIterations(L, PeelCount, SE);
    if (!Core) {
        return LoopUnrollResult::Unmodified;
    }
    auto FailUncommitted = [&]() {
        discardUncommittedPeelClones(*Core);
        return LoopUnrollResult::Unmodified;
    };
    if (!rewriteOriginalHeaderPhisAfterPeeling(
            *Core, true)) {
        return FailUncommitted();
    }
    if (GuardedPeeling) {
        if (!addIncomingToExitPhisForPeeledBlocks(
                *Core, true)) {
            return FailUncommitted();
        }
    } else if (!removeIncomingFromExitPhisForPeeledBlocks(
                   *Core, true)) {
        return FailUncommitted();
    }

    auto *PreBr = Core->Shape.PreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(PreBr) ||
        PreBr->getSuccessor(0) != Core->Shape.Header) {
        return FailUncommitted();
    }
    for (int Iter = 0; Iter < PeelCount; ++Iter) {
        if (LatchExiting) {
            auto *Br = Core->Exitings[Iter]
                ? Core->Exitings[Iter]->getBranchTerminator() : nullptr;
            if (!Br || !Br->isConditional())
                return FailUncommitted();
            bool HasBackedge = false;
            for (BasicBlock *Succ : Core->Exitings[Iter]->getSuccessors())
                HasBackedge |= Succ == Core->Headers[Iter];
            if (!HasBackedge) return FailUncommitted();
        } else {
            auto *HeaderBr = Core->Headers[Iter]
                ? Core->Headers[Iter]->getBranchTerminator() : nullptr;
            auto *LatchBr = Core->Latches[Iter]
                ? Core->Latches[Iter]->getBranchTerminator() : nullptr;
            if (!HeaderBr || !HeaderBr->isConditional() || !LatchBr)
                return FailUncommitted();
            bool LatchTargetsHeader = false;
            for (BasicBlock *Succ : Core->Latches[Iter]->getSuccessors())
                LatchTargetsHeader |= Succ == Core->Headers[Iter];
            if (!LatchTargetsHeader) return FailUncommitted();
        }
    }

    if (LatchExiting) {
        for (int Iter = 0; Iter < PeelCount; ++Iter) {
            BasicBlock *Target =
                Iter + 1 < PeelCount ? Core->Headers[Iter + 1]
                                     : Core->Shape.Header;

            if (!Core->Exitings[Iter] || !Target) {
                return FailUncommitted();
            }
            if (GuardedPeeling) {
                // 6.3 guarded peeling 保留每个副本的短路退出，只把回边改为下一剥离副本或原残余循环。
                if (!BasicBlock::redirectBranchTarget(
                        Core->Exitings[Iter], Core->Headers[Iter], Target)) {
                    return FailUncommitted();
                }
            } else if (!BasicBlock::replaceCondBrWithUncond(
                           Core->Exitings[Iter], Target)) {
                return FailUncommitted();
            }
        }
    } else if (HeaderExiting) {
        for (int Iter = 0; Iter < PeelCount; ++Iter) {
            BasicBlock *Body =
                getNonExitSuccessor(Core->Headers[Iter], Core->Shape.Exit);
            if (!Body) {
                return FailUncommitted();
            }
            if (!GuardedPeeling &&
                !BasicBlock::replaceCondBrWithUncond(Core->Headers[Iter],
                                                     Body)) {
                return FailUncommitted();
            }

            BasicBlock *Target =
                Iter + 1 < PeelCount ? Core->Headers[Iter + 1]
                                     : Core->Shape.Header;

            if (!Core->Latches[Iter] || !Target ||
                !BasicBlock::redirectBranchTarget(Core->Latches[Iter],
                                                  Core->Headers[Iter],
                                                  Target)) {
                return FailUncommitted();
            }
        }
    }

    // 6.4 在新克隆退出边连入 preheader 之前先提交 Exit PHI：guarded 方案添加克隆入边，已证长度方案移除已删退出检查的入边。
    if (GuardedPeeling) {
        if (!addIncomingToExitPhisForPeeledBlocks(*Core))
            return LoopUnrollResult::Unmodified;
    } else if (!removeIncomingFromExitPhisForPeeledBlocks(*Core)) {
        return LoopUnrollResult::Unmodified;
    }

    if (Core->Headers.empty() || !Core->Headers[0] ||
        !BasicBlock::redirectBranchTarget(Core->Shape.PreHeader,
                                          Core->Shape.Header,
                                          Core->Headers[0])) {
        return LoopUnrollResult::Unmodified;
    }

    if (!rewriteOriginalHeaderPhisAfterPeeling(
            *Core, false)) {
        return LoopUnrollResult::Unmodified;
    }

    // 6.5 所有进入残余循环的路径已完成剥离；阶段 3 证明控制、地址和写值不变且无竞争别名的 store 在残余迭代中仅重复同一写入，此处只删 store 不删 load。
    for (StoreInst *SI : RedundantResidualStores) {
        if (!SI || !SI->getParent() || !L->contains(SI->getParent()))
            continue;
        SI->eraseFromParent();
        delete SI;
    }

    SE.clear();
    return LoopUnrollResult::PartiallyUnrolled;
}
