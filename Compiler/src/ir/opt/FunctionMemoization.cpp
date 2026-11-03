// Copyright (c) 2026 Compiler contributors
// SPDX-License-Identifier: MIT

#include "ir/opt/FunctionMemoization.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/DCE.hpp"
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
        if (!Operand || valueMayDependOnUndef(Operand)) return true;
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
//在不识别函数名、参数位置或常量取值的前提下，寻找“便宜直接返回守卫链
//之后才进入单块递归核心”的形态。成功时缓存只覆盖递归核心，基例原样直返。
bool FunctionMemoizationPass::collectDeferredLookupPlan(
    Function &F, FunctionMemoizationCandidate &Candidate) {
    Candidate.DeferredLookupPredecessor = nullptr;
    Candidate.DeferredLookupMissEntry = nullptr;
    Candidate.DeferredCachedReturns.clear();
    if (Candidate.Target != &F || Candidate.RecursiveCalls.size() < 2 ||
        Candidate.Returns.size() < 2 ||
        !Candidate.MutableReadGlobals.empty()) {
        return false;
    }
    //1.6.0 首版把守卫下沉限制在完整i32签名，避免把浮点异常环境、NaN
    //比较或隐式类型转换引入新的控制流等价证明。
    if (!F.getIRType() ||
        !F.getIRType()->hasIntegerBitWidth(32)) {
        return false;
    }
    for (const auto &Parameter : F.getParams()) {
        if (!Parameter || !Parameter->getIRType() ||
            !Parameter->getIRType()->hasIntegerBitWidth(32)) {
            return false;
        }
    }
    //1.6.1 首版只处理全部自递归调用位于同一直接返回块的严格形态；该块必须
    //无PHI、只有一条普通入边，保证下沉后的lookup支配缓存发布所用全部值。
    BasicBlock *Core = Candidate.RecursiveCalls.front()->getParent();
    if (!Core || Core == F.front() || !Core->collectPhiNodes().empty())
        return false;
    for (CallInst *Call : Candidate.RecursiveCalls)
        if (!Call || Call->getParent() != Core) return false;
    auto *CoreReturn = dynamic_cast<RetInst *>(Core->getTerminator());
    if (!CoreReturn) return false;
    std::vector<RetInst *> CoreReturns;
    for (RetInst *Return : Candidate.Returns)
        if (Return && Return->getParent() == Core)
            CoreReturns.push_back(Return);
    if (CoreReturns.size() != 1 || CoreReturns.front() != CoreReturn)
        return false;
    std::vector<BasicBlock *> CorePredecessors = Core->getPredecessors();
    if (CorePredecessors.size() != 1) return false;
    BasicBlock *CorePredecessor = CorePredecessors.front();
    if (!CorePredecessor ||
        CorePredecessor->countEdgesToSucc(Core) != 1) {
        return false;
    }

    //1.6.2 从真实入口正向匹配1至4层守卫。每层恰为一次参数与同类型常量
    //比较和条件分支；一边直达便宜返回，另一边继续到下一层或递归核心。
    BasicBlock *Current = F.front();
    BasicBlock *Previous = nullptr;
    std::unordered_set<BasicBlock *> GuardBlocks;
    std::unordered_set<BasicBlock *> BypassBlocks;
    std::unordered_set<RetInst *> BypassReturns;
    std::size_t CheapInstructionCost = 0;
    unsigned GuardDepth = 0;
    bool ReachedCore = false;
    while (Current && GuardDepth < MaxDeferredGuardDepth) {
        if (!GuardBlocks.insert(Current).second ||
            !Current->collectPhiNodes().empty() || Current->size() != 2) {
            return false;
        }
        if (Previous) {
            std::vector<BasicBlock *> Predecessors =
                Current->getPredecessors();
            if (Predecessors.size() != 1 ||
                Predecessors.front() != Previous ||
                Previous->countEdgesToSucc(Current) != 1) {
                return false;
            }
        } else if (!Current->getPredecessors().empty()) {
            return false;
        }
        auto *Branch = Current->getBranchTerminator();
        auto *Compare = Branch
            ? dynamic_cast<BinaryInst *>(Branch->getCondition()) : nullptr;
        if (!BasicBlock::isCondBr(Branch) || !Compare ||
            Current->front() != Compare || Current->back() != Branch ||
            Compare->getParent() != Current ||
            !BinaryInst::isCompareOp(Compare->getOp()) ||
            Compare->countUses() != 1 ||
            valueMayDependOnUndef(Compare) ||
            instructionMayFaultOrProducePoison(Compare)) {
            return false;
        }
        Value *Left = nullptr;
        Value *Right = nullptr;
        if (!Compare->getOperands(Left, Right) || !Left || !Right ||
            !Left->getIRType() || !Right->getIRType() ||
            !Left->getIRType()->hasIntegerBitWidth(32) ||
            !Right->getIRType()->hasIntegerBitWidth(32) ||
            !IRType::hasSameScalarType(
                Left->getIRType(), Right->getIRType()) ||
            !((Left->isParam() && Right->isConst()) ||
              (Right->isParam() && Left->isConst()))) {
            return false;
        }
        BasicBlock *TrueSuccessor = Branch->getTrueSuccessor();
        BasicBlock *FalseSuccessor = Branch->getFalseSuccessor();
        if (!TrueSuccessor || !FalseSuccessor ||
            TrueSuccessor == FalseSuccessor) {
            return false;
        }
        auto IsDirectReturnBlock = [Core](BasicBlock *Block) {
            return Block && Block != Core &&
                   Block->getSuccessors().empty() &&
                   dynamic_cast<RetInst *>(Block->getTerminator());
        };
        bool TrueReturns = IsDirectReturnBlock(TrueSuccessor);
        bool FalseReturns = IsDirectReturnBlock(FalseSuccessor);
        if (TrueReturns == FalseReturns) return false;
        BasicBlock *Bypass =
            TrueReturns ? TrueSuccessor : FalseSuccessor;
        BasicBlock *Continuation =
            TrueReturns ? FalseSuccessor : TrueSuccessor;
        auto *BypassReturn =
            dynamic_cast<RetInst *>(Bypass->getTerminator());
        if (!BypassReturn) return false;
        BypassBlocks.insert(Bypass);
        BypassReturns.insert(BypassReturn);
        CheapInstructionCost += 2;
        ++GuardDepth;
        if (Continuation == Core) {
            if (Current != CorePredecessor) return false;
            ReachedCore = true;
            break;
        }
        Previous = Current;
        Current = Continuation;
    }
    if (!ReachedCore || GuardDepth == 0) return false;

    //1.6.3 共享基例块可被多层守卫复用，但其全部前驱必须来自本计划；结果
    //反向闭包仅允许参数、常量和块内先定义的安全纯标量二元运算。
    for (BasicBlock *Bypass : BypassBlocks) {
        if (!Bypass || !Bypass->collectPhiNodes().empty()) return false;
        for (BasicBlock *Predecessor : Bypass->getPredecessors()) {
            if (!GuardBlocks.count(Predecessor) ||
                Predecessor->countEdgesToSucc(Bypass) != 1) {
                return false;
            }
        }
        std::unordered_set<Value *> Available;
        for (Instruction *Inst : *Bypass) {
            if (auto *Return = dynamic_cast<RetInst *>(Inst)) {
                Value *Result = Return->getReturnValue();
                if (!Result ||
                    !(Result->isParam() || Result->isConst() ||
                      Available.count(Result)) ||
                    !IRType::hasSameScalarType(
                        Result->getIRType(), F.getIRType())) {
                    return false;
                }
                continue;
            }
            auto *Binary = dynamic_cast<BinaryInst *>(Inst);
            Value *Left = nullptr;
            Value *Right = nullptr;
            if (!Binary || BinaryInst::isCompareOp(Binary->getOp()) ||
                !Binary->getOperands(Left, Right) || !Left || !Right ||
                Binary->isPointerValue() ||
                !Binary->getIRType() ||
                !Binary->getIRType()->hasIntegerBitWidth(32) ||
                !((Left->isParam() || Left->isConst() ||
                   Available.count(Left)) &&
                  (Right->isParam() || Right->isConst() ||
                   Available.count(Right))) ||
                hasUnsafeOrUndefinedOperation(Binary) ||
                valueMayDependOnUndef(Binary) ||
                instructionMayFaultOrProducePoison(Binary)) {
                return false;
            }
            Available.insert(Binary);
            ++CheapInstructionCost;
        }
    }
    //1.6.4 所有返回与可达块都必须被“守卫、基例、核心”完整覆盖，排除未建模
    //的第三条路径、回边或复杂汇合；成本只使用IR结构与参数数统一估计。
    for (RetInst *Return : Candidate.Returns) {
        if (!Return) return false;
        if (Return == CoreReturn) continue;
        if (!BypassReturns.count(Return)) return false;
    }
    std::unordered_set<BasicBlock *> CoveredBlocks = GuardBlocks;
    CoveredBlocks.insert(BypassBlocks.begin(), BypassBlocks.end());
    CoveredBlocks.insert(Core);
    const std::size_t EstimatedLookupCost =
        7 + 7 * F.getParamCount();
    if (CoveredBlocks.size() != static_cast<std::size_t>(F.size()) ||
        CheapInstructionCost > MaxCheapBypassInstructions ||
        CheapInstructionCost >= EstimatedLookupCost) {
        return false;
    }
    Candidate.DeferredLookupPredecessor = CorePredecessor;
    Candidate.DeferredLookupMissEntry = Core;
    Candidate.DeferredCachedReturns = {CoreReturn};
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
                if (!Result || valueMayDependOnUndef(Result) ||
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
            if (!Argument || valueMayDependOnUndef(Argument)) return false;
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
    //1.6 下沉计划是可选增强：严格形态不满足时保留原入口查找，不放宽
    //记忆化候选本身的任何正确性条件。
    (void)collectDeferredLookupPlan(F, Candidate);
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
    const bool HasAnyDeferredState =
        Candidate.DeferredLookupPredecessor ||
        Candidate.DeferredLookupMissEntry ||
        !Candidate.DeferredCachedReturns.empty();
    const bool HasDeferredLookup =
        Candidate.DeferredLookupPredecessor &&
        Candidate.DeferredLookupMissEntry &&
        !Candidate.DeferredCachedReturns.empty();
    if (HasAnyDeferredState != HasDeferredLookup) return false;
    if (HasDeferredLookup) {
        BasicBlock *Predecessor =
            Candidate.DeferredLookupPredecessor;
        BasicBlock *MissEntry = Candidate.DeferredLookupMissEntry;
        std::vector<BasicBlock *> MissPredecessors =
            MissEntry->getPredecessors();
        if (!Candidate.MutableReadGlobals.empty() ||
            Predecessor->getParent() != F || MissEntry->getParent() != F ||
            MissPredecessors.size() != 1 ||
            MissPredecessors.front() != Predecessor ||
            Predecessor->countEdgesToSucc(MissEntry) != 1 ||
            !MissEntry->collectPhiNodes().empty()) {
            return false;
        }
        for (RetInst *Return : Candidate.DeferredCachedReturns) {
            if (!Return || Return->getParent() != MissEntry ||
                Return != MissEntry->getTerminator()) {
                return false;
            }
        }
        for (CallInst *RecursiveCall : Candidate.RecursiveCalls)
            if (!RecursiveCall || RecursiveCall->getParent() != MissEntry)
                return false;
    }
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
    //严格基例计划把未命中边接到递归核心，否则保持进入原函数入口。
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
    BasicBlock *MissEntry = HasDeferredLookup
        ? Candidate.DeferredLookupMissEntry : OriginalEntry;
    LookupBlock->genCondInst(IsHit, HitBlock, MissEntry);
    auto *CachedResult = new LoadInst(ResultPointer);
    HitBlock->pushBack(CachedResult);
    HitBlock->pushBack(new RetInst(CachedResult));
    //2.7 完整 lookup/hit CFG 构造完成后才接入函数。严格基例计划仅改写
    //守卫尾到递归核心的唯一边并保留原入口；普通计划仍把lookup设为新入口。
    if (HasDeferredLookup) {
        bool Redirected = BasicBlock::redirectBranchTarget(
            Candidate.DeferredLookupPredecessor,
            Candidate.DeferredLookupMissEntry, LookupBlock);
        if (!Redirected) {
            delete LookupBlock;
            delete HitBlock;
            return false;
        }
        F->pushBlock(LookupBlock);
        F->pushBlock(HitBlock);
    } else {
        F->pushFront(LookupBlock);
        F->pushBlock(HitBlock);
    }
    for (CallInst *RecursiveCall : Candidate.RecursiveCalls) {
        if (RecursiveCall) RecursiveCall->setTailCall(false);
    }
    //2.8 每个被lookup支配的返回点按“槽代际清零 → 写全部键 → 写结果 →
    //发布活动代际”的顺序更新；便宜基例返回不读写编译器私有缓存。
    const std::vector<RetInst *> &PublishedReturns = HasDeferredLookup
        ? Candidate.DeferredCachedReturns : Candidate.Returns;
    for (RetInst *Return : PublishedReturns) {
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
