#include "ir/opt/GepOpt.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include <vector>
//规范化函数内退化、未定义基址和多非零索引GEP
bool GepEvaluatePass::evaluateGEP(Function &F) {
    //1.对 GEP 做快照，后续可以安全删除或在其前方插入新指令。
    bool changed = false;
    auto geps = collectInstructions<GetElementPtrInst>(F);
    for (GetElementPtrInst *GEP : geps) {
        Value *Base = GEP->getBasePointer();
        if (!Base) continue;
        size_t numIndices = GEP->getNumIndices();
        //2.空索引不改变地址，先把全部use改接同类型基址，再删除旧GEP。
        if (numIndices == 0) {
            GEP->replaceAllUsesWith(Base);
            delete GEP;
            changed = true;
            continue;
        }
        //3.undef基址无法产生定义良好的地址，先传播同类型undef到全部use，
        //再删除旧GEP，避免留下引用已释放定义的Use。
        if (Base->isUndef()) {
            GEP->replaceAllUsesWith(UndefValue::NewUndefValue(GEP->getIRType()));
            delete GEP;
            changed = true;
            continue;
        }
        //4.对含多个非零索引的复杂地址，在第二个非零索引处分段：前段计算
        //中间基址，后段以规范零索引重新进入该对象。索引只按原顺序重新分组，
        //不执行索引算术，因而不会引入新的整数溢出。
        {
            int nonZeroCount = 0;
            size_t splitPoint = 0;
            for (size_t i = 0; i < numIndices; i++) {
                Value *Index = GEP->getIndex(i);
                if (i == 0 && Index && Index->isConstIntZero()) continue;
                if (!Index || !Index->isConstIntZero()) {
                    nonZeroCount++;
                    if (nonZeroCount == 2) {
                        splitPoint = i + 1;
                        break;
                    }
                }
            }
            if (splitPoint > 0) {
                BasicBlock *BB = GEP->getParent();
                if (!BB) continue;
                std::vector<Value *> frontIndices;
                for (size_t i = 0; i + 1 < splitPoint; i++)
                    frontIndices.push_back(GEP->getIndex(i));
                //4.1先插入前段并按布局更新中间指针类型，再用前段作基址原位reset
                //旧GEP；reset保留旧结果定义并改写其操作数，因此原有用户无需迁移。
                auto *frontGEP = new GetElementPtrInst(Base, frontIndices);
                frontGEP->updateType();
                GEP->insertBefore(frontGEP);
                std::vector<Value *> backIndices;
                backIndices.push_back(ConstInt::newConstInt(0));
                for (size_t i = splitPoint - 1; i < numIndices; i++)
                    backIndices.push_back(GEP->getIndex(i));
                GEP->reset(frontGEP, backIndices);
                changed = true;
            }
        }
    }
    return changed;
}
//运行GEP求值规范化并声明CFG结构分析仍然有效
PreservationStatus GepEvaluatePass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    //1—4. 完成退化折叠、undef 传播和复杂 GEP 拆分。
    bool changed = evaluateGEP(F);
    if (!changed) return PreservationStatus::all();
    //5.仅改变地址指令与use关系而不改变CFG，支配树和循环树仍然有效；
    //其余依赖具体值表达式的分析不显式保留。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
