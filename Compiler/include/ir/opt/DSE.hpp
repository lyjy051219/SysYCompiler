#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

class BasicAAResult;
class LoopInfo;
struct MemoryLocation;

enum class DSEBlockColor {
    // 扫描路径已被完整覆盖、遇到可能观察/修改、或尚未发生相关事件。
    Kill,
    Clobber,
    Transparent,
};

 
// 流程：1. MemorySSA 删除重复值写入；2. 块内逆扫删除被后继 store 覆盖的写；
// 3. 对剩余 store 有界遍历全部后继路径；4. 仅当每条路径观察前均被 Kill 才删除。
class DSEPass : public PassDescriptor<DSEPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
     
    static bool storesHaveIdenticalCoverage(const StoreInst *First,
                                             const StoreInst *Second);
    static bool storeFullyCoversPointer(Value *Pointer,
                                        const StoreInst *Store);
    static bool isNonEscapingLocalStackPointer(Value *Pointer);
    static bool eliminateInBlockWithAA(BasicBlock *BB, BasicAAResult &AA,
                                       const LoopInfo &LI);
    static bool eliminateInBlock(BasicBlock *BB);
    static DSEBlockColor classifyFrom(
        Instruction *First, const MemoryLocation &Location, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);
    static bool isStoreDead(StoreInst *Store, BasicAAResult &AA,
                            const LoopInfo &LI,
                            FunctionAnalysisPassManager &FAM);
};
