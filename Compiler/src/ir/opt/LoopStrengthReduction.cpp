#include "ir/opt/LoopStrengthReduction.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/PostDominant.hpp"
#include "ir/analysis/RangeAnalysis.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/analysis/SCEVExpander.hpp"
#include "ir/analysis/SideEffect.hpp"
#include "lib/IRUtils.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
//步骤 3 的分组辅助：优先使用 SCEV 的稳定结构键，缺失时才以表达式对象标识
//区分，避免把未经证明相等的表达式放入同组。
std::string LoopStrengthReductionPass::scevKey(const SCEVExpr *E) {
    if (!E) return "<null>";
    if (!E->Key.empty()) return E->Key;
    return "scev@" + IRKey::pointer(E);
}
//步骤 2.1：预头可直接使用常量、参数、全局值以及支配预头的指令定义。
bool LoopStrengthReductionPass::canUseInPreheader(Value *V, BasicBlock *PreHeader,
                              const DominatorTree &DT) {
    if (!V || !PreHeader) return false;
    if (V->isConst()) return true;
    if (V->isParam() || V->isGlobal()) return true;
    auto *Inst = dynamic_cast<Instruction *>(V);
    if (!Inst || !Inst->getParent()) return false;
    return DT.dominates(Inst->getParent(), PreHeader);
}
//步骤 2.2：所有非直接可用的 SCEV 起点和不变量索引都通过同一 Expander
//预算检查、估价和物化，保证收益模型与实际生成路径一致。
bool LoopStrengthReductionPass::canMaterializeSCEVAtPreheader(const SCEVExpr *E, const Loop *L,
                                          BasicBlock *PreHeader,
                                          const DominatorTree &DT) {
    SCEVExpander Expander(L, PreHeader, DT, 32);
    return Expander.canExpand(E);
}
//估算SCEV表达式在循环预头物化所需的新增指令数
int LoopStrengthReductionPass::estimateMaterializeCost(const SCEVExpr *E, const Loop *L,
                                   BasicBlock *PreHeader,
                                   const DominatorTree &DT) {
    SCEVExpander Expander(L, PreHeader, DT, 32);
    int Cost = Expander.estimateCost(E);
    return Cost < 0 ? UnsupportedMaterializeCost : Cost;
}
//步骤 4 的常量构造辅助：沿用 SCEV 整数类型，拒绝超过当前常量表示能力的
//位宽，布尔起点则规范为 ConstBool。
Value *LoopStrengthReductionPass::makeSCEVConstant(int64_t V, const SCEVExpr *E) {
    auto IntegerType =
        E && E->Ty
            ? std::dynamic_pointer_cast<INTType>(E->Ty)
            : INTType::getInt32Ty();
    if (IntegerType && IntegerType->getIsBool()) {
        return ConstBool::newConstBool(V != 0);
    }
    if (!IntegerType || IntegerType->getBitWidth() > 64)
        return nullptr;
    return ConstInt::newConstIntForType(V, IntegerType);
}
//在循环预头事务式物化SCEV表达式
Value *LoopStrengthReductionPass::materializeSCEVAtPreheader(const SCEVExpr *E, const Loop *L,
                                         BasicBlock *PreHeader,
                                         const DominatorTree &DT) {
    SCEVExpander Expander(L, PreHeader, DT, 32);
    return Expander.expand(E);
}
//步骤 3 的设置成本：可直接在预头使用的维度成本为零，其余累加 SCEV 展开
//成本；任一维度不支持或求和溢出即判整组不可用。
int LoopStrengthReductionPass::estimatePrefixMaterializeCost(
    const std::vector<IndexComponent> &Prefix, const Loop *L,
    BasicBlock *PreHeader, const DominatorTree &DT) {
    int Cost = 0;
    for (const IndexComponent &Component : Prefix) {
        if (Component.Raw &&
            canUseInPreheader(Component.Raw, PreHeader, DT)) {
            continue;
        }
        int ComponentCost =
            estimateMaterializeCost(Component.Expr, L, PreHeader, DT);
        if (ComponentCost >= UnsupportedMaterializeCost ||
            Cost >= UnsupportedMaterializeCost - ComponentCost) {
            return UnsupportedMaterializeCost;
        }
        Cost += ComponentCost;
    }
    return Cost;
}
//步骤 4.1：按原 GEP 维度顺序取得预头索引；Raw 可支配预头则复用，否则
//展开对应 SCEV，失败时不提交后续指针 IV。
bool LoopStrengthReductionPass::materializePrefix(const std::vector<IndexComponent> &Prefix,
                              const Loop *L, BasicBlock *PreHeader,
                              const DominatorTree &DT,
                              std::vector<Value *> &Indices) {
    for (const IndexComponent &Component : Prefix) {
        Value *V = nullptr;
        if (Component.Raw &&
            canUseInPreheader(Component.Raw, PreHeader, DT)) {
            V = Component.Raw;
        } else {
            V = materializeSCEVAtPreheader(Component.Expr, L, PreHeader, DT);
        }
        if (!V) return false;
        Indices.push_back(V);
    }
    return true;
}
//步骤 1.1：地址族的叶子只允许 load 的指针操作数或 store 的地址操作数。
bool LoopStrengthReductionPass::isLoadStoreAddressUse(Instruction *UserI, Use *U) {
    if (!UserI || !U) return false;
    auto OpIdx = U->getOperandIndex();
    if (dynamic_cast<LoadInst *>(UserI)) return OpIdx == 0;
    if (dynamic_cast<StoreInst *>(UserI)) return OpIdx == 1;
    return false;
}
//步骤 1.2：识别以前一 GEP 为基址、仅带一个常量索引的派生地址，并返回
//相对父地址的元素偏移。
bool LoopStrengthReductionPass::getConstantOffsetGEP(GetElementPtrInst *Parent,
                                 Instruction *UserI, Use *U,
                                 GetElementPtrInst *&Child,
                                 int64_t &Offset) {
    Child = dynamic_cast<GetElementPtrInst *>(UserI);
    if (!Parent || !Child || !U || U->getOperandIndex() != 0)
        return false;
    if (Child->getNumIndices() != 1 || Child->getBasePointer() != Parent)
        return false;
    auto *CI = dynamic_cast<ConstInt *>(Child->getIndex(0));
    if (!CI) return false;
    Offset = CI->getVal();
    return true;
}
//步骤 1.3：递归收集“地址叶子 + 常量偏移子 GEP”闭包。族内任一使用逃出
//循环、出现未知消费者或偏移累加溢出，都会保守拒绝整个根。
bool LoopStrengthReductionPass::collectAddressGEPFamily(
    GetElementPtrInst *GEP, Loop *L, int64_t CurrentOffset,
    std::vector<AddressGEPNode> &Nodes,
    std::unordered_set<GetElementPtrInst *> &Visited) {
    if (!GEP || !L || !L->contains(GEP->getParent()) ||
        !Visited.insert(GEP).second) {
        return false;
    }
    bool HasAddressUse = false;
    std::vector<std::pair<GetElementPtrInst *, int64_t>> Children;
    for (Use *U : GEP->getUses()) {
        if (!U || U->getValue() != GEP) continue;
        auto *UserI = dynamic_cast<Instruction *>(U->getUser());
        if (!UserI || !L->contains(UserI->getParent())) return false;
        if (isLoadStoreAddressUse(UserI, U)) {
            HasAddressUse = true;
            continue;
        }
        GetElementPtrInst *Child = nullptr;
        int64_t Offset = 0;
        if (!getConstantOffsetGEP(GEP, UserI, U, Child, Offset))
            return false;
        if ((Offset > 0 &&
             CurrentOffset > std::numeric_limits<int64_t>::max() - Offset) ||
            (Offset < 0 &&
             CurrentOffset < std::numeric_limits<int64_t>::min() - Offset)) {
            return false;
        }
        Children.emplace_back(Child, CurrentOffset + Offset);
    }
    if (HasAddressUse) Nodes.push_back({GEP, CurrentOffset});
    bool HasSupportedUse = HasAddressUse;
    for (const auto &[Child, Offset] : Children) {
        if (!collectAddressGEPFamily(Child, L, Offset, Nodes, Visited))
            return false;
        HasSupportedUse = true;
    }
    return HasSupportedUse;
}
//步骤 1 的布局辅助：单索引数组 GEP 的步长语义与多维零基链分开识别。
bool LoopStrengthReductionPass::isSingleIndexArrayStep(Value *Base, size_t NumIndices) {
    if (NumIndices != 1 || !Base) return false;
    auto PointeeTy = IRType::getPointeeType(Base->getIRType());
    return PointeeTy && PointeeTy->isArray();
}
//步骤 1.4：把零基 GEP 链展平，要求恰有一个维度随当前循环变化；返回该
//活跃索引及前后不变量维度，并验证字节步长可换算为末级元素步长。
bool LoopStrengthReductionPass::matchGEP(GetElementPtrInst *GEP, Loop *L, SCEVResult &SE,
                     Value *&Base, Value *&Index,
                     std::vector<IndexComponent> &Prefix,
                     std::vector<IndexComponent> &Suffix,
                     int64_t &ActiveIndexStride,
                     int64_t &FinalPointeeStride,
                     bool &HasLeadingZero,
                     bool &AllowOffsetReuse) {
    Base = nullptr;
    Index = nullptr;
    Prefix.clear();
    Suffix.clear();
    ActiveIndexStride = 0;
    FinalPointeeStride = 0;
    HasLeadingZero = false;
    AllowOffsetReuse = false;
    if (!GEP || !L) return false;
    std::vector<Value *> Indices;
    if (!GEP->flattenZeroBasedChain(Base, Indices)) return false;
    if (Indices.empty()) return false;
    std::vector<int64_t> IndexStrides;
    if (!GetElementPtrInst::getIndexStrides(Base, Indices, IndexStrides,
                                            FinalPointeeStride)) {
        return false;
    }
    int ActivePos = -1;
    std::vector<IndexComponent> Components;
    Components.reserve(Indices.size());
    for (size_t I = 0; I < Indices.size(); ++I) {
        Value *Raw = Indices[I];
        if (!Raw) return false;
        const SCEVExpr *Expr = SE.getSCEV(Raw);
        if (!Expr) return false;
        bool Varies = Expr && Expr->mentionsLoop(L);
        if (Varies) {
            if (ActivePos != -1) return false;
            ActivePos = static_cast<int>(I);
        }
        Components.push_back({Raw, Expr});
    }
    if (ActivePos == -1) return false;
    Index = Components[ActivePos].Raw;
    ActiveIndexStride = IndexStrides[ActivePos];
    AllowOffsetReuse = ActivePos == static_cast<int>(Indices.size() - 1) &&
                       ActiveIndexStride == FinalPointeeStride;
    for (int I = 0; I < ActivePos; ++I)
        Prefix.push_back(Components[I]);
    for (size_t I = static_cast<size_t>(ActivePos + 1);
         I < Components.size(); ++I) {
        if (Components[I].Expr && Components[I].Expr->mentionsLoop(L))
            return false;
        Suffix.push_back(Components[I]);
    }
    if (ActiveIndexStride <= 0 || FinalPointeeStride <= 0 ||
        ActiveIndexStride % FinalPointeeStride != 0) {
        return false;
    }
    HasLeadingZero = !Prefix.empty() && Prefix.front().Raw &&
                     Prefix.front().Raw->isConstIntZero();
    return true;
}
//按照稳定循环和程序顺序收集通过结构、安全性与无回绕验证的GEP候选
std::vector<LSRCandidate>
LoopStrengthReductionPass::collectCandidates(Function &F, LoopInfo &LI, SCEVResult &SE,
                  const DominatorTree &DT) {
    //1.建立确定的块次序，并按“循环深度降序、头部程序次序升序”扫描，使
    //内层地址族优先占用每循环的指针 IV 预算。
    std::unordered_map<BasicBlock *, size_t> BlockOrder;
    size_t NextBlockOrder = 0;
    for (BasicBlock *BB : F) BlockOrder[BB] = NextBlockOrder++;
    std::vector<Loop *> Loops;
    Loops.reserve(LI.Loops.size());
    for (auto &LoopPtr : LI.Loops) {
        if (LoopPtr) Loops.push_back(LoopPtr.get());
    }
    std::sort(Loops.begin(), Loops.end(), [&](Loop *A, Loop *B) {
        if (A->getDepth() != B->getDepth())
            return A->getDepth() > B->getDepth();
        size_t AO = BlockOrder.count(A->getHeader())
                        ? BlockOrder.at(A->getHeader())
                        : std::numeric_limits<size_t>::max();
        size_t BO = BlockOrder.count(B->getHeader())
                        ? BlockOrder.at(B->getHeader())
                        : std::numeric_limits<size_t>::max();
        return AO < BO;
    });
    std::vector<LSRCandidate> Candidates;
    //同一个物理 GEP 可能从父地址族和自身根两次遇到，用集合保证只建一项。
    std::unordered_set<GetElementPtrInst *> SeenOldGEPs;
    for (Loop *L : Loops) {
        //2.循环级合法性：要求规范预头/锁存块，且 SCEV 已证明完整迭代域
        //无回绕，才能把整数递推等价替换成地址递推。
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Latch = nullptr;
        if (!L || !L->getLoopSimplifyForm(PreHeader, Latch)) continue;
        auto Trip = SE.analyzeLoopTripCount(L);
        if (!Trip || !Trip->Valid || !Trip->ProvenNoWrap) continue;
        BasicBlock *Header = L->getHeader();
        if (!PreHeader || !Header || !Latch) continue;
        std::vector<BasicBlock *> LoopBlocks = L->getBlocksIncludingSubLoops();
        std::sort(LoopBlocks.begin(), LoopBlocks.end(),
                  [&](BasicBlock *A, BasicBlock *B) {
                      size_t AO = BlockOrder.count(A)
                                      ? BlockOrder.at(A)
                                      : std::numeric_limits<size_t>::max();
                      size_t BO = BlockOrder.count(B)
                                      ? BlockOrder.at(B)
                                      : std::numeric_limits<size_t>::max();
                      return AO < BO;
                  });
        for (BasicBlock *BB : LoopBlocks) {
            if (!BB) continue;
            for (Instruction *I : *BB) {
                auto *GEP = dynamic_cast<GetElementPtrInst *>(I);
                if (!GEP) continue;
                if (!PreHeader->getTerminator()) continue;
                Value *Base = nullptr;
                Value *Index = nullptr;
                std::vector<IndexComponent> Prefix;
                std::vector<IndexComponent> Suffix;
                int64_t ActiveIndexStride = 0;
                int64_t FinalPointeeStride = 0;
                bool HasLeadingZero = false;
                bool AllowOffsetReuse = false;
                if (!matchGEP(GEP, L, SE, Base, Index, Prefix, Suffix,
                              ActiveIndexStride, FinalPointeeStride,
                              HasLeadingZero, AllowOffsetReuse)) {
                    continue;
                }
                //2.2 活跃索引必须是当前循环的仿射递推，具有非零常量步长。
                auto Affine = SE.getAffine(Index);
                if (!Affine || Affine->RecLoop != L || !Affine->Start ||
                    !Affine->ConstantStep) {
                    continue;
                }
                int64_t Step = *Affine->ConstantStep;
                if (Step == 0 || Step < std::numeric_limits<int>::min() ||
                    Step > std::numeric_limits<int>::max()) {
                    continue;
                }
                int64_t AddressScale = ActiveIndexStride / FinalPointeeStride;
                //将索引步长缩放为末级元素地址步长，并在乘法前后逐级检查
                //int64 与 IR 索引表示范围。
                if (AddressScale <= 0) continue;
                if ((Step > 0 &&
                     AddressScale >
                         std::numeric_limits<int64_t>::max() / Step) ||
                    (Step < 0 &&
                     AddressScale >
                         std::numeric_limits<int64_t>::max() / -Step)) {
                    continue;
                }
                int64_t AddressStep = Step * AddressScale;
                if (AddressStep < std::numeric_limits<int>::min() ||
                    AddressStep > std::numeric_limits<int>::max()) {
                    continue;
                }
                if (!canUseInPreheader(Base, PreHeader, DT)) continue;
                //2.3 所有不变量维度和起点都必须能在预头直接使用或由 SCEV
                //有界展开；这里同时取得后续成本模型所需的设置成本。
                int PrefixCost =
                    estimatePrefixMaterializeCost(Prefix, L, PreHeader, DT);
                if (PrefixCost >= UnsupportedMaterializeCost) continue;
                int SuffixCost =
                    estimatePrefixMaterializeCost(Suffix, L, PreHeader, DT);
                if (SuffixCost >= UnsupportedMaterializeCost) continue;
                std::optional<int64_t> StartConst = Affine->ConstantStart;
                if (StartConst &&
                    (*StartConst < std::numeric_limits<int>::min() ||
                     *StartConst > std::numeric_limits<int>::max())) {
                    continue;
                }
                if (!StartConst &&
                    !canMaterializeSCEVAtPreheader(Affine->Start, L,
                                                   PreHeader, DT)) {
                    continue;
                }
                std::vector<AddressGEPNode> Family;
                std::unordered_set<GetElementPtrInst *> Visited;
                if (!collectAddressGEPFamily(GEP, L, 0, Family, Visited))
                    continue;
                //2.4 把族中实际作为内存地址的每个 GEP 展开为候选。只有末级
                //活跃索引允许通过常量起点差共享锚点，且偏移加法必须无溢出。
                for (const AddressGEPNode &Node : Family) {
                    std::optional<int64_t> NodeStartConst;
                    if (!AllowOffsetReuse && Node.Offset != 0) {
                        continue;
                    }
                    if (StartConst) {
                        if ((Node.Offset > 0 &&
                             *StartConst >
                                 std::numeric_limits<int64_t>::max() -
                                     Node.Offset) ||
                            (Node.Offset < 0 &&
                             *StartConst <
                                 std::numeric_limits<int64_t>::min() -
                                     Node.Offset)) {
                            continue;
                        }
                        int64_t Start = *StartConst + Node.Offset;
                        if (Start < std::numeric_limits<int>::min() ||
                            Start > std::numeric_limits<int>::max()) {
                            continue;
                        }
                        NodeStartConst = Start;
                    } else if (Node.Offset != 0) {
                        continue;
                    }
                    if (!Node.GEP ||
                        !SeenOldGEPs.insert(Node.GEP).second) {
                        continue;
                    }
                    Candidates.push_back(
                        {Node.GEP, Base, Index, Affine->Start,
                         NodeStartConst, Prefix, Suffix, L, PreHeader,
                         Header, Latch, AddressStep, ActiveIndexStride,
                         HasLeadingZero, AllowOffsetReuse, false});
                }
            }
        }
    }
    return Candidates;
}
//步骤 3.1：生成候选结构键；完全同形组包含起点，常量偏移共享组忽略起点
//但仍要求循环、基址、步长、布局、类型和不变量维度一致。
std::string LoopStrengthReductionPass::candidateShapeKey(const LSRCandidate &C,
                                     bool IncludeStart) {
    std::ostringstream Key;
    Key << "L=" << IRKey::pointer(C.L) << ";B=" << IRKey::pointer(C.Base)
        << ";Step=" << C.Step
        << ";Stride=" << C.LastIndexStride
        << ";Lead0=" << (C.HasLeadingZero ? 1 : 0)
        << ";Ty=" << IRKey::type(C.OldGEP ? C.OldGEP->getIRType() : nullptr)
        << ";PrefixN=" << C.Prefix.size();
    for (const IndexComponent &Component : C.Prefix)
        Key << ";P=" << scevKey(Component.Expr);
    Key << ";SuffixN=" << C.Suffix.size();
    for (const IndexComponent &Component : C.Suffix)
        Key << ";S=" << scevKey(Component.Expr);
    if (IncludeStart) {
        if (C.StartConst)
            Key << ";StartConst=" << *C.StartConst;
        else
            Key << ";Start=" << scevKey(C.Start);
    }
    return Key.str();
}
//偏移 GEP 的索引最终由当前 IR 的 int 常量表示，因此分组和改写前统一检查。
bool LoopStrengthReductionPass::isRepresentablePointerOffset(
    int64_t Offset) {
    return Offset >= std::numeric_limits<int>::min() &&
           Offset <= std::numeric_limits<int>::max();
}
//步骤 3.2：把同形且具有不同常量起点的候选归入偏移共享组，以最小起点为
//锚；剔除单起点或不可表示偏移，并优先处理更深、覆盖项更多的组。
std::vector<OffsetReuseGroup>
LoopStrengthReductionPass::buildOffsetReuseGroups(std::vector<LSRCandidate> &Candidates) {
    std::vector<OffsetReuseGroup> Groups;
    std::unordered_map<std::string, size_t> GroupIndex;
    for (LSRCandidate &C : Candidates) {
        if (C.Used || !C.StartConst || !C.AllowOffsetReuse) continue;
        std::string Key = candidateShapeKey(C, false);
        auto It = GroupIndex.find(Key);
        if (It == GroupIndex.end()) {
            size_t Index = Groups.size();
            GroupIndex.emplace(Key, Index);
            OffsetReuseGroup Group;
            Group.Order = Index;
            Group.Base = C.Base;
            Group.L = C.L;
            Group.PreHeader = C.PreHeader;
            Group.Header = C.Header;
            Group.Latch = C.Latch;
            Group.Step = C.Step;
            Group.Prefix = C.Prefix;
            Group.ResultTy = C.OldGEP ? C.OldGEP->getIRType() : nullptr;
            Group.AnchorStart = *C.StartConst;
            Groups.push_back(std::move(Group));
            It = GroupIndex.find(Key);
        }
        OffsetReuseGroup &Group = Groups[It->second];
        Group.Items.push_back(&C);
        Group.AnchorStart = std::min(Group.AnchorStart, *C.StartConst);
    }
    Groups.erase(
        std::remove_if(Groups.begin(), Groups.end(),
                       [](const OffsetReuseGroup &Group) {
                           if (Group.Items.size() < 2) return true;
                           std::unordered_set<int64_t> Starts;
                           for (const LSRCandidate *C : Group.Items) {
                               if (!C || !C->StartConst ||
                                   *C->StartConst < Group.AnchorStart) {
                                   return true;
                               }
                               int64_t Offset =
                                   *C->StartConst - Group.AnchorStart;
                               if (!isRepresentablePointerOffset(Offset))
                                   return true;
                               Starts.insert(*C->StartConst);
                           }
                           return Starts.size() < 2;
                       }),
        Groups.end());
    std::sort(Groups.begin(), Groups.end(),
              [](const OffsetReuseGroup &A, const OffsetReuseGroup &B) {
                  unsigned AD = A.L ? A.L->getDepth() : 0;
                  unsigned BD = B.L ? B.L->getDepth() : 0;
                  if (AD != BD) return AD > BD;
                  if (A.Items.size() != B.Items.size())
                      return A.Items.size() > B.Items.size();
                  return A.Order < B.Order;
              });
    return Groups;
}
//步骤 3.3：剩余候选按包含起点的完整形状分组；同组旧 GEP 可由同一个
//指针 PHI 直接替换，不需要额外循环内偏移地址。
std::vector<LSRGroup>
LoopStrengthReductionPass::buildExactGroups(std::vector<LSRCandidate> &Candidates) {
    std::vector<LSRGroup> Groups;
    std::unordered_map<std::string, size_t> GroupIndex;
    for (LSRCandidate &C : Candidates) {
        if (C.Used) continue;
        std::string Key = candidateShapeKey(C, true);
        auto It = GroupIndex.find(Key);
        if (It == GroupIndex.end()) {
            size_t Index = Groups.size();
            GroupIndex.emplace(Key, Index);
            LSRGroup Group;
            Group.Order = Index;
            Group.Base = C.Base;
            Group.Start = C.Start;
            Group.StartConst = C.StartConst;
            Group.L = C.L;
            Group.PreHeader = C.PreHeader;
            Group.Header = C.Header;
            Group.Latch = C.Latch;
            Group.Step = C.Step;
            Group.Prefix = C.Prefix;
            Group.Suffix = C.Suffix;
            Group.ResultTy = C.OldGEP ? C.OldGEP->getIRType() : nullptr;
            Groups.push_back(std::move(Group));
            It = GroupIndex.find(Key);
        }
        Groups[It->second].Items.push_back(&C);
    }
    std::sort(Groups.begin(), Groups.end(),
              [](const LSRGroup &A, const LSRGroup &B) {
                  unsigned AD = A.L ? A.L->getDepth() : 0;
                  unsigned BD = B.L ? B.L->getDepth() : 0;
                  if (AD != BD) return AD > BD;
                  if (A.Items.size() != B.Items.size())
                      return A.Items.size() > B.Items.size();
                  return A.Order < B.Order;
              });
    return Groups;
}
//步骤 3.4 的死链范围只包含无副作用的整数、地址、选择和转换指令。
bool LoopStrengthReductionPass::isRemovableIndexInst(Instruction *I) {
    if (!I || !I->getParent()) return false;
    return dynamic_cast<BinaryInst *>(I) ||
           dynamic_cast<GetElementPtrInst *>(I) ||
           dynamic_cast<SelectInst *>(I) || dynamic_cast<TruncInst *>(I) ||
           dynamic_cast<ZextInst *>(I) || dynamic_cast<SextInst *>(I) ||
           dynamic_cast<BitCastInst *>(I);
}
//从候选索引反向收集封闭子图：某定义的所有 use 必须已属于旧 GEP 或已收集
//索引节点，否则该定义仍被外部观察，不能虚构为可删除收益。
bool LoopStrengthReductionPass::collectRemovableIndexChain(
    Instruction *I, std::unordered_set<User *> &AllowedUsers,
    std::unordered_set<Instruction *> &Out) {
    if (!I || Out.count(I)) return true;
    if (!isRemovableIndexInst(I)) return false;
    for (Use *U : I->getUses()) {
        if (!U || U->getValue() != I) continue;
        if (!AllowedUsers.count(U->getUser())) return false;
    }
    Out.insert(I);
    AllowedUsers.insert(I);
    forEachOperand(I, [&](size_t, Use *, Value *Operand) {
        auto *OperandI = dynamic_cast<Instruction *>(Operand);
        if (OperandI && OperandI != I)
            collectRemovableIndexChain(OperandI, AllowedUsers, Out);
    });
    return true;
}
//步骤 3.4：只计算位于循环内、支配 latch、因替换旧 GEP 后确定失去全部
//使用的索引指令；这些节点才对应每次回边都会节省的动态成本。
int64_t LoopStrengthReductionPass::estimateRemovableIndexCost(
    Loop *L, const std::vector<LSRCandidate *> &Items,
    const DominatorTree &DT) {
    if (!L || !L->getHeader()) return 0;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch) {
        return 0;
    }
    std::unordered_set<User *> AllowedUsers;
    for (LSRCandidate *C : Items) {
        if (C && C->OldGEP) AllowedUsers.insert(C->OldGEP);
    }
    std::unordered_set<Instruction *> Removable;
    for (LSRCandidate *C : Items) {
        auto *IndexI = C ? dynamic_cast<Instruction *>(C->Index) : nullptr;
        if (IndexI)
            collectRemovableIndexChain(IndexI, AllowedUsers, Removable);
    }
    int64_t Cost = 0;
    for (Instruction *I : Removable) {
        BasicBlock *Parent = I ? I->getParent() : nullptr;
        if (!Parent || !L->contains(Parent) ||
            !DT.dominates(Parent, Latch)) {
            continue;
        }
        if (Cost == std::numeric_limits<int64_t>::max()) return Cost;
        ++Cost;
    }
    return Cost;
}
//旧地址成本按物理 GEP 去重，且只统计循环内支配 latch、因而每轮必执行的
//地址计算；条件路径上的 GEP 不被高估为固定收益。
int64_t LoopStrengthReductionPass::estimateOldLoopCost(
    Loop *L, const std::vector<LSRCandidate *> &Items,
    const DominatorTree &DT) {
    if (!L) return -1;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Latch = nullptr;
    if (!L->getLoopSimplifyForm(PreHeader, Latch) ||
        !PreHeader || !Latch) {
        return -1;
    }
    int64_t Cost = 0;
    std::unordered_set<GetElementPtrInst *> CountedGEPs;
    for (const LSRCandidate *C : Items) {
        if (!C || C->Used || !C->OldGEP || !C->OldGEP->getParent())
            return -1;
        if (!CountedGEPs.insert(C->OldGEP).second) continue;
        BasicBlock *Parent = C->OldGEP->getParent();
        if (!L->contains(Parent) || !DT.dominates(Parent, Latch))
            continue;
        if (Cost == std::numeric_limits<int64_t>::max()) return Cost;
        ++Cost;
    }
    return Cost;
}
//步骤 3.5：完全同形组的新循环成本仅为 latch 中一条递推 GEP；一次性成本
//包括起始 GEP、前后不变量索引以及非常量起点的 SCEV 展开。
LSRCostEstimate LoopStrengthReductionPass::estimateExactGroupCost(
    const LSRGroup &Group, const DominatorTree &DT) {
    LSRCostEstimate Cost;
    if (Group.Items.empty() || !Group.L || !Group.PreHeader ||
        !Group.Header || !Group.Latch || !Group.Base || !Group.ResultTy) {
        return Cost;
    }
    int PrefixCost = estimatePrefixMaterializeCost(
        Group.Prefix, Group.L, Group.PreHeader, DT);
    int SuffixCost = estimatePrefixMaterializeCost(
        Group.Suffix, Group.L, Group.PreHeader, DT);
    int StartCost =
        Group.StartConst
            ? 0
            : estimateMaterializeCost(Group.Start, Group.L, Group.PreHeader,
                                      DT);
    if (PrefixCost >= UnsupportedMaterializeCost ||
        SuffixCost >= UnsupportedMaterializeCost ||
        StartCost >= UnsupportedMaterializeCost) {
        return Cost;
    }
    int64_t OldGEPCost =
        estimateOldLoopCost(Group.L, Group.Items, DT);
    if (OldGEPCost < 0) return Cost;
    Cost.RemovableIndexCost =
        estimateRemovableIndexCost(Group.L, Group.Items, DT);
    if (OldGEPCost >
        std::numeric_limits<int64_t>::max() -
            Cost.RemovableIndexCost) {
        return LSRCostEstimate();
    }
    Cost.OldLoopCost = OldGEPCost + Cost.RemovableIndexCost;
    //PHI 不计普通运算成本，每轮只新增一条 Next GEP。
    Cost.NewLoopCost = 1;
    //Start GEP 及索引/SCEV 物化均在预头只执行一次。
    Cost.NewSetupCost =
        1 + static_cast<int64_t>(PrefixCost) + SuffixCost + StartCost;
    Cost.Valid = true;
    return Cost;
}
//步骤 3.6：偏移共享组每轮需要一条递推 GEP，并为每个不同非锚起点保留一条
//循环内偏移 GEP；跨基本块共享会产生额外的提升成本，计入一次性开销。
LSRCostEstimate LoopStrengthReductionPass::estimateOffsetGroupCost(
    const OffsetReuseGroup &Group, const DominatorTree &DT) {
    LSRCostEstimate Cost;
    if (Group.Items.size() < 2 || !Group.L || !Group.PreHeader ||
        !Group.Header || !Group.Latch || !Group.Base || !Group.ResultTy) {
        return Cost;
    }
    int PrefixCost = estimatePrefixMaterializeCost(
        Group.Prefix, Group.L, Group.PreHeader, DT);
    if (PrefixCost >= UnsupportedMaterializeCost) return Cost;
    std::unordered_set<int64_t> Starts;
    std::unordered_map<int64_t, BasicBlock *> FirstOffsetBlock;
    std::unordered_set<int64_t> HeaderPromotedOffsets;
    for (const LSRCandidate *C : Group.Items) {
        if (!C || !C->StartConst ||
            *C->StartConst < Group.AnchorStart ||
            !C->OldGEP || !C->OldGEP->getParent()) {
            return Cost;
        }
        int64_t Offset = *C->StartConst - Group.AnchorStart;
        if (!isRepresentablePointerOffset(Offset)) return Cost;
        Starts.insert(*C->StartConst);
        if (Offset == 0) continue;
        BasicBlock *Parent = C->OldGEP->getParent();
        auto [It, Inserted] =
            FirstOffsetBlock.emplace(Offset, Parent);
        if (!Inserted && It->second != Parent)
            HeaderPromotedOffsets.insert(Offset);
    }
    if (Starts.size() < 2) return Cost;
    const uint64_t MaxCost =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    if (static_cast<uint64_t>(Starts.size()) > MaxCost ||
        static_cast<uint64_t>(HeaderPromotedOffsets.size()) > MaxCost) {
        return Cost;
    }
    int64_t OldGEPCost =
        estimateOldLoopCost(Group.L, Group.Items, DT);
    if (OldGEPCost < 0) return Cost;
    Cost.RemovableIndexCost =
        estimateRemovableIndexCost(Group.L, Group.Items, DT);
    if (OldGEPCost >
        std::numeric_limits<int64_t>::max() -
            Cost.RemovableIndexCost) {
        return LSRCostEstimate();
    }
    Cost.OldLoopCost = OldGEPCost + Cost.RemovableIndexCost;
    //每个不同起点对应一个循环内地址值，锚点直接使用指针 PHI。
    Cost.NewLoopCost = static_cast<int64_t>(Starts.size());
    //若同一偏移跨块共享，预检/零迭代路径可能额外求值，保守计作设置成本。
    const int64_t PromotedCost =
        static_cast<int64_t>(HeaderPromotedOffsets.size());
    const int64_t BaseSetupCost =
        1 + static_cast<int64_t>(PrefixCost);
    if (PromotedCost >
        std::numeric_limits<int64_t>::max() - BaseSetupCost) {
        return LSRCostEstimate();
    }
    Cost.NewSetupCost = BaseSetupCost + PromotedCost;
    Cost.Valid = true;
    return Cost;
}
//步骤 3.7：必须先有严格的每轮节省，再用迭代次数偿还设置成本。常量迭代
//次数用除法比较避免乘法溢出；未知次数只接受已证明至少执行一次的循环。
bool LoopStrengthReductionPass::isCostModelProfitable(
    const LSRCostEstimate &Cost, Loop *L, SCEVResult &SE) {
    if (!Cost.Valid || !L || Cost.OldLoopCost < 0 ||
        Cost.NewLoopCost < 0 || Cost.NewSetupCost < 0) {
        return false;
    }
    if (Cost.OldLoopCost <= Cost.NewLoopCost) return false;
    int64_t RecurringSaving = Cost.OldLoopCost - Cost.NewLoopCost;
    auto Trip = SE.analyzeLoopTripCount(L);
    if (Trip && Trip->Valid && Trip->ConstantTripCount &&
        (*Trip->ConstantTripCount == 0 || Trip->ProvenNoWrap)) {
        int64_t Iterations = *Trip->ConstantTripCount;
        if (Iterations <= 0) return false;
        return Iterations > Cost.NewSetupCost / RecurringSaving;
    }
    if (!Trip || !Trip->Valid || Trip->MayBeZeroTrip) return false;
    return RecurringSaving >= Cost.NewSetupCost;
}
//步骤 5：从被替换 GEP 的原索引开始向操作数反向删除无 use 的纯索引节点；
//删除一个节点后，其操作数可能新近变死，再压回 LIFO 队列继续清理。
void LoopStrengthReductionPass::cleanupDeadIndexChains(
    const std::vector<Instruction *> &Seeds) {
    UniqueWorkList<
        Instruction *, WorkListOrder::LIFO> WorkList;
    for (Instruction *Seed : Seeds) {
        if (Seed) WorkList.push(Seed);
    }
    while (!WorkList.empty()) {
        Instruction *Current = WorkList.pop();
        if (!isRemovableIndexInst(Current) || Current->hasUses()) continue;
        std::vector<Instruction *> Operands;
        forEachOperand(
            Current, [&](size_t, Use *, Value *Operand) {
                auto *OperandI = dynamic_cast<Instruction *>(Operand);
                if (OperandI && OperandI != Current)
                    Operands.push_back(OperandI);
            });
        delete Current;
        for (Instruction *OperandI : Operands) {
            if (OperandI && isRemovableIndexInst(OperandI) &&
                !OperandI->hasUses()) {
                WorkList.push(OperandI);
            }
        }
    }
}
//收益门面确保“估价”和“是否接受”始终走同一统一成本模型。
bool LoopStrengthReductionPass::shouldRewriteExactGroup(const LSRGroup &Group,
                                    const DominatorTree &DT,
                                    SCEVResult &SE) {
    LSRCostEstimate Cost = estimateExactGroupCost(Group, DT);
    return isCostModelProfitable(Cost, Group.L, SE);
}
//判断常量偏移复用分组是否满足统一成本模型
bool LoopStrengthReductionPass::shouldRewriteOffsetGroup(const OffsetReuseGroup &Group,
                                     const DominatorTree &DT,
                                     SCEVResult &SE) {
    LSRCostEstimate Cost = estimateOffsetGroupCost(Group, DT);
    return isCostModelProfitable(Cost, Group.L, SE);
}
//步骤 4.2：构造规范指针递推三元组。Start GEP 位于预头，Phi 置于头部 PHI
//序列，Next GEP 位于 latch 终结符前，两条 incoming 精确对应预头与回边。
PointerIV LoopStrengthReductionPass::createPointerIV(Value *Base,
                                 const std::vector<Value *> &StartIndices,
                                 int64_t Step,
                                 const std::shared_ptr<IRType> &ResultTy,
                                 BasicBlock *PreHeader, BasicBlock *Header,
                                 BasicBlock *Latch) {
    PointerIV Result;
    std::vector<Value *> Indices = StartIndices;
    Result.Start = new GetElementPtrInst(Base, Indices);
    Result.Start->updateType();
    PreHeader->insertBeforeTerminator(Result.Start);
    Result.Phi = new PhiInst(ResultTy);
    Header->pushFront(Result.Phi);
    std::vector<Value *> NextIndices;
    NextIndices.push_back(
        ConstInt::newConstInt(static_cast<int>(Step)));
    Result.Next = new GetElementPtrInst(Result.Phi, NextIndices);
    Result.Next->updateType();
    Latch->insertBeforeTerminator(Result.Next);
    Result.Phi->addIncoming(Result.Start, PreHeader);
    Result.Phi->addIncoming(Result.Next, Latch);
    return Result;
}
//步骤 4.3：完全同形组先在预头物化全部起始索引，再建立一个指针 IV；组内
//每个旧 GEP 直接替换为头部指针 PHI，并记录原索引作为步骤 5 的清理种子。
bool LoopStrengthReductionPass::rewriteExactGroup(LSRGroup &Group, const DominatorTree &DT,
                              std::vector<Instruction *> &DeadSeeds) {
    std::vector<Value *> StartIndices;
    if (!materializePrefix(Group.Prefix, Group.L, Group.PreHeader, DT,
                           StartIndices)) {
        return false;
    }
    Value *StartValue =
        Group.StartConst
            ? makeSCEVConstant(*Group.StartConst, Group.Start)
            : materializeSCEVAtPreheader(Group.Start, Group.L,
                                         Group.PreHeader, DT);
    if (!StartValue) return false;
    StartIndices.push_back(StartValue);
    if (!materializePrefix(Group.Suffix, Group.L, Group.PreHeader, DT,
                           StartIndices)) {
        return false;
    }
    PointerIV IV = createPointerIV(
        Group.Base, StartIndices, Group.Step, Group.ResultTy,
        Group.PreHeader, Group.Header, Group.Latch);
    for (LSRCandidate *C : Group.Items) {
        if (!C || C->Used || !C->OldGEP || !C->OldGEP->getParent())
            continue;
        if (auto *IndexI = dynamic_cast<Instruction *>(C->Index))
            DeadSeeds.push_back(IndexI);
        C->OldGEP->replaceAllUsesWith(IV.Phi);
        delete C->OldGEP;
        C->Used = true;
    }
    return true;
}
//步骤 4.4：偏移共享组先验证每个非零偏移的旧 GEP 都位于同一基本块，避免
//把 inbounds 地址提升到原本不执行的路径；随后以最小起点构造锚点指针 IV。
bool LoopStrengthReductionPass::rewriteOffsetGroup(OffsetReuseGroup &Group,
                               const DominatorTree &DT,
                               std::vector<Instruction *> &DeadSeeds) {
    std::unordered_map<int64_t, BasicBlock *> OffsetBlocks;
    for (LSRCandidate *C : Group.Items) {
        if (!C || !C->StartConst || !C->OldGEP ||
            !C->OldGEP->getParent())
            return false;
        int64_t Offset = *C->StartConst - Group.AnchorStart;
        if (Offset == 0) continue;
        auto [It, Inserted] =
            OffsetBlocks.emplace(Offset, C->OldGEP->getParent());
        if (!Inserted && It->second != C->OldGEP->getParent())
            return false;
    }
    std::vector<Value *> StartIndices;
    if (!materializePrefix(Group.Prefix, Group.L, Group.PreHeader, DT,
                           StartIndices)) {
        return false;
    }
    const SCEVExpr *StartTypeExpr =
        Group.Items.empty() ? nullptr : Group.Items.front()->Start;
    Value *Anchor =
        makeSCEVConstant(Group.AnchorStart, StartTypeExpr);
    if (!Anchor) return false;
    StartIndices.push_back(Anchor);
    PointerIV IV = createPointerIV(
        Group.Base, StartIndices, Group.Step, Group.ResultTy,
        Group.PreHeader, Group.Header, Group.Latch);
    std::unordered_map<int64_t, Value *> OffsetValues;
    OffsetValues.emplace(0, IV.Phi);
    //每个非锚偏移只在其全部旧 GEP 所在块的最早原位置生成一次，保持零迭代
    //和条件路径上的地址求值行为；找不到共同原位置则整组拒绝。
    for (LSRCandidate *C : Group.Items) {
        int64_t Offset = *C->StartConst - Group.AnchorStart;
        if (OffsetValues.count(Offset)) continue;
        std::vector<Value *> OffsetIndices;
        OffsetIndices.push_back(
            ConstInt::newConstInt(static_cast<int>(Offset)));
        auto *OffsetGEP = new GetElementPtrInst(IV.Phi, OffsetIndices);
        OffsetGEP->updateType();
        BasicBlock *CommonBB = nullptr;
        bool SameBlock = true;
        std::unordered_set<GetElementPtrInst *> Positions;
        for (LSRCandidate *Item : Group.Items) {
            if (!Item || !Item->StartConst ||
                *Item->StartConst - Group.AnchorStart != Offset ||
                !Item->OldGEP || !Item->OldGEP->getParent()) {
                continue;
            }
            Positions.insert(Item->OldGEP);
            BasicBlock *ItemBB = Item->OldGEP->getParent();
            if (!CommonBB)
                CommonBB = ItemBB;
            else if (CommonBB != ItemBB)
                SameBlock = false;
        }
        Instruction *InsertBefore = nullptr;
        if (SameBlock && CommonBB) {
            for (Instruction *I : *CommonBB) {
                auto *Position = dynamic_cast<GetElementPtrInst *>(I);
                if (Position && Positions.count(Position)) {
                    InsertBefore = Position;
                    break;
                }
            }
        }
        if (!InsertBefore ||
            !Instruction::insertBefore(InsertBefore, OffsetGEP)) {
            delete OffsetGEP;
            return false;
        }
        OffsetValues.emplace(Offset, OffsetGEP);
    }
    //所有可能失败的创建步骤完成后，统一替换旧地址并收集死索引种子。
    for (LSRCandidate *C : Group.Items) {
        if (!C || C->Used || !C->OldGEP || !C->OldGEP->getParent())
            continue;
        int64_t Offset = *C->StartConst - Group.AnchorStart;
        if (auto *IndexI = dynamic_cast<Instruction *>(C->Index))
            DeadSeeds.push_back(IndexI);
        C->OldGEP->replaceAllUsesWith(OffsetValues.at(Offset));
        delete C->OldGEP;
        C->Used = true;
    }
    return true;
}
//内层循环动态收益更高，允许的指针 IV 数量也更大；预算限制寄存器压力和
//单次 pass 的代码增长。
int LoopStrengthReductionPass::pointerIVLimit(const Loop *L) {
    return L && L->isInnermost() ? MaxPointerIVsPerInnerLoop
                                 : MaxPointerIVsPerOuterLoop;
}
//地址 LSR 主流程按“发现—偏移共享组—完全同形组—死链清理”提交；先处理
//偏移组可让多个起点优先共享一个 IV，Used 标志防止候选被二次改写。
bool LoopStrengthReductionPass::runAddressLSR(Function &F, LoopInfo &LI, SCEVResult &SE,
                       DominatorTree &DT) {
    //1—2.发现候选并完成结构、SCEV、支配和无回绕验证。
    std::vector<LSRCandidate> Candidates =
        collectCandidates(F, LI, SE, DT);
    if (Candidates.empty()) return false;
    std::unordered_map<Loop *, int> LoopPointerIVs;
    std::vector<Instruction *> DeadSeeds;
    bool Changed = false;
    //3—4.优先选择有收益的常量偏移共享组，并执行每循环 IV 数量预算。
    std::vector<OffsetReuseGroup> OffsetGroups =
        buildOffsetReuseGroups(Candidates);
    for (OffsetReuseGroup &Group : OffsetGroups) {
        if (!shouldRewriteOffsetGroup(Group, DT, SE)) continue;
        if (LoopPointerIVs[Group.L] >= pointerIVLimit(Group.L)) continue;
        if (!rewriteOffsetGroup(Group, DT, DeadSeeds)) continue;
        ++LoopPointerIVs[Group.L];
        Changed = true;
    }
    //3—4.再处理尚未使用的完全同形组，每组直接共享一个指针 PHI。
    std::vector<LSRGroup> ExactGroups = buildExactGroups(Candidates);
    for (LSRGroup &Group : ExactGroups) {
        if (!shouldRewriteExactGroup(Group, DT, SE)) continue;
        if (LoopPointerIVs[Group.L] >= pointerIVLimit(Group.L)) continue;
        if (!rewriteExactGroup(Group, DT, DeadSeeds)) continue;
        ++LoopPointerIVs[Group.L];
        Changed = true;
    }
    //5.改写全部完成后再递归删除旧索引链，避免组间候选尚未处理时误删。
    cleanupDeadIndexChains(DeadSeeds);
    return Changed;
}
//运行函数级循环地址强度削减并精确更新分析保留状态
PreservationStatus LoopStrengthReductionPass::run(
    Function &F, FunctionAnalysisPassManager &FAM) {
    auto &LI = FAM.getResult<LoopAnalysis>(F);
    auto &SE = FAM.getResult<SCEVAnalysis>(F);
    auto &DT = FAM.getResult<DominantAnalysis>(F);
    //1—5.执行完整地址强度削弱流程。
    bool Changed = runAddressLSR(F, LI, SE, DT);
    if (!Changed) return PreservationStatus::all();
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<PostDominantAnalysis>();
    PA.keep<LoopAnalysis>();
    //6.CFG 未改，支配树和 LoopInfo 仍有效；新指针递推改变 SCEV、范围、
    //基址偏移及 ModRef 关系，相关分析全部失效。
    PA.discard<SCEVAnalysis>();
    PA.discard<RangeAnalysis>();
    PA.discard<BasicAliasAnalysis>();
    PA.discard<SideEffectAnalysis>();
    return PA;
}
