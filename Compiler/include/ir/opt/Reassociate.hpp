#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include "lib/IRUtils.hpp"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

class ReassociateEngine;

//ReassociatePass对整数可重结合表达式执行确定性的代数规范化
class ReassociatePass : public PassDescriptor<ReassociatePass> {
public:
    //构造Pass并选择是否保留参与地址计算的整数表达式
    explicit ReassociatePass(bool PreserveAddressArithmetic = false);
    //在函数内迭代执行重结合直至表达式形状达到不动点
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //允许函数级求解器访问Pass的候选查询辅助接口
    friend class ReassociateEngine;
    //PreserveAddressArithmetic控制是否跳过最终参与GEP地址计算的表达式
    bool PreserveAddressArithmetic = false;
    //判断无用户指令是否是可跳过的无副作用纯计算
    static bool isDeadInstruction(Instruction *I,
                                  FunctionAnalysisPassManager &FAM);
    //判断表达式根是否已按给定操作数次序构成规范链
    static bool isAlreadyBuilt(Instruction *I, BinaryInst::Operation Op,
                               const std::vector<Value *> &Operands);
};

//ReassociateEngine保存函数级排序状态并驱动表达式规范化至不动点
class ReassociateEngine {
public:
    //使用目标函数、分析管理器和地址保护策略构造求解器
    ReassociateEngine(Function &F, FunctionAnalysisPassManager &FAM,
                      bool PreserveAddressArithmetic);
    //扫描函数并反复处理新机会直至没有局部改写
    bool run();

private:
    //F引用当前正在规范化的函数
    Function &F;
    //FAM引用查询副作用等函数分析的管理器
    FunctionAnalysisPassManager &FAM;
    //PreserveAddressArithmetic记录是否保护地址算术表达式
    bool PreserveAddressArithmetic = false;
    //Ranks保存值在规范操作数序列中的语义优先级
    std::unordered_map<Value *, uint32_t> Ranks;
    //StableOrder保存与对象地址无关的确定性平局次序
    std::unordered_map<Value *, uint64_t> StableOrder;
    //NextStableOrder记录下一个稳定次序编号
    uint64_t NextStableOrder = 0;
    //BlockBase保存每个基本块分配的排名区间起点
    std::unordered_map<BasicBlock *, uint32_t> BlockBase;
    //BlockSeq保存每个基本块已分配的局部排名序号
    std::unordered_map<BasicBlock *, uint32_t> BlockSeq;
    //Todo以LIFO顺序保存需要因局部改写而重新检查的指令
    UniqueWorkList<Instruction *, WorkListOrder::LIFO> Todo;

    //初始化参数、基本块和非重结合定义的稳定排名
    void initRankMap();
    //返回值用于规范排序的语义排名
    uint32_t getRank(Value *V) const;
    //按照语义排名和稳定次序比较两个操作数
    bool rankComesBefore(Value *A, Value *B) const;
    //为新建指令登记稳定次序和所属基本块排名
    void stampRank(Instruction *I);
    //把仍存活的指令加入唯一重访队列
    void pushTodo(Instruction *I);
    //消费重访队列并汇总是否产生新的局部改写
    bool processTodo();
    //判断整数表达式是否最终参与GEP地址计算
    bool feedsAddressComputation(Instruction *Root) const;
    //在旧根之前创建二元指令并登记排名与重访状态
    Instruction *createBinaryBefore(Instruction *Pos,
                                    BinaryInst::Operation Op,
                                    Value *LHS, Value *RHS);
    //把单用途乘法子树线性化为乘因子序列
    bool flattenMulFactors(Value *V, std::vector<Value *> &Factors);
    //按给定顺序在原根之前重建乘法链
    bool buildBalancedProduct(Instruction *Root,
                              std::vector<Value *> Terms, Value *&Out);
    //把重复乘因子按指数递归折半并重建乘积
    Value *buildMulPowProduct(
        Instruction *Root,
        const std::vector<std::pair<Value *, int>> &PowList,
        bool &Changed);
    //匹配整数负一与单个值相乘表示的负项
    bool matchMulByMinusOne(Value *V, Value *&X);
    //按给定顺序在原根之前重建加法链
    bool buildAddFromTerms(Instruction *Root,
                           std::vector<Value *> Terms, Value *&Out);
    //在线性操作数序列上执行常量、重复项、公因式和幂化简
    Value *optExp(Instruction *Root, BinaryInst::Operation Op,
                  std::vector<Value *> &Ops, bool &Changed);
    //验证并规范化以指定指令为根的可重结合整数表达式
    bool optimizeInst(Instruction *I);
};
