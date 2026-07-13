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

// A deliberately small, conservative MemorySSA for this IR.  It models one
// unified memory state: stores and calls are MemoryDefs, loads are MemoryUses,
// and every basic-block entry is represented by a MemoryPhi.  The latter is a
// little denser than pruned SSA, but makes construction O(B + E + I), handles
// irreducible CFGs without a fixed point, and keeps invalidation simple.
class MemoryAccess {
public:
    enum class Kind { LiveOnEntry, Phi, Use, Def };

    Kind getKind() const { return AccessKind; }
    bool isLiveOnEntry() const { return AccessKind == Kind::LiveOnEntry; }
    bool isPhi() const { return AccessKind == Kind::Phi; }
    bool isUse() const { return AccessKind == Kind::Use; }
    bool isDef() const { return AccessKind == Kind::Def; }

    unsigned getID() const { return ID; }
    BasicBlock *getBlock() const { return Block; }
    Instruction *getInstruction() const { return Inst; }
    MemoryAccess *getDefiningAccess() const { return DefiningAccess; }

    const std::vector<std::pair<BasicBlock *, MemoryAccess *>> &
    getIncomingAccesses() const {
        return IncomingAccesses;
    }

private:
    friend class MemorySSAResult;
    friend class MemorySSAAnalysis;

    MemoryAccess(Kind K, unsigned AccessID, BasicBlock *BB = nullptr,
                 Instruction *I = nullptr)
        : AccessKind(K), ID(AccessID), Block(BB), Inst(I) {}

    Kind AccessKind;
    unsigned ID = 0;
    BasicBlock *Block = nullptr;
    Instruction *Inst = nullptr;
    MemoryAccess *DefiningAccess = nullptr;
    std::vector<std::pair<BasicBlock *, MemoryAccess *>> IncomingAccesses;
};

class MemorySSAResult {
public:
    MemorySSAResult() = default;
    MemorySSAResult(MemorySSAResult &&) noexcept = default;
    MemorySSAResult &operator=(MemorySSAResult &&) noexcept = default;
    MemorySSAResult(const MemorySSAResult &) = delete;
    MemorySSAResult &operator=(const MemorySSAResult &) = delete;

    MemoryAccess *getLiveOnEntry() const { return LiveOnEntry; }
    MemoryAccess *getMemoryAccess(Instruction *I) const;
    MemoryAccess *getBlockEntry(BasicBlock *BB) const;
    MemoryAccess *getBlockExit(BasicBlock *BB) const;

    // Return the first access that may modify Ptr on every backwards path
    // from At.  A returned Phi means that paths disagree or a transparent
    // cycle has no single reaching definition.  nullptr means an invalid
    // query or walker-budget exhaustion.  Clients must treat both cases
    // conservatively; nullptr never means "no clobber".
    MemoryAccess *getClobberingAccess(
        Instruction *At, Value *Ptr, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;
    MemoryAccess *getClobberingAccess(
        Instruction *At, const MemoryLocation &Location,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;

    // Same query starting at an already known memory state.  This is useful
    // when a consumer walks several accesses in one block.
    MemoryAccess *getClobberingAccess(
        MemoryAccess *Start, Value *Ptr, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;
    MemoryAccess *getClobberingAccess(
        MemoryAccess *Start, const MemoryLocation &Location,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;

    // Prove that Store writes the value already held by its destination on
    // every path.  In a loop, a backedge reaching Store itself is handled as
    // an inductive recurrence, but only for loop-invariant pointer/value
    // operands and only when a non-recursive incoming path has a concrete
    // same-value store.  Reads are transparent; every possible modification
    // that is not a full same-value overwrite makes the query fail.
    bool isStoreValueRedundant(
        StoreInst *Store, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;

    bool invalidate(Function &F, const PreservationStatus &PA);
    std::size_t size() const { return OwnedAccesses.size(); }

private:
    friend class MemorySSAAnalysis;

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

class MemorySSAAnalysis : public AnalysisDescriptor<MemorySSAAnalysis> {
public:
    using Result = MemorySSAResult;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<MemorySSAAnalysis>;
    static PassID Key;
};
