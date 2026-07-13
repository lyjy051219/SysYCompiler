#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <vector>

class DominatorTree;
class Loop;
class SCEVResult;
struct InductionDescriptor;
struct LoopTripCountInfo;

// Recognize the deliberately small, fully-provable subset of scalar memory
// loops that can be replaced by llvm.memcpy/llvm.memset.  This pass does not
// infer general dependence facts: copy roots must be distinct globals/allocas,
// and the loop must have a constant trip count and no unrelated operation.
class LoopIdiomRecognizePass
    : public PassDescriptor<LoopIdiomRecognizePass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    struct LinearAccess {
        GetElementPtrInst *GEP = nullptr;
        Value *Base = nullptr;
        std::vector<Value *> Indices;
        size_t InductionIndex = 0;
        std::shared_ptr<IRType> ElementTy;
    };

    static bool matchLinearAccess(GetElementPtrInst *GEP, PhiInst *IndVar,
                                  Loop *L, LinearAccess &Access);
    static Value *getDisjointObjectRoot(Value *V);
    static bool areProvablyDisjoint(const LinearAccess &A,
                                    const LinearAccess &B);
    static bool getRepeatedByte(Value *V, uint8_t &Byte);
    static bool hasOnlyAllowedLoopInstructions(
        Loop *L, const LoopTripCountInfo &TCI,
        const InductionDescriptor &IV, StoreInst *Store,
        GetElementPtrInst *DestGEP, LoadInst *SourceLoad,
        GetElementPtrInst *SourceGEP, DominatorTree &DT);
    static GetElementPtrInst *materializeStartPointer(
        const LinearAccess &Access, PhiInst *IndVar, int64_t Start,
        BasicBlock *PreHeader);
    static bool transformLoop(Loop *L, SCEVResult &SE, DominatorTree &DT);
};
