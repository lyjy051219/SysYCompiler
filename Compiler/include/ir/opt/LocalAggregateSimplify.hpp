#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <map>
#include <memory>
#include <set>
#include <unordered_set>
#include <vector>

struct LocalAggregatePointerPath {
    AllocaInst *Root = nullptr;
    std::vector<int> Indices;
    bool IsConstant = true;
};

struct LocalAggregateReadOnlyConstantArrayPlan {
    Variable *Source = nullptr;
    CallInst *InitCopy = nullptr;
    std::vector<GetElementPtrInst *> ElementPtrs;
    std::vector<LoadInst *> Loads;
};

struct LocalAggregateDeadUses {
    bool IsDead = true;
    std::vector<Instruction *> Writes;
    std::vector<Instruction *> DerivedPointers;
};

// Simplify small, non-escaping local aggregates:
//   1. forward constant elements copied from a constant global into same-block loads;
//   2. expand read-only small constant arrays into select chains for dynamic loads;
//   3. scalarize constant-initialized, constant-indexed aggregate allocas
//      through transparent aggregate/scalar GEPs so Mem2Reg can promote them;
//   4. delete aggregate allocas which are only written and never observed.
class LocalAggregateSimplifyPass
    : public PassDescriptor<LocalAggregateSimplifyPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    static bool isWholeProgramReadOnlyVariable(Variable *Source);
    static bool traceConstantGlobalPointer(
        Value *Pointer, Variable *&Root, std::vector<int> &Path,
        std::shared_ptr<IRType> &PointeeTy);
    static bool foldReadOnlyGlobalLoads(
        Function &F, FunctionAnalysisPassManager &FAM);
    static void deleteDeadPointerChain(Value *Pointer);
    static LocalAggregatePointerPath tracePointer(
        Value *V, std::unordered_set<Value *> &Visiting);
    static LocalAggregatePointerPath tracePointer(Value *V);
    static bool getDirectConstantPath(
        AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
        std::shared_ptr<IRType> &PointeeTy);
    static bool getDirectConstantScalarPath(
        AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
        std::shared_ptr<IRType> &ElementTy);
    static bool appendConstantAggregateGEPPath(
        Value *Base, GetElementPtrInst *GEP,
        const std::vector<int> &BasePath, std::vector<int> &NewPath,
        std::shared_ptr<IRType> &PointeeTy);
    static bool getDirectOneDimElementIndex(
        AllocaInst *AI, GetElementPtrInst *GEP, Value *&Index);
    static bool getConstantOffsetScalarGEP(
        Value *Base, GetElementPtrInst *GEP, int &Offset);
    static bool isProvablyReadOnlySource(Variable *Source);
    static bool isWholeConstantCopy(
        CallInst *CI, AllocaInst *Destination, Variable *Source);
    static bool hasSupportedSameBlockUses(AllocaInst *AI, BasicBlock *BB);
    static bool forwardConstantLoads(AllocaInst *AI);
    static int getOneDimScalarArrayCount(AllocaInst *AI);
    static bool indexTypeCanSelectAll(Value *Index, int Count);
    static bool collectReadOnlyConstantArrayUses(
        Function &F, AllocaInst *AI,
        LocalAggregateReadOnlyConstantArrayPlan &Plan, int Count);
    static Value *buildConstantArrayLookup(
        Variable *Source, int Count, Value *Index,
        Instruction *InsertBefore);
    static bool expandReadOnlyConstantArrayLoads(
        Function &F, AllocaInst *AI);
    static bool addScalarSlot(
        std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
        const std::vector<int> &Path,
        const std::shared_ptr<IRType> &ElementTy,
        int MaxScalarizedElements);
    static bool collectScalarizedPointerUses(
        AllocaInst *AI, Value *Pointer, const std::vector<int> &PointerPath,
        std::vector<GetElementPtrInst *> &TransparentPointers,
        std::map<GetElementPtrInst *, std::vector<int>> &TransparentPaths,
        std::set<GetElementPtrInst *> &SeenTransparent,
        std::vector<Instruction *> &MemoryAccesses,
        std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
        int MaxScalarizedElements);
    static bool scalarizeConstantIndexedAggregate(AllocaInst *AI);
    static LocalAggregateDeadUses collectDeadAggregateUses(AllocaInst *AI);
    static bool deleteDeadAggregate(AllocaInst *AI);
    static std::vector<AllocaInst *> collectArrayAllocas(Function &F);
};
