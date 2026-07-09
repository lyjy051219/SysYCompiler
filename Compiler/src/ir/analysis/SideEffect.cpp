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

static void ensureInitialized(Value* V, BaseAddressInfoMap& Map) {
    if (!V || Map.count(V)) return;
    if (auto* Var = dynamic_cast<Variable*>(V))
        if (Var->isGlobal() || Var->isParam())
            Map[V].bases.insert(V);
}


static void collectExternalBases(
    const BaseAddressInfo& Info,
    std::unordered_set<Value*>& Dest)
{
    for (auto* B : Info.bases)
        if (B && B->isExternalVariable()) Dest.insert(B);
}

IntrinsicID getIntrinsicID(BuildInFunction* BIF) {
    if (!BIF) return IntrinsicID::None;
    const std::string& N = BIF->getIdent();
    if (N == "llvm.memcpy.p0i8.p0i8.i32" ||
        N == "llvm.memcpy.p0.p0.i32"      ||
        N == "memcpy@plt")
        return IntrinsicID::Memcpy;
    return IntrinsicID::None;
}

// buildBaseAddressInfoMap：构建函数 F 中每个指针值的 BaseAddressInfoMap
BaseAddressInfoMap buildBaseAddressInfoMap(Function& F) {
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
                auto& UL = I->getUserList();
                //如果是GEP指令：获取他的基址指针
                if (dynamic_cast<GetElementPtrInst*>(I)) {
                    if (!UL.empty() && UL[0]) {
                        Value* Base = UL[0]->getValue();
                        ensureInitialized(Base, Map);
                        auto it = Map.find(Base);
                        if (it != Map.end()) NewInfo = it->second;
                        else                 NewInfo.untracked = true;
                    }
                //如果是BitCast指令：获取他的源指针
                } else if (dynamic_cast<BitCastInst*>(I)) {
                    if (!UL.empty() && UL[0]) {
                        Value* Src = UL[0]->getValue();
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
    if (N.find("_sysy_") != std::string::npos) return FuncAttr::Sylib;

    // 2. 标量 I/O 函数：完全没有指针参数，既不 Mod 也不 Ref
    if (N == "getint"   || N == "getch"      || N == "getfloat" ||
        N == "putint"   || N == "putch"       || N == "putfloat") return FuncAttr::Sylib;

    // 3. 数组输入函数：会写入（修改）传入的指针内存
    if (N == "getarray" || N == "getfarray")   return FuncAttr::MemWriteOnly;

    // 4. 数组/格式化输出函数：只会读取传入的指针内存，绝不修改
    if (N == "putarray" || N == "putfarray" || N == "putf")   return FuncAttr::MemReadOnly;

    // 5. 内存拷贝函数：既读又写
    if (N == "llvm.memcpy.p0i8.p0i8.i32" ||
        N == "llvm.memcpy.p0.p0.i32"      ||
        N == "memcpy@plt")                     return FuncAttr::MemReadWrite;

    // 6. 兜底策略：未知的内置函数，保守退让
    return FuncAttr::Untracked;
}

//处理SysY运行时库函数和LLVM内置函数的调用，更新ModSet和RefSet
static void processBuiltinCall(
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
            if (it != CallerPtrMap.end()) collectExternalBases(it->second, Dest);
            else if (Arg && Arg->isExternalVariable())   Dest.insert(Arg);
        };
        if (Args.size() >= 1) addArgBases(Args[0], Info.ModSet);
        if (Args.size() >= 2) addArgBases(Args[1], Info.RefSet);
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
                if (DoMod) collectExternalBases(it->second, Info.ModSet);
                if (DoRef) collectExternalBases(it->second, Info.RefSet);
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
            collectExternalBases(it->second, Info.ModSet);
            collectExternalBases(it->second, Info.RefSet);
        } else if (Arg->isExternalVariable()) {
            Info.ModSet.insert(Arg);
            Info.RefSet.insert(Arg);
        }
    }
}

// SideEffectAnalysis::run
SideEffectInfo SideEffectAnalysis::run(Function &F, FunctionAnalysisPassManager &FAM) const {
    SideEffectInfo Info;
    //1.构建函数 F 中每个指针值的 BaseAddressInfoMap
    Info.ptrMap = buildBaseAddressInfoMap(F);

    //2.遍历函数中每条指令，收集ModSet和RefSet
    for (auto BBIt = F.begin(); BBIt != F.end(); ++BBIt) {
        BasicBlock* BB = *BBIt;
        for (auto IIt = BB->begin(); IIt != BB->end(); ++IIt) {
            Instruction* I = *IIt;

            //2.1 LOAD：遍历基地址，如果基地址是全局变量或者是形参，那么标记为Ref（只读）
            if (I->instType == Instruction::LOAD) {
                auto& UL = I->getUserList();
                if (!UL.empty() && UL[0]) {
                    Value* Ptr = UL[0]->getValue();
                    auto it = Info.ptrMap.find(Ptr);
                    if (it != Info.ptrMap.end())
                        collectExternalBases(it->second, Info.RefSet);
                    else if (Ptr && Ptr->isExternalVariable())
                        Info.RefSet.insert(Ptr);
                }
                continue;
            }

            //2.2 STORE:遍历基地址，如果基地址是全局变量或者是形参，那么标记为Mod（修改）
            if (I->instType == Instruction::STORE) {
                auto& UL = I->getUserList();
                if (UL.size() >= 2 && UL[1]) {
                    Value* Ptr = UL[1]->getValue();
                    auto it = Info.ptrMap.find(Ptr);
                    if (it != Info.ptrMap.end())
                        collectExternalBases(it->second, Info.ModSet);
                    else if (Ptr && Ptr->isExternalVariable())
                        Info.ModSet.insert(Ptr);
                }
                continue;
            }

            //2.3 CALL:
            if (I->instType == Instruction::CALL) {
                auto *CI = dynamic_cast<CallInst *>(I);
                if (!CI) continue;
                Value* Callee = CI->getCallee();

                std::vector<Value*> Args;
                for (unsigned i = 0; i < CI->getArgCount(); ++i)
                    Args.push_back(CI->getArg(i));

                //2.3.1如果这个Callee是用户写的函数
                if (auto* CalleeFunc = dynamic_cast<Function*>(Callee)) {
                    if (CalleeFunc == &F) continue;

                    const SideEffectInfo& CalleeSEI =
                        FAM.getResult<SideEffectAnalysis>(*CalleeFunc);
                    auto& CalleeParams = CalleeFunc->getParams();


                    auto mapEntry = [&](Value* V, std::unordered_set<Value*>& Dest) {
                        auto* Var = dynamic_cast<Variable*>(V);
                        if (!Var) return;
                        if (Var->isGlobal()) { Dest.insert(V); return; }
                        if (Var->isParam()) {
                            for (size_t i = 0; i < CalleeParams.size(); ++i) {
                                if (CalleeParams[i].get() != V) continue;
                                if (i >= Args.size() || !Args[i]) break;
                                auto it = Info.ptrMap.find(Args[i]);
                                if (it != Info.ptrMap.end())
                                    collectExternalBases(it->second, Dest);
                                else if (Args[i] && Args[i]->isExternalVariable())
                                    Dest.insert(Args[i]);
                                break;
                            }
                        }
                    };

                    for (Value* V : CalleeSEI.ModSet) mapEntry(V, Info.ModSet);
                    for (Value* V : CalleeSEI.RefSet) mapEntry(V, Info.RefSet);
                    if (CalleeSEI.hasBuiltin) Info.hasBuiltin = true;

                //2.3.2如果Callee是一个内置函数
                } else if (auto* BIF = dynamic_cast<BuildInFunction*>(Callee)) {
                    processBuiltinCall(BIF, Args, Info.ptrMap, Info);
                }
                continue;
            }
        }
    }
    return Info;
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
    if (I->instType == Instruction::LOAD) {
        auto& UL = I->getUserList();
        if (!UL.empty() && UL[0]) {
            if (overlaps(UL[0]->getValue()))
                Result = unionModRef(Result, ModRefInfo::Ref);
        }
        return Result;
    }

    //STORE
    if (I->instType == Instruction::STORE) {
        auto& UL = I->getUserList();
        if (UL.size() >= 2 && UL[1]) {
            if (overlaps(UL[1]->getValue()))
                Result = unionModRef(Result, ModRefInfo::Mod);
        }
        return Result;
    }

    // CALL
    if (I->instType == Instruction::CALL) {
        auto *CI = dynamic_cast<CallInst *>(I);
        if (!CI) return ModRefInfo::NoModRef;

        Value* Callee = CI->getCallee();
        std::vector<Value*> Args;
        for (unsigned i = 0; i < CI->getArgCount(); ++i)
            Args.push_back(CI->getArg(i));

        // 1. 内置函数
        if (auto* BIF = dynamic_cast<BuildInFunction*>(Callee)) {
            // 1a. memcpy：第0实参写，第1实参读
            if (getIntrinsicID(BIF) == IntrinsicID::Memcpy) {
                if (Args.size() >= 1 && Args[0] && overlaps(Args[0]))
                    Result = unionModRef(Result, ModRefInfo::Mod);
                if (Result == ModRefInfo::ModRef) return Result;
                if (Args.size() >= 2 && Args[1] && overlaps(Args[1]))
                    Result = unionModRef(Result, ModRefInfo::Ref);
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
        if (auto* CalleeFunc = dynamic_cast<Function*>(Callee)) {
            const SideEffectInfo& SEI = FAM.getResult<SideEffectAnalysis>(*CalleeFunc);

            // 含系统调用，保守返回 ModRef
            if (SEI.hasBuiltin)
                return ModRefInfo::ModRef;

            auto& CalleeParams = CalleeFunc->getParams();

            // 遍历 ModSet
            for (Value* MV : SEI.ModSet) {
                auto* Var = dynamic_cast<Variable*>(MV);
                if (!Var) continue;

                if (Var->isGlobal()) {
                    if (overlaps(MV))
                        Result = unionModRef(Result, ModRefInfo::Mod);
                } else if (Var->isParam()) {
                    for (size_t i = 0; i < CalleeParams.size(); ++i) {
                        if (CalleeParams[i].get() != MV) continue;
                        if (i < Args.size() && Args[i] && overlaps(Args[i]))
                            Result = unionModRef(Result, ModRefInfo::Mod);
                        break;
                    }
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
                    for (size_t i = 0; i < CalleeParams.size(); ++i) {
                        if (CalleeParams[i].get() != RV) continue;
                        if (i < Args.size() && Args[i] && overlaps(Args[i]))
                            Result = unionModRef(Result, ModRefInfo::Ref);
                        break;
                    }
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

            Value* Callee = CI->getCallee();
            if (dynamic_cast<BuildInFunction*>(Callee)) {
                return true;
            }

            auto* CalleeFunc = dynamic_cast<Function*>(Callee);
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
            auto& UL = SI->getUserList();
            if (UL.size() < 2 || !UL[1]) return true;

            Value* Ptr = UL[1]->getValue();
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
    std::vector<const Loop*> Stack;
    Stack.push_back(&L);

    while (!Stack.empty()) {
        const Loop* Cur = Stack.back();
        Stack.pop_back();
        if (!Cur) continue;

        for (BasicBlock* BB : Cur->getBlocks()) {
            if (BasicBlockSideEffect(BB, L, AA, LI, FAM)) {
                return true;
            }
        }

        for (Loop* Sub : Cur->getSubLoops()) {
            if (Sub) Stack.push_back(Sub);
        }
    }
    return false;
}
