#include "ir/analysis/SideEffect.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "lib/IRUtils.hpp"
#include <cstdlib>
#include <queue>

PassID SideEffectAnalysis::Key;

bool isModSet(ModRefInfo MRI) {
    return (unsigned(MRI) & unsigned(ModRefInfo::Mod)) != 0;
}

bool isRefSet(ModRefInfo MRI) {
    return (unsigned(MRI) & unsigned(ModRefInfo::Ref)) != 0;
}

bool isNoModRef(ModRefInfo MRI) {
    return MRI == ModRefInfo::NoModRef;
}

ModRefInfo unionModRef(ModRefInfo A, ModRefInfo B) {
    return ModRefInfo(unsigned(A) | unsigned(B));
}

bool SideEffectInfo::invalidate(Function &F, const PreservationStatus &PA) {
    if (PA.isKept(SideEffectAnalysis::ID())) return false;
    (void)F;
    ModSet.clear();
    RefSet.clear();
    ptrMap.clear();
    hasBuiltin = false;
    return true;
}

bool BaseAddressInfo::merge(const BaseAddressInfo& Other) {
    bool changed = false;
    for (auto* V : Other.bases)
        if (bases.insert(V).second) changed = true;
    if (Other.untracked && !untracked) { untracked = true; changed = true; }
    return changed;
}

bool BaseAddressInfo::isEmpty() const {
    return bases.empty() && !untracked;
}

void SideEffectAnalysis::ensureInitialized(Value* V, BaseAddressInfoMap& Map) {
    if (!V || Map.count(V)) return;
    if (V->isGlobal() || V->isParam())
        Map[V].bases.insert(V);
}

void BaseAddressInfo::collectExternalBases(
    std::unordered_set<Value*>& Dest) const {
    for (auto* B : bases)
        if (B && B->isExternalVariable()) Dest.insert(B);
}

IntrinsicID getIntrinsicID(BuildInFunction* BIF) {
    if (!BIF) return IntrinsicID::None;
    const std::string& N = BIF->getIdent();
    if (BuildInFunction::isMemcpyName(N))
        return IntrinsicID::Memcpy;
    if (BuildInFunction::isMemmoveName(N))
        return IntrinsicID::Memmove;
    if (BuildInFunction::isMemsetName(N))
        return IntrinsicID::Memset;
    return IntrinsicID::None;
}

// 指针来源求解先播种参数与栈对象，再沿 GEP、BitCast、PHI 传播到固定点。
BaseAddressInfoMap SideEffectAnalysis::buildBaseAddressInfoMap(Function& F) {
    // 1. 参数和 Alloca 是已知基址，作为单调传播的初始种子。
    BaseAddressInfoMap Map;

    for (auto& P : F.getParams()) {
        Value* V = P.get();
        if (!V || !V->isPointerValue()) continue;
        Map[V].bases.insert(V);
    }

    for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt)
        for (auto IIt = (*BBIt)->begin(); IIt != (*BBIt)->end(); ++IIt)
            if (dynamic_cast<AllocaInst*>(*IIt))
                Map[*IIt].bases.insert(*IIt);

    // 2. 反复扫描指针产生指令；未知生产者标记 untracked，PHI 合并全部可达来源。
    bool changed = true;
    while (changed) {
        changed = false;

        for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt) {
            for (auto IIt = (*BBIt)->begin(); IIt != (*BBIt)->end(); ++IIt) {
                Instruction* I = *IIt;

                if (dynamic_cast<AllocaInst*>(I)) continue;
                if (!I || !I->isPointerValue()) continue;

                BaseAddressInfo NewInfo;

                if (auto *GEP = dynamic_cast<GetElementPtrInst*>(I)) {
                    if (Value *Base = GEP->getBasePointer()) {
                        ensureInitialized(Base, Map);
                        auto it = Map.find(Base);
                        if (it != Map.end()) NewInfo = it->second;
                        else                 NewInfo.untracked = true;
                    }

                } else if (auto *BC = dynamic_cast<BitCastInst*>(I)) {
                    if (Value *Src = BC->getUnaryOperand()) {
                        ensureInitialized(Src, Map);
                        auto it = Map.find(Src);
                        if (it != Map.end()) NewInfo = it->second;
                        else                 NewInfo.untracked = true;
                    }

                } else if (auto* Phi = dynamic_cast<PhiInst*>(I)) {
                    forEachPhiIncoming(
                        Phi, [&](unsigned, Value *V, BasicBlock *) {
                        ensureInitialized(V, Map);
                        auto it = Map.find(V);
                        if (it != Map.end()) {

                            NewInfo.merge(it->second);
                        }

                    });

                    {
                        auto& Existing = Map[I];
                        bool different = (Existing.untracked != NewInfo.untracked) ||
                                         (Existing.bases != NewInfo.bases);
                        if (different) { Existing = NewInfo; changed = true; }
                    }
                    continue;
                } else {

                    NewInfo.untracked = true;
                }

                if (Map[I].merge(NewInfo)) changed = true;
            }
        }
    }
    return Map;
}

// 内置函数分类只发布已知契约；无法识别的名字退化为 Untracked。
FuncAttr classifyBuiltin(BuildInFunction* BIF) {
    if (!BIF) return FuncAttr::Untracked;
    const std::string& N = BIF->getIdent();

    if (BuildInFunction::isTimingName(N)) return FuncAttr::Sylib;

    if (BuildInFunction::isScalarIOName(N)) return FuncAttr::Sylib;

    if (BuildInFunction::isArrayInputName(N)) return FuncAttr::MemWriteOnly;

    if (BuildInFunction::isArrayOutputName(N)) return FuncAttr::MemReadOnly;

    if (BuildInFunction::isMemcpyName(N) ||
        BuildInFunction::isMemmoveName(N))
        return FuncAttr::MemReadWrite;

    if (BuildInFunction::isMemsetName(N)) return FuncAttr::MemWriteOnly;

    return FuncAttr::Untracked;
}

// 内置调用摘要按参数角色把调用者基址映射到 ModSet/RefSet。
void SideEffectAnalysis::processBuiltinCall(
    BuildInFunction* BIF,
    const std::vector<Value*>& Args,
    const BaseAddressInfoMap& CallerPtrMap,
    SideEffectInfo& Info)
{

    if (getIntrinsicID(BIF) == IntrinsicID::Memcpy ||
        getIntrinsicID(BIF) == IntrinsicID::Memmove) {
        auto addArgBases = [&](Value* Arg, std::unordered_set<Value*>& Dest) {
            if (!Arg) return;
            auto it = CallerPtrMap.find(Arg);
            if (it != CallerPtrMap.end()) it->second.collectExternalBases(Dest);
            else if (Arg && Arg->isExternalVariable())   Dest.insert(Arg);
        };
        if (Args.size() >= 1) addArgBases(Args[0], Info.ModSet);
        if (Args.size() >= 2) addArgBases(Args[1], Info.RefSet);
        return;
    }

    if (getIntrinsicID(BIF) == IntrinsicID::Memset) {
        if (!Args.empty() && Args[0]) {
            auto it = CallerPtrMap.find(Args[0]);
            if (it != CallerPtrMap.end())
                it->second.collectExternalBases(Info.ModSet);
            else if (Args[0]->isExternalVariable())
                Info.ModSet.insert(Args[0]);
        }
        return;
    }

    FuncAttr Attr = classifyBuiltin(BIF);

    if (Attr == FuncAttr::Sylib)    { Info.hasBuiltin = true; return; }
    if (Attr == FuncAttr::NoModRef) { return; }

    bool DoMod = (Attr == FuncAttr::MemWriteOnly || Attr == FuncAttr::MemReadWrite);
    bool DoRef = (Attr == FuncAttr::MemReadOnly  || Attr == FuncAttr::MemReadWrite);

    if (DoMod || DoRef) {
        for (auto* Arg : Args) {
            if (!Arg || !Arg->isPointerValue()) continue;
            auto it = CallerPtrMap.find(Arg);
            if (it != CallerPtrMap.end()) {
                if (DoMod) it->second.collectExternalBases(Info.ModSet);
                if (DoRef) it->second.collectExternalBases(Info.RefSet);
            } else if (Arg->isExternalVariable()) {
                if (DoMod) Info.ModSet.insert(Arg);
                if (DoRef) Info.RefSet.insert(Arg);
            }
        }
        Info.hasBuiltin = true;
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
}

// 跨函数副作用分析主流程显式求解可达调用图，避免递归查询分析缓存。
SideEffectInfo SideEffectAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    // 1. 广度发现 F 可达的直接调用图，并为每个函数收集局部 Load/Store/内置调用效果。
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

        // 1.1 用户调用只记录传递边；未知间接调用立即提升为保守屏障。
        for (auto BBIt = Current->begin(); BBIt != Current->end(); ++BBIt) {
            BasicBlock *BB = *BBIt;
            for (auto IIt = BB->begin(); IIt != BB->end(); ++IIt) {
                Instruction *I = *IIt;

                if (auto *LI = dynamic_cast<LoadInst *>(I)) {
                    if (Value *Ptr = LI->getPointerOperand()) {
                        auto It = Info.ptrMap.find(Ptr);
                        if (It != Info.ptrMap.end()) {
                            It->second.collectExternalBases(Info.RefSet);
                            if (It->second.untracked)
                                Info.hasBuiltin = true;
                        } else if (Ptr->isExternalVariable()) {
                            Info.RefSet.insert(Ptr);
                        } else {
                            Info.hasBuiltin = true;
                        }
                    }
                    continue;
                }

                if (auto *SI = dynamic_cast<StoreInst *>(I)) {
                    if (Value *Ptr = SI->getPointerOperand()) {
                        auto It = Info.ptrMap.find(Ptr);
                        if (It != Info.ptrMap.end()) {
                            It->second.collectExternalBases(Info.ModSet);
                            if (It->second.untracked)
                                Info.hasBuiltin = true;
                        } else if (Ptr->isExternalVariable()) {
                            Info.ModSet.insert(Ptr);
                        } else {
                            Info.hasBuiltin = true;
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

    // 2. 单调固定点把被调函数的全局和形参效果映射回每个调用者。
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
                    continue;
                }

                const SideEffectInfo &CalleeInfo = CalleeIt->second.Info;

                // 2.1 递归边先快照被调摘要，避免边遍历边扩容同一集合。
                const std::vector<Value*> CalleeMods(
                    CalleeInfo.ModSet.begin(), CalleeInfo.ModSet.end());
                const std::vector<Value*> CalleeRefs(
                    CalleeInfo.RefSet.begin(), CalleeInfo.RefSet.end());
                const bool CalleeHasBuiltin = CalleeInfo.hasBuiltin;
                auto mapEntry = [&](Value *V,
                                    std::unordered_set<Value*> &Dest) {
                    if (!V) return false;
                    if (V->isGlobal()) return Dest.insert(V).second;
                    if (!V->isParam()) return false;

                    int Index = Call.Callee->getParamIndex(V);
                    if (Index < 0 ||
                        static_cast<size_t>(Index) >= Call.Args.size())
                        return false;

                    Value *Arg = Call.Args[static_cast<size_t>(Index)];
                    if (!Arg) return false;
                    auto ArgIt = CallerInfo.ptrMap.find(Arg);
                    if (ArgIt != CallerInfo.ptrMap.end()) {
                        bool LocalChanged =
                            insertExternalBases(ArgIt->second, Dest);

                        if (ArgIt->second.untracked &&
                            !CallerInfo.hasBuiltin) {
                            CallerInfo.hasBuiltin = true;
                            LocalChanged = true;
                        }
                        return LocalChanged;
                    }
                    if (Arg->isExternalVariable())
                        return Dest.insert(Arg).second;
                    if (Arg->isPointerValue() && !CallerInfo.hasBuiltin) {
                        CallerInfo.hasBuiltin = true;
                        return true;
                    }
                    return false;
                };

                for (Value *V : CalleeMods)
                    Changed |= mapEntry(V, CallerInfo.ModSet);
                for (Value *V : CalleeRefs)
                    Changed |= mapEntry(V, CallerInfo.RefSet);
                if (CalleeHasBuiltin && !CallerInfo.hasBuiltin) {
                    CallerInfo.hasBuiltin = true;
                    Changed = true;
                }
            }
        }
    }

    // 3. 求解结束后批量发布伴随函数摘要，请求函数由管理器正常接收返回值。
    auto ResultIt = States.find(&F);
    if (ResultIt == States.end()) return SideEffectInfo{};

    for (auto &Entry : States) {
        if (Entry.first == &F) continue;
        FAM.setCachedResult<SideEffectAnalysis>(
            *Entry.first, std::move(Entry.second.Info));
    }
    return std::move(ResultIt->second.Info);
}

MemoryLocation SideEffectAnalysis::getUnknownSizeMemoryLocation(
    Value *Pointer) {
    return {Pointer, std::nullopt};
}

std::optional<std::size_t> SideEffectAnalysis::getConstantMemoryIntrinsicLength(
    const std::vector<Value *> &Args) {
    if (Args.size() < 3 || !Args[2]) return std::nullopt;
    int32_t RawLength = 0;
    if (!Args[2]->tryGetIntConst(RawLength)) return std::nullopt;

    return static_cast<std::size_t>(static_cast<uint32_t>(RawLength));
}

// 指令级 ModRef 流程：先构造区间重叠判定，再按 Load、Store、Call 分类求效果。
ModRefInfo getInstModRefInfo(const MemoryLocation &Location, Instruction* I,
                             BasicAAResult& AA, const LoopInfo& LI,
                             FunctionAnalysisPassManager& FAM)
{
    Value *V = Location.Pointer;
    if (!V || !I) return ModRefInfo::ModRef;
    ModRefInfo Result = ModRefInfo::NoModRef;

    // 1. 已知长度使用完整字节区间；未知长度只接受底层对象不同这一强 NoAlias 证明。
    auto overlaps = [&](Value* Ptr) -> bool {
        return Ptr && !AA.haveKnownDistinctUnderlyingObjects(V, Ptr);
    };
    auto overlapsLocation = [&](const MemoryLocation &Other) -> bool {
        if (!Other.Pointer) return true;
        if (!Location.Size || !Other.Size)
            return !AA.haveKnownDistinctUnderlyingObjects(V, Other.Pointer);
        return AA.aliasWithIndexRange(Location, Other, LI) !=
               AliasInfo::NoAlias;
    };

    auto overlapsMemoryIntrinsicRange =
        [&](Value *Start, const std::optional<std::size_t> &Length) -> bool {
        if (!Start) return true;
        if (Length && *Length == 0) return false;
        if (Length && Location.Size) {
            return overlapsLocation(MemoryLocation(Start, *Length));
        }
        return !AA.haveKnownDistinctUnderlyingObjects(V, Start);
    };

    // 2. Load/Store 直接与目标 Location 比较并累加 Ref 或 Mod。
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

    // 3. Call 依次处理观测屏障、内存内建、用户函数摘要和未知被调目标。
    if (I->instType == Instruction::CALL) {
        auto *CI = dynamic_cast<CallInst *>(I);
        if (!CI) return ModRefInfo::ModRef;

        if (CI->mayReachTimingCall()) return ModRefInfo::ModRef;

        std::vector<Value*> Args = CI->getArgs();

        if (auto* BIF = CI->getBuiltinCallee()) {

            if (getIntrinsicID(BIF) == IntrinsicID::Memcpy ||
                getIntrinsicID(BIF) == IntrinsicID::Memmove) {
                if (Args.size() < 2 || !Args[0] || !Args[1])
                    return ModRefInfo::ModRef;
                std::optional<std::size_t> Length =
                    SideEffectAnalysis::getConstantMemoryIntrinsicLength(Args);
                if (overlapsMemoryIntrinsicRange(Args[0], Length))
                    Result = unionModRef(Result, ModRefInfo::Mod);
                if (Result == ModRefInfo::ModRef) return Result;
                if (overlapsMemoryIntrinsicRange(Args[1], Length))
                    Result = unionModRef(Result, ModRefInfo::Ref);
                return Result;
            }
            if (getIntrinsicID(BIF) == IntrinsicID::Memset) {
                if (Args.empty() || !Args[0]) return ModRefInfo::ModRef;
                std::optional<std::size_t> Length =
                    SideEffectAnalysis::getConstantMemoryIntrinsicLength(Args);
                if (overlapsMemoryIntrinsicRange(Args[0], Length))
                    Result = unionModRef(Result, ModRefInfo::Mod);
                return Result;
            }

            FuncAttr Attr = classifyBuiltin(BIF);
            if (Attr == FuncAttr::Sylib || Attr == FuncAttr::NoModRef)
                return ModRefInfo::NoModRef;

            bool DoMod = (Attr == FuncAttr::MemWriteOnly || Attr == FuncAttr::MemReadWrite);
            bool DoRef = (Attr == FuncAttr::MemReadOnly  || Attr == FuncAttr::MemReadWrite);

            if (DoMod || DoRef) {
                for (auto* Arg : Args) {
                    if (!Arg || !Arg->isPointerValue())
                        continue;
                    if (overlaps(Arg)) {
                        if (DoMod) Result = unionModRef(Result, ModRefInfo::Mod);
                        if (Result == ModRefInfo::ModRef) return Result;
                        if (DoRef) Result = unionModRef(Result, ModRefInfo::Ref);
                        if (Result == ModRefInfo::ModRef) return Result;
                    }
                }
                return Result;
            }

            return ModRefInfo::ModRef;
        }

        if (auto* CalleeFunc = CI->getDirectCallee()) {
            const SideEffectInfo& SEI = FAM.getResult<SideEffectAnalysis>(*CalleeFunc);

            if (SEI.hasBuiltin)
                return ModRefInfo::ModRef;

            for (Value* MV : SEI.ModSet) {
                if (!MV) continue;
                if (MV->isGlobal()) {
                    if (overlaps(MV))
                        Result = unionModRef(Result, ModRefInfo::Mod);
                } else if (MV->isParam()) {
                    int Index = CalleeFunc->getParamIndex(MV);
                    if (Index >= 0 && static_cast<size_t>(Index) < Args.size() &&
                        Args[static_cast<size_t>(Index)] &&
                        overlaps(Args[static_cast<size_t>(Index)]))
                        Result = unionModRef(Result, ModRefInfo::Mod);
                }
                if (Result == ModRefInfo::ModRef) return Result;
            }

            for (Value* RV : SEI.RefSet) {
                if (!RV) continue;
                if (RV->isGlobal()) {
                    if (overlaps(RV))
                        Result = unionModRef(Result, ModRefInfo::Ref);
                } else if (RV->isParam()) {
                    int Index = CalleeFunc->getParamIndex(RV);
                    if (Index >= 0 && static_cast<size_t>(Index) < Args.size() &&
                        Args[static_cast<size_t>(Index)] &&
                        overlaps(Args[static_cast<size_t>(Index)]))
                        Result = unionModRef(Result, ModRefInfo::Ref);
                }
                if (Result == ModRefInfo::ModRef) return Result;
            }

            return Result;
        }

        return ModRefInfo::ModRef;
    }

    return ModRefInfo::NoModRef;
}

ModRefInfo getInstModRefInfo(Value* V, Instruction* I,
                             BasicAAResult& AA, const LoopInfo& LI,
                             FunctionAnalysisPassManager& FAM) {

    return getInstModRefInfo(
        SideEffectAnalysis::getUnknownSizeMemoryLocation(V), I, AA, LI, FAM);
}

ModRefInfo getFunctionModRefInfo(const SideEffectInfo& SEI)
{
    if (SEI.hasBuiltin)
        return ModRefInfo::ModRef;

    ModRefInfo Result = ModRefInfo::NoModRef;

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

// 基本块查询逐条筛选不可移动操作，并对局部 Store 继续检查其后是否存在读取。
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

            if (CI->getBuiltinCallee()) {
                return true;
            }

            auto* CalleeFunc = CI->getDirectCallee();
            if (!CalleeFunc) {
                return true;
            }

            const SideEffectInfo& SEI = FAM.getResult<SideEffectAnalysis>(*CalleeFunc);
            ModRefInfo FMRI = getFunctionModRefInfo(SEI);
            if (isModSet(FMRI)) {
                return true;
            }
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

// 循环查询展平子循环块；任一基本块失败即报告循环存在副作用。
bool LoopSideEffect(const Loop& L,
                         BasicAAResult& AA,
                         const LoopInfo& LI,
                         FunctionAnalysisPassManager& FAM) {
    for (BasicBlock *BB : L.getBlocksIncludingSubLoops()) {
        if (BasicBlockSideEffect(BB, L, AA, LI, FAM)) return true;
    }
    return false;
}
