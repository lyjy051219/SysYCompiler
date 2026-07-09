#pragma once

#include "ir/opt/NewPassManager.hpp"
#include "lib/CFG.hpp"

#include <utility>
#include <vector>

struct LoadElimQueryResult {
    enum Kind {
        KNOWN,
        PHI_NEEDED,
        CLOBBERED,
        UNKNOWN,
        NEED_PREDECESSORS
    };

    Kind kind;
    Value *value = nullptr;
    std::vector<std::pair<BasicBlock*, Value*>> predMap;

    static LoadElimQueryResult known(Value *V) { return {KNOWN, V, {}}; }
    static LoadElimQueryResult clobbered() { return {CLOBBERED, nullptr, {}}; }
    static LoadElimQueryResult unknown() { return {UNKNOWN, nullptr, {}}; }
    static LoadElimQueryResult needPredecessors() { return {NEED_PREDECESSORS, nullptr, {}}; }
    static LoadElimQueryResult phiNeeded(std::vector<std::pair<BasicBlock*, Value*>> M) {
        return {PHI_NEEDED, nullptr, std::move(M)};
    }
};

class LoadEliminationPass : public PassDescriptor<LoadEliminationPass> {
public:
    PreservationStatus run(Function &F, FunctionAnalysisPassManager &FAM);
};
