#include "ir/analysis/SideEffect.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "lib/IRUtils.hpp"
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <queue>
//判断ModRef结果是否包含写效果
bool isModSet(ModRefInfo MRI) {
    return (unsigned(MRI) & unsigned(ModRefInfo::Mod)) != 0;
}
//判断ModRef结果是否包含读效果
bool isRefSet(ModRefInfo MRI) {
    return (unsigned(MRI) & unsigned(ModRefInfo::Ref)) != 0;
}
//判断ModRef结果是否不含任何读写效果
bool isNoModRef(ModRefInfo MRI) {
    return MRI == ModRefInfo::NoModRef;
}
//合并两个ModRef结果中的读写效果
ModRefInfo unionModRef(ModRefInfo A, ModRefInfo B) {
    return ModRefInfo(unsigned(A) | unsigned(B));
}
//根据Pass保留状态判断结果是否失效并在失效时清空摘要
bool SideEffectInfo::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(SideEffectAnalysis::ID())) return false;
    (void)F;
    ModSet.clear();
    RefSet.clear();
    ptrMap.clear();
    hasBuiltin = false;
    hasUnknownMemory = false;
    hasObservableEffect = false;
    hasOrderingBarrier = false;
    mayCrossObjectBounds = false;
    crossObjectModRef = ModRefInfo::NoModRef;
    return true;
}
//合并另一条指针来源信息并返回当前摘要是否发生变化
bool BaseAddressInfo::merge(const BaseAddressInfo& Other) {
    bool changed = false;
    for (auto* V : Other.bases)
        if (bases.insert(V).second) changed = true;
    if (Other.untracked && !untracked) { untracked = true; changed = true; }
    return changed;
}
//判断当前摘要是否既没有已知基址也没有未知来源
bool BaseAddressInfo::isEmpty() const {
    return bases.empty() && !untracked;
}
//将所有函数外可见的已知基址加入目标集合
void BaseAddressInfo::collectExternalBases(
    std::unordered_set<Value*>& Dest) const {
    for (auto* B : bases)
        if (B && B->isExternalVariable()) Dest.insert(B);
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
//为指针构造访问长度未知的内存位置
MemoryLocation SideEffectAnalysis::getUnknownSizeMemoryLocation(
    Value *Pointer) {
    return {Pointer, std::nullopt};
}
//从LLVM内存Intrinsic实参中提取可表示的常量访问长度
std::optional<std::size_t> SideEffectAnalysis::getConstantMemoryIntrinsicLength(
    const std::vector<Value *> &Args) {
    if (Args.size() < 3 || !Args[2]) return std::nullopt;
    Value *LengthValue = Args[2];
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
//调用效果先按运行时契约或被调函数摘要映射到调用点实际指针，再独立发布
//程序内存、外部可观察性和严格顺序屏障。
CallEffectSummary getCallEffectSummary(
    CallInst *CI, FunctionAnalysisPassManager &FAM) {
    CallEffectSummary Summary;
    if (!CI || !CI->getCallee()) {
        Summary.HasUnknownMemory = true;
        Summary.HasObservableEffect = true;
        Summary.HasOrderingBarrier = true;
        return Summary;
    }
    auto appendEffect = [&](Value *Pointer,
                            std::optional<std::size_t> Size,
                            ModRefInfo Effect) {
        if (isNoModRef(Effect)) return;
        if (!Pointer || !Pointer->isPointerValue()) {
            Summary.HasUnknownMemory = true;
            return;
        }
        for (CallMemoryEffect &Existing : Summary.ProgramMemory) {
            if (Existing.Location.Pointer == Pointer &&
                Existing.Location.Size == Size) {
                Existing.ModRef = unionModRef(Existing.ModRef, Effect);
                return;
            }
        }
        Summary.ProgramMemory.push_back(
            {MemoryLocation(Pointer, Size), Effect});
    };
    const std::vector<Value *> Args = CI->getArgs();
    if (BuildInFunction *BIF = CI->getBuiltinCallee()) {
        const std::string &Name = BIF->getIdent();
        if (BuildInFunction::isTimingName(Name)) {
            Summary.HasObservableEffect = true;
            Summary.HasOrderingBarrier = true;
            return Summary;
        }
        const IntrinsicID IID = getIntrinsicID(BIF);
        auto recordVolatileMemoryIntrinsic = [&]() {
            int32_t IsVolatile = 1;
            if (Args.size() < 4 || !Args[3] ||
                !Args[3]->tryGetIntConst(IsVolatile) ||
                IsVolatile != 0) {
                Summary.HasObservableEffect = true;
                Summary.HasOrderingBarrier = true;
            }
        };
        if (IID == IntrinsicID::Memcpy || IID == IntrinsicID::Memmove) {
            recordVolatileMemoryIntrinsic();
            if (Args.size() < 2) {
                Summary.HasUnknownMemory = true;
                return Summary;
            }
            const std::optional<std::size_t> Length =
                SideEffectAnalysis::getConstantMemoryIntrinsicLength(Args);
            if (!Length || *Length != 0) {
                appendEffect(Args[0], Length, ModRefInfo::Mod);
                appendEffect(Args[1], Length, ModRefInfo::Ref);
            }
            return Summary;
        }
        if (IID == IntrinsicID::Memset) {
            recordVolatileMemoryIntrinsic();
            if (Args.empty()) {
                Summary.HasUnknownMemory = true;
                return Summary;
            }
            const std::optional<std::size_t> Length =
                SideEffectAnalysis::getConstantMemoryIntrinsicLength(Args);
            if (!Length || *Length != 0)
                appendEffect(Args[0], Length, ModRefInfo::Mod);
            return Summary;
        }
        if (BuildInFunction::isArrayInputName(Name)) {
            Summary.HasObservableEffect = true;
            Summary.MayCrossObjectBounds = true;
            Summary.CrossObjectModRef = ModRefInfo::Mod;
            bool FoundPointer = false;
            for (Value *Arg : Args) {
                if (!Arg || !Arg->isPointerValue()) continue;
                FoundPointer = true;
                //SysY 运行时不承诺输入数量不超过源语言数组容量，长度必须未知。
                appendEffect(Arg, std::nullopt, ModRefInfo::Mod);
            }
            if (!FoundPointer) Summary.HasUnknownMemory = true;
            return Summary;
        }
        if (BuildInFunction::isArrayOutputName(Name)) {
            Summary.HasObservableEffect = true;
            Summary.MayCrossObjectBounds = true;
            Summary.CrossObjectModRef = ModRefInfo::Ref;
            bool FoundPointer = false;
            for (Value *Arg : Args) {
                if (!Arg || !Arg->isPointerValue()) continue;
                FoundPointer = true;
                appendEffect(Arg, std::nullopt, ModRefInfo::Ref);
            }
            if (!FoundPointer) Summary.HasUnknownMemory = true;
            return Summary;
        }
        if (BuildInFunction::isScalarIOName(Name) ||
            BuildInFunction::isOutputName(Name)) {
            Summary.HasObservableEffect = true;
            return Summary;
        }
        //未知运行时可能读写任意程序对象，并且不能跨越或删除。
        Summary.HasUnknownMemory = true;
        Summary.HasObservableEffect = true;
        Summary.HasOrderingBarrier = true;
        return Summary;
    }
    if (Function *Callee = CI->getDirectCallee()) {
        const SideEffectInfo &Info =
            FAM.getResult<SideEffectAnalysis>(*Callee);
        Summary.HasUnknownMemory = Info.hasUnknownMemory;
        Summary.HasObservableEffect = Info.hasObservableEffect;
        Summary.HasOrderingBarrier = Info.hasOrderingBarrier;
        Summary.MayCrossObjectBounds = Info.mayCrossObjectBounds;
        Summary.CrossObjectModRef = Info.crossObjectModRef;
        auto mapToActual = [&](Value *FormalOrGlobal) -> Value * {
            if (!FormalOrGlobal) return nullptr;
            if (FormalOrGlobal->isGlobal()) return FormalOrGlobal;
            if (!FormalOrGlobal->isParam()) return nullptr;
            const int Index = Callee->getParamIndex(FormalOrGlobal);
            if (Index < 0 ||
                static_cast<std::size_t>(Index) >= Args.size()) {
                return nullptr;
            }
            return Args[static_cast<std::size_t>(Index)];
        };
        for (Value *Modified : Info.ModSet) {
            Value *Actual = mapToActual(Modified);
            if (!Actual) {
                Summary.HasUnknownMemory = true;
                continue;
            }
            appendEffect(Actual, std::nullopt, ModRefInfo::Mod);
        }
        for (Value *Referenced : Info.RefSet) {
            Value *Actual = mapToActual(Referenced);
            if (!Actual) {
                Summary.HasUnknownMemory = true;
                continue;
            }
            appendEffect(Actual, std::nullopt, ModRefInfo::Ref);
        }
        return Summary;
    }
    Summary.HasUnknownMemory = true;
    Summary.HasObservableEffect = true;
    Summary.HasOrderingBarrier = true;
    return Summary;
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
        if (Value *Ptr = Load->getPointerOperand()) {
            if (overlapsLocation(MemoryLocation::get(Load)))
                Result = unionModRef(Result, ModRefInfo::Ref);
        }
        return Result;
    }
    if (auto *SI = dynamic_cast<StoreInst *>(I)) {
        if (Value *Ptr = SI->getPointerOperand()) {
            if (overlapsLocation(MemoryLocation::get(SI)))
                Result = unionModRef(Result, ModRefInfo::Mod);
        }
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
        SideEffectAnalysis::getUnknownSizeMemoryLocation(V), I, AA, LI, FAM);
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
//基本块查询逐条筛选不可移动操作，并对局部 Store 继续检查其后是否存在读取。
bool BasicBlockSideEffect(BasicBlock* BB,
                          const Loop& L,
                          BasicAAResult& AA,
                          const LoopInfo& LI,
                          FunctionAnalysisPassManager& FAM) {
    if (!BB) return false;
    for (auto It = BB->begin(); It != BB->end(); ++It) {
        Instruction* I = *It;
        if (!I) continue;
        if (I->instType == Instruction::RET || I->instType == Instruction::ALLOCA) {
            return true;
        }
        if (I->instType == Instruction::CALL) {
            auto *CI = dynamic_cast<CallInst *>(I);
            if (!CI || !CI->getCallee()) return true;
            CallEffectSummary Effects =
                getCallEffectSummary(CI, FAM);
            if (Effects.HasUnknownMemory ||
                Effects.HasObservableEffect ||
                Effects.HasOrderingBarrier)
                return true;
            if (Effects.MayCrossObjectBounds &&
                isModSet(Effects.CrossObjectModRef))
                return true;
            for (const CallMemoryEffect &Effect :
                 Effects.ProgramMemory)
                if (isModSet(Effect.ModRef)) return true;
            continue;
        }
        if (I->instType == Instruction::STORE) {
            auto* SI = dynamic_cast<StoreInst*>(I);
            if (!SI) return true;
            Value* Ptr = SI->getPointerOperand();
            if (!Ptr) return true;
            Value* Base = Value::stripPointerCastsAndGEP(Ptr);
            if (Base && (Base->isGlobal() || Base->isParam())) {
                return true;
            }
            if (!dynamic_cast<AllocaInst*>(Base)) {
                return true;
            }
            for (Instruction* Next = I->getNext(); Next != nullptr; Next = Next->getNext()) {
                ModRefInfo MRI = getInstModRefInfo(
                    MemoryLocation::get(SI), Next, AA, LI, FAM);
                if (isRefSet(MRI)) {
                    return true;
                }
            }
            std::vector<BasicBlock *> Roots = BB->getSuccessors();
            bool FoundRef = false;
            forEachReachablePreOrder<BasicBlock>(
                Roots, [&SI, &AA, &LI, &FAM,
                        &FoundRef](BasicBlock *Cur) {
                    for (Instruction *J : *Cur) {
                        if (!J) continue;
                        ModRefInfo MRI = getInstModRefInfo(
                            MemoryLocation::get(SI), J, AA, LI, FAM);
                        if (isRefSet(MRI)) {
                            FoundRef = true;
                            return TraversalControl::Stop;
                        }
                    }
                    return TraversalControl::Continue;
                });
            if (FoundRef) return true;
        }
    }
    (void)L;
    return false;
}
//循环查询展平子循环块；任一基本块失败即报告循环存在副作用。
bool LoopSideEffect(const Loop& L,
                    BasicAAResult& AA,
                    const LoopInfo& LI,
                    FunctionAnalysisPassManager& FAM) {
    for (BasicBlock *BB : L.getBlocksIncludingSubLoops()) {
        if (BasicBlockSideEffect(BB, L, AA, LI, FAM)) return true;
    }
    return false;
}
//为全局变量或函数形参初始化已知基址来源
void SideEffectAnalysis::ensureInitialized(Value* V, BaseAddressInfoMap& Map) {
    if (!V || Map.count(V)) return;
    if (V->isGlobal() || V->isParam())
        Map[V].bases.insert(V);
}
//指针来源求解先播种参数、栈对象和未知生产者，再沿局部指针槽 Load、
//GEP、BitCast、PHI、Select 传播到固定点。
BaseAddressInfoMap SideEffectAnalysis::buildBaseAddressInfoMap(Function& F) {
    //1. 参数和 Alloca 是已知基址；可传播指令先建立空格点，其他指针
    //生产者立即标为 untracked，避免 PHI 因扫描顺序静默丢掉未知来源。
    BaseAddressInfoMap Map;
    for (auto& P : F.getParams()) {
        Value* V = P.get();
        if (!V || !V->isPointerValue()) continue;
        Map[V].bases.insert(V);
    }
    //入口数组形参会先存入局部 ptr 槽再 Load。只有槽地址完全不逃逸、
    //所有使用均为直接 Load/Store 时，才流不敏感地合并全部存入来源；
    //任一别名、调用参数或把槽地址当值保存都会放弃该证明。
    std::unordered_map<LoadInst *, std::vector<Value *>>
        LocalPointerLoadSources;
    for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt) {
        for (auto IIt = (*BBIt)->begin(); IIt != (*BBIt)->end(); ++IIt) {
            auto *Slot = dynamic_cast<AllocaInst *>(*IIt);
            std::shared_ptr<IRType> AllocatedType =
                Slot ? Slot->getAllocatedType() : nullptr;
            if (!Slot || !AllocatedType || !AllocatedType->isPointer())
                continue;
            bool Trackable = true;
            std::vector<LoadInst *> Loads;
            std::vector<Value *> StoredPointers;
            for (Use *U : Slot->getUses()) {
                auto *UserInst =
                    U ? dynamic_cast<Instruction *>(U->getUser()) : nullptr;
                if (auto *Load = dynamic_cast<LoadInst *>(UserInst)) {
                    if (Load->getPointerOperand() == Slot &&
                        Load->isPointerValue()) {
                        Loads.push_back(Load);
                        continue;
                    }
                }
                if (auto *Store = dynamic_cast<StoreInst *>(UserInst)) {
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
            if (!Trackable) continue;
            for (LoadInst *Load : Loads)
                LocalPointerLoadSources.emplace(Load, StoredPointers);
        }
    }
    for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt) {
        for (auto IIt = (*BBIt)->begin(); IIt != (*BBIt)->end(); ++IIt) {
            Instruction *I = *IIt;
            if (!I || !I->isPointerValue()) continue;
            BaseAddressInfo &Seed = Map[I];
            if (dynamic_cast<AllocaInst *>(I)) {
                Seed.bases.insert(I);
                continue;
            }
            if (!dynamic_cast<GetElementPtrInst *>(I) &&
                !dynamic_cast<BitCastInst *>(I) &&
                !dynamic_cast<PhiInst *>(I) &&
                !dynamic_cast<SelectInst *>(I) &&
                !LocalPointerLoadSources.count(
                    dynamic_cast<LoadInst *>(I))) {
                Seed.untracked = true;
            }
        }
    }
    auto mergeSource = [&](Value *Source, BaseAddressInfo &Destination) {
        if (!Source) {
            Destination.untracked = true;
            return;
        }
        ensureInitialized(Source, Map);
        auto It = Map.find(Source);
        if (It == Map.end()) {
            //所有函数内指针指令均已预播种；这里缺失的来源不是尚未扫描的
            //PHI 环，而是无法跟踪的外部/常量指针值。
            Destination.untracked = true;
            return;
        }
        Destination.merge(It->second);
    };
    //2. 单调固定点合并每条可传播边。纯 PHI 环初始为空，待首轮收敛后
    //再提升为 untracked，避免把带已知入口的循环 PHI 误判为未知。
    bool SeededUnresolved = false;
    do {
        bool Changed = true;
        while (Changed) {
            Changed = false;
            for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt) {
                for (auto IIt = (*BBIt)->begin();
                     IIt != (*BBIt)->end(); ++IIt) {
                    Instruction *I = *IIt;
                    if (!I || !I->isPointerValue() ||
                        dynamic_cast<AllocaInst *>(I)) {
                        continue;
                    }
                    BaseAddressInfo NewInfo;
                    if (auto *Load = dynamic_cast<LoadInst *>(I)) {
                        auto It = LocalPointerLoadSources.find(Load);
                        if (It == LocalPointerLoadSources.end()) continue;
                        for (Value *Stored : It->second)
                            mergeSource(Stored, NewInfo);
                    } else if (auto *GEP =
                            dynamic_cast<GetElementPtrInst *>(I)) {
                        mergeSource(GEP->getBasePointer(), NewInfo);
                    } else if (auto *BC =
                                   dynamic_cast<BitCastInst *>(I)) {
                        mergeSource(BC->getUnaryOperand(), NewInfo);
                    } else if (auto *Phi =
                                   dynamic_cast<PhiInst *>(I)) {
                        forEachPhiIncoming(
                            Phi, [&](unsigned, Value *V, BasicBlock *) {
                                mergeSource(V, NewInfo);
                            });
                    } else if (auto *Select =
                                   dynamic_cast<SelectInst *>(I)) {
                        mergeSource(Select->getTrueValue(), NewInfo);
                        mergeSource(Select->getFalseValue(), NewInfo);
                    } else {
                        continue;
                    }
                    if (Map[I].merge(NewInfo)) Changed = true;
                }
            }
        }
        SeededUnresolved = false;
        for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt) {
            for (auto IIt = (*BBIt)->begin();
                 IIt != (*BBIt)->end(); ++IIt) {
                Instruction *I = *IIt;
                if (!I || !I->isPointerValue()) continue;
                BaseAddressInfo &Info = Map[I];
                if (!Info.isEmpty()) continue;
                Info.untracked = true;
                SeededUnresolved = true;
            }
        }
    } while (SeededUnresolved);
    return Map;
}
//内置调用摘要按参数角色把调用者基址映射到 ModSet/RefSet。
void SideEffectAnalysis::processBuiltinCall(
    BuildInFunction* BIF,
    const std::vector<Value*>& Args,
    const BaseAddressInfoMap& CallerPtrMap,
    SideEffectInfo& Info)
{
    auto recordVolatileMemoryIntrinsic = [&]() {
        int32_t IsVolatile = 1;
        if (Args.size() < 4 || !Args[3] ||
            !Args[3]->tryGetIntConst(IsVolatile) ||
            IsVolatile != 0) {
            Info.hasBuiltin = true;
            Info.hasObservableEffect = true;
            Info.hasOrderingBarrier = true;
        }
    };
    //计时函数不读写 SysY 程序对象，但调用本身可观察且必须保持顺序。
    if (BIF && BuildInFunction::isTimingName(BIF->getIdent())) {
        Info.hasBuiltin = true;
        Info.hasObservableEffect = true;
        Info.hasOrderingBarrier = true;
        return;
    }
    if (getIntrinsicID(BIF) == IntrinsicID::Memcpy ||
        getIntrinsicID(BIF) == IntrinsicID::Memmove) {
        recordVolatileMemoryIntrinsic();
        if (Args.size() < 2) {
            Info.hasBuiltin = true;
            Info.hasUnknownMemory = true;
            return;
        }
        const std::optional<std::size_t> Length =
            getConstantMemoryIntrinsicLength(Args);
        if (Length && *Length == 0) return;
        auto addArgBases = [&](Value* Arg, std::unordered_set<Value*>& Dest) {
            if (!Arg || !Arg->isPointerValue()) {
                Info.hasBuiltin = true;
                Info.hasUnknownMemory = true;
                return;
            }
            auto it = CallerPtrMap.find(Arg);
            if (it != CallerPtrMap.end()) {
                it->second.collectExternalBases(Dest);
                if (it->second.untracked) {
                    Info.hasBuiltin = true;
                    Info.hasUnknownMemory = true;
                }
            } else if (Arg->isExternalVariable()) {
                Dest.insert(Arg);
            } else {
                Info.hasBuiltin = true;
                Info.hasUnknownMemory = true;
            }
        };
        if (Args.size() >= 1) addArgBases(Args[0], Info.ModSet);
        if (Args.size() >= 2) addArgBases(Args[1], Info.RefSet);
        return;
    }
    if (getIntrinsicID(BIF) == IntrinsicID::Memset) {
        recordVolatileMemoryIntrinsic();
        if (Args.empty() || !Args[0] ||
            !Args[0]->isPointerValue()) {
            Info.hasBuiltin = true;
            Info.hasUnknownMemory = true;
            return;
        }
        const std::optional<std::size_t> Length =
            getConstantMemoryIntrinsicLength(Args);
        if (Length && *Length == 0) return;
        {
            auto it = CallerPtrMap.find(Args[0]);
            if (it != CallerPtrMap.end()) {
                it->second.collectExternalBases(Info.ModSet);
                if (it->second.untracked) {
                    Info.hasBuiltin = true;
                    Info.hasUnknownMemory = true;
                }
            } else if (Args[0]->isExternalVariable()) {
                Info.ModSet.insert(Args[0]);
            } else {
                Info.hasBuiltin = true;
                Info.hasUnknownMemory = true;
            }
        }
        return;
    }
    FuncAttr Attr = classifyBuiltin(BIF);
    if (Attr == FuncAttr::Sylib) {
        Info.hasBuiltin = true;
        Info.hasObservableEffect = true;
        return;
    }
    if (Attr == FuncAttr::NoModRef) { return; }
    bool DoMod = (Attr == FuncAttr::MemWriteOnly || Attr == FuncAttr::MemReadWrite);
    bool DoRef = (Attr == FuncAttr::MemReadOnly  || Attr == FuncAttr::MemReadWrite);
    if (DoMod || DoRef) {
        //数组 I/O 的长度来自运行时输入或显式 N，规范不保证它落在实参数组容量内。
        if (BIF && (BuildInFunction::isArrayInputName(BIF->getIdent()) ||
                    BuildInFunction::isArrayOutputName(BIF->getIdent()))) {
            Info.mayCrossObjectBounds = true;
            ModRefInfo CrossEffect = ModRefInfo::NoModRef;
            if (DoMod)
                CrossEffect = unionModRef(CrossEffect, ModRefInfo::Mod);
            if (DoRef)
                CrossEffect = unionModRef(CrossEffect, ModRefInfo::Ref);
            Info.crossObjectModRef =
                unionModRef(Info.crossObjectModRef, CrossEffect);
        }
        bool FoundPointer = false;
        for (auto* Arg : Args) {
            if (!Arg || !Arg->isPointerValue()) continue;
            FoundPointer = true;
            auto it = CallerPtrMap.find(Arg);
            if (it != CallerPtrMap.end()) {
                if (DoMod) it->second.collectExternalBases(Info.ModSet);
                if (DoRef) it->second.collectExternalBases(Info.RefSet);
                if (it->second.untracked)
                    Info.hasUnknownMemory = true;
            } else if (Arg->isExternalVariable()) {
                if (DoMod) Info.ModSet.insert(Arg);
                if (DoRef) Info.RefSet.insert(Arg);
            } else {
                Info.hasUnknownMemory = true;
            }
        }
        if (!FoundPointer) Info.hasUnknownMemory = true;
        Info.hasBuiltin = true;
        Info.hasObservableEffect = true;
        return;
    }
    for (auto* Arg : Args) {
        if (!Arg || !Arg->isPointerValue()) continue;
        auto it = CallerPtrMap.find(Arg);
        if (it != CallerPtrMap.end()) {
            it->second.collectExternalBases(Info.ModSet);
            it->second.collectExternalBases(Info.RefSet);
        } else if (Arg->isExternalVariable()) {
            Info.ModSet.insert(Arg);
            Info.RefSet.insert(Arg);
        }
    }
    Info.hasBuiltin = true;
    Info.hasUnknownMemory = true;
    Info.hasObservableEffect = true;
    Info.hasOrderingBarrier = true;
}
//跨函数副作用分析主流程显式求解可达调用图，避免递归查询分析缓存。
SideEffectInfo SideEffectAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    //1. 广度发现 F 可达的直接调用图，并为每个函数收集局部 Load/Store/内置调用效果。
    std::unordered_map<Function*, SideEffectFunctionState> States;
    std::queue<Function*> WorkList;
    std::unordered_set<Function*> Enqueued;
    WorkList.push(&F);
    Enqueued.insert(&F);
    while (!WorkList.empty()) {
        Function *Current = WorkList.front();
        WorkList.pop();
        if (!Current || States.count(Current)) continue;
        SideEffectFunctionState State;
        SideEffectInfo &Info = State.Info;
        Info.ptrMap = buildBaseAddressInfoMap(*Current);
        //1.1 用户调用只记录传递边；未知间接调用立即提升为保守屏障。
        for (auto BBIt = Current->begin(); BBIt != Current->end(); ++BBIt) {
            BasicBlock *BB = *BBIt;
            for (auto IIt = BB->begin(); IIt != BB->end(); ++IIt) {
                Instruction *I = *IIt;
                if (auto *LI = dynamic_cast<LoadInst *>(I)) {
                    if (Value *Ptr = LI->getPointerOperand()) {
                        auto It = Info.ptrMap.find(Ptr);
                        if (It != Info.ptrMap.end()) {
                            It->second.collectExternalBases(Info.RefSet);
                            if (It->second.untracked) {
                                Info.hasBuiltin = true;
                                Info.hasUnknownMemory = true;
                            }
                        } else if (Ptr->isExternalVariable()) {
                            Info.RefSet.insert(Ptr);
                        } else {
                            Info.hasBuiltin = true;
                            Info.hasUnknownMemory = true;
                        }
                    }
                    continue;
                }
                if (auto *SI = dynamic_cast<StoreInst *>(I)) {
                    if (Value *Ptr = SI->getPointerOperand()) {
                        auto It = Info.ptrMap.find(Ptr);
                        if (It != Info.ptrMap.end()) {
                            It->second.collectExternalBases(Info.ModSet);
                            if (It->second.untracked) {
                                Info.hasBuiltin = true;
                                Info.hasUnknownMemory = true;
                            }
                        } else if (Ptr->isExternalVariable()) {
                            Info.ModSet.insert(Ptr);
                        } else {
                            Info.hasBuiltin = true;
                            Info.hasUnknownMemory = true;
                        }
                    }
                    continue;
                }
                auto *CI = dynamic_cast<CallInst *>(I);
                if (!CI) continue;
                std::vector<Value*> Args = CI->getArgs();
                if (Function *Callee = CI->getDirectCallee()) {
                    State.Calls.push_back({Callee, std::move(Args)});
                    if (Enqueued.insert(Callee).second)
                        WorkList.push(Callee);
                } else if (BuildInFunction *BIF = CI->getBuiltinCallee()) {
                    processBuiltinCall(BIF, Args, Info.ptrMap, Info);
                } else {
                    Info.hasBuiltin = true;
                    Info.hasUnknownMemory = true;
                    Info.hasObservableEffect = true;
                    Info.hasOrderingBarrier = true;
                }
            }
        }
        States.emplace(Current, std::move(State));
    }
    auto insertExternalBases = [](const BaseAddressInfo &Bases,
                                  std::unordered_set<Value*> &Dest) {
        bool Changed = false;
        for (Value *Base : Bases.bases) {
            if (Base && Base->isExternalVariable())
                Changed |= Dest.insert(Base).second;
        }
        return Changed;
    };
    //2. 单调固定点把被调函数的全局和形参效果映射回每个调用者。
    bool Changed = true;
    while (Changed) {
        Changed = false;
        for (auto &Entry : States) {
            SideEffectFunctionState &CallerState = Entry.second;
            SideEffectInfo &CallerInfo = CallerState.Info;
            for (const SideEffectPendingCall &Call : CallerState.Calls) {
                auto CalleeIt = States.find(Call.Callee);
                if (CalleeIt == States.end()) {
                    if (!CallerInfo.hasBuiltin) {
                        CallerInfo.hasBuiltin = true;
                        Changed = true;
                    }
                    if (!CallerInfo.hasUnknownMemory) {
                        CallerInfo.hasUnknownMemory = true;
                        Changed = true;
                    }
                    if (!CallerInfo.hasObservableEffect) {
                        CallerInfo.hasObservableEffect = true;
                        Changed = true;
                    }
                    if (!CallerInfo.hasOrderingBarrier) {
                        CallerInfo.hasOrderingBarrier = true;
                        Changed = true;
                    }
                    continue;
                }
                const SideEffectInfo &CalleeInfo = CalleeIt->second.Info;
                //2.1 递归边先快照被调摘要，避免边遍历边扩容同一集合。
                const std::vector<Value*> CalleeMods(
                    CalleeInfo.ModSet.begin(), CalleeInfo.ModSet.end());
                const std::vector<Value*> CalleeRefs(
                    CalleeInfo.RefSet.begin(), CalleeInfo.RefSet.end());
                const bool CalleeHasBuiltin = CalleeInfo.hasBuiltin;
                const bool CalleeHasUnknownMemory =
                    CalleeInfo.hasUnknownMemory;
                const bool CalleeHasObservableEffect =
                    CalleeInfo.hasObservableEffect;
                const bool CalleeHasOrderingBarrier =
                    CalleeInfo.hasOrderingBarrier;
                const bool CalleeMayCrossObjectBounds =
                    CalleeInfo.mayCrossObjectBounds;
                const ModRefInfo CalleeCrossObjectModRef =
                    CalleeInfo.crossObjectModRef;
                auto mapEntry = [&](Value *V,
                                    std::unordered_set<Value*> &Dest) {
                    auto markUnknownMapping = [&]() {
                        bool LocalChanged = false;
                        if (!CallerInfo.hasBuiltin) {
                            CallerInfo.hasBuiltin = true;
                            LocalChanged = true;
                        }
                        if (!CallerInfo.hasUnknownMemory) {
                            CallerInfo.hasUnknownMemory = true;
                            LocalChanged = true;
                        }
                        return LocalChanged;
                    };
                    if (!V) return markUnknownMapping();
                    if (V->isGlobal()) return Dest.insert(V).second;
                    if (!V->isParam()) return markUnknownMapping();
                    int Index = Call.Callee->getParamIndex(V);
                    if (Index < 0 ||
                        static_cast<size_t>(Index) >= Call.Args.size())
                        return markUnknownMapping();
                    Value *Arg = Call.Args[static_cast<size_t>(Index)];
                    if (!Arg) return markUnknownMapping();
                    auto ArgIt = CallerInfo.ptrMap.find(Arg);
                    if (ArgIt != CallerInfo.ptrMap.end()) {
                        bool LocalChanged =
                            insertExternalBases(ArgIt->second, Dest);
                        if (ArgIt->second.untracked) {
                            if (!CallerInfo.hasBuiltin) {
                                CallerInfo.hasBuiltin = true;
                                LocalChanged = true;
                            }
                            if (!CallerInfo.hasUnknownMemory) {
                                CallerInfo.hasUnknownMemory = true;
                                LocalChanged = true;
                            }
                        }
                        return LocalChanged;
                    }
                    if (Arg->isExternalVariable())
                        return Dest.insert(Arg).second;
                    if (Arg->isPointerValue()) {
                        return markUnknownMapping();
                    }
                    return markUnknownMapping();
                };
                for (Value *V : CalleeMods)
                    Changed |= mapEntry(V, CallerInfo.ModSet);
                for (Value *V : CalleeRefs)
                    Changed |= mapEntry(V, CallerInfo.RefSet);
                if (CalleeHasBuiltin && !CallerInfo.hasBuiltin) {
                    CallerInfo.hasBuiltin = true;
                    Changed = true;
                }
                if (CalleeHasUnknownMemory &&
                    !CallerInfo.hasUnknownMemory) {
                    CallerInfo.hasUnknownMemory = true;
                    Changed = true;
                }
                if (CalleeHasObservableEffect &&
                    !CallerInfo.hasObservableEffect) {
                    CallerInfo.hasObservableEffect = true;
                    Changed = true;
                }
                if (CalleeHasOrderingBarrier &&
                    !CallerInfo.hasOrderingBarrier) {
                    CallerInfo.hasOrderingBarrier = true;
                    Changed = true;
                }
                if (CalleeMayCrossObjectBounds &&
                    !CallerInfo.mayCrossObjectBounds) {
                    CallerInfo.mayCrossObjectBounds = true;
                    Changed = true;
                }
                const ModRefInfo MergedCrossObjectModRef =
                    unionModRef(CallerInfo.crossObjectModRef,
                                CalleeCrossObjectModRef);
                if (MergedCrossObjectModRef !=
                    CallerInfo.crossObjectModRef) {
                    CallerInfo.crossObjectModRef =
                        MergedCrossObjectModRef;
                    Changed = true;
                }
            }
        }
    }
    //3. 求解结束后批量发布伴随函数摘要，请求函数由管理器正常接收返回值。
    auto ResultIt = States.find(&F);
    if (ResultIt == States.end()) return SideEffectInfo{};
    for (auto &Entry : States) {
        if (Entry.first == &F) continue;
        FAM.setCachedResult<SideEffectAnalysis>(
            *Entry.first, std::move(Entry.second.Info));
    }
    return std::move(ResultIt->second.Info);
}
