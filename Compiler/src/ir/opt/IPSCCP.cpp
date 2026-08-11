#include "ir/opt/IPSCCP.hpp"

#include <deque>
#include <unordered_set>
#include <utility>

bool IPSCCPPass::TraversalBudget::tryVisit(unsigned Depth) {
    if (Depth > MaxDepth || VisitedNodes >= MaxNodes) return false;
    ++VisitedNodes;
    return true;
}

Value *IPSCCPPass::meetCallArgumentConstants(
    Function *F, unsigned ParameterIndex,
    const std::vector<CallInst *> &Calls,
    TraversalBudget &Budget) {
    if (!F || Calls.empty() || ParameterIndex >= F->getParamCount())
        return nullptr;

    Value *Common = nullptr;
    for (CallInst *Call : Calls) {
        if (!Budget.tryVisit(1)) return nullptr;
        if (!Call || Call->getDirectCallee() != F ||
            Call->getArgCount() != F->getParamCount())
            return nullptr;
        Value *Argument = Call->getArg(ParameterIndex);
        if (!Argument || !Argument->isConst() || Argument->isUndef())
            return nullptr;
        if (!Common)
            Common = Argument;
        else if (!Value::areEqualConstants(Common, Argument))
            return nullptr;
    }
    return Common;
}

Value *IPSCCPPass::meetReachableReturnConstants(
    Function *F, TraversalBudget &Budget) {
    if (!F || !F->getIRType() || F->getIRType()->isVoid()) return nullptr;

    std::unordered_set<BasicBlock *> Reachable;
    BasicBlock *Entry = F->front();
    if (!Entry || !Budget.tryVisit(0)) return nullptr;
    std::deque<std::pair<BasicBlock *, unsigned>> WorkList;
    Reachable.insert(Entry);
    WorkList.push_back({Entry, 0});
    while (!WorkList.empty()) {
        BasicBlock *Block = WorkList.front().first;
        unsigned Depth = WorkList.front().second;
        WorkList.pop_front();
        if (!Block || Block->getParent() != F) return nullptr;
        for (BasicBlock *Successor : Block->getSuccessors()) {
            if (!Successor || Successor->getParent() != F) return nullptr;
            if (Reachable.count(Successor)) continue;
            if (Depth >= TraversalBudget::MaxDepth ||
                !Budget.tryVisit(Depth + 1))
                return nullptr;
            Reachable.insert(Successor);
            WorkList.push_back({Successor, Depth + 1});
        }
    }
    if (Reachable.empty()) return nullptr;

    Value *Common = nullptr;
    bool SawReturn = false;
    for (BasicBlock *Block : Reachable) {
        if (!Block) return nullptr;
        Instruction *Terminator = Block->getTerminator();
        auto *Return = dynamic_cast<RetInst *>(Terminator);
        if (!Return) continue;
        SawReturn = true;
        Value *Returned = Return->getReturnValue();
        if (!Returned || !Returned->isConst() || Returned->isUndef())
            return nullptr;
        if (!Common)
            Common = Returned;
        else if (!Value::areEqualConstants(Common, Returned))
            return nullptr;
    }
    return SawReturn ? Common : nullptr;
}

PreservationStatus IPSCCPPass::run(
    Module &M, ModuleAnalysisPassManager &MAM) {
    (void)MAM;
    bool Changed = false;

    // 有界固定点允许“被调返回常量”在下一轮继续流入调用者；任何未知调用边
    // 都会让对应格值保持未知，不进行函数克隆或基于调用上下文的猜测。
    for (unsigned Iteration = 0; Iteration < 8; ++Iteration) {
        bool IterationChanged = false;
        std::vector<Function *> Functions;
        for (auto &Owner : M.getFunctions())
            if (Owner) Functions.push_back(Owner.get());

        for (Function *F : Functions) {
            if (!F || F->hasReservedSignature() ||
                !F->hasOnlyDirectCallUses())
                continue;
            std::vector<CallInst *> Calls = F->getDirectCallSites();
            if (Calls.empty()) continue;
            TraversalBudget Budget;
            for (unsigned Index = 0; Index < F->getParamCount(); ++Index) {
                Param *Parameter = F->getParam(Index);
                if (!Parameter || !Parameter->hasUses()) continue;
                Value *Constant = meetCallArgumentConstants(
                    F, Index, Calls, Budget);
                if (!Constant || !Constant->getIRType() ||
                    !Parameter->getIRType() ||
                    !Constant->getIRType()->hasSameShape(
                        *Parameter->getIRType()))
                    continue;
                Parameter->replaceAllUsesWith(Constant);
                IterationChanged = true;
            }
        }

        for (Function *F : Functions) {
            if (!F || F->hasReservedSignature() ||
                !F->hasOnlyDirectCallUses())
                continue;
            TraversalBudget Budget;
            Value *Constant = meetReachableReturnConstants(F, Budget);
            if (!Constant) continue;
            std::vector<CallInst *> Calls = F->getDirectCallSites();
            bool WithinBudget = true;
            for (std::size_t Index = 0; Index < Calls.size(); ++Index)
                if (!Budget.tryVisit(1)) {
                    WithinBudget = false;
                    break;
                }
            if (!WithinBudget) continue;
            for (CallInst *Call : Calls) {
                if (!Call || !Call->hasUses() || !Call->getIRType() ||
                    !Constant->getIRType() ||
                    !Call->getIRType()->hasSameShape(
                        *Constant->getIRType()))
                    continue;
                // 调用本身可能有内存或I/O效果，因此仅替换返回SSA值，不删除调用。
                Call->replaceAllUsesWith(Constant);
                IterationChanged = true;
            }
        }
        Changed |= IterationChanged;
        if (!IterationChanged) break;
    }
    return Changed ? PreservationStatus::none() : PreservationStatus::all();
}
