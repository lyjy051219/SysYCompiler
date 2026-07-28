#include "ir/opt/FunctionMemoization.hpp"

#include "ir/analysis/SideEffect.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

FunctionMemoizationPass::FunctionMemoizationPass(
    FunctionAnalysisPassManager &FAM, unsigned CacheEntries)
    : FAM(FAM),
      CacheEntries(isPowerOfTwo(CacheEntries) && CacheEntries >= 16
                       ? CacheEntries
                       : DefaultCacheEntries) {}

bool FunctionMemoizationPass::isPowerOfTwo(unsigned Value) {
    return Value != 0 && (Value & (Value - 1)) == 0;
}

bool FunctionMemoizationPass::isSupportedScalarType(
    const std::shared_ptr<IRType> &Type) {
    if (!Type) return false;
    if (Type->isFloat()) return Type->getSize() == 4;
    return Type->hasIntegerBitWidth(32);
}

bool FunctionMemoizationPass::hasUnsafeOrUndefinedOperation(
    Instruction *Inst) {
    if (!Inst) return true;
    for (const auto &OperandUse : Inst->getUserList()) {
        Value *Operand = OperandUse ? OperandUse->getValue() : nullptr;
        if (!Operand || Operand->isUndef()) return true;
    }

    if (Inst->instType == Instruction::BinaryUnknown) return true;
    if (Inst->instType == Instruction::FPTOSI) return true;
    if (Inst->instType == Instruction::BITCAST) {
        Value *Source = Inst->getUnaryOperand();
        // 记忆化缓存只编码精确的32位标量，不能用存储字节数代替真实类型。
        return !Source ||
               !isSupportedScalarType(Source->getIRType()) ||
               !isSupportedScalarType(Inst->getIRType());
    }

    auto *Binary = dynamic_cast<BinaryInst *>(Inst);
    if (!Binary) return false;
    Value *Left = nullptr;
    Value *Right = nullptr;
    if (!Binary->getOperands(Left, Right) || !Left || !Right) return true;

    if (Inst->instType == Instruction::SDIV ||
        Inst->instType == Instruction::SREM) {
        int32_t Divisor = 0;
        return !Right->tryGetIntConst(Divisor) ||
               Divisor == 0 || Divisor == -1;
    }
    if (Inst->instType == Instruction::UDIV ||
        Inst->instType == Instruction::UREM) {
        int32_t Divisor = 0;
        return !Right->tryGetIntConst(Divisor) || Divisor == 0;
    }
    if (Inst->instType == Instruction::SHL ||
        Inst->instType == Instruction::LSHR ||
        Inst->instType == Instruction::ASHR) {
        int32_t Amount = 0;
        int BitWidth = Left->integerBitWidth(32);
        return !Right->tryGetIntConst(Amount) ||
               Amount < 0 || Amount >= BitWidth;
    }
    return false;
}

bool FunctionMemoizationPass::collectCandidate(
    Function &F, FunctionAnalysisPassManager &FAM,
    FunctionMemoizationCandidate &Candidate) {
    Candidate = FunctionMemoizationCandidate{};
    if (F.hasReservedSignature() || !F.front() ||
        !F.hasOnlyDirectCallUses() ||
        F.getParamCount() == 0 ||
        F.getParamCount() > MaxScalarParameters ||
        !isSupportedScalarType(F.getIRType())) {
        return false;
    }

    // 计时调用及其包装函数绝不能缓存化；显式调用图门禁不依赖
    // 副作用摘要的构建顺序，防止未来分析调整削弱这条约束。
    if (F.mayReachTimingCall()) return false;

    BasicBlock *OriginalEntry = F.front();
    if (!OriginalEntry || !OriginalEntry->getPredecessors().empty() ||
        !OriginalEntry->collectPhiNodes().empty()) {
        return false;
    }

    for (const auto &Parameter : F.getParams()) {
        if (!Parameter ||
            !isSupportedScalarType(Parameter->getIRType())) {
            return false;
        }
    }

    const SideEffectInfo &Effects =
        FAM.getResult<SideEffectAnalysis>(F);
    if (Effects.hasBuiltin ||
        getFunctionModRefInfo(Effects) != ModRefInfo::NoModRef) {
        return false;
    }

    std::unordered_set<BasicBlock *> Reachable;
    F.collectReachableBlocks(Reachable);
    if (Reachable.size() != static_cast<std::size_t>(F.size())) return false;
    for (BasicBlock *Block : F) {
        if (!Block || !Reachable.count(Block)) continue;
        for (Instruction *Inst : *Block) {
            if (hasUnsafeOrUndefinedOperation(Inst)) return false;

            // 首版只处理纯 SSA 标量函数，避免把局部内存中的未初始化状态缓存化。
            if (dynamic_cast<AllocaInst *>(Inst) ||
                dynamic_cast<LoadInst *>(Inst) ||
                dynamic_cast<StoreInst *>(Inst) ||
                dynamic_cast<GetElementPtrInst *>(Inst)) {
                return false;
            }
            if (Inst->getIRType() && Inst->getIRType()->isPointer()) {
                return false;
            }

            if (auto *Call = dynamic_cast<CallInst *>(Inst)) {
                if (Call->getDirectCallee() != &F ||
                    Call->getArgCount() != F.getParamCount()) {
                    return false;
                }
                for (unsigned Index = 0; Index < Call->getArgCount(); ++Index) {
                    Value *Argument = Call->getArg(Index);
                    if (!Argument ||
                        !isSupportedScalarType(Argument->getIRType())) {
                        return false;
                    }
                }
                Candidate.RecursiveCalls.push_back(Call);
                continue;
            }

            if (auto *Return = dynamic_cast<RetInst *>(Inst)) {
                Value *Result = Return->getReturnValue();
                if (!Result || Result->isUndef() ||
                    !isSupportedScalarType(Result->getIRType()) ||
                    !IRType::hasSameScalarType(
                        Result->getIRType(), F.getIRType())) {
                    return false;
                }
                Candidate.Returns.push_back(Return);
            }
        }
    }

    if (Candidate.RecursiveCalls.size() < 2 ||
        Candidate.Returns.empty()) {
        Candidate = FunctionMemoizationCandidate{};
        return false;
    }

    bool HasExternalCallSite = false;
    for (CallInst *Call : F.getDirectCallSites()) {
        if (!Call || !Call->getParentFunction()) return false;
        if (Call->getParentFunction() != &F) HasExternalCallSite = true;
        for (unsigned Index = 0; Index < Call->getArgCount(); ++Index) {
            Value *Argument = Call->getArg(Index);
            if (!Argument || Argument->isUndef()) return false;
        }
    }
    if (!HasExternalCallSite) return false;

    Candidate.Target = &F;
    return true;
}

std::string FunctionMemoizationPass::makeUniqueGlobalName(
    Module &M, const std::string &Stem) {
    auto IsUsed = [&](const std::string &Name) {
        const std::string IRName = ".G." + Name;
        for (const auto &Global : M.getGlobalVar()) {
            if (Global && Global->getIdent() == IRName) return true;
        }
        for (const auto &FunctionPtr : M.getFunctions()) {
            if (FunctionPtr && FunctionPtr->getIdent() == IRName) return true;
        }
        return false;
    };

    if (!IsUsed(Stem)) return Stem;
    for (unsigned Suffix = 1;; ++Suffix) {
        std::string Candidate = Stem + "." + std::to_string(Suffix);
        if (!IsUsed(Candidate)) return Candidate;
    }
}

GetElementPtrInst *FunctionMemoizationPass::appendSlotPointer(
    BasicBlock &Block, Variable *Cache, Value *Index) {
    if (!Cache || !Index) return nullptr;
    std::vector<Value *> Indices{
        ConstInt::newConstInt(0), Index};
    auto *Pointer = new GetElementPtrInst(Cache, Indices);
    Pointer->updateType();
    Block.pushBack(Pointer);
    return Pointer;
}

Value *FunctionMemoizationPass::appendKeyBits(
    BasicBlock &Block, Value *Argument) {
    if (!Argument || !Argument->getIRType()) return nullptr;
    if (Argument->getIRType()->isInt()) return Argument;
    if (!Argument->getIRType()->isFloat()) return nullptr;

    // 浮点键按位解释，避免把正负零或不同的 NaN 位型合并。
    auto *Bits = new BitCastInst(Argument, INTType::getInt32Ty());
    Block.pushBack(Bits);
    return Bits;
}

Value *FunctionMemoizationPass::appendHashMix(
    BasicBlock &Block, Value *Hash, Value *KeyBits) {
    if (!Hash || !KeyBits) return nullptr;
    auto *Xor = new BinaryInst(Hash, BinaryInst::XOR, KeyBits);
    Block.pushBack(Xor);
    auto *Mixed = new BinaryInst(
        Xor, BinaryInst::MUL, ConstInt::newConstInt(16777619));
    Block.pushBack(Mixed);
    return Mixed;
}

bool FunctionMemoizationPass::transformCandidate(
    Module &M, const FunctionMemoizationCandidate &Candidate,
    unsigned CacheOrdinal) const {
    Function *F = Candidate.Target;
    if (!F || Module::getActiveModule() != &M || !F->front() ||
        Candidate.Returns.empty()) {
        return false;
    }

    BasicBlock *OriginalEntry = F->front();
    auto *LookupBlock = new BasicBlock();
    auto *HitBlock = new BasicBlock();
    LookupBlock->setIdent(F->makeUniqueBlockIdent(".memo.lookup"));
    HitBlock->setIdent(F->makeUniqueBlockIdent(".memo.hit"));

    std::vector<Value *> KeyBits;
    KeyBits.reserve(F->getParamCount());
    Value *Hash = ConstInt::newConstInt(
        static_cast<int32_t>(2166136261u));
    for (unsigned Index = 0; Index < F->getParamCount(); ++Index) {
        Value *Bits = appendKeyBits(*LookupBlock, F->getParam(Index));
        if (!Bits) {
            delete LookupBlock;
            delete HitBlock;
            return false;
        }
        KeyBits.push_back(Bits);
        Hash = appendHashMix(*LookupBlock, Hash, Bits);
        if (!Hash) {
            delete LookupBlock;
            delete HitBlock;
            return false;
        }
    }

    auto *SlotIndex = new BinaryInst(
        Hash, BinaryInst::AND,
        ConstInt::newConstInt(static_cast<int>(CacheEntries - 1)));
    LookupBlock->pushBack(SlotIndex);

    std::string Prefix =
        ".__sysy_memo." + std::to_string(CacheOrdinal);
    auto ValidType = ARRAYType::NewArray(
        INTType::getInt32Ty(), static_cast<int>(CacheEntries));
    auto KeyType = ARRAYType::NewArray(
        INTType::getInt32Ty(), static_cast<int>(CacheEntries));
    auto ResultType = ARRAYType::NewArray(
        F->getIRType(), static_cast<int>(CacheEntries));

    auto *ValidCache = new Variable(
        Variable::GlobalVariable, ValidType,
        makeUniqueGlobalName(M, Prefix + ".valid"));
    std::vector<Variable *> KeyCaches;
    KeyCaches.reserve(F->getParamCount());
    for (unsigned Index = 0; Index < F->getParamCount(); ++Index) {
        KeyCaches.push_back(new Variable(
            Variable::GlobalVariable, KeyType,
            makeUniqueGlobalName(
                M, Prefix + ".key." + std::to_string(Index))));
    }
    auto *ResultCache = new Variable(
        Variable::GlobalVariable, ResultType,
        makeUniqueGlobalName(M, Prefix + ".result"));

    GetElementPtrInst *ValidPointer =
        appendSlotPointer(*LookupBlock, ValidCache, SlotIndex);
    std::vector<GetElementPtrInst *> KeyPointers;
    KeyPointers.reserve(KeyCaches.size());
    for (Variable *KeyCache : KeyCaches) {
        KeyPointers.push_back(
            appendSlotPointer(*LookupBlock, KeyCache, SlotIndex));
    }
    GetElementPtrInst *ResultPointer =
        appendSlotPointer(*LookupBlock, ResultCache, SlotIndex);

    auto *ValidValue = new LoadInst(ValidPointer);
    LookupBlock->pushBack(ValidValue);
    auto *InitialHit = new BinaryInst(
        ValidValue, BinaryInst::E, ConstInt::newConstInt(1));
    LookupBlock->pushBack(InitialHit);
    Value *IsHit = InitialHit;

    for (unsigned Index = 0; Index < KeyPointers.size(); ++Index) {
        // 哈希只负责选槽；命中仍须逐字段比较完整键。
        auto *StoredKey = new LoadInst(KeyPointers[Index]);
        LookupBlock->pushBack(StoredKey);
        auto *KeyMatches = new BinaryInst(
            StoredKey, BinaryInst::E, KeyBits[Index]);
        LookupBlock->pushBack(KeyMatches);
        auto *AllMatch = new BinaryInst(
            IsHit, BinaryInst::AND, KeyMatches);
        LookupBlock->pushBack(AllMatch);
        IsHit = AllMatch;
    }

    LookupBlock->genCondInst(IsHit, HitBlock, OriginalEntry);
    auto *CachedResult = new LoadInst(ResultPointer);
    HitBlock->pushBack(CachedResult);
    HitBlock->pushBack(new RetInst(CachedResult));

    F->pushFront(LookupBlock);
    F->pushBlock(HitBlock);

    for (CallInst *RecursiveCall : Candidate.RecursiveCalls) {
        if (RecursiveCall) RecursiveCall->setTailCall(false);
    }

    for (RetInst *Return : Candidate.Returns) {
        if (!Return || !Return->getParent()) continue;
        Value *Result = Return->getReturnValue();
        if (!Result) continue;

        // 先清除有效位，完整写入精确键和值后再发布缓存项。
        Return->insertBefore(new StoreInst(
            ConstInt::newConstInt(0), ValidPointer));
        for (unsigned Index = 0; Index < KeyPointers.size(); ++Index) {
            Return->insertBefore(new StoreInst(
                KeyBits[Index], KeyPointers[Index]));
        }
        Return->insertBefore(new StoreInst(Result, ResultPointer));
        Return->insertBefore(new StoreInst(
            ConstInt::newConstInt(1), ValidPointer));
    }

    return true;
}

PreservationStatus FunctionMemoizationPass::run(
    Module &M, ModuleAnalysisPassManager &MAM) {
    (void)MAM;
    bool Changed = false;

    std::vector<Function *> Functions;
    Functions.reserve(M.getFunctions().size());
    for (const auto &FunctionPtr : M.getFunctions()) {
        if (FunctionPtr) Functions.push_back(FunctionPtr.get());
    }

    std::vector<FunctionMemoizationCandidate> Candidates;
    for (Function *F : Functions) {
        if (!F) continue;
        FunctionMemoizationCandidate Candidate;
        if (!collectCandidate(*F, FAM, Candidate)) continue;
        Candidates.push_back(std::move(Candidate));
    }

    std::size_t UsedCacheBytes = 0;
    unsigned CacheOrdinal = 0;
    for (const FunctionMemoizationCandidate &Candidate : Candidates) {
        if (!Candidate.Target) continue;
        std::size_t CacheBytes =
            static_cast<std::size_t>(CacheEntries) * sizeof(uint32_t) *
            (Candidate.Target->getParamCount() + 2);
        if (CacheBytes > MaxModuleCacheBytes - UsedCacheBytes) continue;
        if (!transformCandidate(M, Candidate, CacheOrdinal)) continue;
        UsedCacheBytes += CacheBytes;
        ++CacheOrdinal;
        Changed = true;
    }

    if (!Changed) return PreservationStatus::all();
    return PreservationStatus::none();
}
