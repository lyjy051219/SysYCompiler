#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <cstddef>
#include <vector>

// IPSCCPPass在不克隆函数的前提下传播内部函数的常量形参和常量返回值。
class IPSCCPPass : public PassDescriptor<IPSCCPPass> {
public:
    PreservationStatus run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    //节点数与深度共同限制巨大调用图和CFG上的传播工作。
    class TraversalBudget final {
    public:
        static constexpr std::size_t MaxNodes = 4096;
        static constexpr unsigned MaxDepth = 128;

        bool tryVisit(unsigned Depth);

    private:
        std::size_t VisitedNodes = 0;
    };

    static Value *meetCallArgumentConstants(
        Function *F, unsigned ParameterIndex,
        const std::vector<CallInst *> &Calls,
        TraversalBudget &Budget);
    static Value *meetReachableReturnConstants(
        Function *F, TraversalBudget &Budget);
};
