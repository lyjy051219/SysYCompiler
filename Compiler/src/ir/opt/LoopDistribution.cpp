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

// 本 pass 只复制普通计算；call 即使标为 readonly，也可能表达输入或其他
// 可观察行为，因此不依据属性为它开放克隆路径。
bool LoopDistributionPass::hasUnsupportedInstruction(Instruction *I) {
    if (!I) return true;
    if (dynamic_cast<CallInst *>(I) || dynamic_cast<AllocaInst *>(I) ||
        dynamic_cast<RetInst *>(I)) {
        return true;
    }
    return I->instType == Instruction::BinaryUnknown;
}

bool LoopDistributionPass::valueDependsOnLoad(
    Value *V, std::unordered_set<Value *> &Seen,
    const BlockSet &LoopBlocks, unsigned Depth) {
    if (!V) return false;
    // 用尽证明预算表示未知，而不是纯净性证据；按依赖 load 处理并拒绝
    // 分布。重复访问同一 SSA 节点则表示该节点已经完成检查。
    if (Depth > 64) return true;
    if (!Seen.insert(V).second) return false;
    auto *I = dynamic_cast<Instruction *>(V);
    if (!I) return false;
    // 循环外已经求值的 load/call 是分布域的不变量，克隆循环不会重复执行
    // 它们；只有循环内部计算出的内存相关条件才可能改变两分区的控制轨迹。
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

bool LoopDistributionPass::hasFiniteLoopTree(Loop *L, SCEVResult &SE) {
    if (!L || !SE.isKnownFiniteLoop(L)) return false;
    for (Loop *SubLoop : L->getSubLoops()) {
        if (!hasFiniteLoopTree(SubLoop, SE)) return false;
    }
    return true;
}

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

bool LoopDistributionPass::canDuplicateSharedSlice(
    const DistributionShape &Shape, const InstructionSet &First,
    const InstructionSet &Second) {
    for (Instruction *I : First) {
        if (!I || !Second.count(I)) continue;

        // 共享切片会在两个分布后的循环中各执行一次。只拒绝可观察指令；
        // 普通表达式、load 与地址计算由统一的循环依赖分析判断合法性。
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
            // 每个分布后的循环都必须保留自己的分支控制；终结指令不属于
            // 语句切片，也不作为可复制的 statement root。
            if (dynamic_cast<BrInst *>(I)) continue;

            bool NeedsSingleExecution =
                dynamic_cast<StoreInst *>(I) ||
                dynamic_cast<CallInst *>(I) ||
                dynamic_cast<RetInst *>(I) ||
                dynamic_cast<AllocaInst *>(I);
            if (!NeedsSingleExecution) continue;

            bool InFirst = First.count(I) != 0;
            bool InSecond = Second.count(I) != 0;
            // 危险指令必须恰好归属一个分区。交集会在两个循环中都保留，
            // 并集之外的指令也会随整个循环克隆，两者都会增加执行次数。
            if (InFirst == InSecond) return false;

            const InstructionSet &Owner = InFirst ? First : Second;
            for (Use *U : I->getUses()) {
                auto *UserI = dynamic_cast<Instruction *>(
                    U ? U->getUser() : nullptr);
                if (!UserI || !UserI->getParent() ||
                    !LoopBlocks.count(UserI->getParent()))
                    continue;
                // 若分区外仍使用该值，删除专属切片时该指令会被保活，
                // 从而在原循环和克隆循环各执行一次；此时必须拒绝。
                if (!Owner.count(UserI)) return false;
            }
        }
    }
    return true;
}

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

void LoopDistributionPass::deleteExclusiveSlice(
    InstructionSet &Candidates) {
    InstructionSet Live;
    std::vector<Instruction *> WorkList;

    // 先找出仍被保留分区或循环控制使用的值，再沿 def-use 反向保活其
    // 依赖。其余候选构成封闭的死子图，即使内部含 PHI 环也可整体删除。
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
    // 先统一断开操作数，避免删除 PHI/递推环时留下悬空 use。
    for (Instruction *I : Dead)
        I->clearOperands();
    for (Instruction *I : Dead)
        delete I;
    Candidates.clear();
}

void LoopDistributionPass::mergeSlice(InstructionSet &Into,
                                      const InstructionSet &From) {
    Into.insert(From.begin(), From.end());
}

unsigned LoopDistributionPass::strideBucket(std::int64_t Stride) {
    uint64_t Magnitude = 0;
    if (Stride < 0) {
        // 避免直接对 INT64_MIN 取负。
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

    // 至少相差一个完整局部性分桶才接受收益；访问方向特征相同的两个
    // 分区不足以补偿增加一套完整循环控制的成本。
    return Distance >= 100 ? Distance : 0;
}

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
        // 每个候选切分都必须直接通过统一的循环依赖分析。这里不根据
        // store 数量、producer/reload 形状或子循环结构另开合法性旁路。
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

bool LoopDistributionPass::executeDistribution(
    Function &F, const DistributionPlan &Plan) {
    const DistributionShape &Shape = Plan.Shape;
    if (!Shape.L || !Shape.PreHeader || !Shape.Header || !Shape.Exiting ||
        !Shape.Exit || Shape.Blocks.empty()) {
        return false;
    }

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

    // 每个 statement root 都应当只属于自己的分区。若 IR 扩展改变了
    // backward-slice 语义，就保守放弃，避免遗漏有副作用的 store。
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

    // 所有可能失败的结构与依赖检查均已完成。先接好两个循环的串行 CFG，
    // 再删除各分区专属的完整切片；共享的普通计算则按分布语义保留两份。
    NewPreHeader->genUnCondInst(CloneHeader);
    if (!BasicBlock::redirectBranchTarget(Shape.Exiting, Shape.Exit,
                                          NewPreHeader)) {
        NewPreHeader->clear();
        destroyClonedBlocks(Region->ClonedBlocks);
        delete NewPreHeader;
        return false;
    }

    deleteExclusiveSlice(OriginalDiscard);
    deleteExclusiveSlice(ClonedDiscard);
    return true;
}

PassResult LoopDistributionPass::run(Function &F,
                                     FunctionAnalysisPassManager &FAM) {
    unsigned &Count = TransformCounts[&F];
    if (Count >= MaxDistributionsPerFunction)
        return PassResult::unchanged();

    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &LD = FAM.getResult<LoopDependenceAnalysis>(F);
    std::vector<Loop *> Loops = LI.getLoopsInPostOrder();

    // 优先分布外层迭代域，使后续循环交换可以分别为两个分区选择次序。
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
