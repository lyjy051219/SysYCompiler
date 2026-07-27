#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <vector>

class DominatorTree;
class Function;
class Loop;
class RangeInfo;
class SCEVResult;
struct InductionDescriptor;
struct LoopTripCountInfo;

// Recognize a deliberately small, fully-provable subset of scalar memory
// loops.  Constant and guarded runtime copy/fill loops become memcpy/memset;
// memmove is emitted only for an adjacent two-loop transaction through a
// private temporary.  The pass never guesses general dependence facts and
// keeps the original scalar loop as the fallback for dynamic short lengths.
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

    struct CopyLoopMatch {
        Loop *L = nullptr;
        BasicBlock *PreHeader = nullptr;
        BasicBlock *Header = nullptr;
        BasicBlock *ExitBlock = nullptr;
        PhiInst *IndVar = nullptr;
        StoreInst *Store = nullptr;
        LoadInst *Load = nullptr;
        LinearAccess Destination;
        LinearAccess Source;
        Value *RuntimeBound = nullptr;
        int64_t Start = 0;
        int64_t TripCount = 0;
        bool IsRuntime = false;
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
    static bool matchSimpleCopyLoop(Loop *L, SCEVResult &SE,
                                    DominatorTree &DT,
                                    CopyLoopMatch &Match);
    static bool transformTemporaryCopyPair(
        Function &F, const CopyLoopMatch &First,
        const CopyLoopMatch &Second, DominatorTree &DT, RangeInfo &RI,
        bool &CFGChanged);
    static bool transformLoop(Function &F, Loop *L, SCEVResult &SE,
                              DominatorTree &DT, RangeInfo &RI,
                              bool &CFGChanged);
};
