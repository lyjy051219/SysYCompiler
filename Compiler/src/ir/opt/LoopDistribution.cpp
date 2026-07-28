#include "ir/opt/LoopDistribution.hpp"

#include "ir/analysis/LoopDependence.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/LCSSA.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// 步骤 1.1：循环克隆不复制 call、alloca、ret 或未知二元语义；这些指令即使
// 带只读属性也可能表达输入或其他可观察行为。
bool LoopDistributionPass::hasUnsupportedInstruction(Instruction *I) {
    if (!I) return true;
    if (dynamic_cast<CallInst *>(I) || dynamic_cast<AllocaInst *>(I) ||
        dynamic_cast<RetInst *>(I)) {
        return true;
    }
    return I->instType == Instruction::BinaryUnknown;
}

// 步骤 1.2：递归判断控制条件是否依赖循环内存读取。循环外已求值的 load/call
// 是分布域不变量；重复 SSA 节点表示已检查，深度预算耗尽则保守返回依赖。
bool LoopDistributionPass::valueDependsOnLoad(
    Value *V, std::unordered_set<Value *> &Seen,
    const BlockSet &LoopBlocks, unsigned Depth) {
    if (!V) return false;
    if (Depth > 64) return true;
    if (!Seen.insert(V).second) return false;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return false;
    if (!I->getParent() || !LoopBlocks.count(I->getParent())) return false;
    if (dynamic_cast<LoadInst *>(I) || dynamic_cast<CallInst *>(I))
        return true;
    for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
        if (valueDependsOnLoad(I->getOperand(Index), Seen, LoopBlocks,
                               Depth + 1))
            return true;
    }
    return false;
}

// 步骤 1.3：扫描循环内每条条件分支，拒绝可能因两个分区的内存状态不同而
// 改变控制轨迹的循环。
bool LoopDistributionPass::hasMemoryDependentControl(
    const std::vector<BasicBlock *> &Blocks) {
    BlockSet LoopBlocks(Blocks.begin(), Blocks.end());
    for (BasicBlock *BB : Blocks) {
        auto *Br = BB ? BB->getBranchTerminator() : nullptr;
        if (!BasicBlock::isCondBr(Br)) continue;
        std::unordered_set<Value *> Seen;
        if (valueDependsOnLoad(Br->getCondition(), Seen, LoopBlocks))
            return true;
    }
    return false;
}

// 步骤 1.4：候选循环及其完整子循环树都必须由 SCEV 证明有限。
bool LoopDistributionPass::hasFiniteLoopTree(Loop *L, SCEVResult &SE) {
    if (!L || !SE.isKnownFiniteLoop(L)) return false;
    for (Loop *SubLoop : L->getSubLoops()) {
        if (!hasFiniteLoopTree(SubLoop, SE)) return false;
    }
    return true;
}

// 步骤 1.5：循环内定义不得在循环外使用；分布后无需跨两套循环重建 live-out。
bool LoopDistributionPass::hasLoopLiveOut(
    const DistributionShape &Shape) {
    for (BasicBlock *BB : Shape.Blocks) {
        for (Instruction *I : *BB) {
            for (Use *U : I->getUses()) {
                auto *UserI = dynamic_cast<Instruction *>(
                    U ? U->getUser() : nullptr);
                if (!UserI || !UserI->getParent() ||
                    !Shape.L->contains(UserI->getParent())) {
                    return true;
                }
            }
        }
    }
    return false;
}

// 步骤 1 主结构验证：要求 LCSSA、规范唯一预头/锁存块、单一无 PHI 出口、
// 有界块数/指令数，并收集按程序序出现的 store 作为 statement roots。
std::optional<LoopDistributionPass::DistributionShape>
LoopDistributionPass::getDistributionShape(Function &F, Loop *L) {
    if (!L || !L->getHeader() || !LCSSAPass::isLCSSAForm(L))
        return std::nullopt;

    DistributionShape Shape;
    Shape.L = L;
    Shape.Header = L->getHeader();
    if (!L->getLoopSimplifyForm(Shape.PreHeader, Shape.Latch) ||
        !Shape.PreHeader || !Shape.Latch) {
        return std::nullopt;
    }
    auto *PreBr = Shape.PreHeader->getBranchTerminator();
    if (!BasicBlock::isUncondBr(PreBr) ||
        PreBr->getTrueSuccessor() != Shape.Header) {
        return std::nullopt;
    }

    const auto Exits = L->getExitBlocks();
    if (Exits.size() != 1 || !Exits.front() ||
        !Exits.front()->collectPhiNodes().empty()) {
        return std::nullopt;
    }
    Shape.Exit = Exits.front();
    Shape.Blocks = L->getBlocksInFunctionOrder();
    if (Shape.Blocks.empty() ||
        Shape.Blocks.size() > MaxDistributionBlocks) {
        return std::nullopt;
    }

    BlockSet BlockLookup(Shape.Blocks.begin(), Shape.Blocks.end());
    std::size_t InstructionCount = 0;
    unsigned ExitEdgeCount = 0;
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB || BB->getParent() != &F) return std::nullopt;
        InstructionCount += static_cast<std::size_t>(BB->size());
        if (InstructionCount > MaxDistributionInstructions)
            return std::nullopt;

        for (Instruction *I : *BB) {
            if (hasUnsupportedInstruction(I)) return std::nullopt;
            if (auto *Store = dynamic_cast<StoreInst *>(I))
                Shape.Stores.push_back(Store);
        }

        for (BasicBlock *Succ : BB->getSuccessors()) {
            if (!Succ || BlockLookup.count(Succ)) continue;
            if (Succ != Shape.Exit) return std::nullopt;
            ++ExitEdgeCount;
            if (Shape.Exiting && Shape.Exiting != BB)
                return std::nullopt;
            Shape.Exiting = BB;
        }
    }

    if (ExitEdgeCount != 1 || !Shape.Exiting ||
        Shape.Exiting->countEdgesToSucc(Shape.Exit) != 1 ||
        Shape.Stores.size() < 2 ||
        Shape.Stores.size() > MaxStatementRoots ||
        hasMemoryDependentControl(Shape.Blocks) ||
        hasLoopLiveOut(Shape)) {
        return std::nullopt;
    }
    return Shape;
}

// 步骤 2.1：从一个 store 沿操作数反向遍历循环内定义，得到执行该语句所需
// 的闭合计算切片；循环外定义作为不变量边界停止。
LoopDistributionPass::InstructionSet
LoopDistributionPass::collectBackwardSlice(
    StoreInst *Root, const BlockSet &LoopBlocks) {
    InstructionSet Slice;
    std::vector<Instruction *> WorkList;
    if (Root) WorkList.push_back(Root);

    while (!WorkList.empty()) {
        Instruction *I = WorkList.back();
        WorkList.pop_back();
        if (!I || !I->getParent() || !LoopBlocks.count(I->getParent()) ||
            !Slice.insert(I).second) {
            continue;
        }
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            auto *Def = dynamic_cast<Instruction *>(I->getOperand(Index));
            if (Def && Def->getParent() && LoopBlocks.count(Def->getParent()))
                WorkList.push_back(Def);
        }
    }
    return Slice;
}

// 步骤 5 的删除候选由“待丢弃分区 - 待保留分区”得到，共享计算不会删除。
LoopDistributionPass::InstructionSet
LoopDistributionPass::collectExclusiveSlice(
    const InstructionSet &Discarded, const InstructionSet &Retained) {
    InstructionSet Exclusive;
    for (Instruction *I : Discarded) {
        if (I && !Retained.count(I))
            Exclusive.insert(I);
    }
    return Exclusive;
}

// 步骤 3.1：共享切片只允许可重复的普通计算、load 与地址节点；所有需要单次
// 执行的指令必须恰好归属一个分区，且其循环内使用也必须留在同一所有者中。
bool LoopDistributionPass::canDuplicateSharedSlice(
    const DistributionShape &Shape, const InstructionSet &First,
    const InstructionSet &Second) {
    for (Instruction *I : First) {
        if (!I || !Second.count(I)) continue;

        // store/call/终结/alloca 在两个循环中各执行一次会改变可观察行为。
        if (dynamic_cast<StoreInst *>(I) ||
            dynamic_cast<CallInst *>(I) ||
            dynamic_cast<BrInst *>(I) ||
            dynamic_cast<RetInst *>(I) ||
            dynamic_cast<AllocaInst *>(I)) {
            return false;
        }
    }

    BlockSet LoopBlocks(Shape.Blocks.begin(), Shape.Blocks.end());
    for (BasicBlock *BB : Shape.Blocks) {
        if (!BB) return false;
        for (Instruction *I : *BB) {
            if (!I) return false;
            // 分支属于两套循环各自的控制骨架，不作为 statement root 归属判断。
            if (dynamic_cast<BrInst *>(I)) continue;

            bool NeedsSingleExecution =
                dynamic_cast<StoreInst *>(I) ||
                dynamic_cast<CallInst *>(I) ||
                dynamic_cast<RetInst *>(I) ||
                dynamic_cast<AllocaInst *>(I);
            if (!NeedsSingleExecution) continue;

            bool InFirst = First.count(I) != 0;
            bool InSecond = Second.count(I) != 0;
            // 交集会重复执行，并集之外也会随整循环克隆，二者均不合法。
            if (InFirst == InSecond) return false;

            const InstructionSet &Owner = InFirst ? First : Second;
            for (Use *U : I->getUses()) {
                auto *UserI = dynamic_cast<Instruction *>(
                    U ? U->getUser() : nullptr);
                if (!UserI || !UserI->getParent() ||
                    !LoopBlocks.count(UserI->getParent()))
                    continue;
                // 所有循环内使用必须属于同一分区，否则删除专属切片时该指令
                // 会被保活，最终在原循环和克隆循环中各执行一次。
                if (!Owner.count(UserI)) return false;
            }
        }
    }
    return true;
}

// 步骤 5 的映射辅助把原循环指令集合转成克隆集合；任一指令缺少克隆映射则
// 清空结果并使整个提交回滚。
bool LoopDistributionPass::mapInstructionSet(
    const InstructionSet &Original, const ClonedBlockRegion &Region,
    InstructionSet &Mapped) {
    Mapped.clear();
    for (Instruction *I : Original) {
        Instruction *Clone = Region.getClonedInstruction(I);
        if (!Clone) {
            Mapped.clear();
            return false;
        }
        Mapped.insert(Clone);
    }
    return true;
}

// 步骤 5.4：删除分区专属切片时，先从仍有候选集外 use 的值反向标记保活
// 依赖；其余形成封闭死子图，即使包含 PHI 环也可先断开操作数再整体删除。
void LoopDistributionPass::deleteExclusiveSlice(
    InstructionSet &Candidates) {
    InstructionSet Live;
    std::vector<Instruction *> WorkList;

    // 5.4.1 候选集外仍被使用的节点是保活根。
    for (Instruction *I : Candidates) {
        if (!I || !I->getParent()) continue;
        bool HasExternalUse = false;
        for (Use *U : I->getUses()) {
            auto *UserI = dynamic_cast<Instruction *>(
                U ? U->getUser() : nullptr);
            if (!UserI || !Candidates.count(UserI)) {
                HasExternalUse = true;
                break;
            }
        }
        if (HasExternalUse && Live.insert(I).second)
            WorkList.push_back(I);
    }

    while (!WorkList.empty()) {
        Instruction *I = WorkList.back();
        WorkList.pop_back();
        for (std::size_t Index = 0; Index < I->getNumOperands(); ++Index) {
            auto *Def = dynamic_cast<Instruction *>(I->getOperand(Index));
            if (Def && Candidates.count(Def) && Live.insert(Def).second)
                WorkList.push_back(Def);
        }
    }

    std::vector<Instruction *> Dead;
    for (Instruction *I : Candidates) {
        if (I && I->getParent() && !Live.count(I))
            Dead.push_back(I);
    }
    // 5.4.2 统一 clearOperands 后再删除，避免 PHI/递推环产生悬空 use。
    for (Instruction *I : Dead)
        I->clearOperands();
    for (Instruction *I : Dead)
        delete I;
    Candidates.clear();
}

// 步骤 2.2：按 store 区间合并各 statement slice，集合并集自然保留共享依赖。
void LoopDistributionPass::mergeSlice(InstructionSet &Into,
                                      const InstructionSet &From) {
    Into.insert(From.begin(), From.end());
}

// 步骤 4.1：把字节步长绝对值压入粗粒度局部性桶；负最小值用偏移写法取
// 模，避免直接求负产生有符号溢出。
unsigned LoopDistributionPass::strideBucket(std::int64_t Stride) {
    uint64_t Magnitude = 0;
    if (Stride < 0) {
        Magnitude = static_cast<uint64_t>(-(Stride + 1));
        ++Magnitude;
    } else {
        Magnitude = static_cast<uint64_t>(Stride);
    }
    if (Magnitude == 0) return 0;
    if (Magnitude <= 8) return 1;
    if (Magnitude <= 64) return 2;
    if (Magnitude <= 512) return 3;
    return 4;
}

// 步骤 4.2：对每个循环维度汇总专属内存访问的步长桶，store 权重为 load 的
// 两倍；共享切片被两个分区同时执行，不计入二者的差异收益。
LoopDistributionPass::LocalityProfile
LoopDistributionPass::buildLocalityProfile(
    const InstructionSet &Slice, const InstructionSet &OtherSlice,
    const std::vector<Loop *> &Dimensions, LoopDependenceInfo &LD) {
    LocalityProfile Profile(Dimensions.size());
    for (Instruction *I : Slice) {
        if (!I || OtherSlice.count(I)) continue;
        const bool IsStore = dynamic_cast<StoreInst *>(I) != nullptr;
        const bool IsLoad = dynamic_cast<LoadInst *>(I) != nullptr;
        if (!IsStore && !IsLoad) continue;
        const uint64_t Weight = IsStore ? 2 : 1;
        for (std::size_t D = 0; D < Dimensions.size(); ++D) {
            auto Stride = LD.getByteStride(I, Dimensions[D]);
            if (!Stride) continue;
            Profile[D].WeightedBucketSum += Weight * strideBucket(*Stride);
            Profile[D].KnownWeight += Weight;
        }
    }
    return Profile;
}

// 步骤 4.3：比较两个分区在外到内各维度的加权平均桶距离；至少相差一个完整
// 桶才接受，避免为微小或同方向差异额外复制整套循环控制。
unsigned LoopDistributionPass::localityDistance(
    const InstructionSet &First, const InstructionSet &Second, Loop *L,
    LoopDependenceInfo &LD) {
    std::vector<Loop *> Dimensions = L->getLoopsInPostOrder();
    std::reverse(Dimensions.begin(), Dimensions.end());
    if (Dimensions.empty()) return 0;

    LocalityProfile A =
        buildLocalityProfile(First, Second, Dimensions, LD);
    LocalityProfile B =
        buildLocalityProfile(Second, First, Dimensions, LD);
    unsigned Distance = 0;
    for (std::size_t D = 0; D < Dimensions.size(); ++D) {
        if (A[D].KnownWeight == 0 || B[D].KnownWeight == 0) continue;
        const uint64_t AvgA =
            100 * A[D].WeightedBucketSum / A[D].KnownWeight;
        const uint64_t AvgB =
            100 * B[D].WeightedBucketSum / B[D].KnownWeight;
        const uint64_t Delta = AvgA > AvgB ? AvgA - AvgB : AvgB - AvgA;
        Distance += static_cast<unsigned>(std::min<uint64_t>(Delta, 400));
    }

    return Distance >= 100 ? Distance : 0;
}

// 计划搜索枚举保持原 store 次序的每个连续切点，完整执行可复制性、统一依赖
// 合法性和局部性收益检查，最后选择距离最大的计划。
std::optional<LoopDistributionPass::DistributionPlan>
LoopDistributionPass::findDistributionPlan(
    Function &F, Loop *L, LoopDependenceInfo &LD) {
    auto Shape = getDistributionShape(F, L);
    if (!Shape) return std::nullopt;

    BlockSet Blocks(Shape->Blocks.begin(), Shape->Blocks.end());
    std::vector<InstructionSet> Slices;
    Slices.reserve(Shape->Stores.size());
    for (StoreInst *Store : Shape->Stores)
        Slices.push_back(collectBackwardSlice(Store, Blocks));

    std::optional<DistributionPlan> Best;
    InstructionSet First;
    // 2.3 First 增量累积切点之前的 store 切片，Second 每次合并余下切片。
    for (std::size_t Cut = 1; Cut < Shape->Stores.size(); ++Cut) {
        mergeSlice(First, Slices[Cut - 1]);
        InstructionSet Second;
        for (std::size_t I = Cut; I < Slices.size(); ++I)
            mergeSlice(Second, Slices[I]);

        DistributionPlan Plan;
        Plan.Shape = *Shape;
        Plan.FirstStores.assign(Shape->Stores.begin(),
                                Shape->Stores.begin() + Cut);
        Plan.SecondStores.assign(Shape->Stores.begin() + Cut,
                                 Shape->Stores.end());
        Plan.FirstSlice = First;
        Plan.SecondSlice = std::move(Second);
        // 3. 每个切点都必须直接通过共享切片检查和统一循环依赖分析，不能以
        // store 数量、producer/reload 形状或子循环结构旁路。
        if (!canDuplicateSharedSlice(
                Plan.Shape, Plan.FirstSlice, Plan.SecondSlice) ||
            !LD.isDistributionLegal(
                Shape->L, Plan.FirstSlice, Plan.SecondSlice)) {
            continue;
        }

        unsigned Distance = localityDistance(
            Plan.FirstSlice, Plan.SecondSlice, Shape->L, LD);
        if (Distance == 0) continue;
        Plan.LocalityDistance = Distance;
        if (!Best || Plan.LocalityDistance > Best->LocalityDistance)
            Best = std::move(Plan);
    }
    return Best;
}

// 事务回滚辅助分三轮断开克隆指令、清空块并删除块，处理克隆 CFG/PHI 环时
// 不留下悬空引用。
void LoopDistributionPass::destroyClonedBlocks(
    std::vector<BasicBlock *> &Blocks) {
    for (BasicBlock *BB : Blocks) {
        if (!BB) continue;
        for (Instruction *I : *BB)
            if (I) I->clearOperands();
    }
    for (BasicBlock *BB : Blocks)
        if (BB) BB->clear();
    for (BasicBlock *BB : Blocks)
        delete BB;
    Blocks.clear();
}

// 步骤 5 提交：先克隆完整循环并完成全部可失败校验，再串接 CFG，最后在原
// 循环删除第二分区专属切片、在克隆循环删除第一分区专属切片。
bool LoopDistributionPass::executeDistribution(
    Function &F, const DistributionPlan &Plan) {
    const DistributionShape &Shape = Plan.Shape;
    if (!Shape.L || !Shape.PreHeader || !Shape.Header || !Shape.Exiting ||
        !Shape.Exit || Shape.Blocks.empty()) {
        return false;
    }

    // 5.1 创建克隆预头，并在克隆映射中用它替代原预头，使头部 PHI 的初值
    // incoming 自动指向新串行入口；克隆块统一带 .dist 来源标记。
    auto *NewPreHeader = new BasicBlock();
    F.pushBlock(NewPreHeader);
    NewPreHeader->setIdent(F.makeUniqueBlockIdent(
        Shape.Header->getIdent() + ".dist.preheader"));

    ValueToValueMap InitialMap;
    InitialMap[Shape.PreHeader] = NewPreHeader;
    CloneBlockRegionOptions Options;
    Options.MakeBlockIdent = [&F](const BasicBlock *BB, std::size_t) {
        const std::string Base = BB ? BB->getIdent() : "bb";
        return F.makeUniqueBlockIdent(Base + ".dist");
    };

    auto Region =
        cloneBasicBlockRegion(F, Shape.Blocks, InitialMap, Options);
    if (!Region) {
        delete NewPreHeader;
        return false;
    }

    BasicBlock *CloneHeader = Region->getClonedBlock(Shape.Header);
    BasicBlock *CloneExiting = Region->getClonedBlock(Shape.Exiting);
    if (!CloneHeader || !CloneExiting ||
        CloneExiting->countEdgesToSucc(Shape.Exit) != 1) {
        destroyClonedBlocks(Region->ClonedBlocks);
        delete NewPreHeader;
        return false;
    }

    // 5.2 原循环应丢弃 Second-First，克隆循环应丢弃 First-Second；共享切片
    // 保留两份，从而为两套控制流各提供相同依赖。
    InstructionSet OriginalDiscard = collectExclusiveSlice(
        Plan.SecondSlice, Plan.FirstSlice);
    InstructionSet FirstExclusive = collectExclusiveSlice(
        Plan.FirstSlice, Plan.SecondSlice);
    InstructionSet ClonedDiscard;
    if (!mapInstructionSet(FirstExclusive, *Region, ClonedDiscard)) {
        destroyClonedBlocks(Region->ClonedBlocks);
        delete NewPreHeader;
        return false;
    }

    // 5.3 再次断言每个 statement root 恰好落入自己的专属删除集，避免未来
    // backward-slice 语义变化后漏删有副作用 store。
    for (StoreInst *Store : Plan.SecondStores) {
        if (!OriginalDiscard.count(Store)) {
            destroyClonedBlocks(Region->ClonedBlocks);
            delete NewPreHeader;
            return false;
        }
    }
    for (StoreInst *Store : Plan.FirstStores) {
        Instruction *Clone = Region->getClonedInstruction(Store);
        if (!Clone || !ClonedDiscard.count(Clone)) {
            destroyClonedBlocks(Region->ClonedBlocks);
            delete NewPreHeader;
            return false;
        }
    }

    // 5.4 所有结构与依赖检查完成后先连接原循环退出 -> 新预头 -> 克隆头部，
    // 保持第一分区先于第二分区的原语句顺序；失败则销毁全部克隆。
    NewPreHeader->genUnCondInst(CloneHeader);
    if (!BasicBlock::redirectBranchTarget(Shape.Exiting, Shape.Exit,
                                          NewPreHeader)) {
        NewPreHeader->clear();
        destroyClonedBlocks(Region->ClonedBlocks);
        delete NewPreHeader;
        return false;
    }

    // 5.5 CFG 已接好后删除两边各自不属于该分区的封闭专属切片。
    deleteExclusiveSlice(OriginalDiscard);
    deleteExclusiveSlice(ClonedDiscard);
    return true;
}

// Pass 主流程优先从外层迭代域寻找计划，使后续循环交换可分别优化两个分区；
// 每次只提交一次并失效全部分析，累计预算限制克隆代码增长。
PassResult LoopDistributionPass::run(Function &F,
                                     FunctionAnalysisPassManager &FAM) {
    unsigned &Count = TransformCounts[&F];
    if (Count >= MaxDistributionsPerFunction)
        return PassResult::unchanged();

    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &LD = FAM.getResult<LoopDependenceAnalysis>(F);
    std::vector<Loop *> Loops = LI.getLoopsInPostOrder();

    // 后序列表逆向遍历即外层优先；有限性在计划搜索前覆盖整个子循环树。
    for (auto It = Loops.rbegin(); It != Loops.rend(); ++It) {
        Loop *L = *It;
        if (!hasFiniteLoopTree(L, SE)) continue;
        auto Plan = findDistributionPlan(F, L, LD);
        if (!Plan) continue;
        if (!executeDistribution(F, *Plan)) continue;
        ++Count;
        return PassResult::changed(PreservationStatus::none());
    }
    return PassResult::unchanged();
}
