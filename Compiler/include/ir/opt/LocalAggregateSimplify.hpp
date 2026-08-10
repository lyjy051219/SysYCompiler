#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>
#include <vector>

//BasicAAResult提供局部聚合指针的别名与底层对象信息
class BasicAAResult;
//DominatorTree提供跨块重写所需的支配关系
class DominatorTree;
//LoopInfo提供循环归属和循环不变量判断
class LoopInfo;
//MemorySSAResult提供局部聚合读写的内存版本证明
class MemorySSAResult;
//RangeInfo提供动态数组下标的完整定义域证明
class RangeInfo;

//LocalAggregatePointerPath描述指针相对局部alloca的常量聚合路径
struct LocalAggregatePointerPath {
    //Root指向追溯得到的局部聚合alloca
    AllocaInst *Root = nullptr;
    //Indices按照嵌套顺序保存常量聚合下标
    std::vector<int> Indices;
    //IsConstant标记整条指针路径是否均由常量下标构成
    bool IsConstant = true;
};

//LocalAggregateReadOnlyConstantArrayPlan保存只读常量数组展开计划
struct LocalAggregateReadOnlyConstantArrayPlan {
    //Source指向memcpy初始化使用的只读全局常量源
    Variable *Source = nullptr;
    //InitCopy指向初始化局部数组的整表memcpy调用
    CallInst *InitCopy = nullptr;
    //ElementPtrs保存计划中允许删除的元素GEP指令
    std::vector<GetElementPtrInst *> ElementPtrs;
    //Loads保存计划中需要替换的数组读取指令
    std::vector<LoadInst *> Loads;
};

//LocalAggregateDeadUses汇总不可观察聚合的写入和派生指针
struct LocalAggregateDeadUses {
    //IsDead标记聚合地址是否未逃逸且内容从未被观察
    bool IsDead = true;
    //Writes保存确认不可观察后可以删除的写指令
    std::vector<Instruction *> Writes;
    //DerivedPointers保存确认无用后可以删除的派生指针指令
    std::vector<Instruction *> DerivedPointers;
};

//LocalAggregateSynthExpressionKind枚举常量表综合表达式的节点种类
enum class LocalAggregateSynthExpressionKind {
    //Index表示动态数组下标叶节点
    Index,
    //Constant表示i32常量叶节点
    Constant,
    //Binary表示由两个子表达式组成的二元运算节点
    Binary
};

//LocalAggregateSynthExpression描述与源码命名无关的整数表达式语法树
struct LocalAggregateSynthExpression {
    //Kind记录当前综合表达式节点的种类
    LocalAggregateSynthExpressionKind Kind =
        LocalAggregateSynthExpressionKind::Index;
    //Op记录Binary节点使用的二元运算
    BinaryInst::Operation Op = BinaryInst::ADD;
    //Constant记录Constant节点保存的i32值
    int32_t Constant = 0;
    //Cost记录物化当前表达式所需的二元IR指令数
    int Cost = 0;
    //Left指向Binary节点的左子表达式
    std::shared_ptr<LocalAggregateSynthExpression> Left;
    //Right指向Binary节点的右子表达式
    std::shared_ptr<LocalAggregateSynthExpression> Right;
};

//LocalAggregateSimplifyPass分阶段化简局部小聚合与只读常量表
class LocalAggregateSimplifyPass
    : public PassDescriptor<LocalAggregateSimplifyPass> {
public:
    //运行局部聚合化简并返回分析保留状态
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //MaxSynthArrayElements限制允许综合的常量表元素数量
    static constexpr int MaxSynthArrayElements = 32;
    //MaxFastSynthArrayElements限制线性扫描可验证的大型简单整数表
    static constexpr int MaxFastSynthArrayElements = 256;
    //MaxSynthConstants限制综合搜索使用的不同常量数量
    static constexpr int MaxSynthConstants = 12;
    //MaxSynthExpressionCost限制候选表达式的二元指令成本
    static constexpr int MaxSynthExpressionCost = 3;
    //MaxSynthStatesPerCost限制每个成本层保留的综合状态数量
    static constexpr int MaxSynthStatesPerCost = 128;
    //MaxSynthPendingStates限制综合搜索等待处理的状态总数
    static constexpr int MaxSynthPendingStates = 4096;
    //GenericScalarLoadCost记录一次普通标量load的保守目标成本
    static constexpr int GenericScalarLoadCost = 4;

    //判断全局变量的全部指针用途是否只读且未逃逸
    static bool isWholeProgramReadOnlyVariable(Variable *Source);
    //沿全局指针的GEP和bitcast闭包解析常量聚合路径
    static bool traceConstantGlobalPointer(
        Value *Pointer, Variable *&Root, std::vector<int> &Path,
        std::shared_ptr<IRType> &PointeeTy);
    //收集给定一维全局常量数组的全部i32元素值
    static bool collectInt32ConstantTable(
        Variable *Source, int Count, std::vector<int32_t> &Values);
    //在给定数组下标上求值综合表达式并检查i32语义
    static bool evaluateSynthExpression(
        const std::shared_ptr<LocalAggregateSynthExpression> &Expression,
        int32_t Index, int32_t &Result);
    //线性扫描完整定义域并识别索引、常量和一次加减表
    static std::shared_ptr<LocalAggregateSynthExpression>
    synthesizeFastInt32TableExpression(
        const std::vector<int32_t> &Values);
    //搜索覆盖完整常量表定义域的低成本整数表达式
    static std::shared_ptr<LocalAggregateSynthExpression>
    synthesizeInt32TableExpression(const std::vector<int32_t> &Values);
    //在指定指令前物化综合表达式并记录新建指令
    static Value *materializeSynthExpression(
        const std::shared_ptr<LocalAggregateSynthExpression> &Expression,
        Value *Index, Instruction *InsertBefore,
        std::vector<Instruction *> &Created);
    //判断综合表达式是否严格低于普通标量load的保守成本
    static bool isSynthExpressionProfitable(
        const std::shared_ptr<LocalAggregateSynthExpression> &Expression);
    //折叠只读全局常量load并综合可证明等价的动态查表
    static bool foldReadOnlyGlobalLoads(
        Function &F, FunctionAnalysisPassManager &FAM);
    //从叶节点向上删除已经没有用户的透明指针链
    static void deleteDeadPointerChain(Value *Pointer);
    //递归追溯局部指针到根alloca并记录常量聚合路径
    static LocalAggregatePointerPath tracePointer(
        Value *V, std::unordered_set<Value *> &Visiting);
    //追溯局部指针到根alloca并处理递归访问集合
    static LocalAggregatePointerPath tracePointer(Value *V);
    //解析直接基于alloca的常量GEP路径和结果元素类型
    static bool getDirectConstantPath(
        AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
        std::shared_ptr<IRType> &PointeeTy);
    //解析直接基于alloca且最终指向标量的常量GEP路径
    static bool getDirectConstantScalarPath(
        AllocaInst *AI, GetElementPtrInst *GEP, std::vector<int> &Path,
        std::shared_ptr<IRType> &ElementTy);
    //把派生GEP的常量下标追加到已有聚合路径
    static bool appendConstantAggregateGEPPath(
        Value *Base, GetElementPtrInst *GEP,
        const std::vector<int> &BasePath, std::vector<int> &NewPath,
        std::shared_ptr<IRType> &PointeeTy);
    //提取直接一维数组元素GEP使用的动态或常量下标
    static bool getDirectOneDimElementIndex(
        AllocaInst *AI, GetElementPtrInst *GEP, Value *&Index);
    //提取相对给定基指针的常量标量GEP偏移
    static bool getConstantOffsetScalarGEP(
        Value *Base, GetElementPtrInst *GEP, int &Offset);
    //判断全局变量是否可作为已证明只读的memcpy源
    static bool isProvablyReadOnlySource(Variable *Source);
    //判断调用是否把同形只读常量对象完整复制到目标alloca
    static bool isWholeConstantCopy(
        CallInst *CI, AllocaInst *Destination, Variable *Source);
    //判断alloca在指定基本块内是否只有受支持的常量覆盖和读取
    static bool hasSupportedSameBlockUses(AllocaInst *AI, BasicBlock *BB);
    //在单块内转发整表常量复制及常量覆盖后的load
    static bool forwardConstantLoads(AllocaInst *AI);
    //返回一维标量数组的元素数量，形状不支持时返回负值
    static int getOneDimScalarArrayCount(AllocaInst *AI);
    //判断下标类型是否能够表示数组全部合法元素编号
    static bool indexTypeCanSelectAll(Value *Index, int Count);
    //收集跨块展开只读常量数组所允许的完整use闭包
    static bool collectReadOnlyConstantArrayUses(
        Function &F, AllocaInst *AI,
        LocalAggregateReadOnlyConstantArrayPlan &Plan, int Count,
        const DominatorTree &DT, const MemorySSAResult &MSSA,
        BasicAAResult &AA, const LoopInfo &LI,
        FunctionAnalysisPassManager &FAM);
    //使用综合表达式或select链构造常量数组查表结果
    static Value *buildConstantArrayLookup(
        Variable *Source, int Count, Value *Index,
        Instruction *InsertBefore,
        std::vector<Instruction *> &Created);
    //经支配和MemorySSA证明后展开只读常量数组的全部load
    static bool expandReadOnlyConstantArrayLoads(
        Function &F, AllocaInst *AI, const DominatorTree &DT,
        const MemorySSAResult &MSSA, BasicAAResult &AA,
        const LoopInfo &LI, FunctionAnalysisPassManager &FAM);
    //判断调用是否为已注册且明确非volatile的memset语义
    static bool isNonVolatileMemset(CallInst *CI);
    //判断memset是否以零完整覆盖给定局部聚合对象
    static bool isWholeZeroMemset(CallInst *CI, AllocaInst *AI);
    //折叠“完整清零+常量下标补丁”构造的有界局部动态查表
    static bool foldPatchedLocalArrayLoads(
        AllocaInst *AI, const DominatorTree &DT,
        RangeInfo &RI, const LoopInfo &LI);
    //为常量聚合路径登记标量槽类型并检查类型一致性和数量预算
    static bool addScalarSlot(
        std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
        const std::vector<int> &Path,
        const std::shared_ptr<IRType> &ElementTy,
        int MaxScalarizedElements);
    //递归收集标量化允许的透明指针和常量路径内存访问
    static bool collectScalarizedPointerUses(
        AllocaInst *AI, Value *Pointer, const std::vector<int> &PointerPath,
        std::vector<GetElementPtrInst *> &TransparentPointers,
        std::map<GetElementPtrInst *, std::vector<int>> &TransparentPaths,
        std::set<GetElementPtrInst *> &SeenTransparent,
        std::vector<Instruction *> &MemoryAccesses,
        std::map<std::vector<int>, std::shared_ptr<IRType>> &SlotTypes,
        int MaxScalarizedElements);
    //把仅按常量路径访问的局部聚合拆分为独立标量槽
    static bool scalarizeConstantIndexedAggregate(AllocaInst *AI);
    //收集地址未逃逸且内容从未被读取的聚合写入和派生指针
    static LocalAggregateDeadUses collectDeadAggregateUses(AllocaInst *AI);
    //删除已证明不可观察的聚合及其写入和派生指针
    static bool deleteDeadAggregate(AllocaInst *AI);
    //按照函数顺序收集数组类型的局部alloca
    static std::vector<AllocaInst *> collectArrayAllocas(Function &F);
};
