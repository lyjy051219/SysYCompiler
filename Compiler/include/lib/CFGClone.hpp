#pragma once

#include "CFG.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using ValueToValueMap = std::unordered_map<Value *, Value *>;

// Optional, clone-local customization.  The name callback is evaluated after
// the new block has been attached to Destination, so callers may use
// Function::makeUniqueBlockIdent() and observe blocks cloned earlier in the
// same region.
struct CloneBlockRegionOptions {
    std::function<std::string(const BasicBlock *, std::size_t)>
        MakeBlockIdent;
    bool ClearTailCallFlags = false;
};

struct ClonedBlockRegion {
    ValueToValueMap VMap;
    std::vector<BasicBlock *> SourceBlocks;
    std::vector<BasicBlock *> ClonedBlocks;
    std::vector<std::pair<Instruction *, Instruction *>> InstPairs;

    BasicBlock *getClonedBlock(const BasicBlock *Source) const;
    Instruction *getClonedInstruction(const Instruction *Source) const;
};

// Clone SourceBlocks into Destination as one CFG region.
//
// All block mappings are established before any instruction is cloned.  Once
// every old-instruction -> new-instruction entry is available, every cloned
// operand is remapped a second time from the corresponding source operand.
// Values and CFG successors outside SourceBlocks are deliberately preserved
// unless InitialMap supplies an explicit replacement.
//
// Failure is atomic with respect to Destination: every block created by this
// call is detached and destroyed before std::nullopt is returned.
std::optional<ClonedBlockRegion> cloneBasicBlockRegion(
    Function &Destination,
    const std::vector<BasicBlock *> &SourceBlocks,
    const ValueToValueMap &InitialMap = {},
    const CloneBlockRegionOptions &Options = {});
