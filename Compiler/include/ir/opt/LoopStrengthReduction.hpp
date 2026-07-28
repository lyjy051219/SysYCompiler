#pragma once

#include "ir/analysis/LoopInfo.hpp"
#include "ir/analysis/SCEV.hpp"
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

struct IndexComponent {
    Value *Raw = nullptr;
    const SCEVExpr *Expr = nullptr;
};

struct LSRCandidate {
    GetElementPtrInst *OldGEP = nullptr;
    Value *Base = nullptr;
    Value *Index = nullptr;
    const SCEVExpr *Start = nullptr;
    std::optional<int64_t> StartConst;
    std::vector<IndexComponent> Prefix;
    std::vector<IndexComponent> Suffix;
    Loop *L = nullptr;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    int64_t Step = 0;
    int64_t LastIndexStride = 0;
    bool HasLeadingZero = false;
    bool AllowOffsetReuse = true;
    bool Used = false;
};

struct LSRGroup {
    size_t Order = 0;
    std::vector<LSRCandidate *> Items;
    Value *Base = nullptr;
    const SCEVExpr *Start = nullptr;
    std::optional<int64_t> StartConst;
    Loop *L = nullptr;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    int64_t Step = 0;
    std::vector<IndexComponent> Prefix;
    std::vector<IndexComponent> Suffix;
    std::shared_ptr<IRType> ResultTy;
};

struct OffsetReuseGroup {
    size_t Order = 0;
    std::vector<LSRCandidate *> Items;
    Value *Base = nullptr;
    Loop *L = nullptr;
    BasicBlock *PreHeader = nullptr;
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
    int64_t Step = 0;
    int64_t AnchorStart = 0;
    std::vector<IndexComponent> Prefix;
    std::shared_ptr<IRType> ResultTy;
};

struct PointerIV {
    GetElementPtrInst *Start = nullptr;
    PhiInst *Phi = nullptr;
    GetElementPtrInst *Next = nullptr;
};

struct AddressGEPNode {
    GetElementPtrInst *GEP = nullptr;
    int64_t Offset = 0;
};

// 统一成本单位为一条普通 IR 算术或地址计算。OldLoopCost/NewLoopCost 是每轮
// 成本，NewSetupCost 是预头及可能提升到头部的一次性成本。
struct LSRCostEstimate {
    int64_t OldLoopCost = 0;
    int64_t NewLoopCost = 0;
    int64_t NewSetupCost = 0;
    int64_t RemovableIndexCost = 0;
    bool Valid = false;
};

// 把循环内仿射索引 GEP 改写为指针归纳变量：
// 1. 从内到外扫描规范循环，发现仅一个循环变化维度的 GEP 地址族；
// 2. 用 SCEV、支配关系、无回绕证明和计时边界验证起点与步长可安全外提；
// 3. 按完全同形或常量起点偏移分组，并比较旧索引链与新指针 IV 的动态成本；
// 4. 在预头/头部/latch 构造起始地址、PHI 和递推 GEP，替换旧地址；
// 5. 递归删除失去使用的旧索引链，并使数值、范围、别名分析失效。
class LoopStrengthReductionPass
    : public PassDescriptor<LoopStrengthReductionPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static constexpr int UnsupportedMaterializeCost = 1000000;
    static constexpr int MaxPointerIVsPerInnerLoop = 8;
    static constexpr int MaxPointerIVsPerOuterLoop = 4;
    // 服务步骤 2：SCEVExpander 的可展开性与成本接口在预头上使用同一深度
    // 预算；估价失败统一返回 UnsupportedMaterializeCost。
    static std::string scevKey(const SCEVExpr *E);
    static bool canUseInPreheader(Value *V, BasicBlock *PreHeader,
                                  const DominatorTree &DT);
    static bool canMaterializeSCEVAtPreheader(
        const SCEVExpr *E, const Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    static int estimateMaterializeCost(
        const SCEVExpr *E, const Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    static Value *makeSCEVConstant(int64_t V, const SCEVExpr *E);
    static Value *materializeSCEVAtPreheader(
        const SCEVExpr *E, const Loop *L, BasicBlock *PreHeader,
        const DominatorTree &DT);
    static int estimatePrefixMaterializeCost(
        const std::vector<IndexComponent> &Prefix, const Loop *L,
        BasicBlock *PreHeader, const DominatorTree &DT);
    static bool materializePrefix(
        const std::vector<IndexComponent> &Prefix, const Loop *L,
        BasicBlock *PreHeader, const DominatorTree &DT,
        std::vector<Value *> &Indices);
    static bool isLoadStoreAddressUse(Instruction *UserI, Use *U);
    // 服务步骤 1：地址族只允许 GEP 作为 load/store 地址，或继续派生单常量
    // 偏移 GEP；任何其他使用都会使整个族拒绝改写。
    static bool getConstantOffsetGEP(
        GetElementPtrInst *Parent, Instruction *UserI, Use *U,
        GetElementPtrInst *&Child, int64_t &Offset);
    static bool collectAddressGEPFamily(
        GetElementPtrInst *GEP, Loop *L, int64_t CurrentOffset,
        std::vector<AddressGEPNode> &Nodes,
        std::unordered_set<GetElementPtrInst *> &Visited);
    static bool isSingleIndexArrayStep(Value *Base, size_t NumIndices);
    static bool matchGEP(
        GetElementPtrInst *GEP, Loop *L, SCEVResult &SE, Value *&Base,
        Value *&Index, std::vector<IndexComponent> &Prefix,
        std::vector<IndexComponent> &Suffix, int64_t &ActiveIndexStride,
        int64_t &FinalPointeeStride, bool &HasLeadingZero,
        bool &AllowOffsetReuse);
    static std::vector<LSRCandidate> collectCandidates(
        Function &F, LoopInfo &LI, SCEVResult &SE,
        const DominatorTree &DT);
    // 服务步骤 3：ShapeKey 编码循环、基址、步长、布局与不变量维度；偏移组
    // 忽略起点以共享锚点，完全同形组则把起点纳入键。
    static std::string candidateShapeKey(const LSRCandidate &Candidate,
                                         bool IncludeStart);
    static std::vector<OffsetReuseGroup> buildOffsetReuseGroups(
        std::vector<LSRCandidate> &Candidates);
    static std::vector<LSRGroup> buildExactGroups(
        std::vector<LSRCandidate> &Candidates);
    // 服务步骤 3：只把所有使用均封闭在候选旧 GEP/索引子图内、且支配回边的
    // 纯索引指令计入可删除的每轮成本。
    static bool isRemovableIndexInst(Instruction *I);
    static bool collectRemovableIndexChain(
        Instruction *I, std::unordered_set<User *> &AllowedUsers,
        std::unordered_set<Instruction *> &Out);
    static int64_t estimateRemovableIndexCost(
        Loop *L, const std::vector<LSRCandidate *> &Items,
        const DominatorTree &DT);
    static int64_t estimateOldLoopCost(
        Loop *L, const std::vector<LSRCandidate *> &Items,
        const DominatorTree &DT);
    static LSRCostEstimate estimateExactGroupCost(
        const LSRGroup &Group, const DominatorTree &DT);
    static LSRCostEstimate estimateOffsetGroupCost(
        const OffsetReuseGroup &Group, const DominatorTree &DT);
    static bool isCostModelProfitable(
        const LSRCostEstimate &Cost, Loop *L, SCEVResult &SE);
    static bool isRepresentablePointerOffset(int64_t Offset);
    static void cleanupDeadIndexChains(
        const std::vector<Instruction *> &Seeds);
    static bool shouldRewriteExactGroup(const LSRGroup &Group,
                                        const DominatorTree &DT,
                                        SCEVResult &SE);
    static bool shouldRewriteOffsetGroup(const OffsetReuseGroup &Group,
                                         const DominatorTree &DT,
                                         SCEVResult &SE);
    // 服务步骤 4：三个节点形成固定不变量——Start 位于预头、Phi 位于头部、
    // Next 位于 latch，且 PHI 的两条 incoming 分别来自预头和 latch。
    static PointerIV createPointerIV(
        Value *Base, const std::vector<Value *> &StartIndices, int64_t Step,
        const std::shared_ptr<IRType> &ResultTy, BasicBlock *PreHeader,
        BasicBlock *Header, BasicBlock *Latch);
    static bool rewriteExactGroup(
        LSRGroup &Group, const DominatorTree &DT,
        std::vector<Instruction *> &DeadSeeds);
    static bool rewriteOffsetGroup(
        OffsetReuseGroup &Group, const DominatorTree &DT,
        std::vector<Instruction *> &DeadSeeds);
    static int pointerIVLimit(const Loop *L);
    static bool runAddressLSR(Function &F, LoopInfo &LI, SCEVResult &SE,
                              DominatorTree &DT);
};
