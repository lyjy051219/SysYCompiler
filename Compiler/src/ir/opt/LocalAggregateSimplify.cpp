#include "ir/opt/LocalAggregateSimplify.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct PointerPath {
    AllocaInst *Root = nullptr;
    std::vector<int> Indices;
    bool IsConstant = true;
};

static bool isArrayAlloca(AllocaInst *AI) {
    if (!AI) return false;
    auto PtrTy = std::dynamic_pointer_cast<POINTERType>(AI->getIRType());
    return PtrTy && PtrTy->getPointerType() &&
           PtrTy->getPointerType()->getEnum() == IR_ARRAY;
}

static std::shared_ptr<IRType> getAllocatedType(AllocaInst *AI) {
    if (!AI) return nullptr;
    auto PtrTy = std::dynamic_pointer_cast<POINTERType>(AI->getIRType());
    return PtrTy ? PtrTy->getPointerType() : nullptr;
}

static std::shared_ptr<IRType> getPointeeType(Value *V) {
    if (!V) return nullptr;
    auto PtrTy = std::dynamic_pointer_cast<POINTERType>(V->getIRType());
    return PtrTy ? PtrTy->getPointerType() : nullptr;
}

static bool isScalarType(const std::shared_ptr<IRType> &Ty) {
    if (!Ty) return false;
    return Ty->getEnum() == IR_INT || Ty->getEnum() == IR_FLOAT;
}

static bool haveSameScalarType(const std::shared_ptr<IRType> &A,
                               const std::shared_ptr<IRType> &B) {
    return isScalarType(A) && isScalarType(B) &&
           A->getEnum() == B->getEnum() &&
           A->getSize() == B->getSize();
}

static int countScalarElements(const std::shared_ptr<IRType> &Ty,
                               int Limit) {
    if (!Ty) return -1;
    if (isScalarType(Ty)) return 1;
    auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(Ty);
    if (!ArrTy || ArrTy->getElementCount() < 0) return -1;
    int Sub = countScalarElements(ArrTy->getElementType(), Limit);
    if (Sub < 0) return -1;
    long long Total = 1LL * Sub * ArrTy->getElementCount();
    if (Total > Limit) return -1;
    return static_cast<int>(Total);
}

static bool flattenScalarPath(std::shared_ptr<IRType> Ty,
                              const std::vector<int> &Path,
                              int &FlatIndex) {
    FlatIndex = 0;
    for (int Index : Path) {
        auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(Ty);
        if (!ArrTy || Index < 0 || Index >= ArrTy->getElementCount())
            return false;
        int Sub = countScalarElements(ArrTy->getElementType(),
                                      1 << 20);
        if (Sub < 0) return false;
        FlatIndex += Index * Sub;
        Ty = ArrTy->getElementType();
    }
    return isScalarType(Ty);
}

static bool unflattenScalarPath(std::shared_ptr<IRType> Ty, int FlatIndex,
                                std::vector<int> &Path,
                                std::shared_ptr<IRType> &ElementTy) {
    Path.clear();
    ElementTy = nullptr;
    if (!Ty || FlatIndex < 0) return false;

    while (Ty && Ty->getEnum() == IR_ARRAY) {
        auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(Ty);
        if (!ArrTy) return false;
        int Sub = countScalarElements(ArrTy->getElementType(),
                                      1 << 20);
        if (Sub <= 0) return false;
        int Index = FlatIndex / Sub;
        if (Index < 0 || Index >= ArrTy->getElementCount())
            return false;
        Path.push_back(Index);
        FlatIndex %= Sub;
        Ty = ArrTy->getElementType();
    }

    if (FlatIndex != 0 || !isScalarType(Ty)) return false;
    ElementTy = Ty;
    return true;
}

static bool getConstIndex(Value *V, int &Index) {
    if (auto *CI = dynamic_cast<ConstInt *>(V)) {
        Index = CI->getVal();
        return true;
    }
    if (auto *CB = dynamic_cast<ConstBool *>(V)) {
        Index = CB->getVal() ? 1 : 0;
        return true;
    }
    return false;
}

static Value *makeIndexConstant(Value *Index, int Val) {
    if (!Index || !Index->getIRType() ||
        Index->getIRType()->getEnum() != IR_INT)
        return nullptr;

    auto IntTy = std::dynamic_pointer_cast<INTType>(Index->getIRType());
    if (IntTy && IntTy->getIsBool()) {
        if (Val != 0 && Val != 1) return nullptr;
        return ConstBool::newConstBool(Val != 0);
    }

    return ConstInt::newConstInt(Val, Index->getIRType()->getSize());
}

static bool insertInstBefore(Instruction *Pos, Instruction *NewI) {
    if (!Pos || !NewI) return false;
    BasicBlock *BB = Pos->getParent();
    if (!BB) return false;
    for (auto It = BB->begin(); It != BB->end(); ++It) {
        if (*It == Pos) {
            It.insertBefore(NewI);
            return true;
        }
    }
    return false;
}

static bool appearsBefore(Instruction *A, Instruction *B) {
    if (!A || !B || A->getParent() != B->getParent()) return false;
    for (auto It = A->getParent()->begin(); It != A->getParent()->end();
         ++It) {
        if (*It == A) return true;
        if (*It == B) return false;
    }
    return false;
}

// Trace only the pointer forms emitted by the front end. Each aggregate GEP
// starts with a zero pointer-dereference index; the remaining indices select
// array elements and can therefore be concatenated across chained GEPs.
static PointerPath tracePointer(Value *V,
                                std::unordered_set<Value *> &Visiting) {
    PointerPath Result;
    if (!V || !Visiting.insert(V).second) return Result;

    if (auto *AI = dynamic_cast<AllocaInst *>(V)) {
        Result.Root = AI;
        Visiting.erase(V);
        return Result;
    }

    if (auto *BC = dynamic_cast<BitCastInst *>(V)) {
        auto &Ops = BC->getUserList();
        if (Ops.size() == 1 && Ops[0])
            Result = tracePointer(Ops[0]->getValue(), Visiting);
        Visiting.erase(V);
        return Result;
    }

    auto *GEP = dynamic_cast<GetElementPtrInst *>(V);
    if (!GEP) {
        Visiting.erase(V);
        return Result;
    }

    auto &Ops = GEP->getUserList();
    if (Ops.size() < 2 || !Ops[0]) {
        Visiting.erase(V);
        return Result;
    }

    Result = tracePointer(Ops[0]->getValue(), Visiting);
    if (!Result.Root) {
        Visiting.erase(V);
        return Result;
    }

    int FirstIndex = 0;
    if (!Ops[1] || !getConstIndex(Ops[1]->getValue(), FirstIndex) ||
        FirstIndex != 0) {
        Result.IsConstant = false;
        Visiting.erase(V);
        return Result;
    }

    for (size_t I = 2; I < Ops.size(); ++I) {
        int Index = 0;
        if (!Ops[I] || !getConstIndex(Ops[I]->getValue(), Index)) {
            Result.IsConstant = false;
            break;
        }
        Result.Indices.push_back(Index);
    }

    Visiting.erase(V);
    return Result;
}

static PointerPath tracePointer(Value *V) {
    std::unordered_set<Value *> Visiting;
    return tracePointer(V, Visiting);
}

static bool getDirectConstantPath(
    AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
    std::shared_ptr<IRType> &PointeeTy) {
    Path.clear();
    PointeeTy = nullptr;
    if (!AI || !GEP) return false;

    auto &Ops = GEP->getUserList();
    if (Ops.size() < 2 || !Ops[0] || Ops[0]->getValue() != AI)
        return false;

    int FirstIndex = 0;
    if (!Ops[1] || !getConstIndex(Ops[1]->getValue(), FirstIndex) ||
        FirstIndex != 0)
        return false;

    std::shared_ptr<IRType> Ty = getAllocatedType(AI);
    for (size_t I = 2; I < Ops.size(); ++I) {
        int Index = 0;
        if (!Ops[I] || !getConstIndex(Ops[I]->getValue(), Index))
            return false;
        auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(Ty);
        if (!ArrTy || Index < 0 || Index >= ArrTy->getElementCount())
            return false;
        Path.push_back(Index);
        Ty = ArrTy->getElementType();
    }

    if (Path.empty()) return false;
    PointeeTy = Ty;
    return PointeeTy != nullptr;
}

static bool getDirectConstantScalarPath(
    AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
    std::shared_ptr<IRType> &ElementTy) {
    if (!getDirectConstantPath(AI, GEP, Path, ElementTy))
        return false;
    if (!isScalarType(ElementTy)) return false;
    return true;
}

static bool appendConstantAggregateGEPPath(
    Value *Base, GetElementPtrInst *GEP, const std::vector<int> &BasePath,
    std::vector<int> &NewPath, std::shared_ptr<IRType> &PointeeTy) {
    NewPath.clear();
    PointeeTy = nullptr;
    if (!Base || !GEP) return false;

    auto &Ops = GEP->getUserList();
    if (Ops.size() < 3 || !Ops[0] || Ops[0]->getValue() != Base)
        return false;

    int FirstIndex = 0;
    if (!Ops[1] || !getConstIndex(Ops[1]->getValue(), FirstIndex) ||
        FirstIndex != 0)
        return false;

    std::shared_ptr<IRType> Ty = getPointeeType(Base);
    if (!Ty || Ty->getEnum() != IR_ARRAY) return false;

    NewPath = BasePath;
    for (size_t I = 2; I < Ops.size(); ++I) {
        int Index = 0;
        if (!Ops[I] || !getConstIndex(Ops[I]->getValue(), Index))
            return false;
        auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(Ty);
        if (!ArrTy || Index < 0 || Index >= ArrTy->getElementCount())
            return false;
        NewPath.push_back(Index);
        Ty = ArrTy->getElementType();
    }

    PointeeTy = Ty;
    return PointeeTy != nullptr;
}

static bool getDirectOneDimElementIndex(AllocaInst *AI,
                                        GetElementPtrInst *GEP,
                                        Value *&Index) {
    Index = nullptr;
    if (!AI || !GEP) return false;
    auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(getAllocatedType(AI));
    if (!ArrTy || !isScalarType(ArrTy->getElementType())) return false;

    auto &Ops = GEP->getUserList();
    if (Ops.size() != 3 || !Ops[0] || Ops[0]->getValue() != AI ||
        !Ops[1] || !Ops[2])
        return false;

    int FirstIndex = 0;
    if (!getConstIndex(Ops[1]->getValue(), FirstIndex) ||
        FirstIndex != 0)
        return false;

    Index = Ops[2]->getValue();
    return Index && Index->getIRType() &&
           Index->getIRType()->getEnum() == IR_INT;
}

static bool getConstantOffsetScalarGEP(Value *Base, GetElementPtrInst *GEP,
                                       int &Offset) {
    Offset = 0;
    if (!Base || !GEP) return false;
    auto &Ops = GEP->getUserList();
    if (Ops.size() != 2 || !Ops[0] || Ops[0]->getValue() != Base ||
        !Ops[1])
        return false;

    if (!getConstIndex(Ops[1]->getValue(), Offset))
        return false;

    auto BasePtr = std::dynamic_pointer_cast<POINTERType>(Base->getIRType());
    auto GEPPtr = std::dynamic_pointer_cast<POINTERType>(GEP->getIRType());
    return BasePtr && GEPPtr && BasePtr->getPointerType() &&
           GEPPtr->getPointerType() &&
           isScalarType(BasePtr->getPointerType()) &&
           isScalarType(GEPPtr->getPointerType()) &&
           BasePtr->getPointerType()->getEnum() ==
               GEPPtr->getPointerType()->getEnum() &&
           BasePtr->getPointerType()->getSize() ==
               GEPPtr->getPointerType()->getSize();
}

static bool isMemcpy(CallInst *CI) {
    if (!CI) return false;
    auto &Ops = CI->getUserList();
    if (Ops.empty() || !Ops[0]) return false;

    Value *Callee = Ops[0]->getValue();
    std::string Name;
    if (auto *BIF = dynamic_cast<BuildInFunction *>(Callee))
        Name = BIF->getIdent();
    else if (auto *F = dynamic_cast<Function *>(Callee))
        Name = F->getIdent();
    return Name.find("memcpy") != std::string::npos;
}

static bool isFalseConstant(Value *V) {
    if (auto *CB = dynamic_cast<ConstBool *>(V)) return !CB->getVal();
    if (auto *CI = dynamic_cast<ConstInt *>(V)) return CI->getVal() == 0;
    return false;
}

static bool isNonVolatileMemcpy(CallInst *CI) {
    if (!isMemcpy(CI)) return false;
    auto &Ops = CI->getUserList();
    // callee, destination, source, byte count, volatile flag
    return Ops.size() >= 5 && Ops[4] &&
           isFalseConstant(Ops[4]->getValue());
}

static bool haveSameShape(const std::shared_ptr<IRType> &A,
                          const std::shared_ptr<IRType> &B) {
    if (!A || !B || A->getEnum() != B->getEnum() ||
        A->getSize() != B->getSize())
        return false;

    if (A->getEnum() != IR_ARRAY) return true;
    auto AA = std::dynamic_pointer_cast<ARRAYType>(A);
    auto BA = std::dynamic_pointer_cast<ARRAYType>(B);
    return AA && BA && AA->getElementCount() == BA->getElementCount() &&
           haveSameShape(AA->getElementType(), BA->getElementType());
}

static bool isProvablyReadOnlySource(Variable *Source) {
    if (!Source) return false;
    if (Source->vartag == Variable::Constant) return true;
    if (Source->vartag != Variable::GlobalVariable) return false;

    // Global2Local may retain the original synthesized global as the memcpy
    // source. Treat it as immutable only when every remaining use is a
    // non-volatile memcpy source; any load, store, GEP, or unknown call makes
    // this proof fail conservatively.
    for (auto It = Source->getUseList().begin();
         It != Source->getUseList().end(); ++It) {
        Use *U = *It;
        if (!U || U->getValue() != Source) continue;
        auto *CI = dynamic_cast<CallInst *>(U->getUser());
        if (!CI || !isNonVolatileMemcpy(CI) ||
            CI->getUserIndex(U) != 2)
            return false;
    }
    return true;
}

static bool isWholeConstantCopy(CallInst *CI, AllocaInst *Destination,
                                Variable *Source) {
    if (!CI || !Destination || !Source ||
        !isProvablyReadOnlySource(Source) ||
        !isNonVolatileMemcpy(CI))
        return false;

    auto &Ops = CI->getUserList();
    auto *ByteCount = dynamic_cast<ConstInt *>(Ops[3]->getValue());
    auto DestPtr =
        std::dynamic_pointer_cast<POINTERType>(Destination->getIRType());
    auto SourcePtr =
        std::dynamic_pointer_cast<POINTERType>(Source->getIRType());
    if (!ByteCount || !DestPtr || !SourcePtr) return false;

    auto DestTy = DestPtr->getPointerType();
    auto SourceTy = SourcePtr->getPointerType();
    return ByteCount->getVal() >= 0 &&
           static_cast<size_t>(ByteCount->getVal()) == DestTy->getSize() &&
           haveSameShape(DestTy, SourceTy);
}

static Value *makeZero(std::shared_ptr<IRType> Ty) {
    if (!Ty) return nullptr;
    if (Ty->getEnum() == IR_FLOAT) return ConstFloat::newConstFloat();
    if (Ty->getEnum() != IR_INT) return nullptr;

    if (auto IntTy = std::dynamic_pointer_cast<INTType>(Ty);
        IntTy && IntTy->getIsBool())
        return ConstBool::newConstBool();
    return ConstInt::newConstInt(0, Ty->getSize());
}

static bool compatibleTypes(Value *A, Value *B) {
    if (!A || !B || !A->getIRType() || !B->getIRType()) return false;
    auto ATy = A->getIRType();
    auto BTy = B->getIRType();
    return ATy->getEnum() == BTy->getEnum() &&
           ATy->getSize() == BTy->getSize();
}

// Return a scalar element from a constant aggregate. Missing initializer
// entries and a null initializer both have zeroinitializer semantics.
static Value *getConstantElement(Variable *Source,
                                 const std::vector<int> &Path) {
    if (!Source) return nullptr;
    auto PtrTy = std::dynamic_pointer_cast<POINTERType>(Source->getIRType());
    if (!PtrTy) return nullptr;

    std::shared_ptr<IRType> Ty = PtrTy->getPointerType();
    Value *Node = Source->getUsee();

    for (int Index : Path) {
        auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(Ty);
        if (!ArrTy || Index < 0 || Index >= ArrTy->getElementCount())
            return nullptr;

        if (Node) {
            auto *Init = dynamic_cast<Initializer *>(Node);
            if (!Init) return nullptr;
            Node = static_cast<size_t>(Index) < Init->size()
                       ? (*Init)[static_cast<size_t>(Index)]
                       : nullptr;
        }
        Ty = ArrTy->getElementType();
    }

    if (Ty && Ty->getEnum() == IR_ARRAY) return nullptr;
    if (!Node) return makeZero(Ty);
    return dynamic_cast<ConstantSystem *>(Node) ? Node : nullptr;
}

// Constant forwarding is intentionally restricted to one block. Validate the
// whole pointer-use graph first so an unmodelled use cannot silently alias it.
static bool hasSupportedSameBlockUses(AllocaInst *AI, BasicBlock *BB) {
    std::vector<Value *> Worklist{AI};
    std::unordered_set<Value *> Seen;

    while (!Worklist.empty()) {
        Value *Ptr = Worklist.back();
        Worklist.pop_back();
        if (!Seen.insert(Ptr).second) continue;

        for (auto It = Ptr->getUseList().begin();
             It != Ptr->getUseList().end(); ++It) {
            Use *U = *It;
            if (!U || U->getValue() != Ptr) continue;
            auto *I = dynamic_cast<Instruction *>(U->getUser());
            if (!I || I->getParent() != BB) return false;

            int OperandIndex = I->getUserIndex(U);
            if ((dynamic_cast<GetElementPtrInst *>(I) ||
                 dynamic_cast<BitCastInst *>(I)) &&
                OperandIndex == 0) {
                Worklist.push_back(I);
                continue;
            }
            if (dynamic_cast<LoadInst *>(I) && OperandIndex == 0)
                continue;
            if (dynamic_cast<StoreInst *>(I) && OperandIndex == 1)
                continue;
            if (auto *CI = dynamic_cast<CallInst *>(I);
                CI && isNonVolatileMemcpy(CI) && OperandIndex == 1)
                continue;
            return false;
        }
    }
    return true;
}

static bool forwardConstantLoads(AllocaInst *AI) {
    BasicBlock *BB = AI ? AI->getParent() : nullptr;
    if (!BB || !hasSupportedSameBlockUses(AI, BB)) return false;

    bool Changed = false;
    bool StateValid = false;
    Variable *InitialValue = nullptr;
    std::map<std::vector<int>, Value *> Overwrites;
    std::vector<LoadInst *> DeadLoads;

    std::vector<Instruction *> Instructions;
    for (auto It = BB->begin(); It != BB->end(); ++It)
        Instructions.push_back(*It);

    for (Instruction *I : Instructions) {
        if (auto *CI = dynamic_cast<CallInst *>(I)) {
            auto &Ops = CI->getUserList();
            if (!isMemcpy(CI) || Ops.size() < 3) continue;

            PointerPath Dest = tracePointer(Ops[1]->getValue());
            if (Dest.Root != AI) continue;

            auto *Source = dynamic_cast<Variable *>(Ops[2]->getValue());
            if (!Dest.IsConstant || !Dest.Indices.empty() ||
                !isWholeConstantCopy(CI, AI, Source)) {
                StateValid = false;
                InitialValue = nullptr;
                Overwrites.clear();
                continue;
            }
            StateValid = true;
            InitialValue = Source;
            Overwrites.clear();
            continue;
        }

        if (auto *SI = dynamic_cast<StoreInst *>(I)) {
            auto &Ops = SI->getUserList();
            if (Ops.size() < 2) continue;
            PointerPath Dest = tracePointer(Ops[1]->getValue());
            if (Dest.Root != AI) continue;

            if (!StateValid || !Dest.IsConstant || Dest.Indices.empty()) {
                StateValid = false;
                InitialValue = nullptr;
                Overwrites.clear();
                continue;
            }
            Overwrites[Dest.Indices] = Ops[0]->getValue();
            continue;
        }

        auto *LI = dynamic_cast<LoadInst *>(I);
        if (!LI) continue;
        auto &Ops = LI->getUserList();
        if (Ops.empty()) continue;

        PointerPath SourcePath = tracePointer(Ops[0]->getValue());
        if (SourcePath.Root != AI || !StateValid ||
            !SourcePath.IsConstant || SourcePath.Indices.empty())
            continue;

        Value *Replacement = nullptr;
        auto OverwriteIt = Overwrites.find(SourcePath.Indices);
        if (OverwriteIt != Overwrites.end())
            Replacement = OverwriteIt->second;
        else
            Replacement = getConstantElement(InitialValue,
                                             SourcePath.Indices);

        if (!compatibleTypes(LI, Replacement)) continue;
        LI->replaceAllUsesWith(Replacement);
        DeadLoads.push_back(LI);
        Changed = true;
    }

    for (LoadInst *LI : DeadLoads)
        delete LI;
    return Changed;
}

static int getOneDimScalarArrayCount(AllocaInst *AI) {
    auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(getAllocatedType(AI));
    if (!ArrTy || !isScalarType(ArrTy->getElementType()))
        return -1;
    return ArrTy->getElementCount();
}

static bool indexTypeCanSelectAll(Value *Index, int Count) {
    if (!Index || !Index->getIRType() ||
        Index->getIRType()->getEnum() != IR_INT)
        return false;
    auto IntTy = std::dynamic_pointer_cast<INTType>(Index->getIRType());
    return !IntTy || !IntTy->getIsBool() || Count <= 2;
}

struct ReadOnlyConstantArrayPlan {
    Variable *Source = nullptr;
    CallInst *InitCopy = nullptr;
    std::vector<GetElementPtrInst *> ElementPtrs;
    std::vector<LoadInst *> Loads;
};

static bool collectReadOnlyConstantArrayUses(Function &F, AllocaInst *AI,
                                             ReadOnlyConstantArrayPlan &Plan,
                                             int Count) {
    if (!AI || Count <= 0) return false;

    std::set<GetElementPtrInst *> SeenGEPs;
    for (auto It = AI->getUseList().begin(); It != AI->getUseList().end();
         ++It) {
        Use *U = *It;
        if (!U || U->getValue() != AI) continue;

        auto *I = dynamic_cast<Instruction *>(U->getUser());
        if (!I) return false;
        int OperandIndex = I->getUserIndex(U);

        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I);
            GEP && OperandIndex == 0) {
            Value *Index = nullptr;
            if (!getDirectOneDimElementIndex(AI, GEP, Index) ||
                !indexTypeCanSelectAll(Index, Count))
                return false;
            if (SeenGEPs.insert(GEP).second)
                Plan.ElementPtrs.push_back(GEP);
            continue;
        }

        if (auto *CI = dynamic_cast<CallInst *>(I);
            CI && OperandIndex == 1 && isNonVolatileMemcpy(CI)) {
            auto &Ops = CI->getUserList();
            if (Ops.size() < 3 || Ops[1]->getValue() != AI ||
                Plan.InitCopy)
                return false;

            auto *Source = dynamic_cast<Variable *>(Ops[2]->getValue());
            if (!isWholeConstantCopy(CI, AI, Source)) return false;
            Plan.InitCopy = CI;
            Plan.Source = Source;
            continue;
        }

        return false;
    }

    if (!Plan.InitCopy || !Plan.Source || Plan.ElementPtrs.empty())
        return false;

    for (GetElementPtrInst *GEP : Plan.ElementPtrs) {
        for (auto It = GEP->getUseList().begin();
             It != GEP->getUseList().end(); ++It) {
            Use *U = *It;
            if (!U || U->getValue() != GEP) continue;
            auto *I = dynamic_cast<Instruction *>(U->getUser());
            if (!I) return false;
            int OperandIndex = I->getUserIndex(U);
            if (auto *LI = dynamic_cast<LoadInst *>(I);
                LI && OperandIndex == 0) {
                Plan.Loads.push_back(LI);
                continue;
            }
            return false;
        }
    }

    if (Plan.Loads.empty()) return false;

    BasicBlock *InitBB = Plan.InitCopy->getParent();
    if (!InitBB || InitBB != AI->getParent()) return false;
    BasicBlock *Entry = F.front();
    for (LoadInst *LI : Plan.Loads) {
        if (LI->getParent() == InitBB) {
            if (!appearsBefore(Plan.InitCopy, LI)) return false;
        } else if (InitBB != Entry) {
            return false;
        }
    }

    return true;
}

static Value *buildConstantArrayLookup(Variable *Source, int Count,
                                       Value *Index,
                                       Instruction *InsertBefore) {
    int ConstIndex = 0;
    if (getConstIndex(Index, ConstIndex)) {
        if (ConstIndex < 0 || ConstIndex >= Count) return nullptr;
        return getConstantElement(Source, {ConstIndex});
    }

    std::vector<Value *> Elements;
    Elements.reserve(static_cast<size_t>(Count));
    for (int I = 0; I < Count; ++I) {
        Value *Elem = getConstantElement(Source, {I});
        if (!Elem) return nullptr;
        Elements.push_back(Elem);
    }

    Value *Result = Elements[0];
    for (int I = 1; I < Count; ++I) {
        Value *Needle = makeIndexConstant(Index, I);
        if (!Needle) return nullptr;
        auto *Cmp = new BinaryInst(Index, BinaryInst::E, Needle);
        if (!insertInstBefore(InsertBefore, Cmp)) {
            delete Cmp;
            return nullptr;
        }
        auto *Sel = new SelectInst(Cmp, Elements[static_cast<size_t>(I)],
                                   Result);
        if (!insertInstBefore(InsertBefore, Sel)) {
            delete Sel;
            return nullptr;
        }
        Result = Sel;
    }
    return Result;
}

static bool expandReadOnlyConstantArrayLoads(Function &F, AllocaInst *AI) {
    constexpr int kMaxDynamicSelectArrayElements = 2;

    int Count = getOneDimScalarArrayCount(AI);
    if (Count <= 0) return false;

    ReadOnlyConstantArrayPlan Plan;
    if (!collectReadOnlyConstantArrayUses(F, AI, Plan, Count))
        return false;

    Value *FirstElement = getConstantElement(Plan.Source, {0});
    if (!FirstElement) return false;

    bool HasDynamicIndex = false;
    for (LoadInst *LI : Plan.Loads) {
        if (!compatibleTypes(LI, FirstElement)) return false;
        auto &Ops = LI->getUserList();
        if (Ops.empty()) return false;
        auto *GEP = dynamic_cast<GetElementPtrInst *>(Ops[0]->getValue());
        Value *Index = nullptr;
        if (!getDirectOneDimElementIndex(AI, GEP, Index) ||
            !indexTypeCanSelectAll(Index, Count))
            return false;

        int ConstIndex = 0;
        if (getConstIndex(Index, ConstIndex)) {
            if (ConstIndex < 0 || ConstIndex >= Count) return false;
        } else {
            HasDynamicIndex = true;
        }
    }
    if (HasDynamicIndex && Count > kMaxDynamicSelectArrayElements)
        return false;

    std::vector<std::pair<LoadInst *, Value *>> Replacements;
    Replacements.reserve(Plan.Loads.size());
    for (LoadInst *LI : Plan.Loads) {
        auto *GEP =
            dynamic_cast<GetElementPtrInst *>(LI->getUserList()[0]->getValue());
        Value *Index = nullptr;
        if (!getDirectOneDimElementIndex(AI, GEP, Index)) return false;

        Value *Replacement =
            buildConstantArrayLookup(Plan.Source, Count, Index, LI);
        if (!compatibleTypes(LI, Replacement)) return false;
        Replacements.push_back({LI, Replacement});
    }

    for (auto &Replacement : Replacements) {
        Replacement.first->replaceAllUsesWith(Replacement.second);
        delete Replacement.first;
    }

    delete Plan.InitCopy;
    for (auto It = Plan.ElementPtrs.rbegin();
         It != Plan.ElementPtrs.rend(); ++It) {
        if ((*It)->getUseList().is_empty()) delete *It;
    }

    if (!AI->getUseList().is_empty()) return false;
    delete AI;
    return true;
}

static bool addScalarSlot(
    std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
    const std::vector<int> &Path,
    const std::shared_ptr<IRType> &ElementTy,
    int MaxScalarizedElements) {
    auto SlotIt = SlotTypes.find(Path);
    if (SlotIt != SlotTypes.end())
        return haveSameScalarType(SlotIt->second, ElementTy);

    SlotTypes.emplace(Path, ElementTy);
    return static_cast<int>(SlotTypes.size()) <= MaxScalarizedElements;
}

static bool collectScalarizedPointerUses(
    AllocaInst *AI, Value *Ptr, const std::vector<int> &PtrPath,
    std::vector<GetElementPtrInst *> &TransparentPtrs,
    std::map<GetElementPtrInst *, std::vector<int>> &TransparentPath,
    std::set<GetElementPtrInst *> &SeenTransparent,
    std::vector<Instruction *> &MemoryAccesses,
    std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
    int MaxScalarizedElements) {
    auto PtrPointeeTy = getPointeeType(Ptr);
    bool PtrIsScalar = isScalarType(PtrPointeeTy);

    for (auto It = Ptr->getUseList().begin(); It != Ptr->getUseList().end();
         ++It) {
        Use *U = *It;
        if (!U || U->getValue() != Ptr) continue;
        auto *I = dynamic_cast<Instruction *>(U->getUser());
        if (!I) return false;
        int OperandIndex = I->getUserIndex(U);

        if (dynamic_cast<LoadInst *>(I) && OperandIndex == 0 &&
            PtrIsScalar) {
            MemoryAccesses.push_back(I);
            continue;
        }
        if (dynamic_cast<StoreInst *>(I) && OperandIndex == 1 &&
            PtrIsScalar) {
            MemoryAccesses.push_back(I);
            continue;
        }

        int Offset = 0;
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I);
            GEP && OperandIndex == 0 &&
            getConstantOffsetScalarGEP(Ptr, GEP, Offset)) {
            int FlatIndex = 0;
            if (!flattenScalarPath(getAllocatedType(AI), PtrPath,
                                   FlatIndex))
                return false;

            std::vector<int> NewPath;
            std::shared_ptr<IRType> NewElementTy;
            if (!unflattenScalarPath(getAllocatedType(AI),
                                     FlatIndex + Offset, NewPath,
                                     NewElementTy))
                return false;
            if (!addScalarSlot(SlotTypes, NewPath, NewElementTy,
                               MaxScalarizedElements))
                return false;

            auto ExistingPath = TransparentPath.find(GEP);
            if (ExistingPath != TransparentPath.end() &&
                ExistingPath->second != NewPath)
                return false;

            if (SeenTransparent.insert(GEP).second) {
                TransparentPtrs.push_back(GEP);
                TransparentPath[GEP] = NewPath;
                if (!collectScalarizedPointerUses(
                        AI, GEP, NewPath, TransparentPtrs, TransparentPath,
                        SeenTransparent, MemoryAccesses, SlotTypes,
                        MaxScalarizedElements))
                    return false;
            }
            continue;
        }

        std::vector<int> NewPath;
        std::shared_ptr<IRType> NewPointeeTy;
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I);
            GEP && OperandIndex == 0 &&
            appendConstantAggregateGEPPath(Ptr, GEP, PtrPath, NewPath,
                                           NewPointeeTy)) {
            if (isScalarType(NewPointeeTy) &&
                !addScalarSlot(SlotTypes, NewPath, NewPointeeTy,
                               MaxScalarizedElements))
                return false;
            if (!isScalarType(NewPointeeTy) &&
                NewPointeeTy->getEnum() != IR_ARRAY)
                return false;

            auto ExistingPath = TransparentPath.find(GEP);
            if (ExistingPath != TransparentPath.end() &&
                ExistingPath->second != NewPath)
                return false;

            if (SeenTransparent.insert(GEP).second) {
                TransparentPtrs.push_back(GEP);
                TransparentPath[GEP] = NewPath;
                if (!collectScalarizedPointerUses(
                        AI, GEP, NewPath, TransparentPtrs, TransparentPath,
                        SeenTransparent, MemoryAccesses, SlotTypes,
                        MaxScalarizedElements))
                    return false;
            }
            continue;
        }

        return false;
    }
    return true;
}

static bool scalarizeConstantIndexedAggregate(AllocaInst *AI) {
    constexpr int kMaxScalarizedElements = 32;
    if (!AI || !isArrayAlloca(AI)) return false;
    auto Fail = [](const char *) {
        return false;
    };

    CallInst *InitCopy = nullptr;
    Variable *InitSource = nullptr;
    std::vector<GetElementPtrInst *> ElementPtrs;
    std::vector<GetElementPtrInst *> TransparentPtrs;
    std::map<std::vector<int>, std::shared_ptr<IRType>> SlotTypes;
    std::set<GetElementPtrInst *> SeenGEPs;
    std::set<GetElementPtrInst *> SeenTransparent;
    std::vector<Instruction *> MemoryAccesses;
    std::map<GetElementPtrInst *, std::vector<int>> DirectPath;
    std::map<GetElementPtrInst *, std::vector<int>> TransparentPath;

    for (auto It = AI->getUseList().begin(); It != AI->getUseList().end();
         ++It) {
        Use *U = *It;
        if (!U || U->getValue() != AI) continue;
        auto *UserI = dynamic_cast<Instruction *>(U->getUser());
        if (!UserI) return Fail("non-inst use");

        int OperandIndex = UserI->getUserIndex(U);
        if (auto *CI = dynamic_cast<CallInst *>(UserI);
            CI && OperandIndex == 1 && isNonVolatileMemcpy(CI)) {
            auto &Ops = CI->getUserList();
            if (Ops.size() < 3 || Ops[1]->getValue() != AI || InitCopy)
                return Fail("bad memcpy operands");
            auto *Source = dynamic_cast<Variable *>(Ops[2]->getValue());
            if (!isWholeConstantCopy(CI, AI, Source))
                return Fail("not whole const copy");
            InitCopy = CI;
            InitSource = Source;
            continue;
        }

        auto *GEP = dynamic_cast<GetElementPtrInst *>(UserI);
        if (!GEP || OperandIndex != 0) return Fail("non-gep use");

        std::vector<int> Path;
        std::shared_ptr<IRType> PointeeTy;
        if (!getDirectConstantPath(AI, GEP, Path, PointeeTy))
            return false;

        if (!isScalarType(PointeeTy) && PointeeTy->getEnum() != IR_ARRAY)
            return Fail("bad pointee ty");
        if (isScalarType(PointeeTy) &&
            !addScalarSlot(SlotTypes, Path, PointeeTy,
                           kMaxScalarizedElements))
            return Fail("add root slot");

        if (SeenGEPs.insert(GEP).second) {
            ElementPtrs.push_back(GEP);
            DirectPath[GEP] = Path;
        }
    }

    if (ElementPtrs.empty()) return Fail("no element ptrs");

    for (GetElementPtrInst *GEP : ElementPtrs) {
        auto PathIt = DirectPath.find(GEP);
        if (PathIt == DirectPath.end()) return Fail("missing direct path");
        if (!collectScalarizedPointerUses(
                AI, GEP, PathIt->second, TransparentPtrs, TransparentPath,
                SeenTransparent, MemoryAccesses, SlotTypes,
                kMaxScalarizedElements))
            return Fail("collect uses");
    }

    if (SlotTypes.empty()) return Fail("no slots");

    if (InitCopy) {
        for (Instruction *Access : MemoryAccesses) {
            if (!Access) return Fail("bad memory access");
            if (Access->getParent() == InitCopy->getParent()) {
                if (!appearsBefore(InitCopy, Access))
                    return Fail("access before init");
                continue;
            }
            if (InitCopy->getParent() != AI->getParent())
                return Fail("access before init");
        }
    }

    std::map<std::vector<int>, Value *> InitialValues;
    if (InitSource) {
        for (auto &Entry : SlotTypes) {
            Value *Init = getConstantElement(InitSource, Entry.first);
            if (!Init || !Init->getIRType() || !Entry.second ||
                Init->getIRType()->getEnum() != Entry.second->getEnum() ||
                Init->getIRType()->getSize() != Entry.second->getSize())
                return Fail("bad init element");
            InitialValues[Entry.first] = Init;
        }
    }

    std::map<std::vector<int>, AllocaInst *> Slots;
    for (auto &Entry : SlotTypes) {
        auto *Slot =
            new AllocaInst(POINTERType::NewPointer(Entry.second));
        if (!insertInstBefore(AI, Slot)) {
            delete Slot;
            return false;
        }
        Slots[Entry.first] = Slot;
    }

    if (!InitialValues.empty()) {
        Instruction *InitInsertPoint = InitCopy ? static_cast<Instruction *>(InitCopy)
                                                : static_cast<Instruction *>(AI);
        for (auto &Entry : Slots) {
            Value *Init = InitialValues[Entry.first];
            auto *Store = new StoreInst(Init, Entry.second);
            if (!insertInstBefore(InitInsertPoint, Store)) {
                delete Store;
                return false;
            }
        }
    }

    for (auto It = TransparentPtrs.rbegin(); It != TransparentPtrs.rend();
         ++It) {
        GetElementPtrInst *GEP = *It;
        auto PathIt = TransparentPath.find(GEP);
        if (PathIt == TransparentPath.end()) return false;
        auto SlotIt = Slots.find(PathIt->second);
        if (SlotIt != Slots.end())
            GEP->replaceAllUsesWith(SlotIt->second);
        if (!GEP->getUseList().is_empty()) return false;
        delete GEP;
    }

    for (GetElementPtrInst *GEP : ElementPtrs) {
        auto PathIt = DirectPath.find(GEP);
        if (PathIt == DirectPath.end()) return false;
        auto SlotIt = Slots.find(PathIt->second);
        if (SlotIt != Slots.end())
            GEP->replaceAllUsesWith(SlotIt->second);
        if (!GEP->getUseList().is_empty()) return false;
        delete GEP;
    }

    if (InitCopy) delete InitCopy;

    if (!AI->getUseList().is_empty()) return false;
    delete AI;
    return true;
}

struct DeadAggregateUses {
    bool IsDead = true;
    std::vector<Instruction *> Writes;
    std::vector<Instruction *> DerivedPointers;
};

static DeadAggregateUses collectDeadAggregateUses(AllocaInst *AI) {
    DeadAggregateUses Result;
    std::vector<Value *> Worklist{AI};
    std::unordered_set<Value *> SeenValues;
    std::unordered_set<Instruction *> SeenWrites;

    while (!Worklist.empty() && Result.IsDead) {
        Value *Ptr = Worklist.back();
        Worklist.pop_back();
        if (!SeenValues.insert(Ptr).second) continue;

        for (auto It = Ptr->getUseList().begin();
             It != Ptr->getUseList().end(); ++It) {
            Use *U = *It;
            if (!U || U->getValue() != Ptr) continue;
            auto *I = dynamic_cast<Instruction *>(U->getUser());
            if (!I) {
                Result.IsDead = false;
                break;
            }

            int OperandIndex = I->getUserIndex(U);
            if ((dynamic_cast<GetElementPtrInst *>(I) ||
                 dynamic_cast<BitCastInst *>(I)) &&
                OperandIndex == 0) {
                if (!SeenValues.count(I)) {
                    Worklist.push_back(I);
                    Result.DerivedPointers.push_back(I);
                }
                continue;
            }

            if (dynamic_cast<StoreInst *>(I) && OperandIndex == 1) {
                if (SeenWrites.insert(I).second) Result.Writes.push_back(I);
                continue;
            }

            if (auto *CI = dynamic_cast<CallInst *>(I);
                CI && isNonVolatileMemcpy(CI) && OperandIndex == 1) {
                if (SeenWrites.insert(I).second) Result.Writes.push_back(I);
                continue;
            }

            // A load, a memcpy source, storing the pointer itself, or passing
            // it to any other call makes the allocation observable.
            Result.IsDead = false;
            break;
        }
    }
    return Result;
}

static bool deleteDeadAggregate(AllocaInst *AI) {
    DeadAggregateUses Uses = collectDeadAggregateUses(AI);
    if (!Uses.IsDead) return false;

    for (Instruction *I : Uses.Writes)
        delete I;

    // Parents are discovered before their derived pointers, so reverse order
    // removes leaves before roots.
    for (auto It = Uses.DerivedPointers.rbegin();
         It != Uses.DerivedPointers.rend(); ++It) {
        Instruction *I = *It;
        if (I->getUseList().is_empty()) delete I;
    }

    if (!AI->getUseList().is_empty()) return false;
    delete AI;
    return true;
}

static std::vector<AllocaInst *> collectArrayAllocas(Function &F) {
    std::vector<AllocaInst *> Aggregates;
    for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt) {
        BasicBlock *BB = *BBIt;
        for (auto IIt = BB->begin(); IIt != BB->end(); ++IIt) {
            if (auto *AI = dynamic_cast<AllocaInst *>(*IIt);
                isArrayAlloca(AI))
                Aggregates.push_back(AI);
        }
    }
    return Aggregates;
}

} // namespace

PreservationStatus
LocalAggregateSimplifyPass::run(Function &F,
                                FunctionAnalysisPassManager &) {
    bool Changed = false;

    for (AllocaInst *AI : collectArrayAllocas(F))
        Changed |= forwardConstantLoads(AI);
    for (AllocaInst *AI : collectArrayAllocas(F))
        Changed |= expandReadOnlyConstantArrayLoads(F, AI);
    for (AllocaInst *AI : collectArrayAllocas(F))
        Changed |= scalarizeConstantIndexedAggregate(AI);
    for (AllocaInst *AI : collectArrayAllocas(F))
        Changed |= deleteDeadAggregate(AI);

    return Changed ? PreservationStatus::none()
                   : PreservationStatus::all();
}
