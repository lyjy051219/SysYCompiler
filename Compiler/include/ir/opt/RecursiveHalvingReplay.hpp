#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

// 把有界的有符号折半单自递归改写为下降循环和后进先出回放循环。
class RecursiveHalvingReplayPass
    : public PassDescriptor<RecursiveHalvingReplayPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    struct ReturnPath {
        std::vector<BasicBlock *> Blocks;
        std::vector<Instruction *> Slice;
        Value *Result = nullptr;
        BasicBlock *SharedExit = nullptr;
    };

    struct Plan {
        Function *Target = nullptr;
        Param *RecursiveParam = nullptr;
        unsigned RecursiveArgIndex = 0;
        CallInst *SelfCall = nullptr;
        BasicBlock *Entry = nullptr;
        BasicBlock *SecondTest = nullptr;
        BasicBlock *RecursiveBlock = nullptr;
        std::vector<Instruction *> HalfSlice;
        std::vector<Instruction *> SelectorSlice;
        std::vector<Instruction *> CommonSlice;
        ReturnPath BaseZero;
        ReturnPath BaseOne;
        ReturnPath TrueArm;
        ReturnPath FalseArm;
    };

    static bool collectPlan(Function &F, Plan &P);
    static bool matchEqualityDispatch(BasicBlock *BB, Value *Compared,
                                      int64_t Constant,
                                      BasicBlock *&EqualSuccessor,
                                      BasicBlock *&OtherSuccessor);
    static bool matchSignedHalving(Value *V, Value *Source,
                                   BasicBlock *ExpectedBlock,
                                   std::vector<Instruction *> &Slice);
    static bool resolveReturnPath(BasicBlock *Start, BasicBlock *IncomingPred,
                                  ReturnPath &Path);
    static bool resolveSharedExitValue(BasicBlock *Exit, BasicBlock *Pred,
                                       Value *&Result);
    static bool collectSelectorSlice(CallInst *SelfCall, BrInst *Branch,
                                     std::vector<Instruction *> &Selector,
                                     std::vector<Instruction *> &Common);
    static bool collectSelectorDependency(
        Value *V, BasicBlock *BB,
        const std::unordered_set<Instruction *> &Suffix,
        std::unordered_set<Instruction *> &Selected,
        std::unordered_set<Instruction *> &Visiting);
    static bool isAlwaysDefinedIntegerInstruction(Instruction *I);
    static bool validateStraightLineSlice(
        const std::vector<Instruction *> &Slice);
    static bool dependsOnValue(Value *Root, Value *Needle,
                               std::unordered_set<Value *> &Visiting,
                               Value *StopAt = nullptr,
                               unsigned Depth = 0);
    static Value *mapClonedValue(
        Value *V, const std::unordered_map<Value *, Value *> &VMap,
        Function *OldFunction);
    static bool cloneSlice(
        const std::vector<Instruction *> &Slice, BasicBlock *Destination,
        std::unordered_map<Value *, Value *> &VMap, Function *OldFunction);
    static void rollbackBlocks(std::vector<BasicBlock *> &Blocks);
    static bool buildReplacement(Function &F, const Plan &P);
};
