#include "ir/analysis/SideEffect.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include <cstdlib>
#include <queue>

PassID SideEffectAnalysis::Key;

// SideEffectInfo::invalidate
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

void SideEffectAnalysis::ensureInitialized(Value* V, BaseAddressInfoMap& Map) {
    if (!V || Map.count(V)) return;
    if (auto* Var = dynamic_cast<Variable*>(V))
        if (Var->isGlobal() || Var->isParam())
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
    if (BuildInFunction::isMemsetName(N))
        return IntrinsicID::Memset;
    return IntrinsicID::None;
}

// buildBaseAddressInfoMap：构建函数 F 中每个指针值的 BaseAddressInfoMap
BaseAddressInfoMap SideEffectAnalysis::buildBaseAddressInfoMap(Function& F) {
    BaseAddressInfoMap Map;
    //1.遍历所有形参，拿到所有指针，并创建它们的BaseAddressInfo
    for (auto& P : F.getParams()) {
        Value* V = P.get();
        if (!V || !V->isPointerValue()) continue;
        Map[V].bases.insert(V);
    }
    //2.遍历函数内所有Alloca（局部变量）：创建它们的BaseAddressInfo
    for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt)
        for (auto IIt = (*BBIt)->begin(); IIt != (*BBIt)->end(); ++IIt)
            if (dynamic_cast<AllocaInst*>(*IIt))
                Map[*IIt].bases.insert(*IIt);

    //3.不动点迭代:处理函数中其他指令，更新指针值的BaseAddressInfo直到收敛
    bool changed = true;
    while (changed) {
        changed = false;
        //遍历函数内所有指令，更新每个指针值的BaseAddressInfo
        for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt) {
            for (auto IIt = (*BBIt)->begin(); IIt != (*BBIt)->end(); ++IIt) {
                Instruction* I = *IIt;
                //跳过Alloca：因为已经处理过了
                if (dynamic_cast<AllocaInst*>(I)) continue;
                if (!I || !I->isPointerValue()) continue;

                BaseAddressInfo NewInfo;
                //如果是GEP指令：获取他的基址指针
                if (auto *GEP = dynamic_cast<GetElementPtrInst*>(I)) {
                    if (Value *Base = GEP->getBasePointer()) {
                        ensureInitialized(Base, Map);
                        auto it = Map.find(Base);
                        if (it != Map.end()) NewInfo = it->second;
                        else                 NewInfo.untracked = true;
                    }
                //如果是BitCast指令：获取他的源指针
                } else if (auto *BC = dynamic_cast<BitCastInst*>(I)) {
                    if (Value *Src = BC->getUnaryOperand()) {
                        ensureInitialized(Src, Map);
                        auto it = Map.find(Src);
                        if (it != Map.end()) NewInfo = it->second;
                        else                 NewInfo.untracked = true;
                    }
                //如果是Phi指令：合并所有已计算的输入值的BaseAddressInfo
                } else if (auto* Phi = dynamic_cast<PhiInst*>(I)) {
                    unsigned N = Phi->getNumIncomingValues();
                    for (unsigned j = 0; j < N; ++j) {
                        Value* V = Phi->getIncomingValue(j);
                        ensureInitialized(V, Map);
                        auto it = Map.find(V);
                        if (it != Map.end()) {
                            // V 已计算：将其完整潜在别名集合合并入 NewInfo
                            // 这里也会传播 V 自身的 untracked（例如 V 是 LoadInst 结果）
                            NewInfo.merge(it->second);
                        }
                        // V 不在 Map 中：视为 bottom（尚未计算），跳过
                        // 不能设 untracked=true，否则会在 merge 中永久污染
                    }
                    // PHI 使用替换而非合并：允许 untracked 状态在后续迭代中被纠正
                    // （处理顺序导致的虚假 untracked=true 可以在 incoming 值计算后被清除）
                    {
                        auto& Existing = Map[I];
                        bool different = (Existing.untracked != NewInfo.untracked) ||
                                         (Existing.bases != NewInfo.bases);
                        if (different) { Existing = NewInfo; changed = true; }
                    }
                    continue;
                } else {
                    // LoadInst 返回指针、CallInst 返回指针等：来源不可追踪
                    NewInfo.untracked = true;
                }

                if (Map[I].merge(NewInfo)) changed = true;
            }
        }
    }
    return Map;
}

//根据内置函数的类型进行分类
FuncAttr classifyBuiltin(BuildInFunction* BIF) {
    if (!BIF) return FuncAttr::Untracked;
    const std::string& N = BIF->getIdent();

    // 1. 系统/计时函数：对用户代码的指针没有任何内存副作用
    if (BuildInFunction::isTimingName(N)) return FuncAttr::Sylib;

    // 2. 标量 I/O 函数：完全没有指针参数，既不 Mod 也不 Ref
    if (BuildInFunction::isScalarIOName(N)) return FuncAttr::Sylib;

    // 3. 数组输入函数：会写入（修改）传入的指针内存
    if (BuildInFunction::isArrayInputName(N)) return FuncAttr::MemWriteOnly;

    // 4. 数组/格式化输出函数：只会读取传入的指针内存，绝不修改
    if (BuildInFunction::isArrayOutputName(N)) return FuncAttr::MemReadOnly;

    // 5. 内存拷贝函数：既读又写
    if (BuildInFunction::isMemcpyName(N)) return FuncAttr::MemReadWrite;

    // 6. 内存填充函数：只写第一个指针实参。
    if (BuildInFunction::isMemsetName(N)) return FuncAttr::MemWriteOnly;

    // 7. 兜底策略：未知的内置函数，保守退让
    return FuncAttr::Untracked;
}

//处理SysY运行时库函数和LLVM内置函数的调用，更新ModSet和RefSet
void SideEffectAnalysis::processBuiltinCall(
    BuildInFunction* BIF,
    const std::vector<Value*>& Args,
    const BaseAddressInfoMap& CallerPtrMap,
    SideEffectInfo& Info)
{
    //1.Memcpy：目标地址被写，源地址被读
    if (getIntrinsicID(BIF) == IntrinsicID::Memcpy) {
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
    // memset：仅目标地址被写；填充值和长度均不访问内存。
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
    //2.获取内置函数属性，并处理内置函数的 Mod/Ref 影响
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

    //处理Untracked的情况：标记为Mod和Ref
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

// SideEffectAnalysis::run
SideEffectInfo SideEffectAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    struct PendingCall {
        Function *Callee = nullptr;
        std::vector<Value*> Args;
    };
    struct FunctionState {
        SideEffectInfo Info;
        std::vector<PendingCall> Calls;
    };

    // Do not recursively ask FAM for a callee result here.  Analysis manager
    // caches are populated only after run() returns, so direct recursion would
    // recurse forever for a mutually-recursive SCC.  Instead, build local
    // summaries for the call graph reachable from F and solve all transitive
    // effects with a monotone fixed point.  This is equivalent to processing
    // call-graph SCCs bottom-up, while keeping the implementation independent
    // of a particular SCC numbering.
    std::unordered_map<Function*, FunctionState> States;
    std::queue<Function*> WorkList;
    std::unordered_set<Function*> Enqueued;
    WorkList.push(&F);
    Enqueued.insert(&F);

    while (!WorkList.empty()) {
        Function *Current = WorkList.front();
        WorkList.pop();
        if (!Current || States.count(Current)) continue;

        FunctionState State;
        SideEffectInfo &Info = State.Info;
        Info.ptrMap = buildBaseAddressInfoMap(*Current);

        // First collect effects that are local to Current.  User-function
        // calls are recorded as transfer edges and handled by the fixed point
        // below.
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
                    // An indirect/unknown call can read and write arbitrary
                    // externally visible memory.  hasBuiltin is the existing
                    // conservative "unknown call effect" bit used by clients.
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

    bool Changed = true;
    while (Changed) {
        Changed = false;
        for (auto &Entry : States) {
            FunctionState &CallerState = Entry.second;
            SideEffectInfo &CallerInfo = CallerState.Info;

            for (const PendingCall &Call : CallerState.Calls) {
                auto CalleeIt = States.find(Call.Callee);
                if (CalleeIt == States.end()) {
                    if (!CallerInfo.hasBuiltin) {
                        CallerInfo.hasBuiltin = true;
                        Changed = true;
                    }
                    continue;
                }

                const SideEffectInfo &CalleeInfo = CalleeIt->second.Info;
                // Caller and callee are the same object for a self-recursive
                // edge.  Snapshot the current lattice value before mapping it
                // back into the caller; inserting into an unordered_set while
                // iterating that same set can rehash and invalidate iterators.
                const std::vector<Value*> CalleeMods(
                    CalleeInfo.ModSet.begin(), CalleeInfo.ModSet.end());
                const std::vector<Value*> CalleeRefs(
                    CalleeInfo.RefSet.begin(), CalleeInfo.RefSet.end());
                const bool CalleeHasBuiltin = CalleeInfo.hasBuiltin;
                auto mapEntry = [&](Value *V,
                                    std::unordered_set<Value*> &Dest) {
                    auto *Var = dynamic_cast<Variable*>(V);
                    if (!Var) return false;
                    if (Var->isGlobal()) return Dest.insert(V).second;
                    if (!Var->isParam()) return false;

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
                        // A pointer whose base cannot be tracked may designate
                        // any external object.  Preserve soundness by upgrading
                        // the caller summary to the conservative unknown state.
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

    auto ResultIt = States.find(&F);
    if (ResultIt == States.end()) return SideEffectInfo{};

    // The fixed point already computed every reachable callee.  Publish those
    // companion summaries into FAM as one coherent generation; the requested
    // function itself is returned to getResult(), which installs its cache
    // entry after run() completes.
    for (auto &Entry : States) {
        if (Entry.first == &F) continue;
        FAM.setCachedResult<SideEffectAnalysis>(
            *Entry.first, std::move(Entry.second.Info));
    }
    return std::move(ResultIt->second.Info);
}

// 指令级副作用查询接口
ModRefInfo getInstModRefInfo(Value* V, Instruction* I,
                             BasicAAResult& AA, const LoopInfo& LI,
                             FunctionAnalysisPassManager& FAM)
{
    ModRefInfo Result = ModRefInfo::NoModRef;

    // 辅助：别名分析是否重叠（非 NoAlias 即重叠）
    auto overlaps = [&](Value* Ptr) -> bool {
        return AA.alias(V, Ptr, LI) != AliasInfo::NoAlias;
    };

    //LOAD
    if (auto *LI = dynamic_cast<LoadInst *>(I)) {
        if (Value *Ptr = LI->getPointerOperand()) {
            if (overlaps(Ptr))
                Result = unionModRef(Result, ModRefInfo::Ref);
        }
        return Result;
    }

    //STORE
    if (auto *SI = dynamic_cast<StoreInst *>(I)) {
        if (Value *Ptr = SI->getPointerOperand()) {
            if (overlaps(Ptr))
                Result = unionModRef(Result, ModRefInfo::Mod);
        }
        return Result;
    }

    // CALL
    if (I->instType == Instruction::CALL) {
        auto *CI = dynamic_cast<CallInst *>(I);
        if (!CI) return ModRefInfo::NoModRef;

        std::vector<Value*> Args = CI->getArgs();

        // 1. 内置函数
        if (auto* BIF = CI->getBuiltinCallee()) {
            // 1a. memcpy：第0实参写，第1实参读
            if (getIntrinsicID(BIF) == IntrinsicID::Memcpy) {
                if (Args.size() >= 1 && Args[0] && overlaps(Args[0]))
                    Result = unionModRef(Result, ModRefInfo::Mod);
                if (Result == ModRefInfo::ModRef) return Result;
                if (Args.size() >= 2 && Args[1] && overlaps(Args[1]))
                    Result = unionModRef(Result, ModRefInfo::Ref);
                return Result;
            }
            if (getIntrinsicID(BIF) == IntrinsicID::Memset) {
                if (!Args.empty() && Args[0] && overlaps(Args[0]))
                    Result = unionModRef(Result, ModRefInfo::Mod);
                return Result;
            }

            // 1b. 常规内置函数
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

            // Untracked 内置函数
            return ModRefInfo::ModRef;
        }

        // 2. 用户定义函数
        if (auto* CalleeFunc = CI->getDirectCallee()) {
            const SideEffectInfo& SEI = FAM.getResult<SideEffectAnalysis>(*CalleeFunc);

            // 含系统调用，保守返回 ModRef
            if (SEI.hasBuiltin)
                return ModRefInfo::ModRef;

            // 遍历 ModSet
            for (Value* MV : SEI.ModSet) {
                auto* Var = dynamic_cast<Variable*>(MV);
                if (!Var) continue;

                if (Var->isGlobal()) {
                    if (overlaps(MV))
                        Result = unionModRef(Result, ModRefInfo::Mod);
                } else if (Var->isParam()) {
                    int Index = CalleeFunc->getParamIndex(MV);
                    if (Index >= 0 && static_cast<size_t>(Index) < Args.size() &&
                        Args[static_cast<size_t>(Index)] &&
                        overlaps(Args[static_cast<size_t>(Index)]))
                        Result = unionModRef(Result, ModRefInfo::Mod);
                }
                if (Result == ModRefInfo::ModRef) return Result;
            }

            // 遍历 RefSet
            for (Value* RV : SEI.RefSet) {
                auto* Var = dynamic_cast<Variable*>(RV);
                if (!Var) continue;

                if (Var->isGlobal()) {
                    if (overlaps(RV))
                        Result = unionModRef(Result, ModRefInfo::Ref);
                } else if (Var->isParam()) {
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

        // 3. Callee 无法识别，保守返回 ModRef
        return ModRefInfo::ModRef;
    }

    return ModRefInfo::NoModRef;
}

// 函数级副作用查询接口
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

bool BasicBlockSideEffect(BasicBlock* BB,
                               const Loop& L,
                               BasicAAResult& AA,
                               const LoopInfo& LI,
                               FunctionAnalysisPassManager& FAM) {
    if (!BB) return false;

    for (auto It = BB->begin(); It != BB->end(); ++It) {
        Instruction* I = *It;
        if (!I) continue;

        // 按需求：RET/ALLOCA 直接视为有副作用
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

            // 按需求：只允许写局部变量（alloca）
            if (auto* Var = dynamic_cast<Variable*>(Base)) {
                if (Var->isGlobal() || Var->isParam()) {
                    return true;
                }
            } else if (!dynamic_cast<AllocaInst*>(Base)) {
                return true;
            }

            // 沿 CFG 向下扫描：若后续有 Ref/ModRef 命中该指针，视为有副作用
            for (Instruction* Next = I->getNext(); Next != nullptr; Next = Next->getNext()) {
                ModRefInfo MRI = getInstModRefInfo(Ptr, Next, AA, LI, FAM);
                if (isRefSet(MRI)) {
                    return true;
                }
            }

            std::queue<BasicBlock*> Q;
            std::unordered_set<BasicBlock*> Visited;
            for (BasicBlock* Succ : BB->getSuccessors()) {
                if (Succ && Visited.insert(Succ).second) {
                    Q.push(Succ);
                }
            }

            while (!Q.empty()) {
                BasicBlock* Cur = Q.front();
                Q.pop();
                for (auto II = Cur->begin(); II != Cur->end(); ++II) {
                    Instruction* J = *II;
                    if (!J) continue;
                    ModRefInfo MRI = getInstModRefInfo(Ptr, J, AA, LI, FAM);
                    if (isRefSet(MRI)) {
                        return true;
                    }
                }
                for (BasicBlock* Succ : Cur->getSuccessors()) {
                    if (Succ && Visited.insert(Succ).second) {
                        Q.push(Succ);
                    }
                }
            }
        }
    }

    (void)L;
    return false;
}

bool LoopSideEffect(const Loop& L,
                         BasicAAResult& AA,
                         const LoopInfo& LI,
                         FunctionAnalysisPassManager& FAM) {
    for (BasicBlock *BB : L.getBlocksIncludingSubLoops()) {
        if (BasicBlockSideEffect(BB, L, AA, LI, FAM)) return true;
    }
    return false;
}
