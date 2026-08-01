#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"
#include <vector>

class DominatorTree;
class RangeInfo;

//沿GEP与同形bitcast链证明指针始终位于已知对象边界内
bool pointerIsProvablyWithinKnownObject(
    Value *Pointer, RangeInfo &RI, const DominatorTree &DT);
//证明指定GEP的结果始终位于已知对象边界内
bool gepIsProvablyWithinKnownObject(
    GetElementPtrInst *GEP, RangeInfo &RI, const DominatorTree &DT);

//GepCombineGroup保存除末索引外操作数完全相同的一组GEP
struct GepCombineGroup {
    //Geps按基本块内程序顺序保存同组GEP
    std::vector<GetElementPtrInst *> Geps;
};

//GepCombinePass合并链式GEP、外提PHI上的公共GEP并重基址相邻地址
class GepCombinePass : public PassDescriptor<GepCombinePass> {
public:
    //在函数内执行全部GEP组合并声明受改写影响的分析
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);

private:
    //合并以单用户GEP为基址且索引可安全拼接的GEP链
    static bool combineChainedGEPs(Function &F);
    //将同基址同形incoming GEP从指针PHI外提到汇合块
    static bool foldGEPOfPhi(Function &F);
    //使用范围端点证明i32加法不会发生有符号回绕
    static bool isKnownNoWrapAdd(
        BinaryInst *Inst, BasicBlock *ContextBlock, RangeInfo &RI,
        const DominatorTree &DT);
    //把块内相邻循环GEP规范化为以前一地址为基址的常量步进
    static bool combineLoopGEPs(
        Function &F, RangeInfo &RI, const DominatorTree &DT);
};
