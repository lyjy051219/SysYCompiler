#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"
#include <cstdint>
#include <limits>
#include <queue>
bool isModSet(ModRefInfo MRI) {return (unsigned(MRI) & unsigned(ModRefInfo::Mod)) != 0;}
bool isRefSet(ModRefInfo MRI) {return (unsigned(MRI) & unsigned(ModRefInfo::Ref)) != 0;}
bool isNoModRef(ModRefInfo MRI) {return MRI == ModRefInfo::NoModRef;}
//合并两个ModRef结果中的读写效果
ModRefInfo unionModRef(ModRefInfo A, ModRefInfo B) {
    return ModRefInfo(unsigned(A) | unsigned(B));
}
bool SideEffectInfo::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(SideEffectAnalysis::ID())) return false;
    (void)F;
    *this = SideEffectInfo{};
    return true;
}
//将另一份指针来源信息合并到当前对象
/*
bases     = bases ∪ Other.bases
untracked = untracked || Other.untracked
*/
bool BaseAddressInfo::merge(const BaseAddressInfo& Other) {
    bool changed = false;
    for (auto* V : Other.bases)
        if (bases.insert(V).second) changed = true;
    if (Other.untracked && !untracked) { untracked = true; changed = true; }
    return changed;
}
//判断是否既没有已知基址也没有未知来源
bool BaseAddressInfo::isEmpty() const {
    return bases.empty() && !untracked;
}

//识别能够精确建模的LLVM内存Intrinsic
IntrinsicID getIntrinsicID(BuildInFunction* BIF) {
    if (!BIF) return IntrinsicID::None;
    const std::string& N = BIF->getIdent();
    //只接受 CFG/运行库正式发布的六个 LLVM 内存 intrinsic。不能用
    //substring 把名称相似的任意 BIF 当成具有 memcpy 等标准契约。
    if (N == "llvm.memcpy.p0.p0.i32" ||
        N == "llvm.memcpy.p0.p0.i64")
        return IntrinsicID::Memcpy;
    if (N == "llvm.memmove.p0.p0.i32" ||
        N == "llvm.memmove.p0.p0.i64")
        return IntrinsicID::Memmove;
    if (N == "llvm.memset.p0.i32" ||
        N == "llvm.memset.p0.i64")
        return IntrinsicID::Memset;
    return IntrinsicID::None;
}
//按照已知运行时契约分类内置函数
FuncAttr classifyBuiltin(BuildInFunction* BIF) {
    if (!BIF) return FuncAttr::Untracked;
    const std::string& N = BIF->getIdent();
    //timer不访问SysY程序对象，但其可观察顺序由调用效果摘要单独表达
    if (BuildInFunction::isTimingName(N)) return FuncAttr::NoModRef;
    if (BuildInFunction::isScalarIOName(N)) return FuncAttr::Sylib;
    if (BuildInFunction::isArrayInputName(N)) return FuncAttr::MemWriteOnly;
    if (BuildInFunction::isArrayOutputName(N)) return FuncAttr::MemReadOnly;
    const IntrinsicID IID = getIntrinsicID(BIF);
    if (IID == IntrinsicID::Memcpy ||
        IID == IntrinsicID::Memmove)
        return FuncAttr::MemReadWrite;
    if (IID == IntrinsicID::Memset)
        return FuncAttr::MemWriteOnly;
    return FuncAttr::Untracked;
}
//从CFG中的长度Value提取内存Intrinsic的无符号字节数
std::optional<std::size_t> SideEffectAnalysis::getConstantMemoryIntrinsicLength(
    Value *LengthValue) {
    if (!LengthValue) return std::nullopt;
    const int BitWidth = LengthValue->integerBitWidth(0);
    if (BitWidth != 32 && BitWidth != 64) return std::nullopt;
    int64_t SignedBits = 0;
    if (!LengthValue->tryGetInt64ConstAtWidth(
            static_cast<unsigned>(BitWidth), SignedBits)) {
        return std::nullopt;
    }
    //LLVM intrinsic 的长度是无符号位模式；先按声明位宽规范化，再检查宿主
    //size_t 是否能无损承载，不能表示时必须保留未知长度。
    const uint64_t UnsignedLength = BitWidth == 32
        ? static_cast<uint64_t>(
              static_cast<uint32_t>(SignedBits))
        : static_cast<uint64_t>(SignedBits);
    if (UnsignedLength >
        static_cast<uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(UnsignedLength);
}
//判断内存Intrinsic的volatile参数是否要求保留可观察顺序
bool SideEffectAnalysis::isVolatileMemoryIntrinsic(CallInst *CI) {
    int32_t IsVolatile = 1;
    return !CI || CI->getArgCount() < 4 ||
           !Value::tryGetIntConst(CI->getArg(3), IsVolatile) ||
           IsVolatile != 0;
}
//在调用点上按“指针+长度”去重并合并读写角色
void SideEffectAnalysis::appendCallMemoryEffect(
    CallEffectSummary &Summary, Value *Pointer,
    std::optional<std::size_t> Size, ModRefInfo Effect) {
    if (isNoModRef(Effect)) return;
    if (!Pointer || !Pointer->isPointerValue()) {
        Summary.HasUnknownMemory = true;
        return;
    }
    for (CallMemoryEffect &Existing : Summary.ProgramMemory) {
        if (Existing.Location.Pointer != Pointer ||
            Existing.Location.Size != Size) {
            continue;
        }
        Existing.ModRef = unionModRef(Existing.ModRef, Effect);
        return;
    }
    Summary.ProgramMemory.push_back(
        {MemoryLocation(Pointer, Size), Effect});
}
//为间接调用或损坏的调用指令构造最保守的调用摘要
CallEffectSummary SideEffectAnalysis::getOpaqueCallSummary() {
    CallEffectSummary Summary;
    Summary.HasUnknownMemory = true;
    Summary.HasObservableEffect = true;
    Summary.HasOrderingBarrier = true;
    return Summary;
}
//按CFG统一发布的运行时名称和调用参数构造内置调用效果
CallEffectSummary SideEffectAnalysis::summarizeBuiltinCall(CallInst *CI) {
    BuildInFunction *BIF = CI ? CI->getBuiltinCallee() : nullptr;
    if (!BIF) return getOpaqueCallSummary();

    CallEffectSummary Summary;
    const std::string &Name = BIF->getIdent();
    if (BuildInFunction::isTimingName(Name)) {
        Summary.HasObservableEffect = true;
        Summary.HasOrderingBarrier = true;
        return Summary;
    }

    const IntrinsicID IID = getIntrinsicID(BIF);
    if (IID == IntrinsicID::Memcpy || IID == IntrinsicID::Memmove) {
        if (isVolatileMemoryIntrinsic(CI)) {
            Summary.HasObservableEffect = true;
            Summary.HasOrderingBarrier = true;
        }
        if (CI->getArgCount() < 2) {
            Summary.HasUnknownMemory = true;
            return Summary;
        }
        const std::optional<std::size_t> Length =
            getConstantMemoryIntrinsicLength(
                CI->getArgCount() >= 3 ? CI->getArg(2) : nullptr);
        if (!Length || *Length != 0) {
            appendCallMemoryEffect(
                Summary, CI->getArg(0), Length, ModRefInfo::Mod);
            appendCallMemoryEffect(
                Summary, CI->getArg(1), Length, ModRefInfo::Ref);
        }
        return Summary;
    }
    if (IID == IntrinsicID::Memset) {
        if (isVolatileMemoryIntrinsic(CI)) {
            Summary.HasObservableEffect = true;
            Summary.HasOrderingBarrier = true;
        }
        if (CI->getArgCount() == 0) {
            Summary.HasUnknownMemory = true;
            return Summary;
        }
        const std::optional<std::size_t> Length =
            getConstantMemoryIntrinsicLength(
                CI->getArgCount() >= 3 ? CI->getArg(2) : nullptr);
        if (!Length || *Length != 0) {
            appendCallMemoryEffect(
                Summary, CI->getArg(0), Length, ModRefInfo::Mod);
        }
        return Summary;
    }
    if (BuildInFunction::isArrayInputName(Name) ||
        BuildInFunction::isArrayOutputName(Name)) {
        const ModRefInfo Effect = BuildInFunction::isArrayInputName(Name)
            ? ModRefInfo::Mod : ModRefInfo::Ref;
        Summary.HasObservableEffect = true;
        Summary.MayCrossObjectBounds = true;
        Summary.CrossObjectModRef = Effect;
        bool FoundPointer = false;
        for (unsigned Index = 0; Index < CI->getArgCount(); ++Index) {
            Value *Arg = CI->getArg(Index);
            if (!Arg || !Arg->isPointerValue()) continue;
            FoundPointer = true;
            //SysY运行时不保证访问长度不超过源语言数组容量。
            appendCallMemoryEffect(
                Summary, Arg, std::nullopt, Effect);
        }
        if (!FoundPointer) Summary.HasUnknownMemory = true;
        return Summary;
    }
    if (BuildInFunction::isScalarIOName(Name) ||
        BuildInFunction::isOutputName(Name)) {
        Summary.HasObservableEffect = true;
        return Summary;
    }
    //未知运行时可能读写任意程序对象，也不允许删除或跨越。
    return getOpaqueCallSummary();
}
//通过Function形参索引和CallInst实参接口映射一个被调值
Value *SideEffectAnalysis::mapCalleeValueToActual(
    CallInst *CI, Value *CalleeValue) {
    if (!CI || !CalleeValue) return nullptr;
    if (CalleeValue->isGlobal()) return CalleeValue;
    Function *Callee = CI->getDirectCallee();
    if (!Callee || !CalleeValue->isParam()) return nullptr;
    const int Index = Callee->getParamIndex(CalleeValue);
    if (Index < 0 ||
        static_cast<unsigned>(Index) >= CI->getArgCount()) {
        return nullptr;
    }
    return CI->getArg(static_cast<unsigned>(Index));
}
//将函数级的全局量/形参摘要实例化为一次调用的实际指针效果
CallEffectSummary SideEffectAnalysis::instantiateCalleeSummary(
    CallInst *CI, const SideEffectInfo &CalleeSummary,
    bool *HadUnmappedEffect) {
    if (HadUnmappedEffect) *HadUnmappedEffect = false;
    if (!CI || !CI->getDirectCallee()) return getOpaqueCallSummary();

    CallEffectSummary Summary;
    Summary.HasUnknownMemory = CalleeSummary.hasUnknownMemory;
    Summary.HasObservableEffect = CalleeSummary.hasObservableEffect;
    Summary.HasOrderingBarrier = CalleeSummary.hasOrderingBarrier;
    Summary.MayCrossObjectBounds = CalleeSummary.mayCrossObjectBounds;
    Summary.CrossObjectModRef = CalleeSummary.crossObjectModRef;

    for (Value *Modified : CalleeSummary.ModSet) {
        Value *Actual = mapCalleeValueToActual(CI, Modified);
        if (!Actual || !Actual->isPointerValue()) {
            Summary.HasUnknownMemory = true;
            if (HadUnmappedEffect) *HadUnmappedEffect = true;
            continue;
        }
        appendCallMemoryEffect(
            Summary, Actual, std::nullopt, ModRefInfo::Mod);
    }
    for (Value *Referenced : CalleeSummary.RefSet) {
        Value *Actual = mapCalleeValueToActual(CI, Referenced);
        if (!Actual || !Actual->isPointerValue()) {
            Summary.HasUnknownMemory = true;
            if (HadUnmappedEffect) *HadUnmappedEffect = true;
            continue;
        }
        appendCallMemoryEffect(
            Summary, Actual, std::nullopt, ModRefInfo::Ref);
    }
    return Summary;
}
//调用效果先按运行时契约或被调函数摘要映射到调用点实际指针
CallEffectSummary getCallEffectSummary(
    CallInst *CI, FunctionAnalysisPassManager &FAM) {
    if (!CI || !CI->getCallee())
        return SideEffectAnalysis::getOpaqueCallSummary();
    if (CI->getBuiltinCallee())
        return SideEffectAnalysis::summarizeBuiltinCall(CI);
    if (Function *Callee = CI->getDirectCallee()) {
        return SideEffectAnalysis::instantiateCalleeSummary(
            CI, FAM.getResult<SideEffectAnalysis>(*Callee));
    }
    return SideEffectAnalysis::getOpaqueCallSummary();
}

//判断两个位置之间是否存在一条经过有序调用并到达终点的CFG路径
bool hasOrderingBarrierOnPath(
    Function &F, BasicBlock *StartBlock, const Instruction *StartAfter,
    const Instruction *EndBefore, FunctionAnalysisPassManager &FAM) {
    //参数或父子关系损坏时保守报告存在屏障，避免错误IR绕过顺序保护。
    if (!StartBlock || !EndBefore || StartBlock->getParent() != &F ||
        !EndBefore->getParent() ||
        EndBefore->getParent()->getParent() != &F ||
        (StartAfter && StartAfter->getParent() != StartBlock)) {
        return true;
    }

    struct OrderingPathState {
        BasicBlock *Block;
        Instruction *Next;
        bool SawBarrier;
    };

    std::queue<OrderingPathState> WorkList;
    std::unordered_set<BasicBlock *> VisitedWithoutBarrier;
    std::unordered_set<BasicBlock *> VisitedWithBarrier;
    Instruction *First = StartAfter ? StartAfter->getNext()
                                    : StartBlock->front();
    WorkList.push({StartBlock, First, false});
    if (!StartAfter) VisitedWithoutBarrier.insert(StartBlock);

    while (!WorkList.empty()) {
        OrderingPathState State = WorkList.front();
        WorkList.pop();
        if (!State.Block) continue;

        bool SawBarrier = State.SawBarrier;
        bool ReachedEnd = false;
        for (Instruction *I = State.Next; I; I = I->getNext()) {
            if (I == EndBefore) {
                if (SawBarrier) return true;
                ReachedEnd = true;
                break;
            }
            auto *CI = dynamic_cast<CallInst *>(I);
            if (CI && getCallEffectSummary(CI, FAM).HasOrderingBarrier) {
                SawBarrier = true;
            }
        }
        if (ReachedEnd) continue;

        for (BasicBlock *Succ : State.Block->getSuccessors()) {
            if (!Succ || Succ->getParent() != &F) continue;
            auto &Visited = SawBarrier ? VisitedWithBarrier
                                       : VisitedWithoutBarrier;
            if (!Visited.insert(Succ).second) continue;
            WorkList.push({Succ, Succ->front(), SawBarrier});
        }
    }
    return false;
}

//从一条指令之后开始查询通用顺序屏障
bool hasOrderingBarrierOnPath(
    Function &F, const Instruction *StartAfter,
    const Instruction *EndBefore, FunctionAnalysisPassManager &FAM) {
    if (!StartAfter || !StartAfter->getParent()) return true;
    return hasOrderingBarrierOnPath(
        F, StartAfter->getParent(), StartAfter, EndBefore, FAM);
}
//指令级 ModRef 流程：先构造区间重叠判定，再按 Load、Store、Call 分类求效果。
ModRefInfo getInstModRefInfo(const MemoryLocation &Location, Instruction* I,
                             BasicAAResult& AA, const LoopInfo& LI,
                             FunctionAnalysisPassManager& FAM)
{
    Value *V = Location.Pointer;
    if (!V || !I) return ModRefInfo::ModRef;
    ModRefInfo Result = ModRefInfo::NoModRef;
    //1. 已知长度使用完整字节区间；未知长度仍走 MemoryLocation 查询，使
    //BasicAA 验证两侧访问均落在对象内。不能仅凭底层对象不同利用越界 UB。
    auto overlapsLocation = [&](const MemoryLocation &Other) -> bool {
        if (!Other.Pointer) return true;
        if (!Location.Size || !Other.Size)
            return AA.alias(Location, Other, LI) != AliasInfo::NoAlias;
        return AA.aliasWithIndexRange(Location, Other, LI) !=
               AliasInfo::NoAlias;
    };
    //2. Load/Store 直接与目标 Location 比较并累加 Ref 或 Mod。
    if (auto *Load = dynamic_cast<LoadInst *>(I)) {
        if (Load->getPointerOperand() &&
            overlapsLocation(MemoryLocation::get(Load)))
            Result = unionModRef(Result, ModRefInfo::Ref);
        return Result;
    }
    if (auto *SI = dynamic_cast<StoreInst *>(I)) {
        if (SI->getPointerOperand() &&
            overlapsLocation(MemoryLocation::get(SI)))
            Result = unionModRef(Result, ModRefInfo::Mod);
        return Result;
    }
    //3. Call 消费统一的调用效果摘要；可观察性不等于程序内存 ModRef。
    if (I->instType == Instruction::CALL) {
        auto *CI = dynamic_cast<CallInst *>(I);
        if (!CI) return ModRefInfo::ModRef;
        CallEffectSummary Effects = getCallEffectSummary(CI, FAM);
        if (Effects.HasUnknownMemory) return ModRefInfo::ModRef;
        if (Effects.MayCrossObjectBounds)
            Result = unionModRef(Result, Effects.CrossObjectModRef);
        for (const CallMemoryEffect &Effect : Effects.ProgramMemory) {
            if (Effect.Location.Size && *Effect.Location.Size == 0)
                continue;
            if (!Effects.MayCrossObjectBounds &&
                !overlapsLocation(Effect.Location)) {
                continue;
            }
            Result = unionModRef(Result, Effect.ModRef);
            if (Result == ModRefInfo::ModRef) return Result;
        }
        return Result;
    }
    return ModRefInfo::NoModRef;
}
//查询一条指令对长度未知的目标指针的读写效果
ModRefInfo getInstModRefInfo(Value* V, Instruction* I,
                             BasicAAResult& AA, const LoopInfo& LI,
                             FunctionAnalysisPassManager& FAM) {
    return getInstModRefInfo(
        MemoryLocation(V, std::nullopt), I, AA, LI, FAM);
}
//将函数副作用摘要聚合成单个保守ModRef结果
ModRefInfo getFunctionModRefInfo(const SideEffectInfo& SEI)
{
    //该 legacy 聚合接口仍被尾递归消除等消费者当作“整个调用可安全
    //忽略”的门槛；builtin、外部可观察效果和顺序屏障不能伪装成 NoModRef。
    //只关心程序内存的消费者应使用 getCallProgramModRefInfo。
    if (SEI.hasUnknownMemory || SEI.hasBuiltin ||
        SEI.hasObservableEffect || SEI.hasOrderingBarrier)
        return ModRefInfo::ModRef;
    //跨对象效果可能没有可安全映射到形参/全局的具体 Value，但它仍然是
    //真实的程序内存效果，不能因 ModSet/RefSet 为空而被误判为纯函数。
    ModRefInfo Result = SEI.mayCrossObjectBounds
        ? SEI.crossObjectModRef
        : ModRefInfo::NoModRef;
    for (Value* V : SEI.ModSet) {
        if (!dynamic_cast<ConstantSystem*>(V)) {
            Result = unionModRef(Result, ModRefInfo::Mod);
            break;
        }
    }
    for (Value* V : SEI.RefSet) {
        if (!dynamic_cast<ConstantSystem*>(V)) {
            Result = unionModRef(Result, ModRefInfo::Ref);
            break;
        }
    }
    return Result;
}
//返回一次调用对SysY程序内存的整体读写效果
ModRefInfo getCallProgramModRefInfo(
    CallInst *CI, FunctionAnalysisPassManager &FAM) {
    //调用级摘要只聚合 SysY 程序内存；I/O 与 timer 的可观察顺序由
    //CallEffectSummary 的独立字段表达。
    CallEffectSummary Summary = getCallEffectSummary(CI, FAM);
    if (Summary.HasUnknownMemory) return ModRefInfo::ModRef;
    ModRefInfo Result = Summary.MayCrossObjectBounds
        ? Summary.CrossObjectModRef
        : ModRefInfo::NoModRef;
    for (const CallMemoryEffect &Effect : Summary.ProgramMemory)
        Result = unionModRef(Result, Effect.ModRef);
    return Result;
}
//================================================上面是访问接口，下面是构建过程
//为全局变量或函数形参初始化已知基址来源
void SideEffectAnalysis::ensureInitialized(
    Value *V,
    std::unordered_map<Value *, BaseAddressInfo> &PointerOrigins) {
    //如果是其他情况那么就是未知
    if (!V || PointerOrigins.count(V)) return;
    //如果来源是全局变量或形参：基址就是自己
    if (V->isExternalVariable())
        PointerOrigins[V].bases.insert(V);
}
//1.2构建函数的所有基址信息
std::unordered_map<Value *, BaseAddressInfo>
SideEffectAnalysis::buildBaseAddressInfoMap(Function& F) {
    //1.形参：基址为自身
    std::unordered_map<Value *, BaseAddressInfo> Map;
    for (std::size_t Index = 0; Index < F.getParamCount(); ++Index) {
        Value *V = F.getParam(Index);
        if (!V || !V->isPointerValue()) continue;
        Map[V].bases.insert(V);
    }
    /*
    %slot = alloca ptr
    store ptr %a, ptr %slot
    %p = load ptr, ptr %slot
    store i32 1, ptr %p
    为了防止分析%p 来自一次 load ptr，无法知道它最终来自 %a
    需要建立 [某次从槽中 Load 出来的指针 → 所有曾 Store 进这个槽的指针]LocalPointerLoadSources
    */
    std::unordered_map<LoadInst *, std::vector<Value *>>
        LocalPointerLoadSources;
    forEachInstruction<AllocaInst>(F, [&](AllocaInst *Slot) {
        if (!IRType::isPointer(Slot->getAllocatedType())) return;
        bool Trackable = true;
        std::vector<LoadInst *> Loads;
        std::vector<Value *> StoredPointers;
        for (Use *U : Slot->getUses()) {
            Instruction *UserInst =
                U && U->getUser() ? U->getUser()->as<Instruction>() : nullptr;
            if (LoadInst *Load =
                    UserInst ? UserInst->as<LoadInst>() : nullptr) {
                if (Load->getPointerOperand() == Slot &&
                    Load->isPointerValue()) {
                    Loads.push_back(Load);
                    continue;
                }
            }
            if (StoreInst *Store =
                    UserInst ? UserInst->as<StoreInst>() : nullptr) {
                Value *Stored = Store->getValueOperand();
                if (Store->getPointerOperand() == Slot && Stored &&
                    Stored->isPointerValue()) {
                    StoredPointers.push_back(Stored);
                    continue;
                }
            }
            Trackable = false;
            break;
        }
        if (!Trackable) return;
        for (LoadInst *Load : Loads)
            LocalPointerLoadSources.emplace(Load, StoredPointers);
    });

    /*
    2,
    alloca:基址为自身
    GEP：来源可以从基指针传播
    bitcast：来源可以从操作数传播
    phi：来源是所有 incoming 指针来源的并集
    select：来源是真、假分支指针来源的并集
    经过证明的局部指针槽 load：来源是所有存入槽中的指针
    其他产生指针的指令：直接标记 untracked=true
    */
    forEachInstruction(F, [&](Instruction *I) {
        if (!I->isPointerValue()) return;
        BaseAddressInfo &Seed = Map[I];
        if (I->as<AllocaInst>()) {
            Seed.bases.insert(I);
            return;
        }
        LoadInst *Load = I->as<LoadInst>();
        const bool CanPropagate =
            I->as<GetElementPtrInst>() || I->as<BitCastInst>() ||
            I->as<PhiInst>() || I->as<SelectInst>() ||
            (Load && LocalPointerLoadSources.count(Load));
        if (!CanPropagate) Seed.untracked = true;
    });

    //合并基址来源辅助函数
    auto mergeSource = [&](Value *Source, BaseAddressInfo &Destination) {
        if (!Source) {
            Destination.untracked = true;
            return;
        }
        //初始化全局量
        /*
        全局变量/形参：基址为自身
        */
        ensureInitialized(Source, Map);
        auto It = Map.find(Source);
        if (It == Map.end()) {
            //如果找不到就标未知
            Destination.untracked = true;
            return;
        }
        Destination.merge(It->second);
    };

    //3.迭代传播基址来源直到收敛
    /*
    load(pointer slot) → 合并所有曾存入槽中的指针来源
    gep(base)          → 继承 base 来源
    bitcast(x)         → 继承 x 来源
    phi(a,b,...)       → 合并所有 incoming 来源
    select(c,a,b)      → 合并 a、b 来源
    */
    auto propagateToFixedPoint = [&]() {
        bool Changed = true;
        while (Changed) {
            Changed = false;
            forEachInstruction(F, [&](Instruction *I) {
                if (!I->isPointerValue() || I->as<AllocaInst>()) return;
                BaseAddressInfo NewInfo;
                if (LoadInst *Load = I->as<LoadInst>()) {
                    auto It = LocalPointerLoadSources.find(Load);
                    if (It == LocalPointerLoadSources.end()) return;
                    for (Value *Stored : It->second)
                        mergeSource(Stored, NewInfo);
                } else if (GetElementPtrInst *GEP =
                               I->as<GetElementPtrInst>()) {
                    mergeSource(GEP->getBasePointer(), NewInfo);
                } else if (BitCastInst *BitCast = I->as<BitCastInst>()) {
                    mergeSource(BitCast->getUnaryOperand(), NewInfo);
                } else if (PhiInst *Phi = I->as<PhiInst>()) {
                    forEachPhiIncoming(
                        Phi, [&](unsigned, Value *Incoming, BasicBlock *) {
                            mergeSource(Incoming, NewInfo);
                        });
                } else if (SelectInst *Select = I->as<SelectInst>()) {
                    mergeSource(Select->getTrueValue(), NewInfo);
                    mergeSource(Select->getFalseValue(), NewInfo);
                } else {
                    return;
                }
                if (Map[I].merge(NewInfo)) Changed = true;
            });
        }
    };

    //3
    propagateToFixedPoint();
    bool MarkedUnresolved = false;
    //4.如果还存在基址为空的情况，就再迭代一轮
    forEachInstruction(F, [&](Instruction *I) {
        if (!I->isPointerValue()) return;
        BaseAddressInfo &Info = Map[I];
        if (!Info.isEmpty()) return;
        Info.untracked = true;
        MarkedUnresolved = true;
    });
    if (MarkedUnresolved) propagateToFixedPoint();
    return Map;
}
//合并布尔副作用属性
bool SideEffectAnalysis::mergeFlag(bool &Destination, bool Source) {
    if (!Source || Destination) return false;
    Destination = true;
    return true;
}
//设置hasBuiltin = true，hasUnknownMemory = true，表示未知内存
bool SideEffectAnalysis::markUnmodelledMemory(SideEffectInfo &Summary) {
    bool Changed = false;
    Changed |= mergeFlag(Summary.hasBuiltin, true);
    Changed |= mergeFlag(Summary.hasUnknownMemory, true);
    return Changed;
}
//标记无法解析目标的调用，其内存、外部效果和顺序都必须保守
bool SideEffectAnalysis::markOpaqueCall(SideEffectInfo &Summary) {
    bool Changed = markUnmodelledMemory(Summary);
    Changed |= mergeFlag(Summary.hasObservableEffect, true);
    Changed |= mergeFlag(Summary.hasOrderingBarrier, true);
    return Changed;
}
//1.3将Pointer的副作用转换到对基址的副作用
bool SideEffectAnalysis::addPointerEffect(
    Value *Pointer, ModRefInfo Effect,
    const std::unordered_map<Value *, BaseAddressInfo> &PointerOrigins,
    SideEffectInfo &Summary) {
    if (isNoModRef(Effect)) return false;
    if (!Pointer || !Pointer->isPointerValue())
        return markUnmodelledMemory(Summary);

    bool Changed = false;
    //只把外部基址加入 ModSet/RefSet
    auto addExternalBase = [&](Value *Base) {
        /*
        指针形参、全局变量 → 保留
        局部 alloca      → 忽略
        */
        if (!Base || !Base->isExternalVariable()) return;
        if (isModSet(Effect))
            Changed |= Summary.ModSet.insert(Base).second;
        if (isRefSet(Effect))
            Changed |= Summary.RefSet.insert(Base).second;
    };

    auto OriginIt = PointerOrigins.find(Pointer);
    if (OriginIt != PointerOrigins.end()) {
        for (Value *Base : OriginIt->second.bases)
            addExternalBase(Base);
        if (OriginIt->second.untracked)
            Changed |= markUnmodelledMemory(Summary);
        return Changed;
    }
    //本身就是外部对象直接记录
    if (Pointer->isExternalVariable()) {
        addExternalBase(Pointer);
        return Changed;
    }
    Changed |= markUnmodelledMemory(Summary);
    return Changed;
}
//将调用点上的实际指针效果和独立标志合并到函数摘要
bool SideEffectAnalysis::mergeCallEffects(
    const CallEffectSummary &CallSummary, bool LegacyHasBuiltin,
    const std::unordered_map<Value *, BaseAddressInfo> &PointerOrigins,
    SideEffectInfo &FunctionSummary) {
    bool Changed = false;
    for (const CallMemoryEffect &Effect : CallSummary.ProgramMemory) {
        Changed |= addPointerEffect(
            Effect.Location.Pointer, Effect.ModRef,
            PointerOrigins, FunctionSummary);
    }
    Changed |= mergeFlag(FunctionSummary.hasBuiltin, LegacyHasBuiltin);
    Changed |= mergeFlag(
        FunctionSummary.hasUnknownMemory,
        CallSummary.HasUnknownMemory);
    Changed |= mergeFlag(
        FunctionSummary.hasObservableEffect,
        CallSummary.HasObservableEffect);
    Changed |= mergeFlag(
        FunctionSummary.hasOrderingBarrier,
        CallSummary.HasOrderingBarrier);
    Changed |= mergeFlag(
        FunctionSummary.mayCrossObjectBounds,
        CallSummary.MayCrossObjectBounds);
    const ModRefInfo MergedCrossObjectModRef = unionModRef(
        FunctionSummary.crossObjectModRef,
        CallSummary.CrossObjectModRef);
    if (MergedCrossObjectModRef != FunctionSummary.crossObjectModRef) {
        FunctionSummary.crossObjectModRef = MergedCrossObjectModRef;
        Changed = true;
    }
    return Changed;
}
//将唯一的内置调用点模型投影为当前函数的外部效果
void SideEffectAnalysis::processBuiltinCall(
    CallInst *CI,
    const std::unordered_map<Value *, BaseAddressInfo> &PointerOrigins,
    SideEffectInfo &Summary) {
    CallEffectSummary Effects = summarizeBuiltinCall(CI);
    const bool LegacyHasBuiltin =
        Effects.HasUnknownMemory || Effects.HasObservableEffect ||
        Effects.HasOrderingBarrier;
    (void)mergeCallEffects(
        Effects, LegacyHasBuiltin, PointerOrigins, Summary);

    BuildInFunction *BIF = CI ? CI->getBuiltinCallee() : nullptr;
    //旧函数级接口会先验证memset目标，再处理零长度；即使面对绕过
    //CallInst校验构造出的畸形IR，也继续维持相同的保守结果。
    if (getIntrinsicID(BIF) == IntrinsicID::Memset && CI &&
        CI->getArgCount() > 0 &&
        (!CI->getArg(0) || !CI->getArg(0)->isPointerValue())) {
        (void)markUnmodelledMemory(Summary);
    }
    if (classifyBuiltin(BIF) != FuncAttr::Untracked) return;
    //保留旧接口的精度：不透明内置调用虽然已是未知内存，
    //仍把能识别的指针实参同时记录为可读写对象。
    for (unsigned Index = 0; CI && Index < CI->getArgCount(); ++Index) {
        Value *Arg = CI->getArg(Index);
        if (!Arg || !Arg->isPointerValue()) continue;
        (void)addPointerEffect(
            Arg, ModRefInfo::ModRef, PointerOrigins, Summary);
    }
}
//主流程1：收集可达函数的自身副作用
std::unordered_map<Function *, SideEffectInfo>
SideEffectAnalysis::collectReachableFunctionSummaries(Function &Root) {
    //Function* → SideEffectInfo最终结果
    std::unordered_map<Function *, SideEffectInfo> Summaries;
    std::vector<Function *> ReachableFunctions;
    Summaries.try_emplace(&Root);
    ReachableFunctions.push_back(&Root);
    //1.1遍历所有的可达函数
    for (std::size_t Index = 0;Index < ReachableFunctions.size(); ++Index) {
        Function *Current = ReachableFunctions[Index];
        auto CurrentIt = Summaries.find(Current);
        if (!Current || CurrentIt == Summaries.end()) continue;

        SideEffectInfo &Summary = CurrentIt->second;
        //1.2构建函数的所有基址信息
/*
 * 基址规则：
 * 1. 指针形参、全局变量和alloca的基址都是自身；
 * 2. 一个指针可以具有多个已知基址。
 *
 * 指令传播规则：
 * - GEP：继承base pointer的基址；
 * - BitCast：继承操作数的基址；
 * - PHI：合并所有incoming指针的基址；
 * - Select：合并true和false指针的基址；
 * - 局部指针槽Load：合并所有曾Store进该槽的指针值的基址。
 */
        Summary.ptrMap = buildBaseAddressInfoMap(*Current);
        //1.3
/*
 * 指令副作用收集规则：处理Load、Store和Call三类指令。
 * 1. Load/Store：
 *    - Load：将指针的所有外部基址（全局变量或形参）加入RefSet；
 *    - Store：将指针的所有外部基址（全局变量或形参）加入ModSet；
 *    - 局部alloca基址不加入RefSet或ModSet。
 *
 * 2. Call：
 *    - 普通直接调用：
 *      仅记录Callee，在第二阶段传播其副作用。
 *
 *    - 内置函数调用：
 *      - timer：
 *        不加入ModSet或RefSet，只标记可观察效果和顺序屏障；
 *
 *      - memcpy/memmove：
 *        目标指针加入ModSet，源指针加入RefSet；
 *
 *      - memset：
 *        目标指针加入ModSet；
 *
 *      - 数组输入：
 *        指针实参加入ModSet；
 *
 *      - 数组输出：
 *        指针实参加入RefSet；
 *
 *      - 标量输入输出：
 *        不加入ModSet或RefSet，只标记可观察效果。
 */
        std::vector<Function *> DirectCallees;
        forEachInstruction(*Current, [&](Instruction *I) {
            if (LoadInst *Load = I->as<LoadInst>()) {
                (void)addPointerEffect(
                    Load->getPointerOperand(), ModRefInfo::Ref,
                    Summary.ptrMap, Summary);
                return;
            }
            if (StoreInst *Store = I->as<StoreInst>()) {
                (void)addPointerEffect(
                    Store->getPointerOperand(), ModRefInfo::Mod,
                    Summary.ptrMap, Summary);
                return;
            }
            CallInst *Call = I->as<CallInst>();
            if (!Call) return;
            //Call：普通函数调用，仅记录，第二阶段传播副作用
            if (Function *Callee = Call->getDirectCallee()) {
                DirectCallees.push_back(Callee);
                return;
            }
            //Call：内置函数调用
            if (Call->getBuiltinCallee()) {
                processBuiltinCall(Call, Summary.ptrMap, Summary);
                return;
            }
            //Call：其他情况
            /*
            hasBuiltin          = true
            hasUnknownMemory    = true
            hasObservableEffect = true
            hasOrderingBarrier  = true
            */
            (void)markOpaqueCall(Summary);
        });

        //完成当前函数扫描后再扩容unordered_map
        for (Function *Callee : DirectCallees) {
            if (!Callee) continue;
            if (Summaries.try_emplace(Callee).second)
                ReachableFunctions.push_back(Callee);
        }
    }
    return Summaries;
}
//将一条直接调用边的被调摘要实例化并合并到调用者
bool SideEffectAnalysis::mergeCalleeEffects(
    CallInst *CI, const SideEffectInfo &CalleeSummary,
    SideEffectInfo &CallerSummary) {
    bool HadUnmappedEffect = false;
    CallEffectSummary Effects = instantiateCalleeSummary(
        CI, CalleeSummary, &HadUnmappedEffect);
    return mergeCallEffects(
        Effects,
        CalleeSummary.hasBuiltin || HadUnmappedEffect,
        CallerSummary.ptrMap,
        CallerSummary);
}
//2.传播普通函数调用的副作用，直到收敛
void SideEffectAnalysis::solveToFixedPoint(
    std::unordered_map<Function *, SideEffectInfo> &Summaries) {
    bool Changed = true;
    while (Changed) {
        Changed = false;
        for (auto &CalleeEntry : Summaries) {
            Function *Callee = CalleeEntry.first;
            if (!Callee) continue;
            for (CallInst *Call : Callee->getDirectCallSites()) {
                if (!Call || Call->getDirectCallee() != Callee) continue;
                Function *Caller = Call->getParentFunction();
                auto CallerIt = Summaries.find(Caller);
                if (CallerIt == Summaries.end()) continue;
                Changed |= mergeCalleeEffects(
                    Call, CalleeEntry.second, CallerIt->second);
            }
        }
    }
}
//3.缓存
SideEffectInfo SideEffectAnalysis::publishResults(
    Function &Root,
    std::unordered_map<Function *, SideEffectInfo> &Summaries,
    FunctionAnalysisPassManager &FAM) {
    auto ResultIt = Summaries.find(&Root);
    if (ResultIt == Summaries.end()) return SideEffectInfo{};
    for (auto &Entry : Summaries) {
        if (!Entry.first || Entry.first == &Root) continue;
        FAM.setCachedResult<SideEffectAnalysis>(
            *Entry.first, std::move(Entry.second));
    }
    return std::move(ResultIt->second);
}
//SideEffect主流程：
SideEffectInfo SideEffectAnalysis::run(
    Function &F, FunctionAnalysisPassManager &FAM) const {
    //1.收集可达函数的自身副作用
    std::unordered_map<Function *, SideEffectInfo> Summaries =
        collectReachableFunctionSummaries(F);
    //2.按照Call指令调用，将Callee的副作用映射到Caller，直到收敛
    solveToFixedPoint(Summaries);
    //3.缓存结果
    return publishResults(F, Summaries, FAM);
}
