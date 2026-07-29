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

// 流程：1. MemorySSA 正向证明所有路径覆盖写；2. 重建内存图后删除重复值写入；
// 3. 块内逆扫和有界CFG遍历作为保守后备，继续处理不可观察栈对象等特殊情况。
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
                                       const LoopInfo &LI,
                                       FunctionAnalysisPassManager &FAM);
    static bool eliminateInBlock(BasicBlock *BB);
    static DSEBlockColor classifyFrom(
        Instruction *First, const MemoryLocation &Location, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);
    static bool isStoreDead(StoreInst *Store, BasicAAResult &AA,
                            const LoopInfo &LI,
                            FunctionAnalysisPassManager &FAM);
};
