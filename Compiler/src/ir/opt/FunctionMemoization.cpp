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
    // 1.1 缓存键与结果必须可重复：拒绝 undef、未知 opcode、不可控浮点转整数、
    // 非标量 bitcast、零/危险除数及越界移位。
    if (!Inst) return true;
    for (const auto &OperandUse : Inst->getUserList()) {
        Value *Operand = OperandUse ? OperandUse->getValue() : nullptr;
        if (!Operand || Operand->isUndef()) return true;
    }

    if (Inst->instType == Instruction::BinaryUnknown) return true;
    if (Inst->instType == Instruction::FPTOSI) return true;
    if (Inst->instType == Instruction::BITCAST) {
        Value *Source = Inst->getUnaryOperand();
         
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
    // 1. 先检查签名、直接调用约束、参数个数与标量类型，并排除计时调用和带前驱/PHI
    // 的原入口；这些条件保证可以安全在函数最前面插入 lookup 块。
    Candidate = FunctionMemoizationCandidate{};
    if (F.hasReservedSignature() || !F.front() ||
        !F.hasOnlyDirectCallUses() ||
        F.getParamCount() == 0 ||
        F.getParamCount() > MaxScalarParameters ||
        !isSupportedScalarType(F.getIRType())) {
        return false;
    }

     
     
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

    // 1.2 目标函数必须完全 NoModRef，且所有基本块均可达；逐条指令排除内存、指针、
    // 不确定运算及非自递归调用，同时收集递归调用点和返回点。
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

    // 1.3 至少有两个递归调用才值得缓存，并要求存在函数外调用作为递归计算入口；
    // 所有调用实参都必须已定义，避免把 undef 的任意取值固化进缓存。
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
    // 2.1 同时避开模块中的函数名与全局名，为每组缓存生成稳定且唯一的符号。
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
    // 2.2 整数直接作为 32 位键；float 按位 bitcast，区分不同 NaN 载荷和有符号零。
    if (!Argument || !Argument->getIRType()) return nullptr;
    if (Argument->getIRType()->isInt()) return Argument;
    if (!Argument->getIRType()->isFloat()) return nullptr;

     
    auto *Bits = new BitCastInst(Argument, INTType::getInt32Ty());
    Block.pushBack(Bits);
    return Bits;
}

Value *FunctionMemoizationPass::appendHashMix(
    BasicBlock &Block, Value *Hash, Value *KeyBits) {
    // 2.3 每个参数按 XOR 后乘常量混入哈希，最终用 2 次幂容量掩码选择槽位。
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
    // 2. 离线创建 lookup/hit 块：按参数生成键位、哈希与槽下标；任何构造失败都在
    // 接入函数前删除临时块，因此不会留下部分 CFG。
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

    // 2.4 为有效位、每个参数键和结果分别创建同容量全局数组，并计算当前槽地址。
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

    // 2.5 命中条件要求 valid 且所有存储键逐一相等；命中块读取缓存结果并返回，
    // 未命中边进入原函数入口执行正常递归计算。
    auto *ValidValue = new LoadInst(ValidPointer);
    LookupBlock->pushBack(ValidValue);
    auto *InitialHit = new BinaryInst(
        ValidValue, BinaryInst::E, ConstInt::newConstInt(1));
    LookupBlock->pushBack(InitialHit);
    Value *IsHit = InitialHit;

    for (unsigned Index = 0; Index < KeyPointers.size(); ++Index) {
         
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

    // 2.6 完整 lookup/hit CFG 构造完成后才接入函数，并撤销递归调用的 tail 标记。
    F->pushFront(LookupBlock);
    F->pushBlock(HitBlock);

    for (CallInst *RecursiveCall : Candidate.RecursiveCalls) {
        if (RecursiveCall) RecursiveCall->setTailCall(false);
    }

    // 2.7 每个原返回点按“valid 清零 → 写全部键 → 写结果 → valid 置一”的顺序发布，
    // 防止尚未完整更新的直接映射槽被后续递归命中。
    for (RetInst *Return : Candidate.Returns) {
        if (!Return || !Return->getParent()) continue;
        Value *Result = Return->getReturnValue();
        if (!Result) continue;

         
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

    // 1. 先快照模块函数并完成全部候选验证，避免后续插入缓存 CFG 影响其他候选扫描。
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

    // 2. 按模块总缓存预算依次变换候选；单个缓存大小由参数键数组、valid 数组和
    // 结果数组共同估算，超过剩余预算的函数跳过。
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

    // 3. 新增全局状态和 CFG 后不保留任何模块/函数分析缓存。
    if (!Changed) return PreservationStatus::all();
    return PreservationStatus::none();
}
