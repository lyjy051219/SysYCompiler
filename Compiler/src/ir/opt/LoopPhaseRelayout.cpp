#include "ir/opt/LoopPhaseRelayout.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopAccessAnalysis.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/DCE.hpp"
#include "ir/opt/LoopIdiomRecognize.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <queue>
#include <unordered_set>

//绑定模块变换期间查询与失效函数级分析所需的分析管理器
LoopPhaseRelayoutPass::LoopPhaseRelayoutPass(
    FunctionAnalysisPassManager &AnalysisManager)
    : FAM(AnalysisManager) {}

//沿typed GEP链恢复根对象、二维逻辑索引及其根类型字节步长
bool LoopPhaseRelayoutPass::matchTwoDimensionalAccess(
    Value *Pointer, LoopPhaseRelayoutAccess &Access) {
    Access = {};
    auto *Address = dynamic_cast<GetElementPtrInst *>(Pointer);
    if (!Address) return false;
    //1.从叶到根收集纯typed GEP链；bitcast或其他指针合流会丢失维度语义，
    //首版直接拒绝。链深预算防止异常IR造成无界分析。
    std::vector<GetElementPtrInst *> Chain;
    std::unordered_set<Value *> Seen;
    Value *Current = Pointer;
    while (auto *GEP = dynamic_cast<GetElementPtrInst *>(Current)) {
        if (!Seen.insert(Current).second || Chain.size() >= 16)
            return false;
        Chain.push_back(GEP);
        Current = GEP->getBasePointer();
        if (!Current) return false;
    }
    auto *Root = dynamic_cast<Variable *>(Current);
    if (!Root || !Root->isGlobal()) return false;
    std::reverse(Chain.begin(), Chain.end());
    //2.后续GEP的首索引是在当前pointee上的指针位移。只有它或上一段末索引
    //为零时才无须创建新加法即可精确拼接；两个非零项继续保守拒绝。
    std::vector<Value *> LogicalIndices;
    for (GetElementPtrInst *GEP : Chain) {
        Value *Base = nullptr;
        std::vector<Value *> Segment;
        if (!GEP->getBaseAndIndices(Base, Segment) || Segment.empty())
            return false;
        if (LogicalIndices.empty()) {
            LogicalIndices = std::move(Segment);
            continue;
        }
        Value *Previous = LogicalIndices.back();
        Value *Leading = Segment.front();
        std::int64_t PreviousConstant = 0;
        std::int64_t LeadingConstant = 0;
        const bool PreviousIsConstant = Previous &&
            Previous->tryGetInt64Const(PreviousConstant);
        const bool LeadingIsConstant = Leading &&
            Leading->tryGetInt64Const(LeadingConstant);
        if (LeadingIsConstant && LeadingConstant == 0) {
            //上一段已经给出当前维度的完整逻辑索引。
        } else if (PreviousIsConstant && PreviousConstant == 0) {
            LogicalIndices.back() = Leading;
        } else {
            return false;
        }
        LogicalIndices.insert(
            LogicalIndices.end(), Segment.begin() + 1, Segment.end());
    }
    if (LogicalIndices.size() != 3) return false;
    auto ObjectType = IRType::getPointeeType(Root->getIRType());
    auto ElementType = IRType::getPointeeType(Address->getIRType());
    std::vector<std::int64_t> Strides;
    std::int64_t FinalStride = 0;
    if (!ObjectType || !ObjectType->isArray() || !ElementType ||
        ((!ElementType->hasIntegerBitWidth(32)) &&
         !(ElementType->isFloat() && ElementType->getSize() == 4)) ||
        !GetElementPtrInst::getIndexStrides(
            Root, LogicalIndices, Strides, FinalStride) ||
        Strides.size() != LogicalIndices.size() ||
        FinalStride != static_cast<std::int64_t>(ElementType->getSize())) {
        return false;
    }
    for (std::int64_t Stride : Strides)
        if (Stride <= 0) return false;
    Access.Address = Address;
    Access.Root = Root;
    Access.Indices = std::move(LogicalIndices);
    Access.ByteStrides = std::move(Strides);
    Access.AxisAtIndex.assign(Access.Indices.size(), -1);
    return true;
}

//把生产者地址中的两个动态索引绑定到外层与内层归纳PHI
bool LoopPhaseRelayoutPass::bindProducerAxes(
    LoopPhaseRelayoutAccess &Access, PhiInst *OuterIV,
    PhiInst *InnerIV) {
    if (!Access.Address || !Access.Root || !OuterIV || !InnerIV ||
        OuterIV == InnerIV ||
        Access.AxisAtIndex.size() != Access.Indices.size()) {
        return false;
    }
    unsigned Counts[2] = {0, 0};
    for (std::size_t Index = 0; Index < Access.Indices.size(); ++Index) {
        Value *V = Access.Indices[Index];
        if (V == OuterIV) {
            Access.AxisAtIndex[Index] = 0;
            ++Counts[0];
            continue;
        }
        if (V == InnerIV) {
            Access.AxisAtIndex[Index] = 1;
            ++Counts[1];
            continue;
        }
        std::int64_t Constant = 0;
        if (!V || !V->tryGetInt64Const(Constant)) return false;
        Access.AxisAtIndex[Index] = -1;
    }
    return Counts[0] == 1 && Counts[1] == 1;
}

//从全局对象类型中提取精确二维标量数组形状并构造转置类型
bool LoopPhaseRelayoutPass::getTwoDimensionalGlobalShape(
    Variable *Source, std::int64_t &Rows, std::int64_t &Columns,
    std::int64_t &ElementBytes, std::size_t &ObjectBytes,
    std::shared_ptr<IRType> &ShadowObjectType) {
    //1.首版只处理内部可追踪的可写二维i32/f32全局对象；完整矩形类型同时
    //给出精确对象大小，避免从测试尺寸或源变量名称推测布局。
    Rows = 0;
    Columns = 0;
    ElementBytes = 0;
    ObjectBytes = 0;
    ShadowObjectType.reset();
    if (!Source || Source->vartag != Variable::GlobalVariable)
        return false;
    auto ObjectType = IRType::getPointeeType(Source->getIRType());
    auto RowArray = std::dynamic_pointer_cast<ARRAYType>(ObjectType);
    auto ColumnArray = RowArray
        ? std::dynamic_pointer_cast<ARRAYType>(RowArray->getElementType())
        : nullptr;
    auto ElementType = ColumnArray ? ColumnArray->getElementType() : nullptr;
    if (!RowArray || !ColumnArray || !ElementType || ElementType->isArray() ||
        RowArray->getElementCount() <= 0 ||
        ColumnArray->getElementCount() <= 0 ||
        ((!ElementType->hasIntegerBitWidth(32)) &&
         !(ElementType->isFloat() && ElementType->getSize() == 4))) {
        return false;
    }
    Rows = RowArray->getElementCount();
    Columns = ColumnArray->getElementCount();
    ElementBytes = static_cast<std::int64_t>(ElementType->getSize());
    if (ElementBytes <= 0 || ElementBytes > CacheLineBytes ||
        CacheLineBytes % ElementBytes != 0) {
        return false;
    }
    const __int128 ExactBytes = static_cast<__int128>(Rows) * Columns *
                                ElementBytes;
    if (ExactBytes <= 0 ||
        ExactBytes > std::numeric_limits<std::size_t>::max() ||
        static_cast<std::size_t>(ExactBytes) != ObjectType->getSize()) {
        return false;
    }
    ObjectBytes = static_cast<std::size_t>(ExactBytes);
    auto TransposedRow = ARRAYType::NewArray(
        ElementType, static_cast<int>(Rows));
    ShadowObjectType = ARRAYType::NewArray(
        TransposedRow, static_cast<int>(Columns));
    return ShadowObjectType && ShadowObjectType->getSize() == ObjectBytes;
}

//证明两层循环以零起点单位步长完整覆盖给定行列域
bool LoopPhaseRelayoutPass::isCanonicalFullProducer(
    Loop *Outer, Loop *Inner, std::int64_t Rows,
    std::int64_t Columns, LoopAccessInfo &LA, SCEVResult &SE,
    BasicBlock *&ExitBlock) {
    //1.只接受顶层直接父子矩形循环；这样一次函数调用中的shadow完整初始化
    //先于消费者，首版无需猜测外围运行时循环的阶段关系。
    ExitBlock = nullptr;
    if (!Outer || !Inner || Outer->getParentLoop() ||
        Inner->getParentLoop() != Outer || !Inner->isInnermost() ||
        Outer->getSubLoops().size() != 1 ||
        Outer->getSubLoops().front() != Inner ||
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
    auto OuterIV = SE.getCanonicalInduction(Outer);
    auto InnerIV = SE.getCanonicalInduction(Inner);
    auto OuterTrip = SE.analyzeLoopTripCount(Outer);
    auto InnerTrip = SE.analyzeLoopTripCount(Inner);
    if (!OuterIV || !InnerIV || !OuterIV->Valid || !InnerIV->Valid ||
        !OuterIV->ConstantStart || !InnerIV->ConstantStart ||
        !OuterIV->ConstantStep || !InnerIV->ConstantStep ||
        *OuterIV->ConstantStart != 0 || *InnerIV->ConstantStart != 0 ||
        *OuterIV->ConstantStep != 1 || *InnerIV->ConstantStep != 1 ||
        !OuterTrip || !InnerTrip || !OuterTrip->Valid || !InnerTrip->Valid ||
        !OuterTrip->ProvenNoWrap || !InnerTrip->ProvenNoWrap ||
        !OuterTrip->ConstantTripCount || !InnerTrip->ConstantTripCount ||
        *OuterTrip->ConstantTripCount != Rows ||
        *InnerTrip->ConstantTripCount != Columns ||
        OuterTrip->MayBeZeroTrip || InnerTrip->MayBeZeroTrip) {
        return false;
    }
    std::vector<BasicBlock *> Exits = Outer->getExitBlocks();
    if (Exits.size() != 1 || !Exits.front()) return false;
    ExitBlock = Exits.front();
    return true;
}

//证明候选是每个逻辑元素在本轮生产阶段的最后一次必执行访问
bool LoopPhaseRelayoutPass::isFinalPerElementStore(
    Loop *Outer, Loop *Inner, StoreInst *Candidate,
    const std::vector<LoadInst *> &SourceLoads,
    const std::vector<StoreInst *> &SourceStores,
    DominatorTree &DT) {
    if (!Outer || !Inner || !Candidate || !Candidate->getParent() ||
        !Inner->contains(Candidate->getParent()) ||
        !Candidate->getPointerOperand() || !Candidate->getValueOperand() ||
        valueMayDependOnUndef(Candidate->getValueOperand())) {
        return false;
    }
    BasicBlock *InnerPreHeader = nullptr;
    BasicBlock *InnerLatch = nullptr;
    if (!Inner->getLoopSimplifyForm(InnerPreHeader, InnerLatch) ||
        !InnerLatch || !DT.dominates(Candidate->getParent(), InnerLatch)) {
        return false;
    }
    BasicBlock *Body = Candidate->getParent();
    Value *Pointer = Candidate->getPointerOperand();
    //2.同一生产域内对源对象的全部真实访问必须落在一个必经块、使用同一
    //typed地址，并位于候选之前；这同时排除条件末写、跨元素依赖与末写后读取。
    for (LoadInst *Load : SourceLoads) {
        if (!Load || !Load->getParent()) return false;
        if (!Outer->contains(Load->getParent())) continue;
        if (Load->getParent() != Body ||
            Load->getPointerOperand() != Pointer ||
            !Instruction::comesBefore(Load, Candidate)) {
            return false;
        }
    }
    bool SawCandidate = false;
    for (StoreInst *Store : SourceStores) {
        if (!Store || !Store->getParent()) return false;
        if (!Outer->contains(Store->getParent())) continue;
        if (Store == Candidate) {
            SawCandidate = true;
            continue;
        }
        if (Store->getParent() != Body ||
            Store->getPointerOperand() != Pointer ||
            !Instruction::comesBefore(Store, Candidate)) {
            return false;
        }
    }
    if (!SawCandidate) return false;
    //3.生产循环内不跨越调用或函数返回发布额外镜像状态；普通计算、其他对象
    //访存和潜在陷阱仍保持原顺序，镜像store紧跟已完成的原最终store。
    for (BasicBlock *BB : Outer->getBlocksIncludingSubLoops()) {
        if (!BB) return false;
        for (Instruction *I : *BB)
            if (dynamic_cast<CallInst *>(I) ||
                dynamic_cast<RetInst *>(I)) return false;
    }
    return true;
}

//判断生产出口到指定消费者之前是否存在可能重入或形成顺序屏障的调用
bool LoopPhaseRelayoutPass::hasCallOnPath(
    BasicBlock *Start, LoadInst *Target) {
    if (!Start || !Target || !Target->getParent() ||
        Start->getParent() != Target->getParentFunction()) {
        return true;
    }
    BasicBlock *TargetBlock = Target->getParent();
    //1.从消费者反向收集能够到达它的块，再与生产出口的正向可达域相交。
    std::unordered_set<BasicBlock *> CanReachTarget;
    std::queue<BasicBlock *> Worklist;
    CanReachTarget.insert(TargetBlock);
    Worklist.push(TargetBlock);
    while (!Worklist.empty()) {
        BasicBlock *BB = Worklist.front();
        Worklist.pop();
        if (CanReachTarget.size() > MaxPathScanBlocks) return true;
        for (BasicBlock *Pred : BB->getPredecessors()) {
            if (Pred && CanReachTarget.insert(Pred).second)
                Worklist.push(Pred);
        }
    }
    if (!CanReachTarget.count(Start)) return true;
    //2.任何位于某条出口到load路径上的调用都可能递归重入当前函数或形成
    //未知顺序边界；首版统一拒绝，避免共享shadow被覆盖。
    for (BasicBlock *BB : CanReachTarget) {
        if (!BB ||
            !LoopIdiomRecognizePass::isReachableFromAffineViewExit(
                Start, BB)) {
            continue;
        }
        //目标块也扫描完整指令序列：load之后的调用可能经循环回边影响下一次
        //动态load，不能只检查第一次到达该静态指令之前的前缀。
        for (Instruction *I : *BB)
            if (dynamic_cast<CallInst *>(I)) return true;
    }
    return false;
}

//在真实访存点证明二维索引落在对象边界内
bool LoopPhaseRelayoutPass::areIndicesProvablyInBounds(
    Value *RowIndex, Value *ColumnIndex, Instruction *Context,
    std::int64_t Rows, std::int64_t Columns, SCEVResult &SE,
    RangeInfo &RI, const DominatorTree &DT) {
    if (!RowIndex || !ColumnIndex || !Context || Rows <= 0 || Columns <= 0)
        return false;
    //地址索引不能依赖undef；范围收窄只证明数值边界，不能把未定义值当成
    //某个恰好落在对象内的选择，从而利用源程序未定义行为触发布局改写。
    if (valueMayDependOnUndef(RowIndex) ||
        valueMayDependOnUndef(ColumnIndex)) {
        return false;
    }
    auto IsInBounds = [&](Value *Index, std::int64_t Extent) {
        std::int64_t Constant = 0;
        if (Index->tryGetInt64Const(Constant))
            return Constant >= 0 && Constant < Extent;
        IntRange Range = SE.evaluateValueRangeAt(Index, Context, RI, DT);
        return !Range.isBottom() && !Range.isTop() &&
               Range.min >= 0 && Range.max < Extent;
    };
    return IsInBounds(RowIndex, Rows) &&
           IsInBounds(ColumnIndex, Columns);
}

//返回整数索引沿指定循环轴的常量步长，循环不变量返回零
std::optional<std::int64_t>
LoopPhaseRelayoutPass::getIndexStepForLoop(
    Value *Index, Loop *Axis, SCEVResult &SE) {
    if (!Index || !Axis) return std::nullopt;
    if (LoopInfo::isLoopInvariant(Index, Axis)) return std::int64_t{0};
    auto Affine = SE.getAffine(Index);
    if (!Affine || Affine->RecLoop != Axis || !Affine->ConstantStep)
        return std::nullopt;
    return *Affine->ConstantStep;
}

//仅依据包含当前指令的循环次数估计动态执行次数并做饱和保护
std::int64_t LoopPhaseRelayoutPass::estimateExecutionCount(
    Instruction *I, LoopInfo &LI, SCEVResult &SE,
    RangeInfo &RI, DominatorTree &DT) {
    if (!I || !I->getParent()) return 0;
    std::int64_t Count = 1;
    Loop *MandatoryNestedLoop = nullptr;
    //1.只有load所在块在最内层每轮必经，且每个外层迭代都必经下一层子循环
    //时，才把各层次数相乘。这样条件分支中的冷load不会被按完整循环高估。
    for (Loop *L = LI.getLoopFor(I->getParent());
         L; L = L->getParentLoop()) {
        auto Trip = SE.analyzeLoopTripCount(L);
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Latch = nullptr;
        if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
            !PreHeader || !Latch || !Trip || !Trip->Valid ||
            !Trip->ExitingBlock) {
            return 0;
        }
        std::vector<BasicBlock *> ExitingBlocks;
        for (BasicBlock *BB : L->getBlocksIncludingSubLoops())
            if (BB && L->isExitingBlock(BB)) ExitingBlocks.push_back(BB);
        if (ExitingBlocks.size() != 1 ||
            ExitingBlocks.front() != Trip->ExitingBlock) {
            return 0;
        }
        if (!MandatoryNestedLoop) {
            if (!DT.dominates(I->getParent(), Latch)) return 0;
        } else {
            if (MandatoryNestedLoop->getParentLoop() != L ||
                !DT.dominates(MandatoryNestedLoop->getHeader(), Latch)) {
                return 0;
            }
        }
        std::int64_t Factor = 0;
        if (Trip && Trip->Valid && Trip->ConstantTripCount) {
            if (*Trip->ConstantTripCount <= 0) return 0;
            Factor = *Trip->ConstantTripCount;
        } else if (Trip && Trip->Valid && Trip->TripCount &&
                   Trip->PreHeader) {
            auto Range = SE.evaluateRangeAt(
                Trip->TripCount, Trip->PreHeader, RI, DT);
            if (Range && !Range->isBottom() && !Range->isTop() &&
                Range->min > 0) {
                Factor = Range->min;
            }
        }
        //2.符号TripCount尚不能求值时，继续用首轮比较本身证明至少执行一次。
        //只接受直接比较Header PHI或其规范回边更新，并在preheader检查起点、
        //边界和第一步均不回绕；这覆盖变量起点的三角循环而不猜测输入。
        if (Factor <= 0 && Trip && Trip->Valid && Trip->ExitCmp &&
            Trip->PreHeader && Trip->PreHeader->getTerminator()) {
            auto Induction = SE.getCanonicalInduction(L);
            Value *Left = nullptr;
            Value *Right = nullptr;
            if (Induction && Induction->Phi && Induction->UpdateValue &&
                Induction->StartValue && Induction->ConstantStep &&
                *Induction->ConstantStep != 0 &&
                Trip->ExitCmp->getOperands(Left, Right) && Left && Right) {
                BinaryInst::Operation Predicate = Trip->ContinueOnTrue
                    ? Trip->ExitCmp->getOp()
                    : BinaryInst::negatePredicate(Trip->ExitCmp->getOp());
                Value *Compared = Left;
                Value *Bound = Right;
                if (Compared != Induction->Phi &&
                    Compared != Induction->UpdateValue) {
                    if (Bound == Induction->Phi ||
                        Bound == Induction->UpdateValue) {
                        std::swap(Compared, Bound);
                        Predicate = BinaryInst::reversePredicate(Predicate);
                    } else {
                        Compared = nullptr;
                    }
                }
                if (Compared && LoopInfo::isLoopInvariant(Bound, L)) {
                    IntRange StartRange = RI.query(
                        Induction->StartValue,
                        Trip->PreHeader->getTerminator(), DT);
                    IntRange BoundRange = RI.query(
                        Bound, Trip->PreHeader->getTerminator(), DT);
                    IntRange ComparedRange = StartRange;
                    if (Compared == Induction->UpdateValue) {
                        ComparedRange = ComparedRange.add(
                            IntRange(*Induction->ConstantStep));
                    }
                    if (!ComparedRange.isBottom() &&
                        !ComparedRange.isTop() &&
                        !BoundRange.isBottom() && !BoundRange.isTop()) {
                        bool AlwaysContinues = false;
                        switch (Predicate) {
                        case BinaryInst::L:
                            AlwaysContinues =
                                ComparedRange.max < BoundRange.min;
                            break;
                        case BinaryInst::LE:
                            AlwaysContinues =
                                ComparedRange.max <= BoundRange.min;
                            break;
                        case BinaryInst::G:
                            AlwaysContinues =
                                ComparedRange.min > BoundRange.max;
                            break;
                        case BinaryInst::GE:
                            AlwaysContinues =
                                ComparedRange.min >= BoundRange.max;
                            break;
                        default:
                            break;
                        }
                        if (AlwaysContinues) Factor = 1;
                    }
                }
            }
        }
        if (Factor <= 0) return 0;
        //乘法前按最终返回类型上界做饱和，避免深循环的大次数先触发宿主
        //__int128有符号溢出，再在事后比较。
        if (Count > std::numeric_limits<std::int64_t>::max() / Factor)
            Count = std::numeric_limits<std::int64_t>::max();
        else
            Count *= Factor;
        MandatoryNestedLoop = L;
    }
    return Count;
}

//把有符号步长安全转换为无符号幅值
std::uint64_t LoopPhaseRelayoutPass::absoluteMagnitude(
    std::int64_t Value) {
    if (Value >= 0) return static_cast<std::uint64_t>(Value);
    return static_cast<std::uint64_t>(-(Value + 1)) + 1;
}

//从模块函数与全局符号中生成不会冲突的内部shadow名称
std::string LoopPhaseRelayoutPass::makeUniqueShadowName(
    Module &M, unsigned Ordinal) {
    auto IsUsed = [&](const std::string &Name) {
        const std::string IRName = ".G." + Name;
        for (const auto &Global : M.getGlobalVar())
            if (Global && Global->getIdent() == IRName) return true;
        for (const auto &FunctionPtr : M.getFunctions())
            if (FunctionPtr && FunctionPtr->getIdent() == IRName)
                return true;
        return false;
    };
    const std::string Stem =
        ".__sysy_phase_layout." + std::to_string(Ordinal);
    if (!IsUsed(Stem)) return Stem;
    for (unsigned Suffix = 1;; ++Suffix) {
        std::string Candidate = Stem + "." + std::to_string(Suffix);
        if (!IsUsed(Candidate)) return Candidate;
    }
}

//为一个二维生产循环构建完整的镜像与消费者改写计划
bool LoopPhaseRelayoutPass::buildCandidate(
    Function &F, Loop *Outer, LoopAccessInfo &LA, LoopInfo &LI,
    SCEVResult &SE, RangeInfo &RI, DominatorTree &DT,
    LoopPhaseRelayoutCandidate &Candidate) {
    Candidate = {};
    if (!Outer || Outer->getSubLoops().size() != 1) return false;
    Loop *Inner = Outer->getSubLoops().front();
    auto OuterInduction = SE.getCanonicalInduction(Outer);
    auto InnerInduction = SE.getCanonicalInduction(Inner);
    if (!OuterInduction || !InnerInduction ||
        !OuterInduction->Phi || !InnerInduction->Phi) {
        return false;
    }
    LoopPhaseRelayoutCandidate Best;
    //1.逐个检查内层store；二维形状、完整覆盖和“最后访问”共同决定生产者，
    //不会读取全局名、函数名、常量尺寸或测试用例特征。
    for (StoreInst *FinalStore : collectInstructions<StoreInst>(F)) {
        if (!FinalStore || !FinalStore->getParent() ||
            !Inner->contains(FinalStore->getParent())) {
            continue;
        }
        LoopPhaseRelayoutAccess ProducerAccess;
        if (!matchTwoDimensionalAccess(
                FinalStore->getPointerOperand(), ProducerAccess)) {
            continue;
        }
        if (ProducerAccess.Indices.size() != 3) {
            continue;
        }
        if (!bindProducerAxes(
                ProducerAccess, OuterInduction->Phi,
                InnerInduction->Phi)) {
            continue;
        }
        if (ProducerAccess.AxisAtIndex[1] != 0 ||
            ProducerAccess.AxisAtIndex[2] != 1) {
            continue;
        }
        std::int64_t LeadingIndex = 0;
        if (!ProducerAccess.Indices[0] ||
            !ProducerAccess.Indices[0]->tryGetInt64Const(LeadingIndex) ||
            LeadingIndex != 0) {
            continue;
        }
        auto *Source = dynamic_cast<Variable *>(ProducerAccess.Root);
        std::int64_t Rows = 0;
        std::int64_t Columns = 0;
        std::int64_t ElementBytes = 0;
        std::size_t ObjectBytes = 0;
        std::shared_ptr<IRType> ShadowObjectType;
        if (!getTwoDimensionalGlobalShape(
                Source, Rows, Columns, ElementBytes, ObjectBytes,
                ShadowObjectType) ||
            ObjectBytes > MaxShadowBytes ||
            ProducerAccess.ByteStrides.size() != 3 ||
            ProducerAccess.ByteStrides[1] != Columns * ElementBytes ||
            ProducerAccess.ByteStrides[2] != ElementBytes ||
            !FinalStore->getValueOperand() ||
            !IRType::hasSameScalarType(
                FinalStore->getValueOperand()->getIRType(),
                std::dynamic_pointer_cast<ARRAYType>(
                    std::dynamic_pointer_cast<ARRAYType>(
                        IRType::getPointeeType(Source->getIRType()))
                        ->getElementType())->getElementType())) {
            continue;
        }
        BasicBlock *ProducerExit = nullptr;
        if (!isCanonicalFullProducer(
                Outer, Inner, Rows, Columns, LA, SE, ProducerExit) ||
            !LoopIdiomRecognizePass::affineViewCoversWholeObject(
                LA, Outer, FinalStore, Source,
                static_cast<std::int64_t>(Rows * Columns))) {
            continue;
        }
        std::unordered_set<Value *> Visited;
        std::vector<LoadInst *> SourceLoads;
        std::vector<StoreInst *> SourceStores;
        std::vector<Instruction *> SourceWriters;
        if (!LoopIdiomRecognizePass::collectAffineViewObjectUses(
                Source, F, true, Visited, SourceLoads,
                SourceStores, SourceWriters) ||
            !isFinalPerElementStore(
                Outer, Inner, FinalStore, SourceLoads,
                SourceStores, DT)) {
            continue;
        }
        //2.生产完成后若源对象还会被写，shadow可能与原对象失去同步；首版
        //统一拒绝，而不是依据常见执行路径猜测写入是否发生。
        bool HasLaterWriter = false;
        for (Instruction *Writer : SourceWriters) {
            if (!Writer || !Writer->getParent()) {
                HasLaterWriter = true;
                break;
            }
            if (Outer->contains(Writer->getParent())) continue;
            if (LoopIdiomRecognizePass::isReachableFromAffineViewExit(
                    ProducerExit, Writer->getParent())) {
                HasLaterWriter = true;
                break;
            }
        }
        if (HasLaterWriter) continue;
        std::vector<LoopPhaseRelayoutRewrite> Rewrites;
        __int128 ConsumerBenefit = 0;
        const std::int64_t LineElements =
            CacheLineBytes / ElementBytes;
        for (LoadInst *Load : SourceLoads) {
            if (!Load || !Load->getParent() ||
                Outer->contains(Load->getParent()) ||
                !DT.dominates(ProducerExit, Load->getParent()) ||
                !LoopIdiomRecognizePass::isReachableFromAffineViewExit(
                    ProducerExit, Load->getParent()) ||
                hasCallOnPath(ProducerExit, Load)) {
                continue;
            }
            LoopPhaseRelayoutAccess ConsumerAccess;
            if (!matchTwoDimensionalAccess(
                    Load->getPointerOperand(), ConsumerAccess) ||
                ConsumerAccess.Root != Source ||
                ConsumerAccess.Indices.size() != 3 ||
                ConsumerAccess.ByteStrides.size() != 3 ||
                ConsumerAccess.ByteStrides[1] !=
                    Columns * ElementBytes ||
                ConsumerAccess.ByteStrides[2] != ElementBytes) {
                continue;
            }
            std::int64_t ConsumerLeading = 0;
            if (!ConsumerAccess.Indices[0] ||
                !ConsumerAccess.Indices[0]->tryGetInt64Const(
                    ConsumerLeading) ||
                ConsumerLeading != 0 ||
                !areIndicesProvablyInBounds(
                    ConsumerAccess.Indices[1],
                    ConsumerAccess.Indices[2], Load,
                    Rows, Columns, SE, RI, DT)) {
                continue;
            }
            Loop *Axis = LI.getLoopFor(Load->getParent());
            auto AxisTrip = Axis
                ? SE.getSmallConstantTripCount(Axis)
                : std::nullopt;
            auto OriginalStride = Axis
                ? LA.getByteStride(Load, Axis, Axis)
                : std::nullopt;
            auto RowStep = getIndexStepForLoop(
                ConsumerAccess.Indices[1], Axis, SE);
            auto ColumnStep = getIndexStepForLoop(
                ConsumerAccess.Indices[2], Axis, SE);
            if (!Axis || !AxisTrip ||
                !OriginalStride || !RowStep || !ColumnStep ||
                absoluteMagnitude(*OriginalStride) <
                    static_cast<std::uint64_t>(CacheLineBytes)) {
                continue;
            }
            const __int128 TraversalBytes =
                static_cast<__int128>(
                    absoluteMagnitude(*OriginalStride)) * *AxisTrip;
            if (TraversalBytes < MinInnerTraversalBytes) continue;
            const __int128 ExpectedOriginalStride =
                static_cast<__int128>(*RowStep) * Columns * ElementBytes +
                static_cast<__int128>(*ColumnStep) * ElementBytes;
            const __int128 TransposedStride =
                static_cast<__int128>(*ColumnStep) * Rows * ElementBytes +
                static_cast<__int128>(*RowStep) * ElementBytes;
            if (ExpectedOriginalStride != *OriginalStride ||
                TransposedStride < std::numeric_limits<std::int64_t>::min() ||
                TransposedStride > std::numeric_limits<std::int64_t>::max() ||
                absoluteMagnitude(
                    static_cast<std::int64_t>(TransposedStride)) !=
                    static_cast<std::uint64_t>(ElementBytes)) {
                continue;
            }
            std::int64_t Executions =
                estimateExecutionCount(Load, LI, SE, RI, DT);
            if (Executions <= 0) continue;
            std::vector<Value *> TransposedIndices =
                ConsumerAccess.Indices;
            std::swap(TransposedIndices[1], TransposedIndices[2]);
            Rewrites.push_back({Load, std::move(TransposedIndices),
                                Executions});
            ConsumerBenefit += static_cast<__int128>(Executions) *
                               (LineElements - 1);
        }
        if (Rewrites.empty()) continue;
        //3.收益以一个缓存行内的元素数为缩放单位：每个跨行load可节约
        //LineElements-1，镜像的跨行store按一整行收费，并额外计一次冷对象扫描。
        const __int128 ProducerExecutions =
            static_cast<__int128>(Rows) * Columns;
        const __int128 MirrorCost =
            ProducerExecutions * LineElements;
        const __int128 ColdObjectCost =
            ((static_cast<__int128>(ObjectBytes) + CacheLineBytes - 1) /
             CacheLineBytes) * LineElements;
        const __int128 Net =
            ConsumerBenefit - MirrorCost - ColdObjectCost;
        if (Net <= 0 || Net > std::numeric_limits<std::int64_t>::max() ||
            (Best.Target && Net <= Best.EstimatedNetBenefit)) {
            continue;
        }
        Best.Target = &F;
        Best.ProducerOuter = Outer;
        Best.ProducerInner = Inner;
        Best.ProducerExit = ProducerExit;
        Best.Source = Source;
        Best.FinalStore = FinalStore;
        Best.ShadowObjectType = std::move(ShadowObjectType);
        Best.ProducerTransposedIndices = ProducerAccess.Indices;
        std::swap(Best.ProducerTransposedIndices[1],
                  Best.ProducerTransposedIndices[2]);
        Best.Rewrites = std::move(Rewrites);
        Best.Rows = Rows;
        Best.Columns = Columns;
        Best.ElementBytes = ElementBytes;
        Best.ShadowBytes = ObjectBytes;
        Best.EstimatedNetBenefit = static_cast<std::int64_t>(Net);
    }
    if (!Best.Target) return false;
    Candidate = std::move(Best);
    return true;
}

//创建shadow并一次性提交已经完成全部证明的镜像和load改写
bool LoopPhaseRelayoutPass::applyCandidate(
    Module &M, LoopPhaseRelayoutCandidate &Candidate,
    unsigned Ordinal) {
    //1.提交前重新验证所有挂接点和索引快照；通过后不再执行可能失败的分析，
    //避免创建全局对象后进入半提交状态。
    if (Module::getActiveModule() != &M || !Candidate.Target ||
        !Candidate.Source || !Candidate.FinalStore ||
        !Candidate.FinalStore->getParent() ||
        !Candidate.ShadowObjectType || Candidate.ShadowBytes == 0 ||
        Candidate.ProducerTransposedIndices.size() != 3 ||
        Candidate.Rewrites.empty()) {
        return false;
    }
    for (const LoopPhaseRelayoutRewrite &Rewrite : Candidate.Rewrites) {
        if (!Rewrite.Load || !Rewrite.Load->getParent() ||
            Rewrite.TransposedIndices.size() != 3 ||
            Rewrite.EstimatedExecutions <= 0) {
            return false;
        }
    }
    auto *Shadow = new Variable(
        Variable::GlobalVariable, Candidate.ShadowObjectType,
        makeUniqueShadowName(M, Ordinal));
    //2.保留原最终store并紧接着镜像到shadow，因而未改写的读取和函数返回后
    //可观察的原对象状态完全不变；后续DSE可在另有证明时独立删除旧写入。
    auto *ProducerAddress = new GetElementPtrInst(
        Shadow, Candidate.ProducerTransposedIndices);
    auto *MirrorStore = new StoreInst(
        Candidate.FinalStore->getValueOperand(), ProducerAddress);
    Candidate.FinalStore->insertAfter(ProducerAddress);
    ProducerAddress->insertAfter(MirrorStore);
    //3.消费者只替换地址，不移动load和任何归约算术，因此原有整数运算顺序
    //以及计时、I/O边界保持不变。
    for (const LoopPhaseRelayoutRewrite &Rewrite : Candidate.Rewrites) {
        std::vector<Value *> Indices = Rewrite.TransposedIndices;
        auto *Address = new GetElementPtrInst(Shadow, Indices);
        Rewrite.Load->insertBefore(Address);
        Rewrite.Load->replaceOperand(0, Address);
    }
    return true;
}

//识别零起点单位步长动态循环，并直接复用SCEV已经恢复的比较阶段
bool LoopPhaseRelayoutPass::matchCanonicalDynamicAxis(
    Loop *Axis, SCEVResult &SE, PhiInst *&Induction,
    Value *&Bound, BasicBlock *&PreHeader,
    BasicBlock *&Latch, BasicBlock *&Exit) {
    Induction = nullptr;
    Bound = nullptr;
    PreHeader = nullptr;
    Latch = nullptr;
    Exit = nullptr;
    if (!Axis || !Axis->getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch) {
        return false;
    }
    auto IV = SE.getCanonicalInduction(Axis);
    auto Trip = SE.analyzeLoopTripCount(Axis);
    if (!IV || !Trip || !IV->Valid || !Trip->Valid ||
        !IV->Phi || !IV->UpdateValue || !Trip->ExitCmp ||
        !IV->ConstantStart || *IV->ConstantStart != 0 ||
        !IV->ConstantStep || *IV->ConstantStep != 1 ||
        Trip->ComparePhase == LoopExitComparePhase::Unknown ||
        !Trip->ExitBlock) {
        return false;
    }
    Value *Compared = Trip->ComparePhase ==
            LoopExitComparePhase::HeaderPhi
        ? static_cast<Value *>(IV->Phi)
        : IV->UpdateValue;
    Value *Left = Trip->ExitCmp->getOperand(0);
    Value *Right = Trip->ExitCmp->getOperand(1);
    BinaryInst::Operation Predicate = Trip->ContinueOnTrue
        ? Trip->ExitCmp->getOp()
        : BinaryInst::negatePredicate(Trip->ExitCmp->getOp());
    if (Right == Compared) {
        std::swap(Left, Right);
        Predicate = BinaryInst::reversePredicate(Predicate);
    }
    if (Left != Compared || Predicate != BinaryInst::L || !Right ||
        !IRType::hasIntegerBitWidth(Right->getIRType(), 32) ||
        !LoopInfo::isLoopInvariant(Right, Axis) ||
        valueMayDependOnUndef(Right)) {
        return false;
    }
    Induction = IV->Phi;
    Bound = Right;
    Exit = Trip->ExitBlock;
    return true;
}

//识别A[k][j]读取和A[i][j]原位写回，不依赖符号名、具体尺寸或输入值
bool LoopPhaseRelayoutPass::buildConsistentShadowCandidate(
    Function &F, Loop *Outer, LoopAccessInfo &LA,
    SCEVResult &SE, DominatorTree &DT,
    LoopPhaseConsistentShadowCandidate &Candidate) {
    Candidate = {};
    if (!Outer || Outer->getParentLoop() ||
        Outer->getSubLoops().size() != 1) {
        return false;
    }
    Loop *Middle = Outer->getSubLoops().front();
    if (!Middle || Middle->getSubLoops().size() != 1) return false;
    Loop *Inner = Middle->getSubLoops().front();
    if (!Inner || !Inner->isInnermost()) return false;

    PhiInst *OuterIV = nullptr;
    PhiInst *MiddleIV = nullptr;
    PhiInst *InnerIV = nullptr;
    Value *OuterBound = nullptr;
    Value *MiddleBound = nullptr;
    Value *InnerBound = nullptr;
    BasicBlock *OuterPreHeader = nullptr;
    BasicBlock *MiddlePreHeader = nullptr;
    BasicBlock *InnerPreHeader = nullptr;
    BasicBlock *OuterLatch = nullptr;
    BasicBlock *MiddleLatch = nullptr;
    BasicBlock *InnerLatch = nullptr;
    BasicBlock *OuterExit = nullptr;
    BasicBlock *MiddleExit = nullptr;
    BasicBlock *InnerExit = nullptr;
    if (!matchCanonicalDynamicAxis(
            Outer, SE, OuterIV, OuterBound, OuterPreHeader,
            OuterLatch, OuterExit) ||
        !matchCanonicalDynamicAxis(
            Middle, SE, MiddleIV, MiddleBound, MiddlePreHeader,
            MiddleLatch, MiddleExit) ||
        !matchCanonicalDynamicAxis(
            Inner, SE, InnerIV, InnerBound, InnerPreHeader,
            InnerLatch, InnerExit) ||
        OuterBound != MiddleBound || OuterBound != InnerBound ||
        !OuterPreHeader->getTerminator() ||
        OuterPreHeader->countEdgesToSucc(Outer->getHeader()) != 1 ||
        Outer->getExitBlocks().size() != 1 ||
        Outer->getExitBlocks().front() != OuterExit ||
        !OuterExit->collectPhiNodes().empty()) {
        return false;
    }
    //边界必须在版本入口可用；快路径随后只用它证明真实对象范围。
    if (auto *BoundInst = dynamic_cast<Instruction *>(OuterBound)) {
        if (!DT.dominatesValue(
                BoundInst, OuterPreHeader->getTerminator())) {
            return false;
        }
    }
    for (PhiInst *Phi : Outer->getHeader()->collectPhiNodes())
        if (!Phi || Phi->countIncomingFromPred(OuterPreHeader) != 1)
            return false;

    std::vector<BasicBlock *> Blocks = Outer->getBlocksInFunctionOrder();
    if (Blocks.empty() || Blocks.size() > 64) return false;
    std::unordered_set<BasicBlock *> BlockSet(
        Blocks.begin(), Blocks.end());
    std::size_t InstructionCount = 0;
    for (BasicBlock *BB : Blocks) {
        if (!BB || BB->countEdgesToSucc(OuterPreHeader) != 0)
            return false;
        for (Instruction *I : *BB) {
            if (!I || dynamic_cast<CallInst *>(I) ||
                dynamic_cast<RetInst *>(I) || ++InstructionCount > 512) {
                return false;
            }
            //完整慢版本必须自行产生所有SSA值；即使LCSSA暂时失效，也不允许
            //区域外指令直接引用只存在于快版本的定义。
            for (Instruction *User : I->getInstructionUsers())
                if (!User || !User->getParent() ||
                    !BlockSet.count(User->getParent())) return false;
        }
    }

    LoopPhaseConsistentShadowCandidate Best;
    for (BasicBlock *BB : Blocks) {
        for (Instruction *I : *BB) {
            auto *SeedLoad = dynamic_cast<LoadInst *>(I);
            if (!SeedLoad || !Inner->contains(SeedLoad->getParent()))
                continue;
            LoopPhaseRelayoutAccess SeedAccess;
            if (!matchTwoDimensionalAccess(
                    SeedLoad->getPointerOperand(), SeedAccess) ||
                SeedAccess.Indices.size() != 3 ||
                SeedAccess.Indices[1] != InnerIV ||
                SeedAccess.Indices[2] != MiddleIV) {
                continue;
            }
            std::int64_t Leading = 0;
            if (!SeedAccess.Indices[0] ||
                !SeedAccess.Indices[0]->tryGetInt64Const(Leading) ||
                Leading != 0) {
                continue;
            }
            auto *Source = dynamic_cast<Variable *>(SeedAccess.Root);
            std::int64_t Rows = 0;
            std::int64_t Columns = 0;
            std::int64_t ElementBytes = 0;
            std::size_t ObjectBytes = 0;
            std::shared_ptr<IRType> ShadowType;
            if (!getTwoDimensionalGlobalShape(
                    Source, Rows, Columns, ElementBytes, ObjectBytes,
                    ShadowType) || ObjectBytes > MaxShadowBytes ||
                SeedAccess.ByteStrides.size() != 3 ||
                SeedAccess.ByteStrides[1] != Columns * ElementBytes ||
                SeedAccess.ByteStrides[2] != ElementBytes ||
                SeedAccess.ByteStrides[1] < CacheLineBytes ||
                Rows > std::numeric_limits<int32_t>::max() ||
                Columns > std::numeric_limits<int32_t>::max()) {
                continue;
            }
            auto SeedStride = LA.getByteStride(SeedLoad, Inner, Inner);
            if (!SeedStride || *SeedStride !=
                    SeedAccess.ByteStrides[1]) {
                continue;
            }

            std::vector<LoopPhaseRelayoutRewrite> Rewrites;
            StoreInst *FinalStore = nullptr;
            std::vector<Value *> StoreIndices;
            bool InvalidSourceWrite = false;
            for (BasicBlock *UseBlock : Blocks) {
                for (Instruction *UseInst : *UseBlock) {
                    auto *Load = dynamic_cast<LoadInst *>(UseInst);
                    auto *Store = dynamic_cast<StoreInst *>(UseInst);
                    Value *Pointer = Load
                        ? Load->getPointerOperand()
                        : Store ? Store->getPointerOperand() : nullptr;
                    if (!Pointer) continue;
                    //区域内不允许第二个写根；这同时排除可能经参数别名改写Source
                    //的逃逸路径，避免shadow在下一次读取前失去同步。
                    if (Value::stripPointerCastsAndGEP(Pointer) != Source) {
                        if (Store) InvalidSourceWrite = true;
                        continue;
                    }
                    LoopPhaseRelayoutAccess Access;
                    if (!matchTwoDimensionalAccess(Pointer, Access) ||
                        Access.Root != Source ||
                        Access.Indices.size() != 3) {
                        if (Store) InvalidSourceWrite = true;
                        continue;
                    }
                    std::int64_t AccessLeading = 0;
                    if (!Access.Indices[0] ||
                        !Access.Indices[0]->tryGetInt64Const(
                            AccessLeading) || AccessLeading != 0) {
                        if (Store) InvalidSourceWrite = true;
                        continue;
                    }
                    if (Load && Inner->contains(Load->getParent()) &&
                        Access.Indices[1] == InnerIV &&
                        Access.Indices[2] == MiddleIV) {
                        auto Stride = LA.getByteStride(Load, Inner, Inner);
                        if (!Stride || *Stride != Columns * ElementBytes) {
                            InvalidSourceWrite = true;
                            continue;
                        }
                        std::vector<Value *> Transposed = Access.Indices;
                        std::swap(Transposed[1], Transposed[2]);
                        Rewrites.push_back(
                            {Load, std::move(Transposed), 1});
                    }
                    if (!Store) continue;
                    if (FinalStore || Store->getParent() != InnerExit ||
                        Access.Indices[1] != OuterIV ||
                        Access.Indices[2] != MiddleIV ||
                        !Store->getValueOperand() ||
                        valueMayDependOnUndef(Store->getValueOperand()) ||
                        !DT.dominates(Store->getParent(), MiddleLatch) ||
                        !Instruction::comesBefore(
                            Store, Store->getParent()->getTerminator())) {
                        InvalidSourceWrite = true;
                        continue;
                    }
                    FinalStore = Store;
                    StoreIndices = Access.Indices;
                    std::swap(StoreIndices[1], StoreIndices[2]);
                }
            }
            if (InvalidSourceWrite || !FinalStore || Rewrites.empty())
                continue;
            //同一个结构内匹配更多跨行读取时收益不会更低，选择覆盖最多的对象。
            if (Best.Target && Best.Rewrites.size() >= Rewrites.size())
                continue;
            Best.Target = &F;
            Best.PreHeader = OuterPreHeader;
            Best.Header = Outer->getHeader();
            Best.Exit = OuterExit;
            Best.Source = Source;
            Best.Bound = OuterBound;
            Best.FinalStore = FinalStore;
            Best.ShadowObjectType = std::move(ShadowType);
            Best.StoreTransposedIndices = std::move(StoreIndices);
            Best.Rewrites = std::move(Rewrites);
            Best.Blocks = Blocks;
            Best.Rows = Rows;
            Best.Columns = Columns;
            Best.ShadowBytes = ObjectBytes;
        }
    }
    if (!Best.Target) return false;
    Candidate = std::move(Best);
    return true;
}

//版本化动态活动域，并在快路径中维护始终与原对象一致的转置shadow
bool LoopPhaseRelayoutPass::applyConsistentShadowCandidate(
    Module &M, LoopPhaseConsistentShadowCandidate &Candidate,
    unsigned Ordinal) {
    if (Module::getActiveModule() != &M || !Candidate.Target ||
        !Candidate.Source || !Candidate.Bound || !Candidate.FinalStore ||
        !Candidate.PreHeader || !Candidate.Header || !Candidate.Exit ||
        !Candidate.ShadowObjectType || Candidate.ShadowBytes == 0 ||
        Candidate.Blocks.empty() || Candidate.Rewrites.empty() ||
        Candidate.StoreTransposedIndices.size() != 3 ||
        Candidate.PreHeader->countEdgesToSucc(Candidate.Header) != 1 ||
        !Candidate.Exit->collectPhiNodes().empty()) {
        return false;
    }
    for (const LoopPhaseRelayoutRewrite &Rewrite : Candidate.Rewrites)
        if (!Rewrite.Load || !Rewrite.Load->getParent() ||
            Rewrite.TransposedIndices.size() != 3)
            return false;
    for (PhiInst *Phi : Candidate.Header->collectPhiNodes())
        if (!Phi || Phi->countIncomingFromPred(Candidate.PreHeader) != 1)
            return false;

    Function &F = *Candidate.Target;
    auto *PositiveGuard = new BasicBlock();
    auto *UpperGuard = new BasicBlock();
    auto *InitOuter = new BasicBlock();
    auto *InitInner = new BasicBlock();
    auto *InitOuterLatch = new BasicBlock();
    auto *FastPreHeader = new BasicBlock();
    auto *SlowPreHeader = new BasicBlock();
    ValueToValueMap InitialMap;
    InitialMap[Candidate.PreHeader] = SlowPreHeader;
    CloneBlockRegionOptions Options;
    Options.MakeBlockIdent = [&F](const BasicBlock *BB, std::size_t) {
        return F.makeUniqueBlockIdent(
            (BB ? BB->getIdent() : "bb") + ".layout.slow");
    };
    auto Region = cloneBasicBlockRegion(
        F, Candidate.Blocks, InitialMap, Options);
    if (!Region) {
        delete PositiveGuard;
        delete UpperGuard;
        delete InitOuter;
        delete InitInner;
        delete InitOuterLatch;
        delete FastPreHeader;
        delete SlowPreHeader;
        return false;
    }
    BasicBlock *SlowHeader = Region->getClonedBlock(Candidate.Header);
    assert(SlowHeader && "完整区域克隆必须包含外层循环头");

    PositiveGuard->setIdent(F.makeUniqueBlockIdent(
        Candidate.Header->getIdent() + ".layout.positive"));
    UpperGuard->setIdent(F.makeUniqueBlockIdent(
        Candidate.Header->getIdent() + ".layout.upper"));
    InitOuter->setIdent(F.makeUniqueBlockIdent(
        Candidate.Header->getIdent() + ".layout.init.outer"));
    InitInner->setIdent(F.makeUniqueBlockIdent(
        Candidate.Header->getIdent() + ".layout.init.inner"));
    InitOuterLatch->setIdent(F.makeUniqueBlockIdent(
        Candidate.Header->getIdent() + ".layout.init.outer.latch"));
    FastPreHeader->setIdent(F.makeUniqueBlockIdent(
        Candidate.Header->getIdent() + ".layout.fast.preheader"));
    SlowPreHeader->setIdent(F.makeUniqueBlockIdent(
        Candidate.Header->getIdent() + ".layout.slow.preheader"));
    F.pushBlock(PositiveGuard);
    F.pushBlock(UpperGuard);
    F.pushBlock(InitOuter);
    F.pushBlock(InitInner);
    F.pushBlock(InitOuterLatch);
    F.pushBlock(FastPreHeader);
    F.pushBlock(SlowPreHeader);

    auto *Shadow = new Variable(
        Variable::GlobalVariable, Candidate.ShadowObjectType,
        makeUniqueShadowName(M, Ordinal));
    Value *Zero = Value::makeIntegerLike(Candidate.Bound, 0);
    Value *Limit = Value::makeIntegerLike(
        Candidate.Bound, std::min(Candidate.Rows, Candidate.Columns));
    assert(Zero && Limit && "已验证的i32边界必须能够构造常量");

    auto *Positive = new BinaryInst(
        Candidate.Bound, BinaryInst::G, Zero);
    PositiveGuard->pushBack(Positive);
    PositiveGuard->genCondInst(Positive, UpperGuard, SlowPreHeader);
    auto *WithinObject = new BinaryInst(
        Candidate.Bound, BinaryInst::LE, Limit);
    UpperGuard->pushBack(WithinObject);
    UpperGuard->genCondInst(WithinObject, InitOuter, SlowPreHeader);

    auto *InitI = new PhiInst(Candidate.Bound->getIRType());
    auto *InitJ = new PhiInst(Candidate.Bound->getIRType());
    auto *NextI = new BinaryInst(InitI, BinaryInst::ADD,
                                 Value::makeIntegerLike(InitI, 1));
    auto *NextJ = new BinaryInst(InitJ, BinaryInst::ADD,
                                 Value::makeIntegerLike(InitJ, 1));
    InitI->addIncoming(Zero, UpperGuard);
    InitI->addIncoming(NextI, InitOuterLatch);
    InitOuter->pushBack(InitI);
    auto *OuterCondition = new BinaryInst(
        InitI, BinaryInst::L, Candidate.Bound);
    InitOuter->pushBack(OuterCondition);
    InitOuter->genCondInst(OuterCondition, InitInner, FastPreHeader);
    InitJ->addIncoming(Zero, InitOuter);
    InitJ->addIncoming(NextJ, InitInner);
    InitInner->pushBack(InitJ);

    std::vector<Value *> SourceIndices{Zero, InitI, InitJ};
    auto *SourceAddress = new GetElementPtrInst(
        Candidate.Source, SourceIndices);
    auto *SourceValue = new LoadInst(SourceAddress);
    std::vector<Value *> ShadowIndices{Zero, InitJ, InitI};
    auto *ShadowAddress = new GetElementPtrInst(Shadow, ShadowIndices);
    auto *ShadowStore = new StoreInst(SourceValue, ShadowAddress);
    InitInner->pushBack(SourceAddress);
    InitInner->pushBack(SourceValue);
    InitInner->pushBack(ShadowAddress);
    InitInner->pushBack(ShadowStore);
    InitInner->pushBack(NextJ);
    auto *InnerNextCondition = new BinaryInst(
        NextJ, BinaryInst::L, Candidate.Bound);
    InitInner->pushBack(InnerNextCondition);
    InitInner->genCondInst(
        InnerNextCondition, InitInner, InitOuterLatch);

    InitOuterLatch->pushBack(NextI);
    InitOuterLatch->genUnCondInst(InitOuter);
    FastPreHeader->genUnCondInst(Candidate.Header);
    SlowPreHeader->genUnCondInst(SlowHeader);

    //提交点之后只执行已经预检的CFG和地址替换，慢版本保持原始访存语义。
    for (PhiInst *Phi : Candidate.Header->collectPhiNodes()) {
        const bool Replaced = PhiInst::replaceIncomingPred(
            Phi, Candidate.PreHeader, FastPreHeader);
        assert(Replaced);
        (void)Replaced;
    }
    const bool Redirected = BasicBlock::redirectBranchTarget(
        Candidate.PreHeader, Candidate.Header, PositiveGuard);
    assert(Redirected);
    (void)Redirected;

    auto *SyncAddress = new GetElementPtrInst(
        Shadow, Candidate.StoreTransposedIndices);
    auto *SyncStore = new StoreInst(
        Candidate.FinalStore->getValueOperand(), SyncAddress);
    Candidate.FinalStore->insertAfter(SyncAddress);
    SyncAddress->insertAfter(SyncStore);
    for (const LoopPhaseRelayoutRewrite &Rewrite : Candidate.Rewrites) {
        std::vector<Value *> Indices = Rewrite.TransposedIndices;
        auto *Address = new GetElementPtrInst(
            Shadow, Indices);
        Rewrite.Load->insertBefore(Address);
        Rewrite.Load->replaceOperand(0, Address);
    }
    return true;
}

//逐函数选择一个合法且有收益的阶段布局候选并提交改写
PreservationStatus LoopPhaseRelayoutPass::run(
    Module &M, ModuleAnalysisPassManager &MAM) {
    (void)MAM;
    bool Changed = false;
    //1.每轮每个函数至多提交一个收益最大的候选；外层有界固定点会在分析
    //重建后发现其余独立对象，避免在失效的LoopAccess缓存上继续规划。
    for (const auto &FunctionPtr : M.getFunctions()) {
        Function *F = FunctionPtr.get();
        if (!F || !F->front() ||
            CreatedShadowBytes >= MaxTotalShadowBytes) {
            continue;
        }
        auto &LI = FAM.getResult<LoopAnalysis>(*F);
        auto &SE = FAM.getResult<SCEVAnalysis>(*F);
        auto &RI = FAM.getResult<RangeAnalysis>(*F);
        auto &DT = FAM.getResult<DominantAnalysis>(*F);
        auto &LA = FAM.getResult<LoopAccessAnalysis>(*F);
        LoopPhaseRelayoutCandidate Best;
        LoopPhaseConsistentShadowCandidate BestConsistent;
        for (Loop *Outer : LI.getTopLevelLoops()) {
            LoopPhaseRelayoutCandidate Current;
            if (!buildCandidate(
                    *F, Outer, LA, LI, SE, RI, DT, Current)) {
                Current = {};
            }
            if (Current.Target && (!Best.Target ||
                Current.EstimatedNetBenefit > Best.EstimatedNetBenefit)) {
                Best = std::move(Current);
            }
            LoopPhaseConsistentShadowCandidate Consistent;
            if (buildConsistentShadowCandidate(
                    *F, Outer, LA, SE, DT, Consistent) &&
                !VersionedSources.count(Consistent.Source) &&
                (!BestConsistent.Target ||
                 Consistent.Rewrites.size() >
                     BestConsistent.Rewrites.size())) {
                BestConsistent = std::move(Consistent);
            }
        }
        if (BestConsistent.Target &&
            BestConsistent.ShadowBytes <=
                MaxTotalShadowBytes - CreatedShadowBytes &&
            applyConsistentShadowCandidate(
                M, BestConsistent, NextShadowOrdinal++)) {
            CreatedShadowBytes += BestConsistent.ShadowBytes;
            VersionedSources.insert(BestConsistent.Source);
            Changed = true;
            FAM.invalidate(*F, PreservationStatus::none());
            continue;
        }
        if (!Best.Target || Best.ShadowBytes >
                MaxTotalShadowBytes - CreatedShadowBytes ||
            !applyCandidate(M, Best, NextShadowOrdinal++)) continue;
        CreatedShadowBytes += Best.ShadowBytes;
        Changed = true;
        FAM.invalidate(*F, PreservationStatus::none());
    }
    return Changed ? PreservationStatus::none()
                   : PreservationStatus::all();
}
