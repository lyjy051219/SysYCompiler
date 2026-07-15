#include "IRUtils.hpp"
#include "CFG.hpp"
#include "TypeSystem.hpp"

#include <cstdint>
#include <limits>
#include <sstream>
#include <unordered_set>

static void rollbackClonedBlocks(
    std::vector<BasicBlock *> &Blocks) noexcept {
    // First remove every use made by a clone while all cloned definitions and
    // block labels are still alive.  This prevents Value::~Value() from
    // recursively deleting another clone (or, for an external reference, an
    // instruction in the source region) during rollback.
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
        InstructionCount += static_cast<std::size_t>(BB->Size());
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
        // Phase 1: construct the complete old-block -> new-block map.  This
        // makes all internal branch and PHI block operands immediately
        // remappable while leaving region exits untouched.
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

        // Phase 2: clone instructions and complete old-instruction ->
        // new-instruction mappings.  A clone may temporarily retain a forward
        // reference to the source instruction; the final pass below fixes it.
        for (std::size_t BlockIndex = 0;
             BlockIndex < SourceBlocks.size(); ++BlockIndex) {
            BasicBlock *OldBB = SourceBlocks[BlockIndex];
            BasicBlock *NewBB = Region.ClonedBlocks[BlockIndex];
            for (Instruction *OldI : *OldBB) {
                if (!OldI) return Fail();
                Instruction *NewI = OldI->clone(Region.VMap);
                if (!NewI) return Fail();

                NewBB->push_back(NewI);
                Region.InstPairs.emplace_back(OldI, NewI);
                Region.VMap[OldI] = NewI;

                if (Options.ClearTailCallFlags) {
                    if (auto *NewCall = dynamic_cast<CallInst *>(NewI)) {
                        NewCall->setTailCall(false);
                    }
                }
            }
        }

        // The source instruction is authoritative here.  Looking at NewI's
        // current operand when remapping would miss forward references after
        // another provisional mapping had already been applied by clone().
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

int32_t IRMath::normalizeSignedToWidth(int64_t Val, unsigned Bits) {
    if (Bits == 0 || Bits >= 32) return static_cast<int32_t>(Val);
    const uint32_t Mask = (uint32_t{1} << Bits) - 1;
    uint32_t Truncated = static_cast<uint32_t>(Val) & Mask;
    const uint32_t Sign = uint32_t{1} << (Bits - 1);
    if (Truncated & Sign) Truncated |= ~Mask;
    return static_cast<int32_t>(Truncated);
}

std::string IRKey::pointer(const void *Ptr) {
    std::ostringstream OS;
    OS << Ptr;
    return OS.str();
}

std::string IRKey::type(const std::shared_ptr<IRType> &Ty) {
    if (!Ty) return "noty";
    std::ostringstream OS;
    OS << static_cast<int>(Ty->getEnum()) << ':' << Ty->getSize() << ':'
       << Ty.get();
    return OS.str();
}
