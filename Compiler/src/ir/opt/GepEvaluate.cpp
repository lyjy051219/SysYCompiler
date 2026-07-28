#include "ir/opt/GepOpt.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"

#include <vector>

 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 
 

bool GepEvaluatePass::evaluateGEP(Function &F) {
    // 1. 对 GEP 做快照，后续可以安全删除或在其前方插入新指令。
    bool changed = false;

    auto geps = collectInstructions<GetElementPtrInst>(F);

    for (GetElementPtrInst *GEP : geps) {
        Value *Base = GEP->getBasePointer();
        if (!Base) continue;
        size_t numIndices = GEP->getNumIndices();

         
        // 2. 空索引不改变地址，直接退化为基址。
        if (numIndices == 0) {
            GEP->replaceAllUsesWith(Base);
            delete GEP;
            changed = true;
            continue;
        }

         
        // 3. undef 基址无法产生定义良好的地址，将结果统一传播为同类型 undef。
        if (Base->isUndef()) {
            GEP->replaceAllUsesWith(UndefValue::NewUndefValue(GEP->getIRType()));
            delete GEP;
            changed = true;
            continue;
        }

         
         
        // 4. 对含多个非零索引的复杂地址，在第二个非零索引处分段：前段先计算
        // 中间基址，后段从零索引重新进入对象，限制单条 GEP 的寻址复杂度。
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

                // 4.1 先插入前段并更新类型，再原位重置旧 GEP；旧 GEP 的用户无需改写。
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

PreservationStatus GepEvaluatePass::run(Function &F, FunctionAnalysisPassManager &FAM) {
    // 1—4. 完成退化折叠、undef 传播和复杂 GEP 拆分。
    bool changed = evaluateGEP(F);
    if (!changed) return PreservationStatus::all();
    // 5. 指令形态变化但 CFG 不变，支配树和循环树仍然有效。
    PreservationStatus PA;
    PA.keep<DominantAnalysis>();
    PA.keep<LoopAnalysis>();
    return PA;
}
