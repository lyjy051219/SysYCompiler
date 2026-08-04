#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

class BasicAAResult;
class LoopInfo;
struct MemoryLocation;

//DSEBlockColor描述从某一程序点沿基本块扫描目标内存位置的结果。
enum class DSEBlockColor {
    //表示目标写入已被后续完整覆盖。
    Kill,
    //表示扫描遇到可能读取或修改目标位置的指令。
    Clobber,
    //表示扫描区间内没有发生与目标位置相关的内存事件。
    Transparent,
};

//流程：1. MemorySSA 正向证明所有路径覆盖写；2. 重建内存图后删除重复值写入；
//3. 块内逆扫和有界CFG遍历作为保守后备，继续处理不可观察栈对象等特殊情况。
class DSEPass : public PassDescriptor<DSEPass> {
public:
    //依次使用MemorySSA、块内逆扫和CFG路径证明删除函数中的无效store。
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //判断两条store写入值的类型是否代表完全相同的覆盖宽度。
    static bool storesHaveIdenticalCoverage(const StoreInst *First,
                                             const StoreInst *Second);
    //判断store写入范围是否完整覆盖给定指针指向的对象。
    static bool storeFullyCoversPointer(Value *Pointer,
                                        const StoreInst *Store);
    //判断指针是否源自仅被安全地址操作及直接访存使用的局部栈对象。
    static bool isNonEscapingLocalStackPointer(Value *Pointer);
    //判断store是否只是把同一基本块内先前读取的值无变化写回原地址。
    static bool isReloadStoreBackRedundant(
        StoreInst *Store, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //结合别名与调用ModRef信息逆向删除块内被后续写完整覆盖的store。
    static bool eliminateInBlockWithAA(BasicBlock *BB, BasicAAResult &AA,
                                       const LoopInfo &LI,
                                       FunctionAnalysisPassManager &FAM);
    //保留无别名信息块内扫描接口的兼容占位实现。
    static bool eliminateInBlock(BasicBlock *BB);
    //顺序分类从指定指令到块尾对目标内存位置的首次相关事件。
    static DSEBlockColor classifyFrom(
        Instruction *First, const MemoryLocation &Location, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);
    //证明一条store在所有后继路径上均于首次观察前被完整覆盖。
    static bool isStoreDead(StoreInst *Store, BasicAAResult &AA,
                            const LoopInfo &LI,
                            FunctionAnalysisPassManager &FAM);
};
