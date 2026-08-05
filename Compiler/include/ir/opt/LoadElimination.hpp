#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

//BasicAAResult提供基于内存位置与索引范围的别名查询
class BasicAAResult;
//DominatorTree提供基本块和值的支配关系查询
class DominatorTree;
//LoopInfo提供循环归属与循环不变量信息
class LoopInfo;
//MemoryAccess表示MemorySSA中的访问节点
class MemoryAccess;
//MemoryLocation描述一次内存访问的指针、大小与覆盖范围
struct MemoryLocation;

//LoadElimQueryResult表示前驱数据流查询得到的可用值状态
struct LoadElimQueryResult {
    //Kind枚举可用值查询的五种结果
    enum Kind {
        //KNOWN表示已找到可直接复用的确定值
        KNOWN,
        //PHI_NEEDED表示各前驱值均已知但需要合成PHI
        PHI_NEEDED,
        //CLOBBERED表示某条路径上的相关写入破坏了可用值
        CLOBBERED,
        //UNKNOWN表示预算、循环或未知语义使证明无法完成
        UNKNOWN,
        //NEED_PREDECESSORS表示当前块内无定义，需要继续查询前驱
        NEED_PREDECESSORS
    };

    //kind记录本次查询的结果类别
    Kind kind;
    //value在kind为KNOWN时保存可复用值
    Value *value = nullptr;
    //predMap在kind为PHI_NEEDED时保存每条入边对应的值
    std::vector<std::pair<BasicBlock*, Value*>> predMap;

    //构造已找到确定值的查询结果
    static LoadElimQueryResult known(Value *V);
    //构造已确认被相关写入破坏的查询结果
    static LoadElimQueryResult clobbered();
    //构造因证明不足而未知的查询结果
    static LoadElimQueryResult unknown();
    //构造需要继续查询前驱的查询结果
    static LoadElimQueryResult needPredecessors();
    //构造需要按前驱值合成PHI的查询结果
    static LoadElimQueryResult phiNeeded(
        std::vector<std::pair<BasicBlock *, Value *>> M);
};

//LoadEliminationPass利用MemorySSA、别名与支配证明消除可转发的冗余load
class LoadEliminationPass : public PassDescriptor<LoadEliminationPass> {
public:
    //在函数内执行load值转发并声明受改写影响的分析
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //MSSALoadRecord保存load及其完整MemorySSA clobber签名
    struct MSSALoadRecord {
        //Load指向待分析的load指令
        LoadInst *Load = nullptr;
        //Block记录Load所属的基本块
        BasicBlock *Block = nullptr;
        //Clobbers保存各可达路径上的最近MemoryDef
        std::vector<MemoryAccess *> Clobbers;
    };

    //PathScanState描述跨基本块反向扫描的一个工作项
    struct PathScanState {
        //BB指向当前待扫描的基本块
        BasicBlock *BB = nullptr;
        //TargetPrefixOnly表示目标块只扫描目标load之前的前缀
        bool TargetPrefixOnly = false;
    };

    //MaxQueryDepth限制递归前驱数据流的最大深度
    static constexpr int MaxQueryDepth = 8;
    //MaxDominatorSearchDepth限制沿支配链寻找候选load的深度
    static constexpr int MaxDominatorSearchDepth = 16;
    //MaxPathScanBlocks限制一次全路径证明扫描的基本块数
    static constexpr int MaxPathScanBlocks = 64;
    //MaxPathScanInstructions限制一次全路径证明扫描的指令数
    static constexpr int MaxPathScanInstructions = 2048;
    //MaxDominatingQueriesPerFunction限制每个函数的跨回边支配查询次数
    static constexpr int MaxDominatingQueriesPerFunction = 64;
    //MaxMSSALoadComparisons限制每条load比较相同clobber签名候选的次数
    static constexpr std::size_t MaxMSSALoadComparisons = 128;
    //判断候选值的类型形状是否与目标load结果兼容
    static bool isTypeCompatible(Value *Source, LoadInst *TargetLoad);
    //判断指令是否可能修改给定内存位置
    static bool mayModifyPointer(
        const MemoryLocation &Location, Instruction *I, BasicAAResult &AA,
        const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //证明候选load到目标load的所有CFG路径均未修改目标位置
    static bool isUnmodifiedOnAllPaths(
        LoadInst *Candidate, LoadInst *Target, Value *P,
        DominatorTree &DT, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //沿支配链寻找经全路径无写证明的可复用load
    static LoadInst *findAvailableDominatingLoad(
        LoadInst *Target, BasicBlock *BB, DominatorTree &DT,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //从指定位置向块首扫描最近的MustAlias值或相关写入
    static LoadElimQueryResult scanBlockBackward(
        BasicBlock *BB, Value *P, Instruction *StopBefore,
        LoadInst *TargetLoad, BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //递归汇总基本块全部前驱上的可用值
    static LoadElimQueryResult queryAvailableValue(
        Value *P, BasicBlock *BB, LoadInst *TargetLoad, int Depth,
        std::unordered_set<BasicBlock *> &OnStack,
        std::unordered_map<BasicBlock *, LoadElimQueryResult> &Memo,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //为单条load选择块内、支配链或前驱PHI替换方案
    static bool processLoad(
        LoadInst *Load, BasicBlock *BB, DominatorTree &DT,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM, int &DominatingQueryBudget);
    //按支配树前序处理MemorySSA快速层未消除的load
    static bool processFunction(
        Function &F, DominatorTree &DT, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);
};
