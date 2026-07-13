#include "ir/opt/LocalAggregateSimplify.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

bool sameScalarConstant(Value *A, Value *B) {
    if (A == B) return true;
    if (!A || !B || !A->getIRType() || !B->getIRType() ||
        !A->getIRType()->hasSameShape(*B->getIRType()))
        return false;
    if (auto *AI = dynamic_cast<ConstInt *>(A)) {
        auto *BI = dynamic_cast<ConstInt *>(B);
        return BI && AI->getVal() == BI->getVal();
    }
    if (auto *AF = dynamic_cast<ConstFloat *>(A)) {
        auto *BF = dynamic_cast<ConstFloat *>(B);
        if (!BF) return false;
        uint32_t ABits = 0;
        uint32_t BBits = 0;
        float AV = AF->getVal();
        float BV = BF->getVal();
        std::memcpy(&ABits, &AV, sizeof(ABits));
        std::memcpy(&BBits, &BV, sizeof(BBits));
        return ABits == BBits;
    }
    return false;
}

} // namespace

bool LocalAggregateSimplifyPass::isWholeProgramReadOnlyVariable(
    Variable *Source) {
    if (!Source || (Source->vartag != Variable::Constant &&
                    Source->vartag != Variable::GlobalVariable))
        return false;

    std::vector<Value *> Worklist{Source};
    std::unordered_set<Value *> Seen;
    while (!Worklist.empty()) {
        Value *Pointer = Worklist.back();
        Worklist.pop_back();
        if (!Pointer || !Seen.insert(Pointer).second) continue;

        for (auto It = Pointer->getUseList().begin();
             It != Pointer->getUseList().end(); ++It) {
            Use *U = *It;
            if (!U || U->getValue() != Pointer) continue;
            auto *I = dynamic_cast<Instruction *>(U->getUser());
            if (!I) return false;
            int OperandIndex = I->getUserIndex(U);

            if ((dynamic_cast<GetElementPtrInst *>(I) ||
                 dynamic_cast<BitCastInst *>(I)) &&
                OperandIndex == 0) {
                Worklist.push_back(I);
                continue;
            }
            if (dynamic_cast<LoadInst *>(I) && OperandIndex == 0)
                continue;
            if (auto *CI = dynamic_cast<CallInst *>(I);
                CI && CI->isNonVolatileMemcpy() &&
                CI->getArgIndex(U) == 1)
                continue;

            // Passing the address to an arbitrary call, storing through it,
            // or storing the address itself can make the initializer stale or
            // observable.  Keep the proof whole-program and structural.
            return false;
        }
    }
    return true;
}

bool LocalAggregateSimplifyPass::traceConstantGlobalPointer(
    Value *Pointer, Variable *&Root, std::vector<int> &Path,
    std::shared_ptr<IRType> &PointeeTy) {
    Root = nullptr;
    Path.clear();
    PointeeTy = nullptr;
    if (!Pointer) return false;

    std::vector<GetElementPtrInst *> Chain;
    std::unordered_set<Value *> Seen;
    Value *Current = Pointer;
    while (Current && Seen.insert(Current).second) {
        if (auto *BC = dynamic_cast<BitCastInst *>(Current)) {
            Current = BC->getUnaryOperand();
            continue;
        }
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Current)) {
            Chain.push_back(GEP);
            Current = GEP->getBasePointer();
            continue;
        }
        Root = dynamic_cast<Variable *>(Current);
        break;
    }
    if (!Root || (Root->vartag != Variable::Constant &&
                  Root->vartag != Variable::GlobalVariable))
        return false;

    std::shared_ptr<IRType> Ty = Root->getPointeeType();
    for (auto It = Chain.rbegin(); It != Chain.rend(); ++It) {
        GetElementPtrInst *GEP = *It;
        if (!GEP || GEP->getNumIndices() < 1) return false;
        int PointerIndex = 0;
        if (!Value::tryGetIntIndex(GEP->getIndex(0), PointerIndex) ||
            PointerIndex != 0)
            return false;

        for (size_t I = 1; I < GEP->getNumIndices(); ++I) {
            int Index = 0;
            if (!Value::tryGetIntIndex(GEP->getIndex(I), Index))
                return false;
            auto ArrayTy = std::dynamic_pointer_cast<ARRAYType>(Ty);
            if (!ArrayTy || Index < 0 ||
                Index >= ArrayTy->getElementCount())
                return false;
            Path.push_back(Index);
            Ty = ArrayTy->getElementType();
        }
    }
    PointeeTy = Ty;
    return !Path.empty() && PointeeTy && IRType::isScalar(PointeeTy);
}

void LocalAggregateSimplifyPass::deleteDeadPointerChain(Value *Pointer) {
    while (auto *I = dynamic_cast<Instruction *>(Pointer)) {
        if (!I->getUseList().is_empty()) return;
        if (!dynamic_cast<GetElementPtrInst *>(I) &&
            !dynamic_cast<BitCastInst *>(I))
            return;
        Value *Parent = I->getOperand(0);
        delete I;
        Pointer = Parent;
    }
}

bool LocalAggregateSimplifyPass::foldReadOnlyGlobalLoads(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    std::unordered_map<Variable *, bool> ReadOnlyCache;
    std::vector<std::pair<LoadInst *, Value *>> Replacements;

    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            auto *Load = dynamic_cast<LoadInst *>(I);
            if (!Load) continue;
            Value *Pointer = Load->getPointerOperand();

            Variable *Root = nullptr;
            std::vector<int> Path;
            std::shared_ptr<IRType> ElementTy;
            if (traceConstantGlobalPointer(Pointer, Root, Path, ElementTy)) {
                auto CacheIt = ReadOnlyCache.find(Root);
                bool ReadOnly = CacheIt != ReadOnlyCache.end()
                                    ? CacheIt->second
                                    : (ReadOnlyCache[Root] =
                                           isWholeProgramReadOnlyVariable(Root));
                if (!ReadOnly) continue;
                Value *Constant = Root->getConstantElement(Path);
                if (Constant && Value::hasSameStorageType(Load, Constant))
                    Replacements.push_back({Load, Constant});
                continue;
            }

            // Recognize a compact immutable power-of-two table.  A proven
            // in-range dynamic lookup is exactly `1 << index`, which removes
            // both the address calculation and the memory dependence without
            // building an expensive select chain.  This is shape-driven, not
            // tied to a source-level variable name such as SHIFT_TABLE.
            auto *GEP = dynamic_cast<GetElementPtrInst *>(Pointer);
            auto *CandidateRoot = GEP && GEP->getBasePointer()
                                      ? dynamic_cast<Variable *>(
                                            GEP->getBasePointer())
                                      : nullptr;
            if (!GEP || !CandidateRoot || GEP->getNumIndices() != 2)
                continue;
            int Zero = 0;
            if (!Value::tryGetIntIndex(GEP->getIndex(0), Zero) || Zero != 0)
                continue;
            Value *Index = GEP->getIndex(1);
            if (!Index || dynamic_cast<ConstInt *>(Index)) continue;

            auto ArrayTy = std::dynamic_pointer_cast<ARRAYType>(
                CandidateRoot->getPointeeType());
            int Count = ArrayTy ? ArrayTy->getElementCount() : 0;
            if (!ArrayTy || Count <= 0 || Count > 31 ||
                !IRType::isScalar(ArrayTy->getElementType()))
                continue;
            auto CacheIt = ReadOnlyCache.find(CandidateRoot);
            bool ReadOnly = CacheIt != ReadOnlyCache.end()
                                ? CacheIt->second
                                : (ReadOnlyCache[CandidateRoot] =
                                       isWholeProgramReadOnlyVariable(
                                           CandidateRoot));
            if (!ReadOnly) continue;

            bool IsPowerTable = true;
            for (int N = 0; N < Count; ++N) {
                auto *C = dynamic_cast<ConstInt *>(
                    CandidateRoot->getConstantElement({N}));
                if (!C || static_cast<uint32_t>(C->getVal()) !=
                              (uint32_t{1} << N)) {
                    IsPowerTable = false;
                    break;
                }
            }
            if (!IsPowerTable) continue;

            IntRange IndexRange = RI.query(Index, BB, DT);
            if (IndexRange.isBottom() || IndexRange.min < 0 ||
                IndexRange.max >= Count)
                continue;
            Value *One = Value::makeIntegerLike(Index, 1);
            if (!One || !Value::hasSameStorageType(Load, One)) continue;
            auto *Shift = new BinaryInst(One, BinaryInst::SHL, Index);
            if (!Load->insertBefore(Shift)) {
                delete Shift;
                continue;
            }
            Replacements.push_back({Load, Shift});
        }
    }

    for (auto &Replacement : Replacements) {
        LoadInst *Load = Replacement.first;
        if (!Load || !Load->getParent()) continue;
        Value *Pointer = Load->getPointerOperand();
        Load->replaceAllUsesWith(Replacement.second);
        delete Load;
        deleteDeadPointerChain(Pointer);
    }
    return !Replacements.empty();
}

LocalAggregatePointerPath LocalAggregateSimplifyPass::tracePointer(
    Value *V, std::unordered_set<Value *> &Visiting) {
    LocalAggregatePointerPath Result;
    if (!V || !Visiting.insert(V).second) return Result;

    if (auto *AI = dynamic_cast<AllocaInst *>(V)) {
        Result.Root = AI;
        Visiting.erase(V);
        return Result;
    }

    if (auto *BC = dynamic_cast<BitCastInst *>(V)) {
        if (Value *Operand = BC->getUnaryOperand())
            Result = tracePointer(Operand, Visiting);
        Visiting.erase(V);
        return Result;
    }

    auto *GEP = dynamic_cast<GetElementPtrInst *>(V);
    if (!GEP) {
        Visiting.erase(V);
        return Result;
    }

    if (GEP->getNumIndices() < 1 || !GEP->getBasePointer()) {
        Visiting.erase(V);
        return Result;
    }

    Result = tracePointer(GEP->getBasePointer(), Visiting);
    if (!Result.Root) {
        Visiting.erase(V);
        return Result;
    }

    int FirstIndex = 0;
    if (!Value::tryGetIntIndex(GEP->getIndex(0), FirstIndex) ||
        FirstIndex != 0) {
        Result.IsConstant = false;
        Visiting.erase(V);
        return Result;
    }

    for (size_t I = 1; I < GEP->getNumIndices(); ++I) {
        int Index = 0;
        if (!Value::tryGetIntIndex(GEP->getIndex(I), Index)) {
            Result.IsConstant = false;
            break;
        }
        Result.Indices.push_back(Index);
    }

    Visiting.erase(V);
    return Result;
}

LocalAggregatePointerPath LocalAggregateSimplifyPass::tracePointer(Value *V) {
    std::unordered_set<Value *> Visiting;
    return tracePointer(V, Visiting);
}

bool LocalAggregateSimplifyPass::getDirectConstantPath(
    AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
    std::shared_ptr<IRType> &PointeeTy) {
    Path.clear();
    PointeeTy = nullptr;
    if (!AI || !GEP) return false;

    if (GEP->getNumIndices() < 1 || GEP->getBasePointer() != AI)
        return false;

    int FirstIndex = 0;
    if (!Value::tryGetIntIndex(GEP->getIndex(0), FirstIndex) ||
        FirstIndex != 0)
        return false;

    std::shared_ptr<IRType> Ty = AllocaInst::getAllocatedType(AI);
    for (size_t I = 1; I < GEP->getNumIndices(); ++I) {
        int Index = 0;
        if (!Value::tryGetIntIndex(GEP->getIndex(I), Index))
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

bool LocalAggregateSimplifyPass::getDirectConstantScalarPath(
    AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
    std::shared_ptr<IRType> &ElementTy) {
    if (!getDirectConstantPath(AI, GEP, Path, ElementTy))
        return false;
    if (!IRType::isScalar(ElementTy)) return false;
    return true;
}

bool LocalAggregateSimplifyPass::appendConstantAggregateGEPPath(
    Value *Base, GetElementPtrInst *GEP, const std::vector<int> &BasePath,
    std::vector<int> &NewPath, std::shared_ptr<IRType> &PointeeTy) {
    NewPath.clear();
    PointeeTy = nullptr;
    if (!Base || !GEP) return false;

    if (GEP->getNumIndices() < 2 || GEP->getBasePointer() != Base)
        return false;

    int FirstIndex = 0;
    if (!Value::tryGetIntIndex(GEP->getIndex(0), FirstIndex) ||
        FirstIndex != 0)
        return false;

    std::shared_ptr<IRType> Ty = Value::getPointeeType(Base);
    if (!Ty || Ty->getEnum() != IR_ARRAY) return false;

    NewPath = BasePath;
    for (size_t I = 1; I < GEP->getNumIndices(); ++I) {
        int Index = 0;
        if (!Value::tryGetIntIndex(GEP->getIndex(I), Index))
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

bool LocalAggregateSimplifyPass::getDirectOneDimElementIndex(
    AllocaInst *AI, GetElementPtrInst *GEP, Value *&Index) {
    Index = nullptr;
    if (!AI || !GEP) return false;
    auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(AllocaInst::getAllocatedType(AI));
    if (!ArrTy || !IRType::isScalar(ArrTy->getElementType())) return false;

    if (GEP->getNumIndices() != 2 || GEP->getBasePointer() != AI)
        return false;

    int FirstIndex = 0;
    if (!Value::tryGetIntIndex(GEP->getIndex(0), FirstIndex) ||
        FirstIndex != 0)
        return false;

    Index = GEP->getIndex(1);
    return Index && Index->getIRType() &&
           Index->getIRType()->getEnum() == IR_INT;
}

bool LocalAggregateSimplifyPass::getConstantOffsetScalarGEP(
    Value *Base, GetElementPtrInst *GEP, int &Offset) {
    Offset = 0;
    if (!Base || !GEP) return false;
    if (GEP->getNumIndices() != 1 || GEP->getBasePointer() != Base)
        return false;

    if (!Value::tryGetIntIndex(GEP->getIndex(0), Offset))
        return false;

    auto BasePtr = std::dynamic_pointer_cast<POINTERType>(Base->getIRType());
    auto GEPPtr = std::dynamic_pointer_cast<POINTERType>(GEP->getIRType());
    return BasePtr && GEPPtr && BasePtr->getPointerType() &&
           GEPPtr->getPointerType() &&
           IRType::isScalar(BasePtr->getPointerType()) &&
           IRType::isScalar(GEPPtr->getPointerType()) &&
           BasePtr->getPointerType()->hasSameScalarType(
               *GEPPtr->getPointerType());
}

bool LocalAggregateSimplifyPass::isProvablyReadOnlySource(Variable *Source) {
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
        if (!CI || !CI->isNonVolatileMemcpy() || CI->getArgIndex(U) != 1)
            return false;
    }
    return true;
}

bool LocalAggregateSimplifyPass::isWholeConstantCopy(
    CallInst *CI, AllocaInst *Destination, Variable *Source) {
    if (!CI || !Destination || !Source ||
        !isProvablyReadOnlySource(Source) ||
        !CI->isNonVolatileMemcpy())
        return false;

    auto *ByteCount = dynamic_cast<ConstInt *>(CI->getArg(2));
    auto DestPtr =
        std::dynamic_pointer_cast<POINTERType>(Destination->getIRType());
    auto SourcePtr =
        std::dynamic_pointer_cast<POINTERType>(Source->getIRType());
    if (!ByteCount || !DestPtr || !SourcePtr) return false;

    auto DestTy = DestPtr->getPointerType();
    auto SourceTy = SourcePtr->getPointerType();
    return ByteCount->getVal() >= 0 &&
           static_cast<size_t>(ByteCount->getVal()) == DestTy->getSize() &&
           IRType::hasSameShape(DestTy, SourceTy);
}

// Constant forwarding is intentionally restricted to one block. Validate the
// whole pointer-use graph first so an unmodelled use cannot silently alias it.
bool LocalAggregateSimplifyPass::hasSupportedSameBlockUses(
    AllocaInst *AI, BasicBlock *BB) {
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
                CI && CallInst::isNonVolatileMemcpy(CI) && OperandIndex == 1)
                continue;
            return false;
        }
    }
    return true;
}

bool LocalAggregateSimplifyPass::forwardConstantLoads(AllocaInst *AI) {
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
            if (!CallInst::isMemcpyLike(CI) || CI->getArgCount() < 2) continue;

            LocalAggregatePointerPath Dest = tracePointer(CI->getArg(0));
            if (Dest.Root != AI) continue;

            auto *Source = dynamic_cast<Variable *>(CI->getArg(1));
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
            LocalAggregatePointerPath Dest = tracePointer(SI->getPointerOperand());
            if (Dest.Root != AI) continue;

            if (!StateValid || !Dest.IsConstant || Dest.Indices.empty()) {
                StateValid = false;
                InitialValue = nullptr;
                Overwrites.clear();
                continue;
            }
            Overwrites[Dest.Indices] = SI->getValueOperand();
            continue;
        }

        auto *LI = dynamic_cast<LoadInst *>(I);
        if (!LI) continue;

        LocalAggregatePointerPath SourcePath = tracePointer(LI->getPointerOperand());
        if (SourcePath.Root != AI || !StateValid ||
            !SourcePath.IsConstant || SourcePath.Indices.empty())
            continue;

        Value *Replacement = nullptr;
        auto OverwriteIt = Overwrites.find(SourcePath.Indices);
        if (OverwriteIt != Overwrites.end())
            Replacement = OverwriteIt->second;
        else
            Replacement = Variable::getConstantElement(InitialValue,
                                             SourcePath.Indices);

        if (!Value::hasSameStorageType(LI, Replacement)) continue;
        LI->replaceAllUsesWith(Replacement);
        DeadLoads.push_back(LI);
        Changed = true;
    }

    for (LoadInst *LI : DeadLoads)
        delete LI;
    return Changed;
}

int LocalAggregateSimplifyPass::getOneDimScalarArrayCount(AllocaInst *AI) {
    auto ArrTy = std::dynamic_pointer_cast<ARRAYType>(AllocaInst::getAllocatedType(AI));
    if (!ArrTy || !IRType::isScalar(ArrTy->getElementType()))
        return -1;
    return ArrTy->getElementCount();
}

bool LocalAggregateSimplifyPass::indexTypeCanSelectAll(
    Value *Index, int Count) {
    if (!Index || !Index->getIRType() ||
        Index->getIRType()->getEnum() != IR_INT)
        return false;
    auto IntTy = std::dynamic_pointer_cast<INTType>(Index->getIRType());
    return !IntTy || !IntTy->getIsBool() || Count <= 2;
}

bool LocalAggregateSimplifyPass::collectReadOnlyConstantArrayUses(
    Function &F, AllocaInst *AI,
    LocalAggregateReadOnlyConstantArrayPlan &Plan, int Count) {
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
            CI && OperandIndex == 1 && CallInst::isNonVolatileMemcpy(CI)) {
            if (CI->getArgCount() < 2 || CI->getArg(0) != AI ||
                Plan.InitCopy)
                return false;

            auto *Source = dynamic_cast<Variable *>(CI->getArg(1));
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
            if (!Instruction::comesBefore(Plan.InitCopy, LI)) return false;
        } else if (InitBB != Entry) {
            return false;
        }
    }

    return true;
}

Value *LocalAggregateSimplifyPass::buildConstantArrayLookup(
    Variable *Source, int Count, Value *Index, Instruction *InsertBefore) {
    int ConstIndex = 0;
    if (Value::tryGetIntIndex(Index, ConstIndex)) {
        if (ConstIndex < 0 || ConstIndex >= Count) return nullptr;
        return Variable::getConstantElement(Source, {ConstIndex});
    }

    std::vector<Value *> Elements;
    Elements.reserve(static_cast<size_t>(Count));
    for (int I = 0; I < Count; ++I) {
        Value *Elem = Variable::getConstantElement(Source, {I});
        if (!Elem) return nullptr;
        Elements.push_back(Elem);
    }

    bool AllSame = true;
    for (size_t I = 1; I < Elements.size(); ++I) {
        if (!sameScalarConstant(Elements[0], Elements[I])) {
            AllSame = false;
            break;
        }
    }
    if (AllSame) return Elements[0];

    Value *Result = Elements[0];
    for (int I = 1; I < Count; ++I) {
        Value *Needle = Value::makeIntegerLike(Index, I);
        if (!Needle) return nullptr;
        auto *Cmp = new BinaryInst(Index, BinaryInst::E, Needle);
        if (!InsertBefore->insertBefore(Cmp)) {
            delete Cmp;
            return nullptr;
        }
        auto *Sel = new SelectInst(Cmp, Elements[static_cast<size_t>(I)],
                                   Result);
        if (!InsertBefore->insertBefore(Sel)) {
            delete Sel;
            return nullptr;
        }
        Result = Sel;
    }
    return Result;
}

bool LocalAggregateSimplifyPass::expandReadOnlyConstantArrayLoads(
    Function &F, AllocaInst *AI) {
    constexpr int kMaxDynamicSelectArrayElements = 2;

    int Count = getOneDimScalarArrayCount(AI);
    if (Count <= 0) return false;

    LocalAggregateReadOnlyConstantArrayPlan Plan;
    if (!collectReadOnlyConstantArrayUses(F, AI, Plan, Count))
        return false;

    Value *FirstElement = Variable::getConstantElement(Plan.Source, {0});
    if (!FirstElement) return false;

    bool HasDynamicIndex = false;
    for (LoadInst *LI : Plan.Loads) {
        if (!Value::hasSameStorageType(LI, FirstElement)) return false;
        auto *GEP = dynamic_cast<GetElementPtrInst *>(LI->getPointerOperand());
        Value *Index = nullptr;
        if (!getDirectOneDimElementIndex(AI, GEP, Index) ||
            !indexTypeCanSelectAll(Index, Count))
            return false;

        int ConstIndex = 0;
        if (Value::tryGetIntIndex(Index, ConstIndex)) {
            if (ConstIndex < 0 || ConstIndex >= Count) return false;
        } else {
            HasDynamicIndex = true;
        }
    }
    if (HasDynamicIndex && Count > kMaxDynamicSelectArrayElements) {
        Value *First = Variable::getConstantElement(Plan.Source, {0});
        bool Uniform = First != nullptr;
        for (int I = 1; Uniform && I < Count; ++I)
            Uniform = sameScalarConstant(
                First, Variable::getConstantElement(Plan.Source, {I}));
        if (!Uniform) return false;
    }

    std::vector<std::pair<LoadInst *, Value *>> Replacements;
    Replacements.reserve(Plan.Loads.size());
    for (LoadInst *LI : Plan.Loads) {
        auto *GEP = dynamic_cast<GetElementPtrInst *>(LI->getPointerOperand());
        Value *Index = nullptr;
        if (!getDirectOneDimElementIndex(AI, GEP, Index)) return false;

        Value *Replacement =
            buildConstantArrayLookup(Plan.Source, Count, Index, LI);
        if (!Value::hasSameStorageType(LI, Replacement)) return false;
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

bool LocalAggregateSimplifyPass::addScalarSlot(
    std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
    const std::vector<int> &Path,
    const std::shared_ptr<IRType> &ElementTy,
    int MaxScalarizedElements) {
    auto SlotIt = SlotTypes.find(Path);
    if (SlotIt != SlotTypes.end())
        return IRType::hasSameScalarType(SlotIt->second, ElementTy);

    SlotTypes.emplace(Path, ElementTy);
    return static_cast<int>(SlotTypes.size()) <= MaxScalarizedElements;
}

bool LocalAggregateSimplifyPass::collectScalarizedPointerUses(
    AllocaInst *AI, Value *Ptr, const std::vector<int> &PtrPath,
    std::vector<GetElementPtrInst *> &TransparentPtrs,
    std::map<GetElementPtrInst *, std::vector<int>> &TransparentPath,
    std::set<GetElementPtrInst *> &SeenTransparent,
    std::vector<Instruction *> &MemoryAccesses,
    std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
    int MaxScalarizedElements) {
    auto PtrPointeeTy = Value::getPointeeType(Ptr);
    bool PtrIsScalar = IRType::isScalar(PtrPointeeTy);

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
            if (!IRType::flattenScalarPath(AllocaInst::getAllocatedType(AI), PtrPath,
                                   FlatIndex))
                return false;

            std::vector<int> NewPath;
            std::shared_ptr<IRType> NewElementTy;
            if (!IRType::unflattenScalarPath(AllocaInst::getAllocatedType(AI),
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
            if (IRType::isScalar(NewPointeeTy) &&
                !addScalarSlot(SlotTypes, NewPath, NewPointeeTy,
                               MaxScalarizedElements))
                return false;
            if (!IRType::isScalar(NewPointeeTy) &&
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

bool LocalAggregateSimplifyPass::scalarizeConstantIndexedAggregate(
    AllocaInst *AI) {
    constexpr int kMaxScalarizedElements = 32;
    if (!AI || !AllocaInst::isArrayAllocation(AI)) return false;
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
            CI && OperandIndex == 1 && CallInst::isNonVolatileMemcpy(CI)) {
            if (CI->getArgCount() < 2 || CI->getArg(0) != AI || InitCopy)
                return Fail("bad memcpy operands");
            auto *Source = dynamic_cast<Variable *>(CI->getArg(1));
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

        if (!IRType::isScalar(PointeeTy) && PointeeTy->getEnum() != IR_ARRAY)
            return Fail("bad pointee ty");
        if (IRType::isScalar(PointeeTy) &&
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
                if (!Instruction::comesBefore(InitCopy, Access))
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
            Value *Init = Variable::getConstantElement(InitSource, Entry.first);
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
        if (!AI->insertBefore(Slot)) {
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
            if (!InitInsertPoint->insertBefore(Store)) {
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

LocalAggregateDeadUses LocalAggregateSimplifyPass::collectDeadAggregateUses(
    AllocaInst *AI) {
    LocalAggregateDeadUses Result;
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
                CI && CallInst::isNonVolatileMemcpy(CI) && OperandIndex == 1) {
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

bool LocalAggregateSimplifyPass::deleteDeadAggregate(AllocaInst *AI) {
    LocalAggregateDeadUses Uses = collectDeadAggregateUses(AI);
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

std::vector<AllocaInst *>
LocalAggregateSimplifyPass::collectArrayAllocas(Function &F) {
    std::vector<AllocaInst *> Aggregates;
    for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt) {
        BasicBlock *BB = *BBIt;
        for (auto IIt = BB->begin(); IIt != BB->end(); ++IIt) {
            if (auto *AI = dynamic_cast<AllocaInst *>(*IIt);
                AllocaInst::isArrayAllocation(AI))
                Aggregates.push_back(AI);
        }
    }
    return Aggregates;
}

PreservationStatus
LocalAggregateSimplifyPass::run(Function &F,
                                FunctionAnalysisPassManager &FAM) {
    bool Changed = false;

    Changed |= foldReadOnlyGlobalLoads(F, FAM);
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
