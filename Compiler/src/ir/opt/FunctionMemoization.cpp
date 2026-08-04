#include "ir/opt/FunctionMemoization.hpp"
#include "ir/analysis/SideEffect.hpp"
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
//绑定函数级分析管理器并把非法缓存容量回退为默认二次幂。
FunctionMemoizationPass::FunctionMemoizationPass(
    FunctionAnalysisPassManager &FAM, unsigned CacheEntries)
    : FAM(FAM),
      CacheEntries(isPowerOfTwo(CacheEntries) && CacheEntries >= 16
                       ? CacheEntries
                       : DefaultCacheEntries) {}
//判断无符号整数是否为非零二次幂。
bool FunctionMemoizationPass::isPowerOfTwo(unsigned Value) {
    return Value != 0 && (Value & (Value - 1)) == 0;
}
//检查类型是否为缓存支持的i32或f32标量。
bool FunctionMemoizationPass::isSupportedScalarType(
    const std::shared_ptr<IRType> &Type) {
    if (!Type) return false;
    if (Type->isFloat()) return Type->getSize() == 4;
    return Type->hasIntegerBitWidth(32);
}
//检查指令是否包含会使相同键产生不稳定结果的未定义或危险运算。
bool FunctionMemoizationPass::hasUnsafeOrUndefinedOperation(
    Instruction *Inst) {
    //1.1 缓存键与结果必须可重复：拒绝 undef、未知 opcode、不可控浮点转整数、
    //非标量 bitcast、零/危险除数及越界移位。
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
//判断指针是否由副作用分析完整追踪到候选允许读取的全局对象。
bool FunctionMemoizationPass::isTrackedReadGlobalPointer(
    Value *Pointer, const SideEffectInfo &Effects,
    const std::unordered_set<Value *> &AllowedReadGlobals) {
    //1.3 只接受“一个或多个已知全局基址”的来源；未知来源、局部对象、形参地址
    //以及未出现在函数只读摘要中的对象都不能成为记忆化函数的读取状态。
    if (!Pointer || !Pointer->isPointerValue()) return false;
    if (Pointer->isGlobal())
        return AllowedReadGlobals.count(Pointer) != 0;
    auto It = Effects.ptrMap.find(Pointer);
    if (It == Effects.ptrMap.end() || It->second.untracked ||
        It->second.bases.empty()) {
        return false;
    }
    for (Value *Base : It->second.bases) {
        if (!Base || !Base->isGlobal() ||
            !AllowedReadGlobals.count(Base)) {
            return false;
        }
    }
    return true;
}
//验证函数纯度、递归结构与标量语义并建立稳定的候选快照。
bool FunctionMemoizationPass::collectCandidate(
    Function &F, FunctionAnalysisPassManager &FAM,
    FunctionMemoizationCandidate &Candidate) {
    //1.先检查签名、直接调用约束、参数个数、标量类型和原入口形状；
    //函数的外部可观察行为随后由统一副作用摘要排除。
    Candidate = FunctionMemoizationCandidate{};
    if (F.hasReservedSignature() || !F.front() ||
        !F.hasOnlyDirectCallUses() ||
        F.getParamCount() == 0 ||
        F.getParamCount() > MaxScalarParameters ||
        !isSupportedScalarType(F.getIRType())) {
        return false;
    }
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
    //1.2 目标函数至多读取可追踪的全局对象，不能写程序内存、执行I/O、形成顺序
    //屏障或访问未知/跨对象内存。读取全局的函数只在一次顶层递归树内复用缓存。
    const SideEffectInfo &Effects =
        FAM.getResult<SideEffectAnalysis>(F);
    ModRefInfo FunctionEffect = getFunctionModRefInfo(Effects);
    if (Effects.hasBuiltin || Effects.hasUnknownMemory ||
        Effects.hasObservableEffect || Effects.hasOrderingBarrier ||
        Effects.mayCrossObjectBounds || !Effects.ModSet.empty() ||
        (FunctionEffect != ModRefInfo::NoModRef &&
         FunctionEffect != ModRefInfo::Ref)) {
        return false;
    }
    std::unordered_set<Value *> AllowedReadGlobals;
    for (Value *ReadObject : Effects.RefSet) {
        auto *Global = dynamic_cast<Variable *>(ReadObject);
        if (!Global || !Global->isGlobal()) return false;
        AllowedReadGlobals.insert(Global);
        if (Global->vartag == Variable::GlobalVariable)
            Candidate.MutableReadGlobals.push_back(Global);
    }
    std::unordered_set<BasicBlock *> Reachable;
    F.collectReachableBlocks(Reachable);
    if (Reachable.size() != static_cast<std::size_t>(F.size())) return false;
    for (BasicBlock *Block : F) {
        if (!Block || !Reachable.count(Block)) continue;
        for (Instruction *Inst : *Block) {
            if (hasUnsafeOrUndefinedOperation(Inst)) return false;
            if (dynamic_cast<AllocaInst *>(Inst) ||
                dynamic_cast<StoreInst *>(Inst)) {
                return false;
            }
            if (auto *Load = dynamic_cast<LoadInst *>(Inst)) {
                //1.3 每次load都必须能追踪到摘要声明的只读全局；禁止通过指针load
                //把地址带入函数，避免后续访问逃出已证明的对象集合。
                if (Load->isPointerValue() ||
                    !isTrackedReadGlobalPointer(
                        Load->getPointerOperand(), Effects,
                        AllowedReadGlobals)) {
                    return false;
                }
                continue;
            }
            if (auto *GEP = dynamic_cast<GetElementPtrInst *>(Inst)) {
                if (!isTrackedReadGlobalPointer(
                        GEP, Effects, AllowedReadGlobals)) {
                    return false;
                }
                continue;
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
    //1.4 至少有两个递归调用才值得缓存，并要求存在函数外调用作为递归计算入口；
    //所有调用实参都必须已定义，避免把 undef 的任意取值固化进缓存。
    if (Candidate.RecursiveCalls.size() < 2 ||
        Candidate.Returns.empty() ||
        Candidate.Returns.size() > MaxReturnSites) {
        Candidate = FunctionMemoizationCandidate{};
        return false;
    }
    bool HasExternalCallSite = false;
    for (CallInst *Call : F.getDirectCallSites()) {
        if (!Call || !Call->getParentFunction()) return false;
        if (Call->getParentFunction() != &F) {
            HasExternalCallSite = true;
            Candidate.ExternalCalls.push_back(Call);
        }
        for (unsigned Index = 0; Index < Call->getArgCount(); ++Index) {
            Value *Argument = Call->getArg(Index);
            if (!Argument || Argument->isUndef()) return false;
        }
    }
    if (!HasExternalCallSite) return false;
    //1.5 用统一的静态代码增长上限约束查找、返回发布和外部失效逻辑；该模型
    //只依赖IR结构，不读取函数名、输入值或测试用例特征。
    if (Candidate.ExternalCalls.size() > MaxExternalCallSites)
        return false;
    const std::size_t ParameterCount = F.getParamCount();
    const std::size_t EstimatedAddedInstructions =
        7 + 7 * ParameterCount +
        Candidate.Returns.size() * (ParameterCount + 3) +
        (Candidate.MutableReadGlobals.empty()
             ? 0 : 1 + 7 * Candidate.ExternalCalls.size());
    if (EstimatedAddedInstructions > MaxAddedInstructions)
        return false;
    Candidate.Target = &F;
    return true;
}
//生成同时避开模块函数和全局变量名称的唯一缓存符号。
std::string FunctionMemoizationPass::makeUniqueGlobalName(
    Module &M, const std::string &Stem) {
    //2.1 同时避开模块中的函数名与全局名，为每组缓存生成稳定且唯一的符号。
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
//在基本块末尾生成指向缓存数组指定槽位的GEP。
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
//把整数或浮点参数规范化为32位缓存键比特。
Value *FunctionMemoizationPass::appendKeyBits(
    BasicBlock &Block, Value *Argument) {
    //2.2 整数直接作为 32 位键；float 按位 bitcast，区分不同 NaN 载荷和有符号零。
    if (!Argument || !Argument->getIRType()) return nullptr;
    if (Argument->getIRType()->isInt()) return Argument;
    if (!Argument->getIRType()->isFloat()) return nullptr;
    auto *Bits = new BitCastInst(Argument, INTType::getInt32Ty());
    Block.pushBack(Bits);
    return Bits;
}
//以异或和常量乘法把一个参数键混入当前哈希值。
Value *FunctionMemoizationPass::appendHashMix(
    BasicBlock &Block, Value *Hash, Value *KeyBits) {
    //2.3 每个参数按 XOR 后乘常量混入哈希，最终用 2 次幂容量掩码选择槽位。
    if (!Hash || !KeyBits) return nullptr;
    auto *Xor = new BinaryInst(Hash, BinaryInst::XOR, KeyBits);
    Block.pushBack(Xor);
    auto *Mixed = new BinaryInst(
        Xor, BinaryInst::MUL, ConstInt::newConstInt(16777619));
    Block.pushBack(Mixed);
    return Mixed;
}
//在读取可变全局的候选的每个函数外入口推进缓存代际。
bool FunctionMemoizationPass::insertExternalCacheInvalidation(
    Module &M, const FunctionMemoizationCandidate &Candidate,
    Variable *SlotEpochCache, Variable *CurrentEpoch,
    unsigned CacheEntries) {
    //2.4 纯计算或只读常量不需要跨调用失效；读取可变全局时，每个顶层调用
    //推进一次非零代际。自递归调用不推进，因而同一棵递归树内仍可命中。
    if (Candidate.MutableReadGlobals.empty()) return true;
    if (!SlotEpochCache || !CurrentEpoch ||
        Candidate.ExternalCalls.empty()) {
        return false;
    }
    for (CallInst *Call : Candidate.ExternalCalls) {
        if (!Call || !Call->getParent() ||
            Call->getDirectCallee() != Candidate.Target ||
            Call->getParentFunction() == Candidate.Target) {
            return false;
        }
    }
    const uint64_t ByteCount =
        static_cast<uint64_t>(CacheEntries) * sizeof(uint32_t);
    if (ByteCount == 0 || ByteCount > UINT32_MAX) return false;
    const char *MemsetName = BuildInFunction::getDefaultMemsetName();
    for (CallInst *Call : Candidate.ExternalCalls) {
        //普通路径只把零长度交给memset；32位代际回绕到零时才清空整个槽位表，
        //并把活动代际恢复为1，因此不能把调用次数不溢出当作正确性前提。
        auto *OldEpoch = new LoadInst(CurrentEpoch);
        auto *NextEpoch = new BinaryInst(
            OldEpoch, BinaryInst::ADD, ConstInt::newConstInt(1));
        auto *Wrapped = new BinaryInst(
            NextEpoch, BinaryInst::E, ConstInt::newConstInt(0));
        auto *ActiveEpoch = new SelectInst(
            Wrapped, ConstInt::newConstInt(1), NextEpoch);
        auto *ClearBytes = new SelectInst(
            Wrapped,
            ConstInt::newConstInt(static_cast<int64_t>(ByteCount)),
            ConstInt::newConstInt(0));
        std::vector<Value *> Arguments{
            SlotEpochCache, ConstInt::newConstInt(0, 1), ClearBytes,
            ConstBool::newConstBool(false)};
        auto *Clear = new CallInst(
            BuildInFunction::genBuildInFunction(MemsetName), Arguments);
        auto *PublishEpoch = new StoreInst(ActiveEpoch, CurrentEpoch);
        const std::vector<Instruction *> Sequence{
            OldEpoch, NextEpoch, Wrapped, ActiveEpoch,
            ClearBytes, Clear, PublishEpoch};
        bool Inserted = true;
        for (Instruction *Inst : Sequence) {
            if (Call->insertBefore(Inst)) continue;
            Inserted = false;
            break;
        }
        if (!Inserted) {
            for (Instruction *Inst : Sequence) {
                if (Inst && !Inst->getParent()) delete Inst;
            }
            return false;
        }
    }
    M.registerBuiltin(MemsetName);
    return true;
}
//为候选创建缓存全局量、命中入口和返回点有序发布逻辑。
bool FunctionMemoizationPass::transformCandidate(
    Module &M, const FunctionMemoizationCandidate &Candidate,
    unsigned CacheOrdinal) const {
    //2.离线创建 lookup/hit 块：按参数生成键位、哈希与槽下标；任何构造失败都在
    //接入函数前删除临时块，因此不会留下部分 CFG。
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
    auto SlotEpochType = ARRAYType::NewArray(
        INTType::getInt32Ty(), static_cast<int>(CacheEntries));
    auto KeyType = ARRAYType::NewArray(
        INTType::getInt32Ty(), static_cast<int>(CacheEntries));
    auto ResultType = ARRAYType::NewArray(
        F->getIRType(), static_cast<int>(CacheEntries));
    //2.4 为槽位代际、每个参数键和结果分别创建同容量全局数组。
    auto *SlotEpochCache = new Variable(
        Variable::GlobalVariable, SlotEpochType,
        makeUniqueGlobalName(M, Prefix + ".slot.epoch"));
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
    //2.5 读取可变全局的函数为每个槽保存代际，并以独立标量记录当前代际；
    //全局零初始化配合外部入口先推进到1，保证零始终表示无效槽。
    Variable *CurrentEpoch = nullptr;
    if (!Candidate.MutableReadGlobals.empty()) {
        CurrentEpoch = new Variable(
            Variable::GlobalVariable, INTType::getInt32Ty(),
            makeUniqueGlobalName(M, Prefix + ".epoch"));
    }
    if (!insertExternalCacheInvalidation(
            M, Candidate, SlotEpochCache, CurrentEpoch, CacheEntries)) {
        delete LookupBlock;
        delete HitBlock;
        return false;
    }
    GetElementPtrInst *SlotEpochPointer =
        appendSlotPointer(*LookupBlock, SlotEpochCache, SlotIndex);
    std::vector<GetElementPtrInst *> KeyPointers;
    KeyPointers.reserve(KeyCaches.size());
    for (Variable *KeyCache : KeyCaches) {
        KeyPointers.push_back(
            appendSlotPointer(*LookupBlock, KeyCache, SlotIndex));
    }
    GetElementPtrInst *ResultPointer =
        appendSlotPointer(*LookupBlock, ResultCache, SlotIndex);
    //2.6 命中条件要求槽位代际等于当前活动代际，且所有存储键逐一相等；
    //命中块读取缓存结果并返回，未命中边进入原函数入口执行正常递归计算。
    auto *SlotEpoch = new LoadInst(SlotEpochPointer);
    LookupBlock->pushBack(SlotEpoch);
    Value *ActiveEpoch = ConstInt::newConstInt(1);
    if (CurrentEpoch) {
        auto *LoadedEpoch = new LoadInst(CurrentEpoch);
        LookupBlock->pushBack(LoadedEpoch);
        ActiveEpoch = LoadedEpoch;
    }
    auto *InitialHit = new BinaryInst(
        SlotEpoch, BinaryInst::E, ActiveEpoch);
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
    //2.7 完整 lookup/hit CFG 构造完成后才接入函数，并撤销递归调用的 tail 标记。
    F->pushFront(LookupBlock);
    F->pushBlock(HitBlock);
    for (CallInst *RecursiveCall : Candidate.RecursiveCalls) {
        if (RecursiveCall) RecursiveCall->setTailCall(false);
    }
    //2.8 每个原返回点按“槽代际清零 → 写全部键 → 写结果 → 发布活动代际”
    //的顺序更新，防止尚未完整写入的直接映射槽被后续递归命中。
    for (RetInst *Return : Candidate.Returns) {
        if (!Return || !Return->getParent()) continue;
        Value *Result = Return->getReturnValue();
        if (!Result) continue;
        Return->insertBefore(new StoreInst(
            ConstInt::newConstInt(0), SlotEpochPointer));
        for (unsigned Index = 0; Index < KeyPointers.size(); ++Index) {
            Return->insertBefore(new StoreInst(
                KeyBits[Index], KeyPointers[Index]));
        }
        Return->insertBefore(new StoreInst(Result, ResultPointer));
        Return->insertBefore(new StoreInst(
            ActiveEpoch, SlotEpochPointer));
    }
    return true;
}
//扫描模块候选并在总缓存预算内提交递归纯函数记忆化。
PreservationStatus FunctionMemoizationPass::run(
    Module &M, ModuleAnalysisPassManager &MAM) {
    (void)MAM;
    bool Changed = false;
    //1.先快照模块函数并完成全部候选验证，避免后续插入缓存 CFG 影响其他候选扫描。
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
    //2.按模块总缓存预算依次变换候选；单个缓存大小由参数键、槽位代际和
    //结果数组共同估算，超过剩余预算的函数跳过。
    std::size_t UsedCacheBytes = 0;
    unsigned CacheOrdinal = 0;
    for (const FunctionMemoizationCandidate &Candidate : Candidates) {
        if (!Candidate.Target) continue;
        std::size_t CacheBytes =
            static_cast<std::size_t>(CacheEntries) * sizeof(uint32_t) *
            (Candidate.Target->getParamCount() + 2);
        if (!Candidate.MutableReadGlobals.empty())
            CacheBytes += sizeof(uint32_t);
        if (CacheBytes > MaxModuleCacheBytes - UsedCacheBytes) continue;
        if (!transformCandidate(M, Candidate, CacheOrdinal)) continue;
        UsedCacheBytes += CacheBytes;
        ++CacheOrdinal;
        Changed = true;
    }
    //3.新增全局状态和 CFG 后不保留任何模块/函数分析缓存。
    if (!Changed) return PreservationStatus::all();
    return PreservationStatus::none();
}
