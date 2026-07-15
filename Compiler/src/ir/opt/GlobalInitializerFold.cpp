#include "ir/opt/GlobalInitializerFold.hpp"

#include <cstdint>
#include <limits>
#include <unordered_set>
#include <vector>

bool GlobalInitializerFoldPass::isRecursiveMain(Module &M, Function *Main) {
    if (!Main) return true;
    for (CallInst *CI : M.collectInstructions<CallInst>())
        if (CI->getDirectCallee() == Main) return true;
    return false;
}

Variable *GlobalInitializerFoldPass::getWholeObjectGlobal(Value *Pointer) {
    std::unordered_set<Value *> Seen;
    while (Pointer && Seen.insert(Pointer).second) {
        if (auto *BC = dynamic_cast<BitCastInst *>(Pointer)) {
            Pointer = BC->getUnaryOperand();
            continue;
        }
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Pointer)) {
            for (Value *Index : GEP->getIndices()) {
                int ConstantIndex = 0;
                if (!Value::tryGetIntIndex(Index, ConstantIndex) ||
                    ConstantIndex != 0)
                    return nullptr;
            }
            Pointer = GEP->getBasePointer();
            continue;
        }
        break;
    }
    return dynamic_cast<Variable *>(Pointer);
}

bool GlobalInitializerFoldPass::isFullGlobalCopy(
    CallInst *CI, Variable *&Destination, Variable *&Source) {
    Destination = nullptr;
    Source = nullptr;
    if (!CI || !CI->isNonVolatileMemcpy() || CI->getArgCount() < 4)
        return false;

    Destination = getWholeObjectGlobal(CI->getArg(0));
    Source = getWholeObjectGlobal(CI->getArg(1));
    auto *ByteCount = dynamic_cast<ConstInt *>(CI->getArg(2));
    if (!Destination || !Source || !ByteCount || Destination == Source)
        return false;
    if (Destination->vartag != Variable::GlobalVariable)
        return false;
    if (Source->vartag != Variable::GlobalVariable &&
        Source->vartag != Variable::Constant)
        return false;

    auto DestTy = Destination->getPointeeType();
    auto SourceTy = Source->getPointeeType();
    if (!DestTy || !SourceTy || !DestTy->hasSameShape(*SourceTy) ||
        DestTy->getSize() != SourceTy->getSize())
        return false;
    if (DestTy->getSize() >
        static_cast<size_t>(std::numeric_limits<int32_t>::max()))
        return false;
    return ByteCount->getVal() >= 0 &&
           static_cast<size_t>(ByteCount->getVal()) == DestTy->getSize();
}

void GlobalInitializerFoldPass::copyInitializer(Variable *Destination,
                                                 Variable *Source) {
    if (!Destination || !Source) return;
    Value *SourceInit = Source->getUsee();
    if (!SourceInit) {
        // An absent initializer is this IR's canonical zeroinitializer.
        Destination->clearOperands();
        return;
    }
    if (Destination->getNumOperands() == 0)
        Destination->addUse(SourceInit);
    else
        Destination->replaceOperand(0, SourceInit);
}

PreservationStatus GlobalInitializerFoldPass::run(
    Module &M, ModuleAnalysisPassManager &MAM) {
    (void)MAM;
    Function *Main = M.getMainFunction();
    if (!Main || Main->Size() == 0 || isRecursiveMain(M, Main))
        return PreservationStatus::all();

    BasicBlock *Entry = Main->front();
    if (!Entry) return PreservationStatus::all();

    // A failed/unknown memory operation makes every later copy ineligible:
    // it may have observed the old destination initializer or changed the
    // source.  Arithmetic, alloca, GEP, and casts are harmless in the prefix.
    bool SafePrefix = true;
    std::vector<CallInst *> FoldedCalls;
    for (Instruction *I : *Entry) {
        if (!I) continue;
        if (auto *CI = dynamic_cast<CallInst *>(I)) {
            Variable *Destination = nullptr;
            Variable *Source = nullptr;
            if (SafePrefix &&
                isFullGlobalCopy(CI, Destination, Source)) {
                copyInitializer(Destination, Source);
                FoldedCalls.push_back(CI);
                continue;
            }
            SafePrefix = false;
            continue;
        }
        if (dynamic_cast<LoadInst *>(I) || dynamic_cast<StoreInst *>(I))
            SafePrefix = false;
    }

    if (FoldedCalls.empty()) return PreservationStatus::all();
    for (CallInst *CI : FoldedCalls) delete CI;
    return PreservationStatus::none();
}
