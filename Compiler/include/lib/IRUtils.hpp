#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class BasicBlock;
class Function;
class Instruction;
class IRType;
class Value;

using ValueToValueMap = std::unordered_map<Value *, Value *>;

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

std::optional<ClonedBlockRegion> cloneBasicBlockRegion(
    Function &Destination,
    const std::vector<BasicBlock *> &SourceBlocks,
    const ValueToValueMap &InitialMap = {},
    const CloneBlockRegionOptions &Options = {});

class IRMath {
public:
    static bool isPowerOf2(int64_t Val);
    static bool isUnsignedPowerOf2(uint64_t Val);
    static int log2u(uint64_t Val);
    static int log2i(int64_t Val);
    static bool addI32(int64_t A, int64_t B, int64_t &Out);
    static bool addNoOverflow(int64_t A, int64_t B, int64_t &Out);
    static int64_t ceilDivPositive(int64_t Num, int64_t Den);
    static int32_t normalizeSignedToWidth(int64_t Val, unsigned Bits);
};

class IRKey {
public:
    static std::string pointer(const void *Ptr);
    static std::string type(const std::shared_ptr<IRType> &Ty);
};
