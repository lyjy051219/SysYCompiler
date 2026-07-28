#include "ir/opt/LoopIdiomRecognize.hpp"

#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/GepOpt.hpp"
#include "lib/IRUtils.hpp"

#include <cstring>
#include <limits>
#include <unordered_set>

bool LoopIdiomRecognizePass::matchLinearAccess(
    GetElementPtrInst *GEP, PhiInst *IndVar, Loop *L,
    LinearAccess &Access) {
    // 1.1 将地址规范为循环外基址加索引列；恰有一个索引必须是归纳变量，其余均为
    // 常量，并验证该维步长等于 4 字节标量元素，保证每次迭代访问相邻元素。
    Access = {};
    if (!GEP || !IndVar || !L) return false;

    Value *Base = nullptr;
    std::vector<Value *> Indices;
    if (!GEP->getBaseAndIndices(Base, Indices) || !Base || Indices.empty())
        return false;
    if (auto *BaseI = dynamic_cast<Instruction *>(Base)) {
        BasicBlock *BaseBB = BaseI->getParent();
        if (BaseBB && L->contains(BaseBB)) return false;
    }

    size_t DynamicIndex = 0;
    unsigned DynamicCount = 0;
    for (size_t I = 0; I < Indices.size(); ++I) {
        if (Indices[I] == IndVar) {
            DynamicIndex = I;
            ++DynamicCount;
            continue;
        }
        int ConstantIndex = 0;
        if (!Value::tryGetIntIndex(Indices[I], ConstantIndex)) return false;
    }
    if (DynamicCount != 1) return false;

    auto ElementTy = IRType::getPointeeType(GEP->getIRType());
     
     
    if (!ElementTy ||
        (!ElementTy->hasIntegerBitWidth(32) && !ElementTy->isFloat()))
        return false;

    std::vector<int64_t> Strides;
    int64_t FinalStride = 0;
    if (!GetElementPtrInst::getIndexStrides(Base, Indices, Strides,
                                            FinalStride) ||
        DynamicIndex >= Strides.size() ||
        Strides[DynamicIndex] != static_cast<int64_t>(ElementTy->getSize()) ||
        FinalStride != static_cast<int64_t>(ElementTy->getSize()))
        return false;

    Access.GEP = GEP;
    Access.Base = Base;
    Access.Indices = std::move(Indices);
    Access.InductionIndex = DynamicIndex;
    Access.ElementTy = ElementTy;
    return true;
}

Value *LoopIdiomRecognizePass::getDisjointObjectRoot(Value *V) {
    // 1.2 剥离 GEP/bitcast 后只承认不同的全局或 alloca 根为确定不相交对象。
    std::unordered_set<Value *> Seen;
    while (V && Seen.insert(V).second) {
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(V)) {
            V = GEP->getBasePointer();
            continue;
        }
        if (auto *BC = dynamic_cast<BitCastInst *>(V)) {
            V = BC->getUnaryOperand();
            continue;
        }
        break;
    }
    if (auto *Var = dynamic_cast<Variable *>(V)) {
        return (Var->vartag == Variable::GlobalVariable ||
                Var->vartag == Variable::GlobalConstant)
                   ? V
                   : nullptr;
    }
    return dynamic_cast<AllocaInst *>(V) ? V : nullptr;
}

bool LoopIdiomRecognizePass::areProvablyDisjoint(
    const LinearAccess &A, const LinearAccess &B) {
    Value *ARoot = getDisjointObjectRoot(A.Base);
    Value *BRoot = getDisjointObjectRoot(B.Base);
    return ARoot && BRoot && ARoot != BRoot;
}

bool LoopIdiomRecognizePass::getRepeatedByte(Value *V, uint8_t &Byte) {
    // 1.3 将 i32/float 常量按位查看，四个字节完全相同才可用单字节 memset 表示。
    uint32_t Bits = 0;
    if (auto *CI = dynamic_cast<ConstInt *>(V)) {
        Bits = static_cast<uint32_t>(CI->getVal());
    } else if (auto *CF = dynamic_cast<ConstFloat *>(V)) {
        float F = CF->getVal();
        static_assert(sizeof(Bits) == sizeof(F), "unexpected float width");
        std::memcpy(&Bits, &F, sizeof(Bits));
    } else {
        return false;
    }

    uint8_t B0 = static_cast<uint8_t>(Bits & 0xffu);
    if (static_cast<uint8_t>((Bits >> 8) & 0xffu) != B0 ||
        static_cast<uint8_t>((Bits >> 16) & 0xffu) != B0 ||
        static_cast<uint8_t>((Bits >> 24) & 0xffu) != B0)
        return false;
    Byte = B0;
    return true;
}

bool LoopIdiomRecognizePass::hasOnlyAllowedLoopInstructions(
    Loop *L, const LoopTripCountInfo &TCI,
    const InductionDescriptor &IV, StoreInst *Store,
    GetElementPtrInst *DestGEP, LoadInst *SourceLoad,
    GetElementPtrInst *SourceGEP, DominatorTree &DT) {
    // 2.1 循环体只能包含归纳更新、唯一退出比较、地址、单次 load/store 和分支；
    // store 必须支配 latch，且这些关键值不能在循环外另有用户。
    if (!L || !Store || !DestGEP || !IV.Phi || !IV.UpdateValue ||
        !TCI.ExitCmp || !TCI.Latch || !TCI.ExitingBlock)
        return false;
    if (!DT.dominates(Store->getParent(), TCI.Latch)) return false;

    std::unordered_set<Instruction *> Allowed{
        IV.Phi, dynamic_cast<Instruction *>(IV.UpdateValue), TCI.ExitCmp,
        DestGEP, Store};
    if (SourceLoad) Allowed.insert(SourceLoad);
    if (SourceGEP) Allowed.insert(SourceGEP);
    if (Allowed.count(nullptr)) Allowed.erase(nullptr);

    unsigned ConditionalBranches = 0;
    auto LoopBlocks = L->getBlocksIncludingSubLoops();
    for (BasicBlock *BB : LoopBlocks)
        if (!BB) return false;
    if (!forEachInstructionInBlocks(
            LoopBlocks, [&](Instruction *I) {
        if (auto *Br = dynamic_cast<BrInst *>(I)) {
            if (Br->isConditional()) {
                ++ConditionalBranches;
                if (Br->getCondition() != TCI.ExitCmp) return false;
            }
            return true;
        }
        return Allowed.count(I) != 0;
    })) return false;
    if (ConditionalBranches != 1) return false;

     
     
    for (Instruction *I : Allowed) {
        if (!I) continue;
        for (Use *U : I->getUses()) {
            auto *UserI = U ? dynamic_cast<Instruction *>(U->getUser()) : nullptr;
            if (!UserI || !UserI->getParent() ||
                !L->contains(UserI->getParent()))
                return false;
        }
    }
    return true;
}

GetElementPtrInst *LoopIdiomRecognizePass::materializeStartPointer(
    const LinearAccess &Access, PhiInst *IndVar, int64_t Start,
    BasicBlock *PreHeader) {
    // 3.1 用常量起始值替换线性地址中的归纳索引，并在 preheader 终结指令前生成
    // intrinsic 所需首元素指针。
    if (!Access.Base || !IndVar || !PreHeader ||
        Access.InductionIndex >= Access.Indices.size() ||
        Start < std::numeric_limits<int32_t>::min() ||
        Start > std::numeric_limits<int32_t>::max())
        return nullptr;

    std::vector<Value *> Indices = Access.Indices;
    Value *StartValue = Value::makeIntegerLike(
        Indices[Access.InductionIndex], static_cast<int32_t>(Start));
    if (!StartValue) return nullptr;
    Indices[Access.InductionIndex] = StartValue;

    auto *StartGEP = new GetElementPtrInst(Access.Base, Indices);
     
    StartGEP->updateType();
    if (!PreHeader->insertBeforeTerminator(StartGEP)) {
        delete StartGEP;
        return nullptr;
    }
    return StartGEP;
}

bool LoopIdiomRecognizePass::matchSimpleCopyLoop(
    Loop *L, SCEVResult &SE, DominatorTree &DT,
    CopyLoopMatch &Match) {
    // 4.1 识别单位正步长的最内层复制循环；常量次数必须正且无回绕，运行期形态
    // 必须是 header 中 iv < bound，并记录 preheader/exit/起点与边界。
    Match = {};
    if (!L || !L->isInnermost()) return false;

    auto TCI = SE.analyzeLoopTripCount(L);
    auto IV = SE.getCanonicalInduction(L);
    if (!TCI || !TCI->Valid || !IV || !IV->Valid ||
        !IV->ConstantStart || !IV->ConstantStep ||
        *IV->ConstantStep != 1 || TCI->IndVar != IV->Phi)
        return false;

    Match.L = L;
    Match.PreHeader = TCI->PreHeader;
    Match.Header = TCI->Header;
    Match.ExitBlock = TCI->ExitBlock;
    Match.IndVar = IV->Phi;
    Match.Start = *IV->ConstantStart;
    if (!Match.PreHeader || !Match.Header || !Match.ExitBlock)
        return false;

    if (TCI->ConstantTripCount) {
        if (!TCI->ProvenNoWrap || *TCI->ConstantTripCount <= 0)
            return false;
        Match.TripCount = *TCI->ConstantTripCount;
    } else {
        if (TCI->ExitingBlock != TCI->Header || Match.Start < 0)
            return false;
        BinaryInst::Operation Pred = TCI->ContinueOnTrue
                                         ? TCI->ExitCmp->getOp()
                                         : BinaryInst::negatePredicate(
                                               TCI->ExitCmp->getOp());
        Value *Bound = nullptr;
        if (TCI->ExitCmp->getOperand(0) == IV->Phi) {
            Bound = TCI->ExitCmp->getOperand(1);
        } else if (TCI->ExitCmp->getOperand(1) == IV->Phi) {
            Bound = TCI->ExitCmp->getOperand(0);
            Pred = BinaryInst::reversePredicate(Pred);
        }
        if (!Bound || Pred != BinaryInst::L ||
            !IRType::hasIntegerBitWidth(Bound->getIRType(), 32))
            return false;
        Match.IsRuntime = true;
        Match.RuntimeBound = Bound;
    }

    // 4.2 要求恰好一条 load feeding 唯一 store，二者地址均为同型连续线性访问，
    // 并通过允许指令集合检查排除额外计算、调用和条件控制。
    std::vector<LoadInst *> Loads;
    auto LoopBlocks = L->getBlocksIncludingSubLoops();
    if (!forEachInstructionInBlocks(
            LoopBlocks, [&](Instruction *I) {
        if (auto *Store = dynamic_cast<StoreInst *>(I)) {
            if (Match.Store) return false;
            Match.Store = Store;
        } else if (auto *Load = dynamic_cast<LoadInst *>(I)) {
            Loads.push_back(Load);
        } else if (dynamic_cast<CallInst *>(I)) {
            return false;
        }
        return true;
    })) return false;
    if (!Match.Store || Loads.size() != 1 ||
        Match.Store->getValueOperand() != Loads.front())
        return false;
    Match.Load = Loads.front();

     
     
     
     
    if (TCI->ExitingBlock == TCI->Header &&
        (Match.Store->getParent() == TCI->Header ||
         Match.Load->getParent() == TCI->Header))
        return false;

    auto *DestGEP = dynamic_cast<GetElementPtrInst *>(
        Match.Store->getPointerOperand());
    auto *SourceGEP = dynamic_cast<GetElementPtrInst *>(
        Match.Load->getPointerOperand());
    if (!matchLinearAccess(DestGEP, IV->Phi, L, Match.Destination) ||
        !matchLinearAccess(SourceGEP, IV->Phi, L, Match.Source) ||
        !Match.Destination.ElementTy->hasSameStorageType(
            *Match.Source.ElementTy) ||
        !hasOnlyAllowedLoopInstructions(
            L, *TCI, *IV, Match.Store, DestGEP, Match.Load,
            SourceGEP, DT))
        return false;
    return true;
}

bool LoopIdiomRecognizePass::transformTemporaryCopyPair(
    Function &F, const CopyLoopMatch &First,
    const CopyLoopMatch &Second, DominatorTree &DT, RangeInfo &RI,
    bool &CFGChanged) {
    // 4. 从已匹配复制循环中寻找 source→temporary 与 temporary→destination 的相邻
    // 二段复制；起点、次数/运行期边界及桥接 CFG 必须完全一致。
    if (!First.L || !Second.L || First.L == Second.L ||
        First.ExitBlock != Second.PreHeader ||
        First.Start != Second.Start ||
        First.IsRuntime != Second.IsRuntime)
        return false;
    if (First.IsRuntime) {
        if (!First.RuntimeBound ||
            First.RuntimeBound != Second.RuntimeBound)
            return false;
    } else if (First.TripCount != Second.TripCount ||
               First.TripCount >
                   std::numeric_limits<int32_t>::max() / 4) {
        return false;
    }

     
     
     
    BasicBlock *Bridge = First.ExitBlock;
    if (!Bridge || !Bridge->getBranchTerminator() ||
        Bridge->getBranchTerminator()->isConditional() ||
        Bridge->getSuccessors().size() != 1 ||
        Bridge->getSuccessors().front() != Second.Header)
        return false;
    for (Instruction *I : *Bridge)
        if (I != Bridge->getBranchTerminator()) return false;

    // 4.3 中间对象必须是只被两条对应 GEP 使用的数组 alloca，读写索引形状一致；
    // 这证明临时数组可消去并将两段复制合成为 memmove。
    auto *Temporary = dynamic_cast<AllocaInst *>(First.Destination.Base);
    if (!Temporary || Second.Source.Base != Temporary ||
        !AllocaInst::isArrayAllocation(Temporary) ||
        !First.Source.ElementTy->hasSameStorageType(
            *Second.Destination.ElementTy))
        return false;

    GetElementPtrInst *TempWrite = First.Destination.GEP;
    GetElementPtrInst *TempRead = Second.Source.GEP;
    if (!TempWrite || !TempRead || TempWrite == TempRead ||
        TempWrite->getBasePointer() != Temporary ||
        TempRead->getBasePointer() != Temporary)
        return false;

     
     
     
    if (First.Destination.Indices.size() !=
            Second.Source.Indices.size() ||
        First.Destination.InductionIndex !=
            Second.Source.InductionIndex)
        return false;
    for (size_t I = 0; I < First.Destination.Indices.size(); ++I) {
        if (I == First.Destination.InductionIndex) continue;
        int WriteIndex = 0;
        int ReadIndex = 0;
        if (!Value::tryGetIntIndex(First.Destination.Indices[I],
                                   WriteIndex) ||
            !Value::tryGetIntIndex(Second.Source.Indices[I], ReadIndex) ||
            WriteIndex != ReadIndex)
            return false;
    }

     
     
    std::unordered_set<Value *> TempPointerUses;
    for (Use *U : Temporary->getUses()) {
        if (!U || U->getValue() != Temporary) continue;
        Value *User = dynamic_cast<Value *>(U->getUser());
        if (User != TempWrite && User != TempRead) return false;
        TempPointerUses.insert(User);
    }
    if (TempPointerUses.size() != 2) return false;
    for (auto Pair : {std::make_pair(static_cast<Value *>(TempWrite),
                                    static_cast<Instruction *>(First.Store)),
                      std::make_pair(static_cast<Value *>(TempRead),
                                    static_cast<Instruction *>(Second.Load))}) {
        unsigned Uses = 0;
        for (Use *U : Pair.first->getUses()) {
            if (!U || U->getValue() != Pair.first) continue;
            if (U->getUser() != Pair.second) return false;
            ++Uses;
        }
        if (Uses != 1) return false;
    }

    // 4.4 源、最终目标和运行期边界必须在第一段 preheader 可用，才能把合并调用前移。
    auto AvailableAt = [&](Value *V, BasicBlock *Where) {
        auto *I = dynamic_cast<Instruction *>(V);
        if (!I) return true;
        BasicBlock *DefBB = I->getParent();
        if (!DefBB || !DT.dominates(DefBB, Where)) return false;
        return DefBB != Where ||
               Instruction::comesBefore(I, Where->back());
    };
    if (!AvailableAt(First.Source.Base, First.PreHeader) ||
        !AvailableAt(Second.Destination.Base, First.PreHeader))
        return false;
    if (First.IsRuntime &&
        !AvailableAt(First.RuntimeBound, First.PreHeader))
        return false;

     
     
    // 4.5 先物化两端首地址，并用范围/对象边界证明整条 GEP 链在已知对象内。
    GetElementPtrInst *SourceStart = materializeStartPointer(
        First.Source, First.IndVar, First.Start, First.PreHeader);
    if (!SourceStart ||
        !gepIsProvablyWithinKnownObject(SourceStart, RI, DT)) {
        delete SourceStart;
        return false;
    }
    GetElementPtrInst *DestStart = materializeStartPointer(
        Second.Destination, Second.IndVar, Second.Start,
        First.PreHeader);
    if (!DestStart ||
        !gepIsProvablyWithinKnownObject(DestStart, RI, DT)) {
        delete SourceStart;
        delete DestStart;
        return false;
    }

    Value *ByteCount = nullptr;
    Value *RuntimeGuard = nullptr;
    BasicBlock *FastBlock = nullptr;
    std::vector<Instruction *> RuntimeValues;
    // 4.6 常量次数直接生成字节数；运行期次数构造有工作量、收益阈值及字节乘法
    // 不溢出的 guard，并离线创建 fast block，失败路径统一回滚临时指令。
    if (!First.IsRuntime) {
        ByteCount = ConstInt::newConstInt(
            static_cast<int32_t>(First.TripCount * 4));
    } else {
        BrInst *OldBranch = First.PreHeader->getBranchTerminator();
        if (!OldBranch || OldBranch->isConditional() ||
            First.PreHeader->getSuccessors().size() != 1 ||
            First.PreHeader->getSuccessors().front() != First.Header) {
            delete DestStart;
            delete SourceStart;
            return false;
        }
        for (Instruction *I : *Second.ExitBlock) {
            if (dynamic_cast<PhiInst *>(I)) {
                delete DestStart;
                delete SourceStart;
                return false;
            }
            break;
        }

        Value *Bound = First.RuntimeBound;
        Value *Start = Value::makeIntegerLike(Bound,
                                               static_cast<int32_t>(First.Start));
        Value *Zero = Value::makeIntegerLike(Bound, 0);
        Value *Four = Value::makeIntegerLike(Bound, 4);
        Value *MinCount = Value::makeIntegerLike(Bound, 8);
        Value *MaxCount = Value::makeIntegerLike(
            Bound, std::numeric_limits<int32_t>::max() / 4);
        if (!Start || !Zero || !Four || !MinCount || !MaxCount) {
            delete DestStart;
            delete SourceStart;
            return false;
        }
        auto Insert = [&](Instruction *I) {
            if (!I || !First.PreHeader->insertBeforeTerminator(I)) {
                delete I;
                return false;
            }
            RuntimeValues.push_back(I);
            return true;
        };
        auto *Positive = new BinaryInst(Bound, BinaryInst::G, Start);
        auto *Difference = new BinaryInst(Bound, BinaryInst::SUB, Start);
        if (!Insert(Positive) || !Insert(Difference)) goto PairFailure;
        {
            auto *Count = new SelectInst(Positive, Difference, Zero);
            if (!Insert(Count)) goto PairFailure;
            auto *LargeEnough = new BinaryInst(Count, BinaryInst::GE,
                                                MinCount);
            auto *Fits = new BinaryInst(Count, BinaryInst::LE, MaxCount);
            auto *Guard = new BinaryInst(LargeEnough, BinaryInst::AND, Fits);
            auto *Bytes = new BinaryInst(Count, BinaryInst::MUL, Four);
            if (!Insert(LargeEnough) || !Insert(Fits) || !Insert(Guard) ||
                !Insert(Bytes))
                goto PairFailure;
            RuntimeGuard = Guard;
            ByteCount = Bytes;
        }
        FastBlock = new BasicBlock();
        FastBlock->setIdent(F.makeUniqueBlockIdent(
            First.PreHeader->getIdent() + ".memmove"));
        F.pushBlock(FastBlock);
    }

    {
        std::vector<Value *> Args{
            DestStart, SourceStart, ByteCount,
            ConstBool::newConstBool(false)};
        auto *Call = new CallInst(BuildInFunction::genBuildInFunction(
                                      BuildInFunction::getDefaultMemmoveName()),
                                  Args);
        if (FastBlock) FastBlock->pushBack(Call);
        else if (!First.PreHeader->insertBeforeTerminator(Call)) {
            delete Call;
            goto PairFailure;
        }
        if (Module *M = Module::getActiveModule())
            M->registerBuiltin(BuildInFunction::getDefaultMemmoveName());
    }

    // 4.7 运行期版本保留原两循环为慢路径，fast block 直达第二段出口；常量版本
    // 直接删除两段 load/store/GEP 及失去 use 的临时 alloca。
    if (FastBlock) {
        delete First.PreHeader->getBranchTerminator();
        First.PreHeader->genCondInst(RuntimeGuard, FastBlock, First.Header);
        FastBlock->genUnCondInst(Second.ExitBlock);
        CFGChanged = true;
    } else {
        for (Instruction *I : {static_cast<Instruction *>(First.Store),
                               static_cast<Instruction *>(Second.Store)})
            delete I;
        for (Instruction *I : {static_cast<Instruction *>(First.Load),
                               static_cast<Instruction *>(Second.Load)})
            if (!I->hasUses()) delete I;
        for (Instruction *I : {static_cast<Instruction *>(First.Source.GEP),
                               static_cast<Instruction *>(TempWrite),
                               static_cast<Instruction *>(TempRead),
                               static_cast<Instruction *>(Second.Destination.GEP)})
            if (!I->hasUses()) delete I;
        if (!Temporary->hasUses()) delete Temporary;
    }
    return true;

PairFailure:
    // 4.8 接入 CFG 前失败时按创建逆序删除 fast block、guard 与首地址，保持原循环不变。
    if (FastBlock) {
        FastBlock->eraseFromParent();
        delete FastBlock;
    }
    for (auto It = RuntimeValues.rbegin(); It != RuntimeValues.rend(); ++It)
        delete *It;
    delete DestStart;
    delete SourceStart;
    return false;
}

bool LoopIdiomRecognizePass::transformLoop(
    Function &F, Loop *L, SCEVResult &SE, DominatorTree &DT,
    RangeInfo &RI,
    bool &CFGChanged) {
    // 2. 仅处理 LoopSimplify 形态的最内层单位正步长循环，要求常量 trip count
    // 正且无回绕；运行期合法性稍后结合退出谓词验证。
    if (!L || !L->isInnermost()) return false;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) || !PreHeader ||
        !PreHeader->hasTerminator())
        return false;

    auto TCI = SE.analyzeLoopTripCount(L);
    auto IV = SE.getCanonicalInduction(L);
    if (!TCI || !TCI->Valid || !IV || !IV->Valid ||
        !IV->ConstantStart || !IV->ConstantStep ||
        *IV->ConstantStep != 1 || TCI->IndVar != IV->Phi)
        return false;

    const bool IsConstant = TCI->ConstantTripCount.has_value();
    if (IsConstant && (!TCI->ProvenNoWrap ||
                       *TCI->ConstantTripCount <= 0))
        return false;

    // 2.2 扫描循环体，要求唯一 store、无调用，并收集 load；目标必须是连续线性地址。
    StoreInst *Store = nullptr;
    std::vector<LoadInst *> Loads;
    auto LoopBlocks = L->getBlocksIncludingSubLoops();
    if (!forEachInstructionInBlocks(
            LoopBlocks, [&](Instruction *I) {
        if (auto *SI = dynamic_cast<StoreInst *>(I)) {
            if (Store) return false;
            Store = SI;
        } else if (auto *LI = dynamic_cast<LoadInst *>(I)) {
            Loads.push_back(LI);
        } else if (dynamic_cast<CallInst *>(I)) {
            return false;
        }
        return true;
    })) return false;
    if (!Store) return false;

    auto *DestGEP = dynamic_cast<GetElementPtrInst *>(
        Store->getPointerOperand());
    LinearAccess DestAccess;
    if (!matchLinearAccess(DestGEP, IV->Phi, L, DestAccess))
        return false;

     
     
     
     
     
     
    if (TCI->ExitingBlock == TCI->Header &&
        Store->getParent() == TCI->Header)
        return false;

    int64_t TripCount = 0;
    int32_t ConstantByteCount = 0;
    if (IsConstant) {
        TripCount = *TCI->ConstantTripCount;
        if (TripCount > std::numeric_limits<int32_t>::max() / 4)
            return false;
        ConstantByteCount = static_cast<int32_t>(TripCount * 4);
    }

    LoadInst *SourceLoad = nullptr;
    GetElementPtrInst *SourceGEP = nullptr;
    LinearAccess SourceAccess;
    uint8_t FillByte = 0;
    // 2.3 store 常量若能表示为重复字节则匹配 memset 且不得有 load；否则必须是
    // 唯一 load feeding store，源/目标同存储类型且由不同对象根证明不重叠，匹配 memcpy。
    bool IsMemset = getRepeatedByte(Store->getValueOperand(), FillByte);
    if (IsMemset) {
        if (!Loads.empty()) return false;
    } else {
        SourceLoad = dynamic_cast<LoadInst *>(Store->getValueOperand());
        if (!SourceLoad || Loads.size() != 1 || Loads.front() != SourceLoad)
            return false;
        SourceGEP = dynamic_cast<GetElementPtrInst *>(
            SourceLoad->getPointerOperand());
        if (!matchLinearAccess(SourceGEP, IV->Phi, L, SourceAccess) ||
            !DestAccess.ElementTy->hasSameStorageType(*SourceAccess.ElementTy) ||
            !areProvablyDisjoint(DestAccess, SourceAccess))
            return false;
    }

     
     
     
    if (TCI->ExitingBlock == TCI->Header && SourceLoad &&
        SourceLoad->getParent() == TCI->Header)
        return false;

    if (!hasOnlyAllowedLoopInstructions(
            L, *TCI, *IV, Store, DestGEP, SourceLoad, SourceGEP, DT))
        return false;

     
     
    // 3. 物化目标及可选源首地址，并证明它们位于已知 alloca/global 对象边界内。
    auto *DestStart = materializeStartPointer(
        DestAccess, IV->Phi, *IV->ConstantStart, PreHeader);
    if (!DestStart ||
        !gepIsProvablyWithinKnownObject(DestStart, RI, DT)) {
        delete DestStart;
        return false;
    }

    GetElementPtrInst *SourceStart = nullptr;
    if (!IsMemset) {
        SourceStart = materializeStartPointer(
            SourceAccess, IV->Phi, *IV->ConstantStart, PreHeader);
        if (!SourceStart ||
            !gepIsProvablyWithinKnownObject(SourceStart, RI, DT)) {
            delete SourceStart;
            delete DestStart;
            return false;
        }
    }

    Value *ByteCount = nullptr;
    BasicBlock *CallBlock = PreHeader;
    BasicBlock *FastBlock = nullptr;
    Value *RuntimeGuard = nullptr;
    std::vector<Instruction *> RuntimeValues;

    // 3.2 常量次数直接计算 4*tripCount；运行期仅接受 iv < bound，且 bound 在
    // preheader 可用。构造 count、最小收益和最大安全字节数 guard 以及 fast block。
    if (IsConstant) {
        ByteCount = ConstInt::newConstInt(ConstantByteCount);
    } else {
         
         
         
         
        if (TCI->ExitingBlock != TCI->Header ||
            *IV->ConstantStart < 0 ||
            *IV->ConstantStart > std::numeric_limits<int32_t>::max()) {
            delete SourceStart;
            delete DestStart;
            return false;
        }

        BinaryInst::Operation Pred = TCI->ContinueOnTrue
                                         ? TCI->ExitCmp->getOp()
                                         : BinaryInst::negatePredicate(
                                               TCI->ExitCmp->getOp());
        Value *Bound = nullptr;
        if (TCI->ExitCmp->getOperand(0) == IV->Phi) {
            Bound = TCI->ExitCmp->getOperand(1);
        } else if (TCI->ExitCmp->getOperand(1) == IV->Phi) {
            Bound = TCI->ExitCmp->getOperand(0);
            Pred = BinaryInst::reversePredicate(Pred);
        }
        if (!Bound || Pred != BinaryInst::L ||
            !IRType::hasIntegerBitWidth(Bound->getIRType(), 32)) {
            delete SourceStart;
            delete DestStart;
            return false;
        }
        if (auto *BoundI = dynamic_cast<Instruction *>(Bound)) {
            BasicBlock *BoundBB = BoundI->getParent();
            if (!BoundBB || L->contains(BoundBB) ||
                !DT.dominates(BoundBB, PreHeader) ||
                (BoundBB == PreHeader &&
                 !Instruction::comesBefore(BoundI,
                                            PreHeader->back()))) {
                delete SourceStart;
                delete DestStart;
                return false;
            }
        }

         
         
         
         
        if (!PreHeader->getBranchTerminator() ||
            PreHeader->getBranchTerminator()->isConditional() ||
            PreHeader->getSuccessors().size() != 1 ||
            PreHeader->getSuccessors().front() != TCI->Header) {
            delete SourceStart;
            delete DestStart;
            return false;
        }
        for (Instruction *ExitI : *TCI->ExitBlock) {
            if (dynamic_cast<PhiInst *>(ExitI)) {
                delete SourceStart;
                delete DestStart;
                return false;
            }
            break;
        }

        Value *StartValue = Value::makeIntegerLike(
            Bound, static_cast<int32_t>(*IV->ConstantStart));
        Value *ZeroValue = Value::makeIntegerLike(Bound, 0);
        Value *FourValue = Value::makeIntegerLike(Bound, 4);
        Value *MinFastCount = Value::makeIntegerLike(Bound, 8);
        Value *MaxFastCount = Value::makeIntegerLike(
            Bound, std::numeric_limits<int32_t>::max() / 4);
        if (!StartValue || !ZeroValue || !FourValue || !MinFastCount ||
            !MaxFastCount) {
            delete SourceStart;
            delete DestStart;
            return false;
        }

        auto InsertRuntime = [&](Instruction *I) -> bool {
            if (!I || !PreHeader->insertBeforeTerminator(I)) {
                delete I;
                return false;
            }
            RuntimeValues.push_back(I);
            return true;
        };

        auto *HasIterations = new BinaryInst(Bound, BinaryInst::G,
                                             StartValue);
        auto *Difference = new BinaryInst(Bound, BinaryInst::SUB,
                                          StartValue);
        if (!InsertRuntime(HasIterations) || !InsertRuntime(Difference)) {
            for (auto It = RuntimeValues.rbegin();
                 It != RuntimeValues.rend(); ++It)
                delete *It;
            delete SourceStart;
            delete DestStart;
            return false;
        }
        auto *Count = new SelectInst(HasIterations, Difference, ZeroValue);
        if (!InsertRuntime(Count)) {
            for (auto It = RuntimeValues.rbegin();
                 It != RuntimeValues.rend(); ++It)
                delete *It;
            delete SourceStart;
            delete DestStart;
            return false;
        }
        auto *EnoughWork = new BinaryInst(Count, BinaryInst::GE,
                                          MinFastCount);
        auto *FitsByteCount = new BinaryInst(Count, BinaryInst::LE,
                                             MaxFastCount);
        auto *UseFastPath = new BinaryInst(EnoughWork, BinaryInst::AND,
                                           FitsByteCount);
        auto *Bytes = new BinaryInst(Count, BinaryInst::MUL, FourValue);
        if (!InsertRuntime(EnoughWork) || !InsertRuntime(FitsByteCount) ||
            !InsertRuntime(UseFastPath) || !InsertRuntime(Bytes)) {
            for (auto It = RuntimeValues.rbegin();
                 It != RuntimeValues.rend(); ++It)
                delete *It;
            delete SourceStart;
            delete DestStart;
            return false;
        }
        RuntimeGuard = UseFastPath;
        ByteCount = Bytes;

        FastBlock = new BasicBlock();
        FastBlock->setIdent(F.makeUniqueBlockIdent(
            PreHeader->getIdent() + ".memidiom"));
        F.pushBlock(FastBlock);
        CallBlock = FastBlock;
    }

    // 3.3 根据模式生成 memset/memcpy 参数并注册 builtin；所有离线构造成功后才接入。
    std::vector<Value *> Args;
    const char *IntrinsicName = nullptr;
    if (IsMemset) {
        IntrinsicName = BuildInFunction::getDefaultMemsetName();
        Args = {DestStart, ConstInt::newConstInt(FillByte, 1),
                ByteCount,
                ConstBool::newConstBool(false)};
    } else {
        IntrinsicName = BuildInFunction::getDefaultMemcpyName();
        Args = {DestStart, SourceStart, ByteCount,
                ConstBool::newConstBool(false)};
    }
    auto *Call = new CallInst(
        BuildInFunction::genBuildInFunction(IntrinsicName), Args);
    bool InsertedCall = FastBlock ? (FastBlock->pushBack(Call), true)
                                  : PreHeader->insertBeforeTerminator(Call);
    if (!InsertedCall) {
        delete Call;
        if (FastBlock) {
            FastBlock->eraseFromParent();
            delete FastBlock;
        }
        for (auto It = RuntimeValues.rbegin();
             It != RuntimeValues.rend(); ++It)
            delete *It;
        delete SourceStart;
        delete DestStart;
        return false;
    }
    if (Module *M = Module::getActiveModule()) M->registerBuiltin(IntrinsicName);

    // 3.4 运行期用 guard 在 intrinsic fast path 与原循环 slow path 间选择；常量版本
    // 不改 CFG，只删除被调用完全取代且已无用户的循环内内存指令。
    if (FastBlock) {
        BrInst *OldBranch = PreHeader->getBranchTerminator();
        delete OldBranch;
        PreHeader->genCondInst(RuntimeGuard, FastBlock, TCI->Header);
        FastBlock->genUnCondInst(TCI->ExitBlock);
        CFGChanged = true;
    } else {
        delete Store;
        if (SourceLoad && !SourceLoad->hasUses()) delete SourceLoad;
        if (SourceGEP && !SourceGEP->hasUses()) delete SourceGEP;
        if (DestGEP && !DestGEP->hasUses()) delete DestGEP;
    }
    return true;
}

PreservationStatus LoopIdiomRecognizePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    // 1. 获取支配、循环、范围与 SCEV，并按内到外顺序快照循环，后续改 CFG 时停止。
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);

    bool Changed = false;
    bool CFGChanged = false;
    std::vector<Loop *> Loops = LI.getLoopsInPostOrder(true);

     
     
     
    // 4. 优先预匹配全部简单复制循环并尝试消除相邻临时数组复制对；该变换跨两个循环，
    // 成功后直接使全部分析失效并返回。
    std::vector<CopyLoopMatch> Copies;
    Copies.reserve(Loops.size());
    // 2—3. 未命中成对模式时逐循环识别 memset/memcpy；首次运行期 fast path 改 CFG
    // 后立即停止，避免继续使用陈旧 LoopInfo/SCEV。
    for (Loop *L : Loops) {
        CopyLoopMatch Match;
        if (matchSimpleCopyLoop(L, SE, DT, Match))
            Copies.push_back(Match);
    }
    for (const CopyLoopMatch &First : Copies) {
        for (const CopyLoopMatch &Second : Copies) {
            if (transformTemporaryCopyPair(
                    F, First, Second, DT, RI, CFGChanged)) {
                 
                 
                 
                return PreservationStatus::none();
            }
        }
    }

    for (Loop *L : Loops) {
        Changed |= transformLoop(F, L, SE, DT, RI, CFGChanged);
         
         
         
        if (CFGChanged) break;
    }
    if (!Changed) return PreservationStatus::all();

    if (CFGChanged) return PreservationStatus::none();

    // 5. 常量次数版本不改 CFG，可保留支配/循环；地址和值变化使 AA/Range/SCEV/副作用失效。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<SCEVAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
