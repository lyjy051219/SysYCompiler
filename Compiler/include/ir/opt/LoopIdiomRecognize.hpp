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

//LoopIdiomRecognizePass识别连续内存循环并用memset、memcpy或memmove替换
class LoopIdiomRecognizePass
    : public PassDescriptor<LoopIdiomRecognizePass> {
public:
    //遍历函数内循环并提交已证明安全且有收益的内存惯用法替换
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //LinearAccess保存相对循环归纳变量呈单位步长变化的线性GEP访问
    struct LinearAccess {
        //GEP指向被匹配的原始地址计算指令
        GetElementPtrInst *GEP = nullptr;
        //Base记录位于循环外的地址基址
        Value *Base = nullptr;
        //Indices保存GEP的完整索引序列
        std::vector<Value *> Indices;
        //InductionIndex记录归纳变量在索引序列中的位置
        size_t InductionIndex = 0;
        //ElementTy记录每次迭代访问的标量元素类型
        std::shared_ptr<IRType> ElementTy;
    };

    //CopyLoopMatch保存一个简单连续复制循环的完整匹配结果
    struct CopyLoopMatch {
        //L指向被匹配的复制循环
        Loop *L = nullptr;
        //PreHeader指向循环唯一前置块
        BasicBlock *PreHeader = nullptr;
        //Header指向循环唯一头块
        BasicBlock *Header = nullptr;
        //ExitBlock指向循环唯一出口块
        BasicBlock *ExitBlock = nullptr;
        //IndVar指向单位步长规范归纳PHI
        PhiInst *IndVar = nullptr;
        //Store指向循环内唯一目标写入
        StoreInst *Store = nullptr;
        //Load指向循环内唯一源读取
        LoadInst *Load = nullptr;
        //Destination保存目标连续地址模式
        LinearAccess Destination;
        //Source保存源连续地址模式
        LinearAccess Source;
        //RuntimeBound记录运行时循环上界
        Value *RuntimeBound = nullptr;
        //Start记录归纳变量的常量起始值
        int64_t Start = 0;
        //TripCount记录已证明的常量迭代次数
        int64_t TripCount = 0;
        //IsRuntime标记该循环是否使用运行时迭代次数
        bool IsRuntime = false;
    };

    //匹配由单个归纳索引驱动的连续同型GEP访问
    static bool matchLinearAccess(GetElementPtrInst *GEP, PhiInst *IndVar,
                                  Loop *L, LinearAccess &Access);
    //剥离GEP和BitCast并返回可证明独立的全局或栈对象根
    static Value *getDisjointObjectRoot(Value *V);
    //判断两个线性访问是否来自不同且确定不相交的对象
    static bool areProvablyDisjoint(const LinearAccess &A,
                                    const LinearAccess &B);
    //判断常量的全部字节是否相同并返回memset填充值
    static bool getRepeatedByte(Value *V, uint8_t &Byte);
    //证明循环仅含惯用法替换允许的归纳、地址、访存和控制指令
    static bool hasOnlyAllowedLoopInstructions(
        Loop *L, const LoopTripCountInfo &TCI,
        const InductionDescriptor &IV, StoreInst *Store,
        GetElementPtrInst *DestGEP, LoadInst *SourceLoad,
        GetElementPtrInst *SourceGEP, DominatorTree &DT);
    //在preheader中物化指定起始迭代对应的首元素地址
    static GetElementPtrInst *materializeStartPointer(
        const LinearAccess &Access, PhiInst *IndVar, int64_t Start,
        BasicBlock *PreHeader);
    //匹配可参与临时数组双复制消除的简单连续复制循环
    static bool matchSimpleCopyLoop(Loop *L, SCEVResult &SE,
                                    DominatorTree &DT,
                                    CopyLoopMatch &Match);
    //把相邻的源到临时数组和临时数组到目标复制合并为memmove
    static bool transformTemporaryCopyPair(
        Function &F, const CopyLoopMatch &First,
        const CopyLoopMatch &Second, DominatorTree &DT, RangeInfo &RI,
        bool &CFGChanged);
    //识别并把单个连续填充或复制循环替换为memset或memcpy
    static bool transformLoop(Function &F, Loop *L, SCEVResult &SE,
                              DominatorTree &DT, RangeInfo &RI,
                              bool &CFGChanged);
};
