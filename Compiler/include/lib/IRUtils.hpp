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

// 控制克隆块的命名方式，并可清除克隆调用上的尾调用标记。
struct CloneBlockRegionOptions {
    std::function<std::string(const BasicBlock *, std::size_t)>
        MakeBlockIdent;
    bool ClearTailCallFlags = false;
};

// 保存源块到克隆块、源指令到克隆指令的完整对应关系。
struct ClonedBlockRegion {
    ValueToValueMap VMap;
    std::vector<BasicBlock *> SourceBlocks;
    std::vector<BasicBlock *> ClonedBlocks;
    std::vector<std::pair<Instruction *, Instruction *>> InstPairs;

    BasicBlock *getClonedBlock(const BasicBlock *Source) const;
    Instruction *getClonedInstruction(const Instruction *Source) const;
};

// 事务性克隆同一函数中的块区域；任一步失败都会回滚，区域外出口保持原目标。
// InitialMap 可预置外部值映射，Options 可为新块命名并调整调用标记。
std::optional<ClonedBlockRegion> cloneBasicBlockRegion(
    Function &Destination,
    const std::vector<BasicBlock *> &SourceBlocks,
    const ValueToValueMap &InitialMap = {},
    const CloneBlockRegionOptions &Options = {});

// 提供优化过程中常用的定宽整数运算，失败返回值表示溢出或参数无效。
class IRMath {
public:
    static bool isPowerOf2(int64_t Val);
    static bool isUnsignedPowerOf2(uint64_t Val);
    static int log2u(uint64_t Val);
    static int log2i(int64_t Val);
    static bool addI32(int64_t A, int64_t B, int64_t &Out);
    static bool addNoOverflow(int64_t A, int64_t B, int64_t &Out);
    static int64_t ceilDivPositive(int64_t Num, int64_t Den);
    static int64_t normalizeSignedToWidth(int64_t Val, unsigned Bits);
};

// 为分析缓存构造进程内稳定键；结果只在本次编译进程中有效。
class IRKey {
public:
    static std::string pointer(const void *Ptr);
    static std::string type(const std::shared_ptr<IRType> &Ty);
};
