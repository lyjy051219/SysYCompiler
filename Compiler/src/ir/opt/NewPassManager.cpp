#include "ir/opt/NewPassManager.hpp"

PreservationStatus PreservationStatus::none() {
    return PreservationStatus();
}

PreservationStatus PreservationStatus::all() {
    PreservationStatus PA;
    PA.PreservedAll = true;
    return PA;
}

void PreservationStatus::keep(PassID *ID) {
    AbandonedIDs.erase(ID);
    if (!PreservedAll) PreservedIDs.insert(ID);
}

void PreservationStatus::discard(PassID *ID) {
    PreservedIDs.erase(ID);
    AbandonedIDs.insert(ID);
}

bool PreservationStatus::areAllKept() const {
    return PreservedAll && AbandonedIDs.empty();
}

bool PreservationStatus::wasCrossUnitInvalidationHandled() const {
    return CrossUnitInvalidationHandled;
}

bool PreservationStatus::isKept(PassID *ID) const {
    if (AbandonedIDs.count(ID)) return false;
    return PreservedAll || PreservedIDs.count(ID);
}

void PreservationStatus::markCrossUnitInvalidationHandled() {
    CrossUnitInvalidationHandled = true;
}

void PreservationStatus::merge(const PreservationStatus &Arg) {
    if (Arg.areAllKept()) return;

    // 无限制的 all() 是交集运算的单位元；带弃用集合的 all() 则不是。
    if (areAllKept()) {
        *this = Arg;
        return;
    }

    // 只有两侧都已经处理跨单元失效时，合并结果才可继续标记为已处理。
    CrossUnitInvalidationHandled =
        CrossUnitInvalidationHandled && Arg.CrossUnitInvalidationHandled;

    const bool ThisPreservedAll = PreservedAll;
    const bool ArgPreservedAll = Arg.PreservedAll;

    std::set<PassID *> MergedAbandoned = AbandonedIDs;
    MergedAbandoned.insert(
        Arg.AbandonedIDs.begin(), Arg.AbandonedIDs.end());

    if (ThisPreservedAll && ArgPreservedAll) {
        PreservedAll = true;
        PreservedIDs.clear();
    } else if (ThisPreservedAll) {
        PreservedAll = false;
        PreservedIDs = Arg.PreservedIDs;
    } else if (!ArgPreservedAll) {
        for (auto It = PreservedIDs.begin(); It != PreservedIDs.end();) {
            if (!Arg.PreservedIDs.count(*It))
                It = PreservedIDs.erase(It);
            else
                ++It;
        }
    }

    for (PassID *ID : MergedAbandoned) PreservedIDs.erase(ID);
    AbandonedIDs = std::move(MergedAbandoned);
}

PassResult::PassResult() = default;

PassResult::PassResult(bool Changed, PreservationStatus PA)
    : IRChanged(Changed), Preserved(std::move(PA)) {}

PassResult PassResult::unchanged(PreservationStatus PA) {
    return PassResult(false, std::move(PA));
}

PassResult PassResult::changed(PreservationStatus PA) {
    return PassResult(true, std::move(PA));
}

PassResult PassResult::fromLegacy(PreservationStatus PA) {
    const bool Changed = !PA.areAllKept();
    return PassResult(Changed, std::move(PA));
}

bool PassResult::changedIR() const {
    return IRChanged;
}

const PreservationStatus &PassResult::preservedAnalyses() const {
    return Preserved;
}

PreservationStatus &PassResult::preservedAnalyses() {
    return Preserved;
}

void PassResult::merge(const PassResult &Arg) {
    IRChanged = IRChanged || Arg.IRChanged;
    Preserved.merge(Arg.Preserved);
}

ModuleFunctionBridge::ModuleFunctionBridge(
    std::unique_ptr<FunctionPassInterfaceT> Pass,
    FunctionAnalysisPassManager &FAM)
    : Pass(std::move(Pass)), FAM(FAM) {}

PassResult ModuleFunctionBridge::run(
    Module &M, ModuleAnalysisPassManager &) {
    PreservationStatus PA = PreservationStatus::all();
    bool Changed = false;
    for (auto &F : M.getFunctions()) {
        PassResult FuncResult = Pass->run(*F, FAM);
        // 桥接器直接调用类型擦除接口，因此需要在此显式执行函数分析失效。
        FAM.invalidate(*F, FuncResult);
        PA.merge(FuncResult.preservedAnalyses());
        if (FuncResult.changedIR()) Changed = true;
    }
    if (!Changed) return PassResult::unchanged(std::move(PA));
    PA.markCrossUnitInvalidationHandled();
    return PassResult::changed(std::move(PA));
}

// 常用管理器仅在此处显式实例化，避免每个 Pass 翻译单元重复生成代码。
template class AnalysisPassManager<Module>;
template class AnalysisPassManager<Function>;
template class PassManager<Module>;
template class PassManager<Function>;
