#include "ir/opt/LoopStrengthReduction.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr int kUnsupportedMaterializeCost = 1000000;
constexpr int64_t kMaxOffsetRange = 16;
constexpr int kMaxPointerIVsPerInnerLoop = 8;
constexpr int kMaxPointerIVsPerOuterLoop = 4;

struct IndexComponent {
    Value *Raw = nullptr;
    const SCEVExpr *Expr = nullptr;
};

struct LSRCandidate {
    GetElementPtrInst *OldGEP = nullptr;
    Value *Base = nullptr;
    Value *Index = nullptr;
    const SCEVExpr *Start = nullptr;
    std::optional<int64_t> StartConst;
    std::vector<IndexComponent> Prefix;
    Loop *L = nullptr;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    int64_t Step = 0;
    int64_t LastIndexStride = 0;
    bool HasLeadingZero = false;
    bool Used = false;
};

struct LSRGroup {
    size_t Order = 0;
    std::vector<LSRCandidate *> Items;
    Value *Base = nullptr;
    const SCEVExpr *Start = nullptr;
    std::optional<int64_t> StartConst;
    Loop *L = nullptr;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    int64_t Step = 0;
    int64_t LastIndexStride = 0;
    std::vector<IndexComponent> Prefix;
    std::shared_ptr<IRType> ResultTy;
};

struct OffsetReuseGroup {
    size_t Order = 0;
    std::vector<LSRCandidate *> Items;
    Value *Base = nullptr;
    Loop *L = nullptr;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    int64_t Step = 0;
    int64_t LastIndexStride = 0;
    int64_t AnchorStart = 0;
    int64_t MaxStart = 0;
    std::vector<IndexComponent> Prefix;
    std::shared_ptr<IRType> ResultTy;
};

struct PointerIV {
    GetElementPtrInst *Start = nullptr;
    PhiInst *Phi = nullptr;
    GetElementPtrInst *Next = nullptr;
};

struct AddressGEPNode {
    GetElementPtrInst *GEP = nullptr;
    int64_t Offset = 0;
};

static bool isConstZero(Value *V) {
    auto *CI = dynamic_cast<ConstInt *>(V);
    return CI && CI->getVal() == 0;
}

static bool isSCEVConstZero(const SCEVExpr *E) {
    return E && E->Kind == SCEVExprKind::Constant && E->ConstValue == 0;
}

static bool isPowerOfTwo(int64_t V) {
    return V > 0 && (V & (V - 1)) == 0;
}

static bool hasCostlyElementStride(int64_t Stride) {
    return Stride > 0 && !isPowerOfTwo(Stride);
}

static bool isRecKind(SCEVExprKind K) {
    return K == SCEVExprKind::AddRec || K == SCEVExprKind::MulRec ||
           K == SCEVExprKind::DivRec;
}

static bool mentionsLoop(const SCEVExpr *E, const Loop *L) {
    if (!E || !L) return false;
    if (isRecKind(E->Kind) && E->RecLoop == L) return true;
    for (const SCEVExpr *Op : E->Operands) {
        if (mentionsLoop(Op, L)) return true;
    }
    return false;
}

static std::string pointerKey(const void *P) {
    std::ostringstream OS;
    OS << P;
    return OS.str();
}

static std::string scevKey(const SCEVExpr *E) {
    if (!E) return "<null>";
    if (!E->Key.empty()) return E->Key;
    return "scev@" + pointerKey(E);
}

static std::string typeKey(const std::shared_ptr<IRType> &Ty) {
    if (!Ty) return "noty";
    std::ostringstream OS;
    OS << static_cast<int>(Ty->getEnum()) << ":" << Ty->getSize() << ":"
       << Ty.get();
    return OS.str();
}

static bool hasNonTrivialPrefix(const std::vector<IndexComponent> &Prefix) {
    if (Prefix.empty()) return false;
    if (Prefix.size() == 1 && isSCEVConstZero(Prefix.front().Expr))
        return false;
    return true;
}

static std::vector<BasicBlock *> collectLoopBlocks(Loop *L) {
    std::vector<BasicBlock *> Blocks;
    std::vector<Loop *> Stack;
    std::unordered_set<BasicBlock *> Seen;

    if (L) Stack.push_back(L);
    while (!Stack.empty()) {
        Loop *Current = Stack.back();
        Stack.pop_back();

        for (BasicBlock *BB : Current->getBlocks()) {
            if (BB && Seen.insert(BB).second) Blocks.push_back(BB);
        }
        for (Loop *SubLoop : Current->getSubLoops()) {
            if (SubLoop) Stack.push_back(SubLoop);
        }
    }
    return Blocks;
}

static void insertBeforeTerminator(BasicBlock *BB, Instruction *I) {
    if (!BB || !I) return;
    for (auto It = BB->begin(); It != BB->end(); ++It) {
        Instruction *Current = *It;
        if (!Current) break;
        if (Current->instType == Instruction::BR ||
            Current->instType == Instruction::RET) {
            It.insertBefore(I);
            return;
        }
    }
    BB->push_back(I);
}

static void insertAfterPhiNodes(BasicBlock *BB, Instruction *I) {
    if (!BB || !I) return;
    for (auto It = BB->begin(); It != BB->end(); ++It) {
        if (!dynamic_cast<PhiInst *>(*It)) {
            It.insertBefore(I);
            return;
        }
    }
    BB->push_back(I);
}

static bool insertBeforeInstruction(Instruction *Position, Instruction *I) {
    if (!Position || !I || !Position->getParent()) return false;
    BasicBlock *BB = Position->getParent();
    for (auto It = BB->begin(); It != BB->end(); ++It) {
        if (*It == Position) {
            It.insertBefore(I);
            return true;
        }
    }
    return false;
}

static bool canUseInPreheader(Value *V, BasicBlock *PreHeader,
                              const DominatorTree &DT) {
    if (!V || !PreHeader) return false;
    if (V->isConst()) return true;

    if (auto *Var = dynamic_cast<Variable *>(V)) {
        if (Var->isParam() || Var->isGlobal()) return true;
    }

    auto *Inst = dynamic_cast<Instruction *>(V);
    if (!Inst || !Inst->getParent()) return false;
    return DT.dominates(Inst->getParent(), PreHeader);
}

static bool canMaterializeSCEVAtPreheader(const SCEVExpr *E, const Loop *L,
                                          BasicBlock *PreHeader,
                                          const DominatorTree &DT) {
    if (!E || mentionsLoop(E, L)) return false;

    switch (E->Kind) {
    case SCEVExprKind::Constant:
        return E->ConstValue >= std::numeric_limits<int>::min() &&
               E->ConstValue <= std::numeric_limits<int>::max();
    case SCEVExprKind::Unknown:
        return E->RawValue &&
               canUseInPreheader(E->RawValue, PreHeader, DT);
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub:
        return E->Operands.size() == 2 &&
               canMaterializeSCEVAtPreheader(E->Operands[0], L, PreHeader,
                                             DT) &&
               canMaterializeSCEVAtPreheader(E->Operands[1], L, PreHeader,
                                             DT);
    case SCEVExprKind::Mul:
        return E->Operands.size() == 2 &&
               (E->Operands[0]->Kind == SCEVExprKind::Constant ||
                E->Operands[1]->Kind == SCEVExprKind::Constant) &&
               canMaterializeSCEVAtPreheader(E->Operands[0], L, PreHeader,
                                             DT) &&
               canMaterializeSCEVAtPreheader(E->Operands[1], L, PreHeader,
                                             DT);
    case SCEVExprKind::Cast:
        return E->Ty && E->Operands.size() == 1 &&
               canMaterializeSCEVAtPreheader(E->Operands[0], L, PreHeader,
                                             DT);
    default:
        return false;
    }
}

static int estimateMaterializeCost(const SCEVExpr *E, const Loop *L,
                                   BasicBlock *PreHeader,
                                   const DominatorTree &DT) {
    if (!canMaterializeSCEVAtPreheader(E, L, PreHeader, DT))
        return kUnsupportedMaterializeCost;

    switch (E->Kind) {
    case SCEVExprKind::Constant:
    case SCEVExprKind::Unknown:
        return 0;
    case SCEVExprKind::Add:
    case SCEVExprKind::Sub:
    case SCEVExprKind::Mul:
        return 1 + estimateMaterializeCost(E->Operands[0], L, PreHeader, DT) +
               estimateMaterializeCost(E->Operands[1], L, PreHeader, DT);
    case SCEVExprKind::Cast:
        return 1 + estimateMaterializeCost(E->Operands[0], L, PreHeader, DT);
    default:
        return kUnsupportedMaterializeCost;
    }
}

static Value *makeSCEVConstant(int64_t V, const SCEVExpr *E) {
    if (V < std::numeric_limits<int>::min() ||
        V > std::numeric_limits<int>::max()) {
        return nullptr;
    }

    int IntValue = static_cast<int>(V);
    if (E && E->Ty && E->Ty->getEnum() == IR_INT &&
        E->Ty == INTType::NewBool()) {
        return ConstBool::newConstBool(IntValue != 0);
    }

    size_t Size = 4;
    if (E && E->Ty && E->Ty->getEnum() == IR_INT) Size = E->Ty->getSize();
    return ConstInt::newConstInt(IntValue, Size);
}

static Value *materializeSCEVAtPreheader(const SCEVExpr *E, const Loop *L,
                                         BasicBlock *PreHeader,
                                         const DominatorTree &DT) {
    if (!canMaterializeSCEVAtPreheader(E, L, PreHeader, DT)) return nullptr;

    if (E->Kind == SCEVExprKind::Constant)
        return makeSCEVConstant(E->ConstValue, E);
    if (E->Kind == SCEVExprKind::Unknown) return E->RawValue;

    Instruction *NewI = nullptr;
    if (E->Kind == SCEVExprKind::Add || E->Kind == SCEVExprKind::Sub ||
        E->Kind == SCEVExprKind::Mul) {
        Value *LHS =
            materializeSCEVAtPreheader(E->Operands[0], L, PreHeader, DT);
        Value *RHS =
            materializeSCEVAtPreheader(E->Operands[1], L, PreHeader, DT);
        if (!LHS || !RHS) return nullptr;

        BinaryInst::Operation Op = BinaryInst::ADD;
        if (E->Kind == SCEVExprKind::Sub) Op = BinaryInst::SUB;
        if (E->Kind == SCEVExprKind::Mul) Op = BinaryInst::MUL;
        NewI = new BinaryInst(LHS, Op, RHS);
    } else if (E->Kind == SCEVExprKind::Cast &&
               E->Operands.size() == 1) {
        Value *OpV =
            materializeSCEVAtPreheader(E->Operands[0], L, PreHeader, DT);
        if (!OpV) return nullptr;
        switch (E->CastKind) {
        case SCEVCastKind::Trunc:
            NewI = new TruncInst(E->Ty);
            NewI->addUse(OpV);
            break;
        case SCEVCastKind::ZExt:
            NewI = new ZextInst(E->Ty);
            NewI->addUse(OpV);
            break;
        case SCEVCastKind::SExt:
            NewI = new SextInst(E->Ty);
            NewI->addUse(OpV);
            break;
        case SCEVCastKind::BitCast:
            NewI = new BitCastInst(OpV, E->Ty);
            break;
        }
    }

    if (!NewI) return nullptr;
    insertBeforeTerminator(PreHeader, NewI);
    return NewI;
}

static int estimatePrefixMaterializeCost(
    const std::vector<IndexComponent> &Prefix, const Loop *L,
    BasicBlock *PreHeader, const DominatorTree &DT) {
    int Cost = 0;
    for (const IndexComponent &Component : Prefix) {
        if (Component.Raw &&
            canUseInPreheader(Component.Raw, PreHeader, DT)) {
            continue;
        }
        int ComponentCost =
            estimateMaterializeCost(Component.Expr, L, PreHeader, DT);
        if (ComponentCost >= kUnsupportedMaterializeCost)
            return kUnsupportedMaterializeCost;
        Cost += ComponentCost;
    }
    return Cost;
}

static bool materializePrefix(const std::vector<IndexComponent> &Prefix,
                              const Loop *L, BasicBlock *PreHeader,
                              const DominatorTree &DT,
                              std::vector<Value *> &Indices) {
    for (const IndexComponent &Component : Prefix) {
        Value *V = nullptr;
        if (Component.Raw &&
            canUseInPreheader(Component.Raw, PreHeader, DT)) {
            V = Component.Raw;
        } else {
            V = materializeSCEVAtPreheader(Component.Expr, L, PreHeader, DT);
        }
        if (!V) return false;
        Indices.push_back(V);
    }
    return true;
}

static bool isLoadStoreAddressUse(Instruction *UserI, Use *U) {
    if (!UserI || !U) return false;
    int OpIdx = UserI->getUserIndex(U);
    if (dynamic_cast<LoadInst *>(UserI)) return OpIdx == 0;
    if (dynamic_cast<StoreInst *>(UserI)) return OpIdx == 1;
    return false;
}

static int countAddressUses(GetElementPtrInst *GEP) {
    if (!GEP) return 0;
    int Count = 0;
    for (auto It = GEP->getUseList().begin(); It != GEP->getUseList().end();
         ++It) {
        Use *U = *It;
        if (!U || U->getValue() != GEP) continue;
        auto *UserI = dynamic_cast<Instruction *>(U->getUser());
        if (isLoadStoreAddressUse(UserI, U)) ++Count;
    }
    return Count;
}

static int countLoopMemoryAddressUses(Loop *L) {
    int Count = 0;
    for (BasicBlock *BB : collectLoopBlocks(L)) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            if (dynamic_cast<LoadInst *>(I) ||
                dynamic_cast<StoreInst *>(I)) {
                ++Count;
            }
        }
    }
    return Count;
}

static bool getConstantOffsetGEP(GetElementPtrInst *Parent,
                                 Instruction *UserI, Use *U,
                                 GetElementPtrInst *&Child,
                                 int64_t &Offset) {
    Child = dynamic_cast<GetElementPtrInst *>(UserI);
    if (!Parent || !Child || !U || Child->getUserIndex(U) != 0)
        return false;

    auto &Operands = Child->getUserList();
    if (Operands.size() != 2 || Operands.front()->getValue() != Parent)
        return false;
    auto *CI = dynamic_cast<ConstInt *>(Operands.back()->getValue());
    if (!CI) return false;
    Offset = CI->getVal();
    return true;
}

static bool collectAddressGEPFamily(
    GetElementPtrInst *GEP, Loop *L, int64_t CurrentOffset,
    std::vector<AddressGEPNode> &Nodes,
    std::unordered_set<GetElementPtrInst *> &Visited) {
    if (!GEP || !L || !L->contains(GEP->getParent()) ||
        !Visited.insert(GEP).second) {
        return false;
    }

    bool HasAddressUse = false;
    std::vector<std::pair<GetElementPtrInst *, int64_t>> Children;
    for (auto It = GEP->getUseList().begin(); It != GEP->getUseList().end();
         ++It) {
        Use *U = *It;
        if (!U || U->getValue() != GEP) continue;
        auto *UserI = dynamic_cast<Instruction *>(U->getUser());
        if (!UserI || !L->contains(UserI->getParent())) return false;

        if (isLoadStoreAddressUse(UserI, U)) {
            HasAddressUse = true;
            continue;
        }

        GetElementPtrInst *Child = nullptr;
        int64_t Offset = 0;
        if (!getConstantOffsetGEP(GEP, UserI, U, Child, Offset))
            return false;
        if ((Offset > 0 &&
             CurrentOffset > std::numeric_limits<int64_t>::max() - Offset) ||
            (Offset < 0 &&
             CurrentOffset < std::numeric_limits<int64_t>::min() - Offset)) {
            return false;
        }
        Children.emplace_back(Child, CurrentOffset + Offset);
    }

    if (HasAddressUse) Nodes.push_back({GEP, CurrentOffset});
    bool HasSupportedUse = HasAddressUse;
    for (const auto &[Child, Offset] : Children) {
        if (!collectAddressGEPFamily(Child, L, Offset, Nodes, Visited))
            return false;
        HasSupportedUse = true;
    }
    return HasSupportedUse;
}

static std::optional<int64_t> getLastIndexStride(GetElementPtrInst *GEP) {
    if (!GEP) return std::nullopt;
    auto &Operands = GEP->getUserList();
    if (Operands.size() < 2 || !Operands.front()) return std::nullopt;

    Value *Base = Operands.front()->getValue();
    if (!Base || !Base->getIRType()) return std::nullopt;

    std::shared_ptr<IRType> CurrentTy = Base->getIRType();
    int64_t LastStride = 0;
    for (size_t I = 1; I < Operands.size(); ++I) {
        std::shared_ptr<IRType> ElementTy;
        if (auto PtrTy = std::dynamic_pointer_cast<POINTERType>(CurrentTy)) {
            ElementTy = PtrTy->getPointerType();
        } else if (auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(CurrentTy)) {
            ElementTy = ArrTy->getElementType();
        } else {
            return std::nullopt;
        }
        if (!ElementTy ||
            ElementTy->getSize() >
                static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            return std::nullopt;
        }
        LastStride = static_cast<int64_t>(ElementTy->getSize());
        CurrentTy = ElementTy;
    }
    return LastStride;
}

static bool isSingleIndexArrayStep(Value *Base, size_t NumIndices) {
    if (NumIndices != 1 || !Base) return false;
    auto PtrTy = std::dynamic_pointer_cast<POINTERType>(Base->getIRType());
    return PtrTy && PtrTy->getPointerType() &&
           PtrTy->getPointerType()->getEnum() == IR_ARRAY;
}

static bool matchGEP(GetElementPtrInst *GEP, Loop *L, SCEVResult &SE,
                     Value *&Base, Value *&Index,
                     std::vector<IndexComponent> &Prefix,
                     bool &HasLeadingZero) {
    Base = nullptr;
    Index = nullptr;
    Prefix.clear();
    HasLeadingZero = false;
    if (!GEP || !L) return false;

    auto &Operands = GEP->getUserList();
    if (Operands.size() < 2) return false;

    Base = Operands.front()->getValue();
    Index = Operands.back()->getValue();
    if (!Base || !Index) return false;
    if (isSingleIndexArrayStep(Base, Operands.size() - 1)) return false;

    for (size_t I = 1; I + 1 < Operands.size(); ++I) {
        Value *Raw = Operands[I]->getValue();
        if (!Raw) return false;
        const SCEVExpr *Expr = SE.getSCEV(Raw);
        if (!Expr || mentionsLoop(Expr, L)) return false;
        Prefix.push_back({Raw, Expr});
    }

    HasLeadingZero = !Prefix.empty() && isConstZero(Prefix.front().Raw);
    return true;
}

static std::vector<LSRCandidate>
collectCandidates(Function &F, LoopInfo &LI, SCEVResult &SE,
                  const DominatorTree &DT) {
    std::unordered_map<BasicBlock *, size_t> BlockOrder;
    size_t NextBlockOrder = 0;
    for (BasicBlock *BB : F) BlockOrder[BB] = NextBlockOrder++;

    std::vector<Loop *> Loops;
    Loops.reserve(LI.Loops.size());
    for (auto &LoopPtr : LI.Loops) {
        if (LoopPtr) Loops.push_back(LoopPtr.get());
    }
    std::sort(Loops.begin(), Loops.end(), [&](Loop *A, Loop *B) {
        if (A->getDepth() != B->getDepth())
            return A->getDepth() > B->getDepth();
        size_t AO = BlockOrder.count(A->getHeader())
                        ? BlockOrder.at(A->getHeader())
                        : std::numeric_limits<size_t>::max();
        size_t BO = BlockOrder.count(B->getHeader())
                        ? BlockOrder.at(B->getHeader())
                        : std::numeric_limits<size_t>::max();
        return AO < BO;
    });

    std::vector<LSRCandidate> Candidates;
    for (Loop *L : Loops) {
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Latch = nullptr;
        if (!L || !L->getLoopSimplifyForm(PreHeader, Latch)) continue;

        BasicBlock *Header = L->getHeader();
        if (!PreHeader || !Header || !Latch) continue;

        std::vector<BasicBlock *> LoopBlocks = collectLoopBlocks(L);
        std::sort(LoopBlocks.begin(), LoopBlocks.end(),
                  [&](BasicBlock *A, BasicBlock *B) {
                      size_t AO = BlockOrder.count(A)
                                      ? BlockOrder.at(A)
                                      : std::numeric_limits<size_t>::max();
                      size_t BO = BlockOrder.count(B)
                                      ? BlockOrder.at(B)
                                      : std::numeric_limits<size_t>::max();
                      return AO < BO;
                  });
        for (BasicBlock *BB : LoopBlocks) {
            if (!BB) continue;
            for (Instruction *I : *BB) {
                auto *GEP = dynamic_cast<GetElementPtrInst *>(I);
                if (!GEP) continue;

                Value *Base = nullptr;
                Value *Index = nullptr;
                std::vector<IndexComponent> Prefix;
                bool HasLeadingZero = false;
                if (!matchGEP(GEP, L, SE, Base, Index, Prefix,
                              HasLeadingZero)) {
                    continue;
                }

                std::optional<int64_t> LastIndexStride =
                    getLastIndexStride(GEP);
                if (!LastIndexStride || *LastIndexStride <= 0) continue;

                auto Affine = SE.getAffine(Index);
                if (!Affine || Affine->RecLoop != L || !Affine->Start ||
                    !Affine->ConstantStep) {
                    continue;
                }

                int64_t Step = *Affine->ConstantStep;
                if (Step == 0 || Step < std::numeric_limits<int>::min() ||
                    Step > std::numeric_limits<int>::max()) {
                    continue;
                }
                if (!canUseInPreheader(Base, PreHeader, DT)) continue;

                int PrefixCost =
                    estimatePrefixMaterializeCost(Prefix, L, PreHeader, DT);
                if (PrefixCost >= kUnsupportedMaterializeCost) continue;

                std::optional<int64_t> StartConst = Affine->ConstantStart;
                if (StartConst &&
                    (*StartConst < std::numeric_limits<int>::min() ||
                     *StartConst > std::numeric_limits<int>::max())) {
                    continue;
                }
                if (!StartConst &&
                    !canMaterializeSCEVAtPreheader(Affine->Start, L,
                                                   PreHeader, DT)) {
                    continue;
                }

                std::vector<AddressGEPNode> Family;
                std::unordered_set<GetElementPtrInst *> Visited;
                if (!collectAddressGEPFamily(GEP, L, 0, Family, Visited))
                    continue;

                for (const AddressGEPNode &Node : Family) {
                    std::optional<int64_t> NodeStartConst;
                    if (StartConst) {
                        if ((Node.Offset > 0 &&
                             *StartConst >
                                 std::numeric_limits<int64_t>::max() -
                                     Node.Offset) ||
                            (Node.Offset < 0 &&
                             *StartConst <
                                 std::numeric_limits<int64_t>::min() -
                                     Node.Offset)) {
                            continue;
                        }
                        int64_t Start = *StartConst + Node.Offset;
                        if (Start < std::numeric_limits<int>::min() ||
                            Start > std::numeric_limits<int>::max()) {
                            continue;
                        }
                        NodeStartConst = Start;
                    } else if (Node.Offset != 0) {
                        continue;
                    }

                    Candidates.push_back(
                        {Node.GEP, Base, Index, Affine->Start,
                         NodeStartConst, Prefix, L, PreHeader, Header, Latch,
                         Step, *LastIndexStride, HasLeadingZero, false});
                }
            }
        }
    }
    return Candidates;
}

static std::string candidateShapeKey(const LSRCandidate &C,
                                     bool IncludeStart) {
    std::ostringstream Key;
    Key << "L=" << pointerKey(C.L) << ";B=" << pointerKey(C.Base)
        << ";Step=" << C.Step
        << ";Stride=" << C.LastIndexStride
        << ";Lead0=" << (C.HasLeadingZero ? 1 : 0)
        << ";Ty=" << typeKey(C.OldGEP ? C.OldGEP->getIRType() : nullptr)
        << ";PrefixN=" << C.Prefix.size();
    for (const IndexComponent &Component : C.Prefix)
        Key << ";P=" << scevKey(Component.Expr);
    if (IncludeStart) {
        if (C.StartConst)
            Key << ";StartConst=" << *C.StartConst;
        else
            Key << ";Start=" << scevKey(C.Start);
    }
    return Key.str();
}

static std::vector<OffsetReuseGroup>
buildOffsetReuseGroups(std::vector<LSRCandidate> &Candidates) {
    std::vector<OffsetReuseGroup> Groups;
    std::unordered_map<std::string, size_t> GroupIndex;

    for (LSRCandidate &C : Candidates) {
        if (C.Used || !C.StartConst) continue;
        std::string Key = candidateShapeKey(C, false);
        auto It = GroupIndex.find(Key);
        if (It == GroupIndex.end()) {
            size_t Index = Groups.size();
            GroupIndex.emplace(Key, Index);
            OffsetReuseGroup Group;
            Group.Order = Index;
            Group.Base = C.Base;
            Group.L = C.L;
            Group.PreHeader = C.PreHeader;
            Group.Header = C.Header;
            Group.Latch = C.Latch;
            Group.Step = C.Step;
            Group.LastIndexStride = C.LastIndexStride;
            Group.Prefix = C.Prefix;
            Group.ResultTy = C.OldGEP ? C.OldGEP->getIRType() : nullptr;
            Group.AnchorStart = *C.StartConst;
            Group.MaxStart = *C.StartConst;
            Groups.push_back(std::move(Group));
            It = GroupIndex.find(Key);
        }

        OffsetReuseGroup &Group = Groups[It->second];
        Group.Items.push_back(&C);
        Group.AnchorStart = std::min(Group.AnchorStart, *C.StartConst);
        Group.MaxStart = std::max(Group.MaxStart, *C.StartConst);
    }

    Groups.erase(
        std::remove_if(Groups.begin(), Groups.end(),
                       [](const OffsetReuseGroup &Group) {
                           if (Group.Items.size() < 2) return true;
                           std::unordered_set<int64_t> Starts;
                           for (const LSRCandidate *C : Group.Items)
                               Starts.insert(*C->StartConst);
                           return Starts.size() < 2 ||
                                  Group.MaxStart - Group.AnchorStart >
                                      kMaxOffsetRange;
                       }),
        Groups.end());

    std::sort(Groups.begin(), Groups.end(),
              [](const OffsetReuseGroup &A, const OffsetReuseGroup &B) {
                  unsigned AD = A.L ? A.L->getDepth() : 0;
                  unsigned BD = B.L ? B.L->getDepth() : 0;
                  if (AD != BD) return AD > BD;
                  if (A.Items.size() != B.Items.size())
                      return A.Items.size() > B.Items.size();
                  int64_t ARange = A.MaxStart - A.AnchorStart;
                  int64_t BRange = B.MaxStart - B.AnchorStart;
                  if (ARange != BRange) return ARange < BRange;
                  return A.Order < B.Order;
              });
    return Groups;
}

static std::vector<LSRGroup>
buildExactGroups(std::vector<LSRCandidate> &Candidates) {
    std::vector<LSRGroup> Groups;
    std::unordered_map<std::string, size_t> GroupIndex;

    for (LSRCandidate &C : Candidates) {
        if (C.Used) continue;
        std::string Key = candidateShapeKey(C, true);
        auto It = GroupIndex.find(Key);
        if (It == GroupIndex.end()) {
            size_t Index = Groups.size();
            GroupIndex.emplace(Key, Index);
            LSRGroup Group;
            Group.Order = Index;
            Group.Base = C.Base;
            Group.Start = C.Start;
            Group.StartConst = C.StartConst;
            Group.L = C.L;
            Group.PreHeader = C.PreHeader;
            Group.Header = C.Header;
            Group.Latch = C.Latch;
            Group.Step = C.Step;
            Group.LastIndexStride = C.LastIndexStride;
            Group.Prefix = C.Prefix;
            Group.ResultTy = C.OldGEP ? C.OldGEP->getIRType() : nullptr;
            Groups.push_back(std::move(Group));
            It = GroupIndex.find(Key);
        }
        Groups[It->second].Items.push_back(&C);
    }

    std::sort(Groups.begin(), Groups.end(),
              [](const LSRGroup &A, const LSRGroup &B) {
                  unsigned AD = A.L ? A.L->getDepth() : 0;
                  unsigned BD = B.L ? B.L->getDepth() : 0;
                  if (AD != BD) return AD > BD;
                  if (A.Items.size() != B.Items.size())
                      return A.Items.size() > B.Items.size();
                  return A.Order < B.Order;
              });
    return Groups;
}

static bool isRemovableIndexInst(Instruction *I) {
    if (!I || !I->getParent()) return false;
    return dynamic_cast<BinaryInst *>(I) ||
           dynamic_cast<GetElementPtrInst *>(I) ||
           dynamic_cast<SelectInst *>(I) || dynamic_cast<TruncInst *>(I) ||
           dynamic_cast<ZextInst *>(I) || dynamic_cast<SextInst *>(I) ||
           dynamic_cast<BitCastInst *>(I);
}

static bool hasNoUses(Instruction *I) {
    if (!I) return false;
    for (auto It = I->getUseList().begin(); It != I->getUseList().end();
         ++It) {
        Use *U = *It;
        if (U && U->getValue() == I) return false;
    }
    return true;
}

static bool collectRemovableIndexChain(
    Instruction *I, std::unordered_set<User *> &AllowedUsers,
    std::unordered_set<Instruction *> &Out) {
    if (!I || Out.count(I)) return true;
    if (!isRemovableIndexInst(I)) return false;

    for (auto It = I->getUseList().begin(); It != I->getUseList().end();
         ++It) {
        Use *U = *It;
        if (!U || U->getValue() != I) continue;
        if (!AllowedUsers.count(U->getUser())) return false;
    }

    Out.insert(I);
    AllowedUsers.insert(I);
    for (auto &OperandUse : I->getUserList()) {
        if (!OperandUse) continue;
        auto *OperandI =
            dynamic_cast<Instruction *>(OperandUse->getValue());
        if (OperandI && OperandI != I)
            collectRemovableIndexChain(OperandI, AllowedUsers, Out);
    }
    return true;
}

static int countRemovableIndexInsts(
    const std::vector<LSRCandidate *> &Items) {
    std::unordered_set<User *> AllowedUsers;
    for (LSRCandidate *C : Items) {
        if (C && C->OldGEP) AllowedUsers.insert(C->OldGEP);
    }

    std::unordered_set<Instruction *> Removable;
    for (LSRCandidate *C : Items) {
        auto *IndexI = C ? dynamic_cast<Instruction *>(C->Index) : nullptr;
        if (IndexI)
            collectRemovableIndexChain(IndexI, AllowedUsers, Removable);
    }
    return static_cast<int>(Removable.size());
}

static int countCoveredAddressUses(
    const std::vector<LSRCandidate *> &Items) {
    int Count = 0;
    for (const LSRCandidate *C : Items) {
        if (C) Count += countAddressUses(C->OldGEP);
    }
    return Count;
}

static bool coversDenseInnermostAddresses(
    Loop *L, const std::vector<LSRCandidate *> &Items) {
    if (!L || !L->isInnermost()) return false;
    int CoveredAddressUses = countCoveredAddressUses(Items);
    int LoopAddressUses = countLoopMemoryAddressUses(L);
    return CoveredAddressUses >= 3 && LoopAddressUses <= 8 &&
           CoveredAddressUses * 2 >= LoopAddressUses;
}

static void cleanupDeadIndexChains(
    const std::vector<Instruction *> &Seeds) {
    std::vector<Instruction *> WorkList;
    std::unordered_set<Instruction *> Queued;
    for (Instruction *Seed : Seeds) {
        if (Seed && Queued.insert(Seed).second) WorkList.push_back(Seed);
    }

    while (!WorkList.empty()) {
        Instruction *Current = WorkList.back();
        WorkList.pop_back();
        Queued.erase(Current);
        if (!isRemovableIndexInst(Current) || !hasNoUses(Current)) continue;

        std::vector<Instruction *> Operands;
        for (auto &OperandUse : Current->getUserList()) {
            if (!OperandUse) continue;
            auto *OperandI =
                dynamic_cast<Instruction *>(OperandUse->getValue());
            if (OperandI && OperandI != Current) Operands.push_back(OperandI);
        }

        delete Current;
        for (Instruction *OperandI : Operands) {
            if (OperandI && isRemovableIndexInst(OperandI) &&
                hasNoUses(OperandI) && Queued.insert(OperandI).second) {
                WorkList.push_back(OperandI);
            }
        }
    }
}

static bool isSimpleSingleGEP(const LSRCandidate &C) {
    if (C.Step != 1 || !C.Start || !isSCEVConstZero(C.Start) ||
        hasNonTrivialPrefix(C.Prefix)) {
        return false;
    }
    auto *Phi = dynamic_cast<PhiInst *>(C.Index);
    return Phi && Phi->getParent() == C.Header;
}

static bool shouldRewriteExactGroup(const LSRGroup &Group,
                                    const DominatorTree &DT) {
    if (Group.Items.empty()) return false;
    bool CostlyStride = hasCostlyElementStride(Group.LastIndexStride);

    int PrefixCost = estimatePrefixMaterializeCost(
        Group.Prefix, Group.L, Group.PreHeader, DT);
    int StartCost =
        Group.StartConst
            ? 0
            : estimateMaterializeCost(Group.Start, Group.L, Group.PreHeader,
                                      DT);
    int MaxMaterializeCost = CostlyStride ? 2 : 1;
    if (PrefixCost > MaxMaterializeCost || StartCost > MaxMaterializeCost)
        return false;

    if (Group.Items.size() == 1) {
        const LSRCandidate &C = *Group.Items.front();
        int Removable = countRemovableIndexInsts(Group.Items);
        if (isSimpleSingleGEP(C) && !CostlyStride) return false;
        if (CostlyStride) return countAddressUses(C.OldGEP) > 0;
        if (C.Step == 1 || C.Step == -1) return false;
        return Removable > 0 || hasNonTrivialPrefix(C.Prefix);
    }

    if (Group.Items.size() >= 3) return true;

    int Removable = countRemovableIndexInsts(Group.Items);
    return CostlyStride || Removable > 0 ||
           (Group.Step != 1 && Group.Step != -1) ||
           hasNonTrivialPrefix(Group.Prefix);
}

static bool shouldRewriteOffsetGroup(const OffsetReuseGroup &Group,
                                     const DominatorTree &DT) {
    if (Group.Items.size() < 2) return false;
    int64_t Range = Group.MaxStart - Group.AnchorStart;
    if (Range <= 0 || Range > kMaxOffsetRange) return false;

    bool CostlyStride = hasCostlyElementStride(Group.LastIndexStride);
    int MaxPrefixCost = CostlyStride ? 2 : 1;
    if (estimatePrefixMaterializeCost(Group.Prefix, Group.L, Group.PreHeader,
                                      DT) > MaxPrefixCost) {
        return false;
    }

    std::unordered_set<int64_t> Starts;
    for (const LSRCandidate *C : Group.Items)
        Starts.insert(*C->StartConst);
    if (Starts.size() < 2) return false;

    int Removable = countRemovableIndexInsts(Group.Items);
    int OffsetGEPs = static_cast<int>(Starts.size()) - 1;
    int NewLoopGEPs = 1 + OffsetGEPs;
    if (Group.Items.size() == 2) {
        if (CostlyStride) return true;
        if (Range > 4) return false;
        if (Group.Step != 1 && Group.Step != -1) return true;
        return Removable > 0 && Range <= 2;
    }

    if (NewLoopGEPs < static_cast<int>(Group.Items.size()) ||
        CostlyStride || Removable > 0 ||
        (Group.Step != 1 && Group.Step != -1)) {
        return true;
    }

    return coversDenseInnermostAddresses(Group.L, Group.Items);
}

static PointerIV createPointerIV(Value *Base,
                                 const std::vector<Value *> &StartIndices,
                                 int64_t Step,
                                 const std::shared_ptr<IRType> &ResultTy,
                                 BasicBlock *PreHeader, BasicBlock *Header,
                                 BasicBlock *Latch) {
    PointerIV Result;

    std::vector<Value *> Indices = StartIndices;
    Result.Start = new GetElementPtrInst(Base, Indices);
    Result.Start->updateType();
    insertBeforeTerminator(PreHeader, Result.Start);

    Result.Phi = new PhiInst(ResultTy);
    Header->push_front(Result.Phi);

    std::vector<Value *> NextIndices;
    NextIndices.push_back(
        ConstInt::newConstInt(static_cast<int>(Step)));
    Result.Next = new GetElementPtrInst(Result.Phi, NextIndices);
    Result.Next->updateType();
    insertBeforeTerminator(Latch, Result.Next);

    Result.Phi->addIncoming(Result.Start, PreHeader);
    Result.Phi->addIncoming(Result.Next, Latch);
    return Result;
}

static bool rewriteExactGroup(LSRGroup &Group, const DominatorTree &DT,
                              std::vector<Instruction *> &DeadSeeds) {
    std::vector<Value *> StartIndices;
    if (!materializePrefix(Group.Prefix, Group.L, Group.PreHeader, DT,
                           StartIndices)) {
        return false;
    }

    Value *StartValue =
        Group.StartConst
            ? makeSCEVConstant(*Group.StartConst, Group.Start)
            : materializeSCEVAtPreheader(Group.Start, Group.L,
                                         Group.PreHeader, DT);
    if (!StartValue) return false;
    StartIndices.push_back(StartValue);

    PointerIV IV = createPointerIV(
        Group.Base, StartIndices, Group.Step, Group.ResultTy,
        Group.PreHeader, Group.Header, Group.Latch);

    for (LSRCandidate *C : Group.Items) {
        if (!C || C->Used || !C->OldGEP || !C->OldGEP->getParent())
            continue;
        if (auto *IndexI = dynamic_cast<Instruction *>(C->Index))
            DeadSeeds.push_back(IndexI);
        C->OldGEP->replaceAllUsesWith(IV.Phi);
        delete C->OldGEP;
        C->Used = true;
    }
    return true;
}

static bool rewriteOffsetGroup(OffsetReuseGroup &Group,
                               const DominatorTree &DT,
                               std::vector<Instruction *> &DeadSeeds) {
    std::vector<Value *> StartIndices;
    if (!materializePrefix(Group.Prefix, Group.L, Group.PreHeader, DT,
                           StartIndices)) {
        return false;
    }

    const SCEVExpr *StartTypeExpr =
        Group.Items.empty() ? nullptr : Group.Items.front()->Start;
    Value *Anchor =
        makeSCEVConstant(Group.AnchorStart, StartTypeExpr);
    if (!Anchor) return false;
    StartIndices.push_back(Anchor);

    PointerIV IV = createPointerIV(
        Group.Base, StartIndices, Group.Step, Group.ResultTy,
        Group.PreHeader, Group.Header, Group.Latch);

    std::unordered_map<int64_t, Value *> OffsetValues;
    OffsetValues.emplace(0, IV.Phi);
    for (LSRCandidate *C : Group.Items) {
        int64_t Offset = *C->StartConst - Group.AnchorStart;
        if (OffsetValues.count(Offset)) continue;

        std::vector<Value *> OffsetIndices;
        OffsetIndices.push_back(
            ConstInt::newConstInt(static_cast<int>(Offset)));
        auto *OffsetGEP = new GetElementPtrInst(IV.Phi, OffsetIndices);
        OffsetGEP->updateType();

        BasicBlock *CommonBB = nullptr;
        bool SameBlock = true;
        std::unordered_set<GetElementPtrInst *> Positions;
        for (LSRCandidate *Item : Group.Items) {
            if (!Item || !Item->StartConst ||
                *Item->StartConst - Group.AnchorStart != Offset ||
                !Item->OldGEP || !Item->OldGEP->getParent()) {
                continue;
            }
            Positions.insert(Item->OldGEP);
            BasicBlock *ItemBB = Item->OldGEP->getParent();
            if (!CommonBB)
                CommonBB = ItemBB;
            else if (CommonBB != ItemBB)
                SameBlock = false;
        }

        Instruction *InsertBefore = nullptr;
        if (SameBlock && CommonBB) {
            for (Instruction *I : *CommonBB) {
                auto *Position = dynamic_cast<GetElementPtrInst *>(I);
                if (Position && Positions.count(Position)) {
                    InsertBefore = Position;
                    break;
                }
            }
        }
        if (!insertBeforeInstruction(InsertBefore, OffsetGEP))
            insertAfterPhiNodes(Group.Header, OffsetGEP);
        OffsetValues.emplace(Offset, OffsetGEP);
    }

    for (LSRCandidate *C : Group.Items) {
        if (!C || C->Used || !C->OldGEP || !C->OldGEP->getParent())
            continue;
        int64_t Offset = *C->StartConst - Group.AnchorStart;
        if (auto *IndexI = dynamic_cast<Instruction *>(C->Index))
            DeadSeeds.push_back(IndexI);
        C->OldGEP->replaceAllUsesWith(OffsetValues.at(Offset));
        delete C->OldGEP;
        C->Used = true;
    }
    return true;
}

static int pointerIVLimit(const Loop *L) {
    return L && L->isInnermost() ? kMaxPointerIVsPerInnerLoop
                                 : kMaxPointerIVsPerOuterLoop;
}

static bool AddressLSR(Function &F, LoopInfo &LI, SCEVResult &SE,
                       DominatorTree &DT) {
    std::vector<LSRCandidate> Candidates =
        collectCandidates(F, LI, SE, DT);
    if (Candidates.empty()) return false;

    std::unordered_map<Loop *, int> LoopPointerIVs;
    std::vector<Instruction *> DeadSeeds;
    bool Changed = false;

    std::vector<OffsetReuseGroup> OffsetGroups =
        buildOffsetReuseGroups(Candidates);
    for (OffsetReuseGroup &Group : OffsetGroups) {
        if (!shouldRewriteOffsetGroup(Group, DT)) continue;
        if (LoopPointerIVs[Group.L] >= pointerIVLimit(Group.L)) continue;
        if (!rewriteOffsetGroup(Group, DT, DeadSeeds)) continue;
        ++LoopPointerIVs[Group.L];
        Changed = true;
    }

    std::vector<LSRGroup> ExactGroups = buildExactGroups(Candidates);
    for (LSRGroup &Group : ExactGroups) {
        if (!shouldRewriteExactGroup(Group, DT)) continue;
        if (LoopPointerIVs[Group.L] >= pointerIVLimit(Group.L)) continue;
        if (!rewriteExactGroup(Group, DT, DeadSeeds)) continue;
        ++LoopPointerIVs[Group.L];
        Changed = true;
    }

    cleanupDeadIndexChains(DeadSeeds);
    return Changed;
}

} // namespace

PreservationStatus LoopStrengthReductionPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);

    bool Changed = AddressLSR(F, LI, SE, DT);
    if (!Changed) return PreservationStatus::all();

    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<SCEVAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
