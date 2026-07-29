#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>
class BasicAAResult;
struct MemoryLocation;
class LoopInfo;
class Loop;

//MemorySSA：将内存状态抽象成SSA风格定义链，标记每一处内存写入点和内存读取点，构建内存图。

//MemoryAccess：内存访问节点
class MemoryAccess {
public:
    //MemoryAccess四种状态/节点类型
    enum class Kind { LiveOnEntry, Phi, Use, Def };
    /*
    LiveOnEntry：函数入口时未知的初始内存状态，无对应IR，无任何对应，每个函数只创建一个
    Phi：多个CFG路径上的内存状态需要汇合，只出现在迭代支配边界基本块入口
    Use：读取内存但不产生新内存版本,Load/只读Call
    Def：可能修改内存，产生新内存版本，Store/可能写内存的Call
     */

    //一些基本的接口
    Kind getKind() const;
    bool isLiveOnEntry() const;
    bool isPhi() const;
    bool isUse() const;
    bool isDef() const;
    unsigned getID() const;
    BasicBlock *getBlock() const;
    Instruction *getInstruction() const;
    MemoryAccess *getDefiningAccess() const;
    const std::vector<std::pair<BasicBlock *, MemoryAccess *>> &
    getIncomingAccesses() const;
    //返回直接消费该内存状态的Use/Def/Phi；同一个Phi即使有多条同源边也只记录一次；
    //结果只在当前MemorySSA快照未因IR改写失效时可用
    const std::vector<MemoryAccess *> &getUsers() const;

private:
    friend class MemorySSAResult;
    friend class MemorySSAAnalysis;
    MemoryAccess(Kind K, unsigned AccessID, BasicBlock *BB = nullptr,
                 Instruction *I = nullptr);

    //MemoryAccess的节点类型
    Kind AccessKind;
    //节点编号
    unsigned ID = 0;
    //所在基本块
    BasicBlock *Block = nullptr;
    //对应的IR指令
    Instruction *Inst = nullptr;
    //该节点之前的内存状态
    MemoryAccess *DefiningAccess = nullptr;
    //Phi节点主要使用：(PredBB1, PredBB1ExitMemory)记录每个可达前驱的出口内存状态；
    //入口块有回边时用(nullptr, LiveOnEntry)表示函数调用前的虚拟入口边
    std::vector<std::pair<BasicBlock *, MemoryAccess *>> IncomingAccesses;
    //反向使用边供正向MemorySSA walker使用
    std::vector<MemoryAccess *> Users;
};

//最近写查询的完整结果；Clobbers保存所有路径上的首个潜在写，
//NearestPhi保存反向遍历实际遇到的最近汇合点，Complete为false表示查询不可信
struct MemoryClobberResult {
    std::vector<MemoryAccess *> Clobbers;
    MemoryAccess *NearestPhi = nullptr;
    bool Complete = false;
};

//MemorySSAResult：内存访问图
class MemorySSAResult {
public:
    //一些基本的接口
    MemorySSAResult();
    MemorySSAResult(MemorySSAResult &&) noexcept;
    MemorySSAResult &operator=(MemorySSAResult &&) noexcept;
    MemorySSAResult(const MemorySSAResult &) = delete;
    MemorySSAResult &operator=(const MemorySSAResult &) = delete;
    MemoryAccess *getLiveOnEntry() const;
    MemoryAccess *getMemoryAccess(Instruction *I) const;
    //返回进入该块时实际可见的内存状态，可能是LiveOnEntry、支配块的Def或本块Phi
    MemoryAccess *getBlockEntry(BasicBlock *BB) const;
    //可达块返回实际出口状态；不可达块仅返回最后Def或LiveOnEntry结构摘要
    MemoryAccess *getBlockExit(BasicBlock *BB) const;
    //按IR顺序返回该基本块中的MemoryUse/MemoryDef，不包含块入口MemoryPhi
    const std::vector<MemoryAccess *> &
    getBlockAccesses(BasicBlock *BB) const;

    //查询接口1：从后向前寻找最后一条可能改变这个位置的内存访问节点
    //四个重载
    //1.Instrution+Value版本
    MemoryAccess *getClobberingAccess(
        Instruction *At, Value *Ptr, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;
    //2.Instruction+MemoryLocation版本
    MemoryAccess *getClobberingAccess(
        Instruction *At, const MemoryLocation &Location,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;
    //3.MemoryAccess+Value版本
    MemoryAccess *getClobberingAccess(
        MemoryAccess *Start, Value *Ptr, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;
    //4.MemoryAccess+MemoryLocation版本（核心）
    MemoryAccess *getClobberingAccess(
        MemoryAccess *Start, const MemoryLocation &Location,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;

    //查询接口2：返回所有路径上首个潜在写；Instruction版本从该指令之前开始查询
    MemoryClobberResult getClobberingAccesses(
        Instruction *At, const MemoryLocation &Location,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;
    MemoryClobberResult getClobberingAccesses(
        MemoryAccess *Start, const MemoryLocation &Location,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;

    //查询接口3：从ToState反向遍历到From执行后的内存边界；ToState若为Def也要
    //检查该定义。任一路径无法到达边界、遇到环或预算耗尽时保守返回true
    bool hasClobberBetween(
        Instruction *From, MemoryAccess *ToState,
        const MemoryLocation &Location, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;

    //查询接口4：检查循环中除Ignore外是否存在对Location的写，CheckReads为true
    //时也检查读；预算耗尽保守视作存在干扰
    bool hasInterferingAccessInLoop(
        const MemoryLocation &Location, const Loop &L,
        Instruction *Ignore, bool CheckReads, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 512) const;

    //查询接口5：通过MemorySSA图分析StoreInst是否冗余写入，如果是，则可以通过DSE删除
    bool isStoreValueRedundant(
        StoreInst *Store, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 256) const;

    //查询接口6：沿CFG证明候选Store的每条后继路径都在首次读取前被完整覆盖
    bool isStoreOverwrittenOnAllPaths(
        StoreInst *Store, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM,
        std::size_t MaxVisitedAccesses = 512) const;

    bool invalidate(Function &F, const PreservationStatus &PA);
    //返回该函数整张MemorySSA图的节点数
    std::size_t size() const;

private:
    friend class MemorySSAAnalysis;
    //创建一个新的MemoryAccess节点，并加入到MemorySSA图中
    MemoryAccess *createAccess(MemoryAccess::Kind K, BasicBlock *BB = nullptr,
                               Instruction *I = nullptr);
    //建立DefiningAccess/Phi incoming时同步维护反向使用边
    static void addUser(MemoryAccess *Definition, MemoryAccess *User);
    //查询接口1的辅助函数，详情看实现
    bool mayClobber(MemoryAccess *Access, const MemoryLocation &Location,
                    BasicAAResult &AA,
                    const LoopInfo &LI,
                    FunctionAnalysisPassManager &FAM) const;

    //整张MemorySSA图上所有节点
    std::vector<std::unique_ptr<MemoryAccess>> OwnedAccesses;
    //指向唯一的函数入口内存状态节点
    MemoryAccess *LiveOnEntry = nullptr;
    //IR指令->内存状态节点
    std::unordered_map<Instruction *, MemoryAccess *> InstructionAccesses;
    //基本块->进入该块时实际可见的内存状态；仅当节点Block等于该块时才是本块MemoryPhi
    std::unordered_map<BasicBlock *, MemoryAccess *> BlockEntries;
    //基本块->块出口最后一个内存状态
    std::unordered_map<BasicBlock *, MemoryAccess *> BlockExits;
    //基本块->块内按IR顺序排列的MemoryUse/MemoryDef
    std::unordered_map<BasicBlock *, std::vector<MemoryAccess *>>
        BlockAccesses;
};

//外层包装类
class MemorySSAAnalysis : public AnalysisDescriptor<MemorySSAAnalysis> {
public:
    using Result = MemorySSAResult;
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;
private:
    friend AnalysisDescriptor<MemorySSAAnalysis>;
    static PassID Key;
};
