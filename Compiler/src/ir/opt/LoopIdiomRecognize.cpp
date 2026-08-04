#include "ir/opt/LoopIdiomRecognize.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "ir/opt/GepCombine.hpp"
#include "lib/IRUtils.hpp"
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_set>
//剥离地址转换并返回可用于不相交证明的对象根
Value *LoopIdiomRecognizePass::getDisjointObjectRoot(Value *V) {
    //1.2 剥离 GEP/bitcast 后只承认不同的全局或 alloca 根为确定不相交对象。
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
//判断两个线性访问是否来自不同的确定对象
bool LoopIdiomRecognizePass::areProvablyDisjoint(
    const LinearAccess &A, const LinearAccess &B) {
    Value *ARoot = getDisjointObjectRoot(A.Base);
    Value *BRoot = getDisjointObjectRoot(B.Base);
    return ARoot && BRoot && ARoot != BRoot;
}
//展平最终标量typed GEP并记录完整对象布局
bool LoopIdiomRecognizePass::matchAffineViewAccess(
    Value *Pointer, AffineViewAccess &Access) {
    //1.只接受没有bitcast的零索引typed GEP链；根对象必须是全局数组或栈数组。
    Access = {};
    auto *Address = dynamic_cast<GetElementPtrInst *>(Pointer);
    if (!Address) return false;
    Value *Root = nullptr;
    std::vector<Value *> Indices;
    if (!GetElementPtrInst::flattenZeroBasedChain(
            Address, Root, Indices) || !Root || Indices.size() < 3) {
        return false;
    }
    Value *ObjectRoot = getDisjointObjectRoot(Root);
    if (!ObjectRoot || ObjectRoot != Root) return false;
    auto ObjectType = IRType::getPointeeType(Root->getIRType());
    auto ElementType = IRType::getPointeeType(Address->getIRType());
    if (!ObjectType || !ObjectType->isArray() || !ElementType ||
        (!ElementType->hasIntegerBitWidth(32) && !ElementType->isFloat())) {
        return false;
    }
    //2.每个typed索引都必须具有精确的正字节步长，最终指针必须落到标量元素。
    std::vector<int64_t> Strides;
    int64_t FinalStride = 0;
    if (!GetElementPtrInst::getIndexStrides(
            Root, Indices, Strides, FinalStride) ||
        Strides.size() != Indices.size() ||
        FinalStride != static_cast<int64_t>(ElementType->getSize())) {
        return false;
    }
    for (int64_t Stride : Strides)
        if (Stride <= 0) return false;
    Access.Address = Address;
    Access.Root = Root;
    Access.Indices = std::move(Indices);
    Access.ByteStrides = std::move(Strides);
    Access.AxisAtIndex.assign(Access.Indices.size(), -1);
    return true;
}
//把二维生产者GEP的动态索引绑定到两条规范归纳轴
bool LoopIdiomRecognizePass::bindAffineViewAxes(
    AffineViewAccess &Access, PhiInst *OuterIV, PhiInst *InnerIV) {
    //1.首版只承认直接归纳PHI形成的轴置换；其他索引必须是真正常量。
    if (!Access.Address || !Access.Root || !OuterIV || !InnerIV ||
        OuterIV == InnerIV ||
        Access.AxisAtIndex.size() != Access.Indices.size()) {
        return false;
    }
    unsigned AxisCounts[2] = {0, 0};
    for (size_t Index = 0; Index < Access.Indices.size(); ++Index) {
        Value *V = Access.Indices[Index];
        if (V == OuterIV) {
            Access.AxisAtIndex[Index] = 0;
            ++AxisCounts[0];
            continue;
        }
        if (V == InnerIV) {
            Access.AxisAtIndex[Index] = 1;
            ++AxisCounts[1];
            continue;
        }
        int64_t Constant = 0;
        if (!V || !V->tryGetInt64Const(Constant)) return false;
        Access.AxisAtIndex[Index] = -1;
    }
    return AxisCounts[0] == 1 && AxisCounts[1] == 1;
}
//递归检查对象地址的全部使用并收集真实读写
bool LoopIdiomRecognizePass::collectAffineViewObjectUses(
    Value *Current, Function &F, bool AllowArrayInput,
    std::unordered_set<Value *> &Visited,
    std::vector<LoadInst *> &Loads,
    std::vector<StoreInst *> &Stores,
    std::vector<Instruction *> &Writers) {
    if (!Current) return false;
    if (!Visited.insert(Current).second) return true;
    for (Use *U : Current->getUses()) {
        if (!U || U->getValue() != Current) return false;
        auto *I = dynamic_cast<Instruction *>(U->getUser());
        if (!I || I->getParentFunction() != &F) return false;
        if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I)) {
            //1.对象指针只能作为typed GEP的基址，不能偷偷参与索引或其他运算。
            if (GEP->getBasePointer() != Current ||
                !collectAffineViewObjectUses(
                    GEP, F, AllowArrayInput, Visited,
                    Loads, Stores, Writers)) {
                return false;
            }
            continue;
        }
        if (auto *Load = dynamic_cast<LoadInst *>(I)) {
            if (Load->getPointerOperand() != Current) return false;
            Loads.push_back(Load);
            continue;
        }
        if (auto *Store = dynamic_cast<StoreInst *>(I)) {
            if (Store->getPointerOperand() != Current) return false;
            Stores.push_back(Store);
            Writers.push_back(Store);
            continue;
        }
        if (auto *Call = dynamic_cast<CallInst *>(I)) {
            //2.源数组只额外允许SysY数组输入接口；这是运行时ABI的内存语义，
            //不是按测试函数名选择优化。该接口不会保存实参地址。
            if (!AllowArrayInput || Call->getArgIndex(U) != 0 ||
                !Call->getBuiltinCallee() ||
                !BuildInFunction::isArrayInputName(
                    Call->getCalleeName())) {
                return false;
            }
            Writers.push_back(Call);
            continue;
        }
        return false;
    }
    return true;
}
//收集最终标量地址向根对象回溯时经过的全部typed GEP
bool LoopIdiomRecognizePass::collectAffineViewGEPChain(
    Value *Pointer, Value *ExpectedRoot,
    std::unordered_set<Instruction *> &Instructions) {
    if (!Pointer || !ExpectedRoot) return false;
    Value *Current = Pointer;
    std::unordered_set<Value *> Seen;
    while (Current && Current != ExpectedRoot && Seen.insert(Current).second) {
        auto *GEP = dynamic_cast<GetElementPtrInst *>(Current);
        if (!GEP || !GEP->getBasePointer()) return false;
        Instructions.insert(GEP);
        Current = GEP->getBasePointer();
    }
    return Current == ExpectedRoot;
}
//证明生产循环只承担二维置换复制和必要控制
bool LoopIdiomRecognizePass::hasOnlyAffineViewProducerInstructions(
    const AffineViewCandidate &Candidate, SCEVResult &SE) {
    if (!Candidate.Outer || !Candidate.Inner || !Candidate.OuterIV ||
        !Candidate.InnerIV || !Candidate.Store || !Candidate.Load) {
        return false;
    }
    auto OuterInduction = SE.getCanonicalInduction(Candidate.Outer);
    auto InnerInduction = SE.getCanonicalInduction(Candidate.Inner);
    auto OuterTrip = SE.analyzeLoopTripCount(Candidate.Outer);
    auto InnerTrip = SE.analyzeLoopTripCount(Candidate.Inner);
    if (!OuterInduction || !InnerInduction || !OuterTrip || !InnerTrip ||
        !OuterInduction->UpdateValue || !InnerInduction->UpdateValue ||
        !OuterTrip->ExitCmp || !InnerTrip->ExitCmp) {
        return false;
    }
    //1.允许集合只包含两条规范归纳链、两端GEP、唯一load/store和分支。
    std::unordered_set<Instruction *> Allowed{
        Candidate.OuterIV,
        Candidate.InnerIV,
        dynamic_cast<Instruction *>(OuterInduction->UpdateValue),
        dynamic_cast<Instruction *>(InnerInduction->UpdateValue),
        OuterTrip->ExitCmp,
        InnerTrip->ExitCmp,
        Candidate.Load,
        Candidate.Store};
    if (Allowed.count(nullptr)) return false;
    if (!collectAffineViewGEPChain(
            Candidate.Destination.Address,
            Candidate.Destination.Root, Allowed) ||
        !collectAffineViewGEPChain(
            Candidate.Source.Address,
            Candidate.Source.Root, Allowed)) {
        return false;
    }
    for (BasicBlock *BB : Candidate.Outer->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (dynamic_cast<BrInst *>(I)) continue;
            if (!Allowed.count(I)) return false;
        }
    }
    //2.生产循环定义的值不得在循环外活跃，避免删除复制后仍保留隐藏计算语义。
    for (Instruction *I : Allowed) {
        if (!I || I == Candidate.Store) continue;
        for (Use *U : I->getUses()) {
            auto *UserI = U ? dynamic_cast<Instruction *>(U->getUser())
                            : nullptr;
            if (!UserI || !UserI->getParent() ||
                !Candidate.Outer->contains(UserI->getParent())) {
                return false;
            }
        }
    }
    return true;
}
//判断一次写入足迹是否精确覆盖整个目标对象
bool LoopIdiomRecognizePass::affineViewCoversWholeObject(
    LoopAccessInfo &LA, Loop *Scope, Instruction *Access,
    Value *ExpectedRoot, int64_t ElementCount) {
    if (!Scope || !Access || !ExpectedRoot || ElementCount <= 0)
        return false;
    std::vector<const LoopAccessFootprint *> Footprints =
        LA.getFootprints(Scope, Access);
    if (Footprints.size() != 1 || !Footprints.front()) return false;
    const LoopAccessFootprint &Footprint = *Footprints.front();
    if (!Footprint.Exact || !Footprint.IsFullAccess ||
        Footprint.Base != ExpectedRoot || !Footprint.ObjectSize ||
        !Footprint.MinByteOffset || !Footprint.MaxByteOffsetExclusive ||
        *Footprint.MinByteOffset != 0 || Footprint.ElementSize == 0 ||
        *Footprint.MaxByteOffsetExclusive !=
            static_cast<int64_t>(*Footprint.ObjectSize)) {
        return false;
    }
    const __int128 Bytes = static_cast<__int128>(ElementCount) *
                           static_cast<__int128>(Footprint.ElementSize);
    return Bytes == static_cast<__int128>(*Footprint.ObjectSize);
}
//判断生产循环完成后是否仍可能执行某个源对象写入块
bool LoopIdiomRecognizePass::isReachableFromAffineViewExit(
    BasicBlock *Exit, BasicBlock *Target) {
    if (!Exit || !Target) return false;
    std::vector<BasicBlock *> WorkList{Exit};
    std::unordered_set<BasicBlock *> Visited;
    while (!WorkList.empty()) {
        BasicBlock *BB = WorkList.back();
        WorkList.pop_back();
        if (!BB || !Visited.insert(BB).second) continue;
        if (BB == Target) return true;
        for (BasicBlock *Successor : BB->getSuccessors())
            if (Successor && !Visited.count(Successor))
                WorkList.push_back(Successor);
    }
    return false;
}
//构建完整二维只读仿射视图消除候选
bool LoopIdiomRecognizePass::buildAffineViewCandidate(
    Function &F, Loop *Outer, LoopAccessInfo &LA, SCEVResult &SE,
    DominatorTree &DT, AffineViewCandidate &Candidate) {
    Candidate = {};
    //1.首版只处理函数顶层、直接父子、正单位步长且常量次数的完整矩形循环。
    if (!Outer || Outer->getParentLoop() ||
        Outer->getSubLoops().size() != 1) {
        return false;
    }
    Loop *Inner = Outer->getSubLoops().front();
    if (!Inner || !Inner->isInnermost() ||
        !LA.isRectangularNest(Outer, Inner)) {
        return false;
    }
    BasicBlock *OuterPreHeader = nullptr;
    BasicBlock *OuterLatch = nullptr;
    BasicBlock *InnerPreHeader = nullptr;
    BasicBlock *InnerLatch = nullptr;
    if (!Outer->getLoopSimplifyForm(OuterPreHeader, OuterLatch) ||
        !Inner->getLoopSimplifyForm(InnerPreHeader, InnerLatch) ||
        !OuterPreHeader || !OuterLatch || !InnerPreHeader || !InnerLatch) {
        return false;
    }
    std::vector<BasicBlock *> Exits = Outer->getExitBlocks();
    if (Exits.size() != 1 || !Exits.front()) return false;
    auto OuterInduction = SE.getCanonicalInduction(Outer);
    auto InnerInduction = SE.getCanonicalInduction(Inner);
    auto OuterTrip = SE.analyzeLoopTripCount(Outer);
    auto InnerTrip = SE.analyzeLoopTripCount(Inner);
    if (!OuterInduction || !InnerInduction || !OuterTrip || !InnerTrip ||
        !OuterInduction->Valid || !InnerInduction->Valid ||
        !OuterTrip->Valid || !InnerTrip->Valid ||
        !OuterInduction->Phi || !InnerInduction->Phi ||
        !OuterInduction->ConstantStart || !InnerInduction->ConstantStart ||
        *OuterInduction->ConstantStart != 0 ||
        *InnerInduction->ConstantStart != 0 ||
        !OuterInduction->ConstantStep || !InnerInduction->ConstantStep ||
        *OuterInduction->ConstantStep != 1 ||
        *InnerInduction->ConstantStep != 1 ||
        !OuterTrip->ConstantTripCount || !InnerTrip->ConstantTripCount ||
        !OuterTrip->ProvenNoWrap || !InnerTrip->ProvenNoWrap ||
        *OuterTrip->ConstantTripCount <= 0 ||
        *InnerTrip->ConstantTripCount <= 0) {
        return false;
    }
    const __int128 WideElementCount =
        static_cast<__int128>(*OuterTrip->ConstantTripCount) *
        static_cast<__int128>(*InnerTrip->ConstantTripCount);
    if (WideElementCount < MinAffineViewElements ||
        WideElementCount > std::numeric_limits<int64_t>::max()) {
        return false;
    }
    Candidate.Outer = Outer;
    Candidate.Inner = Inner;
    Candidate.ExitBlock = Exits.front();
    Candidate.OuterIV = OuterInduction->Phi;
    Candidate.InnerIV = InnerInduction->Phi;
    Candidate.ElementCount = static_cast<int64_t>(WideElementCount);
    //2.生产循环内必须恰有一条load feeding唯一store，且不能含调用或其他访存。
    for (BasicBlock *BB : Outer->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (auto *Load = dynamic_cast<LoadInst *>(I)) {
                if (Candidate.Load) return false;
                Candidate.Load = Load;
            } else if (auto *Store = dynamic_cast<StoreInst *>(I)) {
                if (Candidate.Store) return false;
                Candidate.Store = Store;
            } else if (dynamic_cast<CallInst *>(I)) {
                return false;
            }
        }
    }
    if (!Candidate.Load || !Candidate.Store ||
        Candidate.Store->getValueOperand() != Candidate.Load ||
        !Candidate.Load->hasExactlyOneUse()) {
        return false;
    }
    //3.两端必须是同型、不同唯一对象上的直接二维轴置换，任何动态算术索引都拒绝。
    if (!matchAffineViewAccess(
            Candidate.Store->getPointerOperand(), Candidate.Destination) ||
        !matchAffineViewAccess(
            Candidate.Load->getPointerOperand(), Candidate.Source) ||
        Candidate.Destination.Root == Candidate.Source.Root ||
        !Candidate.Destination.Root->getIRType()->hasSameShape(
            *Candidate.Source.Root->getIRType()) ||
        Candidate.Destination.Indices.size() !=
            Candidate.Source.Indices.size() ||
        Candidate.Destination.ByteStrides != Candidate.Source.ByteStrides ||
        !bindAffineViewAxes(
            Candidate.Destination, Candidate.OuterIV,
            Candidate.InnerIV) ||
        !bindAffineViewAxes(
            Candidate.Source, Candidate.OuterIV,
            Candidate.InnerIV)) {
        return false;
    }
    for (size_t Index = 0;
         Index < Candidate.Destination.Indices.size(); ++Index) {
        const int DestinationAxis = Candidate.Destination.AxisAtIndex[Index];
        const int SourceAxis = Candidate.Source.AxisAtIndex[Index];
        if ((DestinationAxis < 0) != (SourceAxis < 0)) return false;
        if (DestinationAxis < 0 &&
            !Value::areEqualConstants(
                Candidate.Destination.Indices[Index],
                Candidate.Source.Indices[Index])) {
            return false;
        }
    }
    //4.目标写足迹必须无洞覆盖整个对象；源置换也必须精确落在同尺寸完整对象内。
    if (!affineViewCoversWholeObject(
            LA, Outer, Candidate.Store,
            Candidate.Destination.Root, Candidate.ElementCount)) {
        return false;
    }
    std::vector<const LoopAccessFootprint *> SourceFootprints =
        LA.getFootprints(Outer, Candidate.Load);
    if (SourceFootprints.size() != 1 || !SourceFootprints.front())
        return false;
    const LoopAccessFootprint &SourceFootprint = *SourceFootprints.front();
    const __int128 SourceBytes =
        static_cast<__int128>(Candidate.ElementCount) *
        static_cast<__int128>(SourceFootprint.ElementSize);
    if (!SourceFootprint.Exact ||
        SourceFootprint.Base != Candidate.Source.Root ||
        !SourceFootprint.ObjectSize || !SourceFootprint.MinByteOffset ||
        !SourceFootprint.MaxByteOffsetExclusive ||
        *SourceFootprint.MinByteOffset != 0 ||
        *SourceFootprint.MaxByteOffsetExclusive !=
            static_cast<int64_t>(*SourceFootprint.ObjectSize) ||
        SourceBytes != static_cast<__int128>(*SourceFootprint.ObjectSize)) {
        return false;
    }
    if (!hasOnlyAffineViewProducerInstructions(Candidate, SE)) return false;
    //5.临时对象不得逃逸或有第二次写入；全部读取必须位于生产循环完成之后。
    std::vector<LoadInst *> TemporaryLoads;
    std::vector<StoreInst *> TemporaryStores;
    std::vector<Instruction *> TemporaryWriters;
    std::unordered_set<Value *> TemporaryVisited;
    if (!collectAffineViewObjectUses(
            Candidate.Destination.Root, F, false, TemporaryVisited,
            TemporaryLoads, TemporaryStores, TemporaryWriters) ||
        TemporaryStores.size() != 1 ||
        TemporaryStores.front() != Candidate.Store ||
        TemporaryWriters.size() != 1 || TemporaryLoads.empty()) {
        return false;
    }
    for (LoadInst *Load : TemporaryLoads) {
        if (!Load || !Load->getParent() || Outer->contains(Load->getParent()) ||
            !DT.dominates(Candidate.ExitBlock, Load->getParent())) {
            return false;
        }
    }
    //6.源对象同样不得逃逸；所有源写必须位于从生产循环出口不可达的先行区域。
    std::vector<LoadInst *> SourceLoads;
    std::vector<StoreInst *> SourceStores;
    std::vector<Instruction *> SourceWriters;
    std::unordered_set<Value *> SourceVisited;
    if (!collectAffineViewObjectUses(
            Candidate.Source.Root, F, true, SourceVisited,
            SourceLoads, SourceStores, SourceWriters)) {
        return false;
    }
    for (Instruction *Writer : SourceWriters) {
        if (!Writer || !Writer->getParent() ||
            isReachableFromAffineViewExit(
                Candidate.ExitBlock, Writer->getParent())) {
            return false;
        }
    }
    //7.逐个消费者反解目标轴并代入源置换；只复用原索引值，不生成任何新算术。
    Candidate.Rewrites.clear();
    Candidate.Rewrites.reserve(TemporaryLoads.size());
    for (LoadInst *Load : TemporaryLoads) {
        AffineViewAccess Consumer;
        if (!matchAffineViewAccess(Load->getPointerOperand(), Consumer) ||
            Consumer.Root != Candidate.Destination.Root ||
            Consumer.Indices.size() != Candidate.Destination.Indices.size() ||
            Consumer.ByteStrides != Candidate.Destination.ByteStrides ||
            !Load->getIRType()->hasSameStorageType(
                *Candidate.Load->getIRType())) {
            return false;
        }
        Value *AxisValues[2] = {nullptr, nullptr};
        for (size_t Index = 0; Index < Consumer.Indices.size(); ++Index) {
            const int Axis = Candidate.Destination.AxisAtIndex[Index];
            if (Axis < 0) {
                if (!Value::areEqualConstants(
                        Consumer.Indices[Index],
                        Candidate.Destination.Indices[Index])) {
                    return false;
                }
                continue;
            }
            Value *ConsumerIndex = Consumer.Indices[Index];
            Value *ProducerIndex = Candidate.Destination.Indices[Index];
            if (!ConsumerIndex || !ProducerIndex ||
                !ConsumerIndex->getIRType()->hasSameShape(
                    *ProducerIndex->getIRType())) {
                return false;
            }
            AxisValues[Axis] = ConsumerIndex;
        }
        if (!AxisValues[0] || !AxisValues[1]) return false;
        AffineViewRewrite Rewrite;
        Rewrite.Load = Load;
        Rewrite.SourceIndices.reserve(Candidate.Source.Indices.size());
        for (size_t Index = 0;
             Index < Candidate.Source.Indices.size(); ++Index) {
            const int Axis = Candidate.Source.AxisAtIndex[Index];
            Value *NewIndex = Axis < 0
                ? Candidate.Source.Indices[Index]
                : AxisValues[Axis];
            if (!NewIndex ||
                !NewIndex->getIRType()->hasSameShape(
                    *Candidate.Source.Indices[Index]->getIRType())) {
                return false;
            }
            Rewrite.SourceIndices.push_back(NewIndex);
        }
        Candidate.Rewrites.push_back(std::move(Rewrite));
    }
    return !Candidate.Rewrites.empty();
}
//提交完整证明后的二维只读仿射视图消除
bool LoopIdiomRecognizePass::transformAffineView(
    Function &F, Loop *Outer, LoopAccessInfo &LA, SCEVResult &SE,
    DominatorTree &DT) {
    AffineViewCandidate Candidate;
    if (!buildAffineViewCandidate(F, Outer, LA, SE, DT, Candidate))
        return false;
    //1.先离线构造所有源地址，确保类型完全一致后才触碰现有消费者。
    std::vector<std::unique_ptr<GetElementPtrInst>> Planned;
    Planned.reserve(Candidate.Rewrites.size());
    for (const AffineViewRewrite &Rewrite : Candidate.Rewrites) {
        if (!Rewrite.Load || !Rewrite.Load->getParent()) return false;
        std::vector<Value *> Indices = Rewrite.SourceIndices;
        auto Address = std::make_unique<GetElementPtrInst>(
            Candidate.Source.Root, Indices);
        if (!Address->getIRType()->hasSameShape(
                *Rewrite.Load->getPointerOperand()->getIRType())) {
            return false;
        }
        Planned.push_back(std::move(Address));
    }
    //2.每个旧load保持原值身份，仅把地址换成等价源GEP；新GEP不增加索引算术。
    for (size_t Index = 0; Index < Candidate.Rewrites.size(); ++Index) {
        LoadInst *Load = Candidate.Rewrites[Index].Load;
        GetElementPtrInst *Address = Planned[Index].release();
        if (!Load->insertBefore(Address) ||
            !Load->replaceOperand(0, Address)) {
            delete Address;
            return false;
        }
    }
    //3.生产store已无任何可观察消费者，删除唯一复制访存；地址与空循环交给既有清理链。
    delete Candidate.Store;
    if (!Candidate.Load->hasUses()) delete Candidate.Load;
    return true;
}
//提取可由memset表示的重复字节常量
bool LoopIdiomRecognizePass::getRepeatedByte(Value *V, uint8_t &Byte) {
    //1.3 将 i32/float 常量按位查看，四个字节完全相同才可用单字节 memset 表示。
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
//匹配由单个归纳索引驱动的连续同型GEP访问
bool LoopIdiomRecognizePass::matchLinearAccess(
    GetElementPtrInst *GEP, PhiInst *IndVar, Loop *L,
    LinearAccess &Access) {
    //1.1 先清空输出并拆分基址与索引；失败时不留下半匹配状态
    Access = {};
    if (!GEP || !IndVar || !L) return false;
    Value *Base = nullptr;
    std::vector<Value *> Indices;
    if (!GEP->getBaseAndIndices(Base, Indices) || !Base || Indices.empty())
        return false;
    //1.2 基址必须定义在循环外，否则intrinsic首地址无法安全前移到preheader
    if (auto *BaseI = dynamic_cast<Instruction *>(Base)) {
        BasicBlock *BaseBB = BaseI->getParent();
        if (BaseBB && L->contains(BaseBB)) return false;
    }
    //1.3 恰有一个索引等于规范归纳PHI，其余索引必须是循环不变常量
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
    //1.4 动态维步长与最终元素步长都必须等于元素大小，保证迭代间地址严格连续
    std::vector<int64_t> Strides;
    int64_t FinalStride = 0;
    if (!GetElementPtrInst::getIndexStrides(Base, Indices, Strides,
                                            FinalStride) ||
        DynamicIndex >= Strides.size() ||
        Strides[DynamicIndex] != static_cast<int64_t>(ElementTy->getSize()) ||
        FinalStride != static_cast<int64_t>(ElementTy->getSize()))
        return false;
    //1.5 所有证明完成后一次性提交匹配结果，任何保守回退都保持输出为空
    Access.GEP = GEP;
    Access.Base = Base;
    Access.Indices = std::move(Indices);
    Access.InductionIndex = DynamicIndex;
    Access.ElementTy = ElementTy;
    return true;
}
//证明循环仅含惯用法替换允许的指令
bool LoopIdiomRecognizePass::hasOnlyAllowedLoopInstructions(
    Loop *L, const LoopTripCountInfo &TCI,
    const InductionDescriptor &IV, StoreInst *Store,
    GetElementPtrInst *DestGEP, LoadInst *SourceLoad,
    GetElementPtrInst *SourceGEP, DominatorTree &DT) {
    //2.1 循环体只能包含归纳更新、唯一退出比较、地址、单次 load/store 和分支；
    //store 必须支配 latch，且这些关键值不能在循环外另有用户。
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
//在循环preheader中物化线性访问的首元素地址
GetElementPtrInst *LoopIdiomRecognizePass::materializeStartPointer(
    const LinearAccess &Access, PhiInst *IndVar, int64_t Start,
    BasicBlock *PreHeader) {
    //3.1 用常量起始值替换线性地址中的归纳索引，并在 preheader 终结指令前生成
    //intrinsic 所需首元素指针。
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
//计算线性访问起点到唯一已知底层对象末端的剩余容量
bool LoopIdiomRecognizePass::getKnownRemainingObjectBytes(
    const LinearAccess &Access, int64_t Start, BasicAAResult &AA,
    size_t &RemainingBytes) {
    //1.基址必须具有唯一对象、精确字节偏移和静态对象大小；参数、合流指针或
    //动态子对象偏移均没有足够证据，不能据此生成整段内存 intrinsic。
    RemainingBytes = 0;
    if (!Access.Base || !Access.GEP || Access.Indices.empty() ||
        Access.InductionIndex >= Access.Indices.size()) {
        return false;
    }
    const PointerProvenanceInfo &Provenance =
        AA.getPointerProvenance(Access.Base);
    auto BaseOffset = Provenance.getExactByteOffset();
    if (!Provenance.getUniqueBase() || !Provenance.ObjectSize ||
        !BaseOffset) {
        return false;
    }
    //2.按原 typed GEP 布局计算指定起始迭代的精确字节偏移，使用宽整数承接
    //乘加，避免在证明过程中依赖宿主整数回绕。
    std::vector<int64_t> Strides;
    int64_t FinalStride = 0;
    if (!GetElementPtrInst::getIndexStrides(
            Access.Base, Access.Indices, Strides, FinalStride) ||
        Strides.size() != Access.Indices.size() || FinalStride <= 0) {
        return false;
    }
    __int128 Offset = static_cast<__int128>(*BaseOffset);
    for (size_t Index = 0; Index < Access.Indices.size(); ++Index) {
        int64_t IndexValue = 0;
        if (Index == Access.InductionIndex) {
            IndexValue = Start;
        } else if (!Access.Indices[Index] ||
                   !Access.Indices[Index]->tryGetInt64Const(IndexValue)) {
            return false;
        }
        Offset += static_cast<__int128>(IndexValue) *
                  static_cast<__int128>(Strides[Index]);
    }
    //3.只在起点位于完整分配对象的半开区间内时发布剩余容量；调用方再用
    //count*元素字节约束整段访问，因而不会把首地址合法误当成完整足迹合法。
    const __int128 ObjectBytes =
        static_cast<__int128>(*Provenance.ObjectSize);
    if (Offset < 0 || Offset > ObjectBytes) return false;
    RemainingBytes = static_cast<size_t>(ObjectBytes - Offset);
    return true;
}
//匹配可参与临时数组双复制消除的简单连续复制循环
bool LoopIdiomRecognizePass::matchSimpleCopyLoop(
    Loop *L, SCEVResult &SE, DominatorTree &DT,
    CopyLoopMatch &Match) {
    //4.1 识别单位正步长的最内层复制循环；常量次数必须正且无回绕，运行期形态
    //必须是 header 中 iv < bound，并记录 preheader/exit/起点与边界。
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
    //4.2 要求恰好一条 load feeding 唯一 store，二者地址均为同型连续线性访问，
    //并通过允许指令集合检查排除额外计算、调用和条件控制。
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
//把经临时数组衔接的两段连续复制合并为memmove
bool LoopIdiomRecognizePass::transformTemporaryCopyPair(
    Function &F, const CopyLoopMatch &First,
    const CopyLoopMatch &Second, DominatorTree &DT, RangeInfo &RI,
    BasicAAResult &AA, bool &CFGChanged) {
    //4. 从已匹配复制循环中寻找 source→temporary 与 temporary→destination 的相邻
    //二段复制；起点、次数/运行期边界及桥接 CFG 必须完全一致。
    if (!First.L || !Second.L || First.L == Second.L ||
        First.ExitBlock != Second.PreHeader ||
        First.Start != Second.Start ||
        First.IsRuntime != Second.IsRuntime)
        return false;
    const size_t ElementBytes = First.Source.ElementTy
                                    ? First.Source.ElementTy->getSize()
                                    : 0;
    if (ElementBytes == 0 ||
        ElementBytes >
            static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    const int64_t MaxRepresentableCount =
        std::numeric_limits<int32_t>::max() /
        static_cast<int64_t>(ElementBytes);
    if (First.IsRuntime) {
        if (!First.RuntimeBound ||
            First.RuntimeBound != Second.RuntimeBound)
            return false;
    } else if (First.TripCount != Second.TripCount ||
               First.TripCount > MaxRepresentableCount) {
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
    //4.3 中间对象必须是只被两条对应 GEP 使用的数组 alloca，读写索引形状一致；
    //这证明临时数组可消去并将两段复制合成为 memmove。
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
    //4.4 源、最终目标和运行期边界必须在第一段 preheader 可用，才能把合并调用前移。
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
    //4.5 分别求出最终源、临时写入、临时读取和最终目标从首元素到完整对象末端的
    //容量。四者都必须具有唯一对象与精确起点；这既约束新 memmove，也不借助原
    //两段循环可能存在的越界行为。运行期 guard 使用四者容量的最小值。
    size_t SourceRemaining = 0;
    size_t TempWriteRemaining = 0;
    size_t TempReadRemaining = 0;
    size_t DestRemaining = 0;
    if (!getKnownRemainingObjectBytes(
            First.Source, First.Start, AA, SourceRemaining) ||
        !getKnownRemainingObjectBytes(
            First.Destination, First.Start, AA, TempWriteRemaining) ||
        !getKnownRemainingObjectBytes(
            Second.Source, Second.Start, AA, TempReadRemaining) ||
        !getKnownRemainingObjectBytes(
            Second.Destination, Second.Start, AA, DestRemaining)) {
        return false;
    }
    size_t SafeBytes = SourceRemaining;
    if (TempWriteRemaining < SafeBytes) SafeBytes = TempWriteRemaining;
    if (TempReadRemaining < SafeBytes) SafeBytes = TempReadRemaining;
    if (DestRemaining < SafeBytes) SafeBytes = DestRemaining;
    size_t SafeCount = SafeBytes / ElementBytes;
    if (SafeCount > static_cast<size_t>(MaxRepresentableCount))
        SafeCount = static_cast<size_t>(MaxRepresentableCount);
    if (!First.IsRuntime) {
        const __int128 RequiredBytes =
            static_cast<__int128>(First.TripCount) *
            static_cast<__int128>(ElementBytes);
        if (RequiredBytes > static_cast<__int128>(SafeBytes)) return false;
    } else if (SafeCount < 8) {
        //容量不足以达到统一收益阈值时 fast path 永远不可达，不为其引入版本化开销。
        return false;
    }
    //4.6 容量证明完成后再物化两端首地址，并校验 typed GEP 每一级均在已知对象内。
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
    //4.7 常量次数直接生成已完成足迹证明的字节数；运行期次数同时守卫收益、
    //字节乘法不溢出和完整对象容量，并离线创建 fast block。
    if (!First.IsRuntime) {
        ByteCount = ConstInt::newConstInt(
            static_cast<int32_t>(
                First.TripCount * static_cast<int64_t>(ElementBytes)));
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
        Value *ElementByteValue = Value::makeIntegerLike(
            Bound, static_cast<int32_t>(ElementBytes));
        Value *MinCount = Value::makeIntegerLike(Bound, 8);
        Value *MaxCount = Value::makeIntegerLike(
            Bound, static_cast<int32_t>(SafeCount));
        if (!Start || !Zero || !ElementByteValue || !MinCount ||
            !MaxCount) {
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
            auto *Bytes = new BinaryInst(
                Count, BinaryInst::MUL, ElementByteValue);
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
    //4.8 运行期版本保留原两循环为慢路径，fast block 直达第二段出口；常量版本
    //直接删除两段 load/store/GEP 及失去 use 的临时 alloca。
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
    //4.9 接入 CFG 前失败时按创建逆序删除 fast block、guard 与首地址，保持原循环不变。
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
//把单个连续填充或复制循环替换为memset或memcpy
bool LoopIdiomRecognizePass::transformLoop(
    Function &F, Loop *L, SCEVResult &SE, DominatorTree &DT,
    RangeInfo &RI, BasicAAResult &AA, bool &CFGChanged) {
    //2. 仅处理 LoopSimplify 形态的最内层单位正步长循环，要求常量 trip count
    //正且无回绕；运行期合法性稍后结合退出谓词验证。
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
    //2.2 扫描循环体，要求唯一 store、无调用，并收集 load；目标必须是连续线性地址。
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
    const size_t ElementBytes = DestAccess.ElementTy
                                    ? DestAccess.ElementTy->getSize()
                                    : 0;
    if (ElementBytes == 0 ||
        ElementBytes >
            static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return false;
    }
    const int64_t MaxRepresentableCount =
        std::numeric_limits<int32_t>::max() /
        static_cast<int64_t>(ElementBytes);
    if (TCI->ExitingBlock == TCI->Header &&
        Store->getParent() == TCI->Header)
        return false;
    int64_t TripCount = 0;
    int32_t ConstantByteCount = 0;
    if (IsConstant) {
        TripCount = *TCI->ConstantTripCount;
        if (TripCount > MaxRepresentableCount)
            return false;
        ConstantByteCount = static_cast<int32_t>(
            TripCount * static_cast<int64_t>(ElementBytes));
    }
    LoadInst *SourceLoad = nullptr;
    GetElementPtrInst *SourceGEP = nullptr;
    LinearAccess SourceAccess;
    uint8_t FillByte = 0;
    //2.3 store 常量若能表示为重复字节则匹配 memset 且不得有 load；否则必须是
    //唯一 load feeding store，源/目标同存储类型且由不同对象根证明不重叠，匹配 memcpy。
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
    //3.先从精确起点求目标及可选源对象的剩余容量。常量次数必须静态证明完整
    //count*元素字节均在界内；运行期以两端容量最小值形成 fast-path 上界。
    size_t DestRemaining = 0;
    if (!getKnownRemainingObjectBytes(
            DestAccess, *IV->ConstantStart, AA, DestRemaining)) {
        return false;
    }
    size_t SafeBytes = DestRemaining;
    if (!IsMemset) {
        size_t SourceRemaining = 0;
        if (!getKnownRemainingObjectBytes(
                SourceAccess, *IV->ConstantStart, AA,
                SourceRemaining)) {
            return false;
        }
        if (SourceRemaining < SafeBytes) SafeBytes = SourceRemaining;
    }
    size_t SafeCount = SafeBytes / ElementBytes;
    if (SafeCount > static_cast<size_t>(MaxRepresentableCount))
        SafeCount = static_cast<size_t>(MaxRepresentableCount);
    if (IsConstant) {
        if (static_cast<__int128>(ConstantByteCount) >
            static_cast<__int128>(SafeBytes)) {
            return false;
        }
    } else if (SafeCount < 8) {
        //fast path 不可能达到统一收益阈值时不增加分支和不可达 intrinsic 块。
        return false;
    }
    //3.1 容量证明完成后物化目标及可选源首地址，并继续校验 typed GEP 每级边界。
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
    //3.2 常量次数使用已完成足迹证明的字节数；运行期仅接受 iv < bound，且 bound
    //在preheader可用。构造 count、收益阈值和完整对象容量 guard 以及 fast block。
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
        Value *ElementByteValue = Value::makeIntegerLike(
            Bound, static_cast<int32_t>(ElementBytes));
        Value *MinFastCount = Value::makeIntegerLike(Bound, 8);
        Value *MaxFastCount = Value::makeIntegerLike(
            Bound, static_cast<int32_t>(SafeCount));
        if (!StartValue || !ZeroValue || !ElementByteValue ||
            !MinFastCount || !MaxFastCount) {
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
        auto *FitsFootprint = new BinaryInst(Count, BinaryInst::LE,
                                             MaxFastCount);
        auto *UseFastPath = new BinaryInst(EnoughWork, BinaryInst::AND,
                                           FitsFootprint);
        auto *Bytes = new BinaryInst(
            Count, BinaryInst::MUL, ElementByteValue);
        if (!InsertRuntime(EnoughWork) || !InsertRuntime(FitsFootprint) ||
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
    //3.3 根据模式生成 memset/memcpy 参数并注册 builtin；所有离线构造成功后才接入。
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
    //3.4 运行期用 guard 在 intrinsic fast path 与原循环 slow path 间选择；常量版本
    //不改 CFG，只删除被调用完全取代且已无用户的循环内内存指令。
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
//遍历函数内循环并提交安全的内存惯用法替换
PreservationStatus LoopIdiomRecognizePass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    //1. 获取支配、循环、范围与 SCEV，并按内到外顺序快照循环，后续改 CFG 时停止。
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &RI = FAM.getResult<RangeAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &LA = FAM.getResult<LoopAccessAnalysis>(F);
    auto &AA = FAM.getResult<BasicAliasAnalysis>(F);
    bool Changed = false;
    bool CFGChanged = false;
    std::vector<Loop *> Loops = LI.getLoopsInPostOrder(true);
    //2.先尝试完整二维复制视图消除；成功后内存图已经变化，立即返回让外层固定点
    //重建LoopAccess、别名和SCEV，再继续处理其他候选。
    for (Loop *L : Loops) {
        if (transformAffineView(F, L, LA, SE, DT))
            return PreservationStatus::none();
    }
    //4. 优先预匹配全部简单复制循环并尝试消除相邻临时数组复制对；该变换跨两个循环，
    //成功后直接使全部分析失效并返回。
    std::vector<CopyLoopMatch> Copies;
    Copies.reserve(Loops.size());
    //2—3. 未命中成对模式时逐循环识别 memset/memcpy；首次运行期 fast path 改 CFG
    //后立即停止，避免继续使用陈旧 LoopInfo/SCEV。
    for (Loop *L : Loops) {
        CopyLoopMatch Match;
        if (matchSimpleCopyLoop(L, SE, DT, Match))
            Copies.push_back(Match);
    }
    for (const CopyLoopMatch &First : Copies) {
        for (const CopyLoopMatch &Second : Copies) {
            if (transformTemporaryCopyPair(
                    F, First, Second, DT, RI, AA, CFGChanged)) {
                return PreservationStatus::none();
            }
        }
    }
    for (Loop *L : Loops) {
        Changed |= transformLoop(F, L, SE, DT, RI, AA, CFGChanged);
        if (CFGChanged) break;
    }
    if (!Changed) return PreservationStatus::all();
    if (CFGChanged) return PreservationStatus::none();
    //5. 常量次数版本不改 CFG，可保留支配/循环；地址和值变化使 AA/Range/SCEV/副作用失效。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<SCEVAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
