#include "lib/IRUtils.hpp"
#include "lib/CFG.hpp"
#include "lib/TypeSystem.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_set>

static void rollbackClonedBlocks(
    std::vector<BasicBlock *> &Blocks) noexcept {
    // 在克隆定义和块标签仍存活时先清空全部操作数，避免回滚析构递归删除
    // 其他克隆对象或被临时引用的源区域指令。
    for (BasicBlock *BB : Blocks) {
        if (!BB) continue;
        for (Instruction *I : *BB) {
            if (I) I->clearOperands();
        }
    }

    for (BasicBlock *BB : Blocks) {
        if (BB) BB->clear();
    }
    for (BasicBlock *BB : Blocks) delete BB;
    Blocks.clear();
}

BasicBlock *ClonedBlockRegion::getClonedBlock(
    const BasicBlock *Source) const {
    auto It = VMap.find(const_cast<BasicBlock *>(Source));
    return It == VMap.end() ? nullptr
                            : dynamic_cast<BasicBlock *>(It->second);
}

Instruction *ClonedBlockRegion::getClonedInstruction(
    const Instruction *Source) const {
    auto It = VMap.find(const_cast<Instruction *>(Source));
    return It == VMap.end() ? nullptr
                            : dynamic_cast<Instruction *>(It->second);
}

std::optional<ClonedBlockRegion> cloneBasicBlockRegion(
    Function &Destination,
    const std::vector<BasicBlock *> &SourceBlocks,
    const ValueToValueMap &InitialMap,
    const CloneBlockRegionOptions &Options) {
    if (SourceBlocks.empty()) return std::nullopt;

    std::unordered_set<BasicBlock *> Seen;
    Seen.reserve(SourceBlocks.size());
    Function *SourceFunction = nullptr;
    std::size_t InstructionCount = 0;
    for (BasicBlock *BB : SourceBlocks) {
        if (!BB || !Seen.insert(BB).second || !BB->getParent()) {
            return std::nullopt;
        }
        if (!SourceFunction) SourceFunction = BB->getParent();
        if (BB->getParent() != SourceFunction) return std::nullopt;
        InstructionCount += static_cast<std::size_t>(BB->size());
    }
    for (const auto &Entry : InitialMap) {
        if (!Entry.first || !Entry.second) return std::nullopt;
    }

    ClonedBlockRegion Region;
    Region.VMap = InitialMap;
    Region.VMap.reserve(InitialMap.size() + SourceBlocks.size() +
                        InstructionCount);
    Region.SourceBlocks = SourceBlocks;
    Region.ClonedBlocks.reserve(SourceBlocks.size());
    Region.InstPairs.reserve(InstructionCount);

    auto Fail = [&]() -> std::optional<ClonedBlockRegion> {
        rollbackClonedBlocks(Region.ClonedBlocks);
        return std::nullopt;
    };

    try {
        // 第一阶段建立完整的源块到新块映射，使区域内分支和 PHI 块操作数
        // 都可重映射，同时保持指向区域外的出口不变。
        for (std::size_t Index = 0; Index < SourceBlocks.size(); ++Index) {
            BasicBlock *OldBB = SourceBlocks[Index];
            auto *NewBB = new BasicBlock();
            Region.ClonedBlocks.push_back(NewBB);
            Destination.pushBlock(NewBB);
            Region.VMap[OldBB] = NewBB;

            if (Options.MakeBlockIdent) {
                NewBB->setIdent(Options.MakeBlockIdent(OldBB, Index));
            }
        }

        // 第二阶段按块顺序克隆指令并补全指令映射；此时允许克隆指令暂时
        // 保留指向源指令的前向引用，最后一阶段统一修正。
        for (std::size_t BlockIndex = 0;
             BlockIndex < SourceBlocks.size(); ++BlockIndex) {
            BasicBlock *OldBB = SourceBlocks[BlockIndex];
            BasicBlock *NewBB = Region.ClonedBlocks[BlockIndex];
            for (Instruction *OldI : *OldBB) {
                if (!OldI) return Fail();
                Instruction *NewI = OldI->clone(Region.VMap);
                if (!NewI) return Fail();

                NewBB->pushBack(NewI);
                Region.InstPairs.emplace_back(OldI, NewI);
                Region.VMap[OldI] = NewI;

                if (Options.ClearTailCallFlags) {
                    if (auto *NewCall = dynamic_cast<CallInst *>(NewI)) {
                        NewCall->setTailCall(false);
                    }
                }
            }
        }

        // 最终以源指令的操作数为准重新映射，避免临时映射覆盖后遗漏前向引用。
        for (const auto &Pair : Region.InstPairs) {
            Instruction *OldI = Pair.first;
            Instruction *NewI = Pair.second;
            if (!OldI || !NewI ||
                OldI->getNumOperands() != NewI->getNumOperands()) {
                return Fail();
            }

            for (std::size_t OperandIndex = 0;
                 OperandIndex < OldI->getNumOperands(); ++OperandIndex) {
                Value *OldOperand = OldI->getOperand(OperandIndex);
                auto It = Region.VMap.find(OldOperand);
                Value *NewOperand =
                    It == Region.VMap.end() ? OldOperand : It->second;
                if (!NewOperand ||
                    (NewI->getOperand(OperandIndex) != NewOperand &&
                     !NewI->replaceOperand(OperandIndex, NewOperand))) {
                    return Fail();
                }
            }
        }
    } catch (...) {
        return Fail();
    }

    return std::optional<ClonedBlockRegion>(std::move(Region));
}

bool IRMath::isPowerOf2(int64_t Val) {
    if (Val <= 0) return false;
    auto U = static_cast<uint64_t>(Val);
    return (U & (U - 1)) == 0;
}

bool IRMath::isUnsignedPowerOf2(uint64_t Val) {
    return Val != 0 && ((Val & (Val - 1)) == 0);
}

int IRMath::log2u(uint64_t Val) {
    int N = 0;
    while (Val > 1) {
        Val >>= 1;
        ++N;
    }
    return N;
}

int IRMath::log2i(int64_t Val) {
    return log2u(static_cast<uint64_t>(Val));
}

bool IRMath::addI32(int64_t A, int64_t B, int64_t &Out) {
    Out = A + B;
    return Out >= std::numeric_limits<int32_t>::min() &&
           Out <= std::numeric_limits<int32_t>::max();
}

bool IRMath::addNoOverflow(int64_t A, int64_t B, int64_t &Out) {
    if ((B > 0 && A > std::numeric_limits<int64_t>::max() - B) ||
        (B < 0 && A < std::numeric_limits<int64_t>::min() - B)) {
        return false;
    }
    Out = A + B;
    return true;
}

int64_t IRMath::ceilDivPositive(int64_t Num, int64_t Den) {
    if (Num <= 0 || Den <= 0) return 0;
    return (Num + Den - 1) / Den;
}

int64_t IRMath::normalizeSignedToWidth(int64_t Val, unsigned Bits) {
    if (Bits == 0 || Bits >= 64)
        return Val;
    const uint64_t Mask = (uint64_t{1} << Bits) - 1U;
    uint64_t Truncated = static_cast<uint64_t>(Val) & Mask;
    const uint64_t Sign = uint64_t{1} << (Bits - 1U);
    if (Truncated & Sign)
        Truncated |= ~Mask;
    int64_t Result = 0;
    std::memcpy(&Result, &Truncated, sizeof(Result));
    return Result;
}

std::string IRKey::pointer(const void *Ptr) {
    std::ostringstream OS;
    OS << Ptr;
    return OS.str();
}

std::string IRKey::type(const std::shared_ptr<IRType> &Ty) {
    if (!Ty) return "noty";
    std::ostringstream OS;
    switch (Ty->getTypeSystem()) {
    case IR_INT: {
        auto Integer = std::dynamic_pointer_cast<INTType>(Ty);
        OS << 'i' << (Integer ? Integer->getBitWidth() : 0);
        break;
    }
    case IR_FLOAT:
        OS << "f32";
        break;
    case IR_VOID:
        OS << "void";
        break;
    case IR_POINTER: {
        auto Pointer = std::dynamic_pointer_cast<POINTERType>(Ty);
        OS << "ptr(" << type(Pointer ? Pointer->getPointerType() : nullptr)
           << ')';
        break;
    }
    case IR_ARRAY: {
        auto Array = std::dynamic_pointer_cast<ARRAYType>(Ty);
        OS << "arr" << (Array ? Array->getElementCount() : 0) << '('
           << type(Array ? Array->getElementType() : nullptr) << ')';
        break;
    }
    case IR_UNDEF:
        OS << "undef";
        break;
    }
    return OS.str();
}
