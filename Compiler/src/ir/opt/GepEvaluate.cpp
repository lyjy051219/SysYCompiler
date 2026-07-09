#include "ir/opt/GepOpt.hpp"
#include "ir/analysis/Dominant.hpp"
#include "ir/analysis/LoopInfo.hpp"

#include <vector>

// =====================================================================
// GepEvaluate — 单条 GEP 指令化简
//
// 当前 IR 使用 typed pointer。GEP 的零索引不是 no-op，它会改变结果指针类型。
// 因此全零消除和尾零裁剪在 typed pointer IR 下不安全，不做。
//
// 保留的规则：
//
// 首先处理无索引的退化 GEP。gep(p) 没有任何索引时结果等于 p 本身，
// 类型不变，可以安全替换。前端正常不会生成这种，但做防御性处理。
//
// 然后处理 undef 基址传播。gep(undef, ...) 的结果无论偏移多少仍然是 undef，
// 直接替换为结果类型的 undef 值。
//
// 最后做多非零索引拆分。如果一条 GEP 有超过 1 个非零索引（首部零索引不算），
// 在第二个非零索引处拆分成两条串联的 GEP，每条只有一个非零索引。
// 这样更利于别名分析区分不同数组元素的访问，也更利于后端指令选择。
// 拆分不改变地址计算语义：两个 GEP 串联的偏移等价于原始单个 GEP。
// =====================================================================

static bool isConstZero(Value *V) {
    return V && V->isConstIntZero();
}

static bool evaluateGEP(Function &F) {
    bool changed = false;

    std::vector<GetElementPtrInst *> geps;
    for (BasicBlock *BB : F) {
        for (Instruction *I : *BB) {
            if (auto *GEP = dynamic_cast<GetElementPtrInst *>(I))
                geps.push_back(GEP);
        }
    }

    for (GetElementPtrInst *GEP : geps) {
        auto &UL = GEP->getUserList();
        if (UL.empty()) continue;
        Value *Base = UL[0]->getValue();
        size_t numIndices = UL.size() - 1;

        // 无索引的退化 GEP：gep(p) 没有索引，结果就是 p，类型不变
        if (numIndices == 0) {
            GEP->replaceAllUsesWith(Base);
            delete GEP;
            changed = true;
            continue;
        }

        // undef 基址传播：gep(undef, ...) → undef
        if (dynamic_cast<UndefValue *>(Base)) {
            GEP->replaceAllUsesWith(UndefValue::NewUndefValue(GEP->getIRType()));
            delete GEP;
            changed = true;
            continue;
        }

        // 多非零索引拆分：有 >= 2 个非零索引时，在第二个非零索引处拆分。
        // 前半 GEP 保留到第一个非零索引，后半 GEP 从前半结果出发、首索引补 0。
        {
            int nonZeroCount = 0;
            size_t splitPoint = 0;
            for (size_t i = 1; i < UL.size(); i++) {
                if (i == 1 && isConstZero(UL[i]->getValue())) continue;
                if (!isConstZero(UL[i]->getValue())) {
                    nonZeroCount++;
                    if (nonZeroCount == 2) {
                        splitPoint = i;
                        break;
                    }
                }
            }

            if (splitPoint > 0) {
                BasicBlock *BB = GEP->ChainNode<BasicBlock, Instruction>::getParent();
                if (!BB) continue;

                std::vector<Value *> frontIndices;
                for (size_t i = 1; i < splitPoint; i++)
                    frontIndices.push_back(UL[i]->getValue());

                auto *frontGEP = new GetElementPtrInst(Base, frontIndices);
                frontGEP->updateType();

                GEP->insertBefore(frontGEP);

                std::vector<Value *> backIndices;
                backIndices.push_back(ConstInt::newConstInt(0));
                for (size_t i = splitPoint; i < UL.size(); i++)
                    backIndices.push_back(UL[i]->getValue());

                while (UL.size() > 0) UL.pop_back();
                GEP->addUse(frontGEP);
                for (Value *idx : backIndices) GEP->addUse(idx);
                GEP->updateType();

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
