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

 
 
 
 
 
// 流程：1. 由 SCEV 匹配单位步长连续访问；2. 区分 memset、memcpy 与临时数组双复制；
// 3. 证明对象边界、无额外副作用及 trip count 安全；4. 常量次数直接替换，
// 运行期次数建立有收益/溢出 guard 的 intrinsic fast path 并保留原循环慢路径。
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
