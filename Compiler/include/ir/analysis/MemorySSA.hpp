#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

class BasicAAResult;
struct MemoryLocation;
class LoopInfo;
class Loop;

// MemorySSA 使用单一内存状态：Load 为 Use，Store/Call 为 Def，块入口统一为 Phi。
// Call 保留为 Def 以承载其他变换所需的调用顺序，walker 再用程序内存摘要跳过 timer。
// 每个访问通过 DefiningAccess 或 IncomingAccesses 连到之前的内存状态。
class MemoryAccess {
public:
    enum class Kind { LiveOnEntry, Phi, Use, Def };

    Kind getKind() const;
    bool isLiveOnEntry() const;
    bool isPhi() const;
    bool isUse() const;
    bool isDef() const;

    unsigned getID() const;
    BasicBlock *getBlock() const;
    Instruction *getInstruction() const;
    MemoryAccess *getDefiningAccess() const;

    const std::vector<std::pair<BasicBlock *, MemoryAccess *>> &
    getIncomingAccesses() const;

private:
    friend class MemorySSAResult;
    friend class MemorySSAAnalysis;

    MemoryAccess(Kind K, unsigned AccessID, BasicBlock *BB = nullptr,
                 Instruction *I = nullptr);

    Kind AccessKind;
    unsigned ID = 0;
    BasicBlock *Block = nullptr;
    Instruction *Inst = nullptr;
    MemoryAccess *DefiningAccess = nullptr;
    std::vector<std::pair<BasicBlock *, MemoryAccess *>> IncomingAccesses;
};

// 函数级结果拥有访问图，并提供最近潜在写和冗余写入证明查询。
class MemorySSAResult {
public:
    MemorySSAResult();
    MemorySSAResult(MemorySSAResult &&) noexcept;
    MemorySSAResult &operator=(MemorySSAResult &&) noexcept;
    MemorySSAResult(const MemorySSAResult &) = delete;
    MemorySSAResult &operator=(const MemorySSAResult &) = delete;

    MemoryAccess *getLiveOnEntry() const;
    MemoryAccess *getMemoryAccess(Instruction *I) const;
    MemoryAccess *getBlockEntry(BasicBlock *BB) const;
    MemoryAccess *getBlockExit(BasicBlock *BB) const;

    // 反向查询在所有路径寻找首个可能写；分歧、环或预算耗尽均保守返回 Phi/空值。
    MemoryAccess *getClobberingAccess(
        Instruction *At, Value *Ptr, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;
    MemoryAccess *getClobberingAccess(
        Instruction *At, const MemoryLocation &Location,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;

    MemoryAccess *getClobberingAccess(
        MemoryAccess *Start, Value *Ptr, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;
    MemoryAccess *getClobberingAccess(
        MemoryAccess *Start, const MemoryLocation &Location,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;

    // 冗余存储证明要求所有路径已有同址、同宽、同 SSA 值写入，循环还需操作数不变。
    bool isStoreValueRedundant(
        StoreInst *Store, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;

    bool invalidate(Function &F, const PreservationStatus &PA);
    std::size_t size() const;

private:
    friend class MemorySSAAnalysis;

    static bool isInvariantInLoop(Value *V, const Loop *L);
    static bool storeFullyCoversItsPointer(const StoreInst *SI);

    MemoryAccess *createAccess(MemoryAccess::Kind K, BasicBlock *BB = nullptr,
                               Instruction *I = nullptr);
    bool mayClobber(MemoryAccess *Access, const MemoryLocation &Location,
                    BasicAAResult &AA,
                    const LoopInfo &LI,
                    FunctionAnalysisPassManager &FAM) const;

    std::vector<std::unique_ptr<MemoryAccess>> OwnedAccesses;
    MemoryAccess *LiveOnEntry = nullptr;
    std::unordered_map<Instruction *, MemoryAccess *> InstructionAccesses;
    std::unordered_map<BasicBlock *, MemoryAccess *> BlockEntries;
    std::unordered_map<BasicBlock *, MemoryAccess *> BlockExits;
};

// 构建流程先创建所有块入口，再线性连接块内访问，最后回填 CFG 入边。
class MemorySSAAnalysis : public AnalysisDescriptor<MemorySSAAnalysis> {
public:
    using Result = MemorySSAResult;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<MemorySSAAnalysis>;
    static PassID Key;
};
