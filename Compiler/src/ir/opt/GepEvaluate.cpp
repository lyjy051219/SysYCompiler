#include "ir/opt/GepEvaluate.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"
#include <vector>
bool GepEvaluatePass::evaluateGEP(Function &F) {
    //1.收集所有GEP
    bool changed = false;
    auto geps = collectInstructions<GetElementPtrInst>(F);
    for (GetElementPtrInst *GEP : geps) {
        Value *Base = GEP->getBasePointer();
        if (!Base) continue;
        //2.获得索引数量，如[base, 0, i, j]，索引数量为3
        size_t numIndices = GEP->getNumIndices();
        //3.如果没有索引，直接用基址替换GEP，并删除GEP
        if (numIndices == 0) {
            GEP->replaceAllUsesWith(Base);
            delete GEP;
            changed = true;
            continue;
        }
        //3.如果基址是undef，直接用undef替换GEP，并删除GEP
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
            //找到第二个非零索引
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
                //收集前段索引并构建新GEP
                    frontIndices.push_back(GEP->getIndex(i));
                auto *frontGEP = new GetElementPtrInst(Base, frontIndices);
                frontGEP->updateType();
                GEP->insertBefore(frontGEP);
                std::vector<Value *> backIndices;
                //构建后段索引替换原来旧索引
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
PreservationStatus GepEvaluatePass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    bool changed = evaluateGEP(F);
    if (!changed) return PreservationStatus::all();
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
