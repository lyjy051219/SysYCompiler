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

template <typename IRUnitT>
void AnalysisPassManager<IRUnitT>::recordDependency(
    const typename AnalysisPassManager<IRUnitT>::CacheKey &Dependent,
    const typename AnalysisPassManager<IRUnitT>::CacheKey &Dependency) {
    if (Dependent == Dependency) return;

    auto &Dependencies = AnalysisDependencies[Dependent];
    if (!Dependencies.insert(Dependency).second) return;
    ReverseAnalysisDependencies[Dependency].insert(Dependent);
}

template <typename IRUnitT>
void AnalysisPassManager<IRUnitT>::eraseCachedResults(
    const std::set<typename AnalysisPassManager<IRUnitT>::CacheKey> &Keys) {
    for (const CacheKey &Key : Keys) ResultCache.erase(Key);

    // 先移除每个失效结果的出边，并同步更新依赖对象的反向边。
    for (const CacheKey &Key : Keys) {
        auto ForwardIt = AnalysisDependencies.find(Key);
        if (ForwardIt == AnalysisDependencies.end()) continue;

        std::set<CacheKey> Dependencies = std::move(ForwardIt->second);
        AnalysisDependencies.erase(ForwardIt);
        for (const CacheKey &Dependency : Dependencies) {
            auto ReverseIt = ReverseAnalysisDependencies.find(Dependency);
            if (ReverseIt == ReverseAnalysisDependencies.end()) continue;
            ReverseIt->second.erase(Key);
            if (ReverseIt->second.empty())
                ReverseAnalysisDependencies.erase(ReverseIt);
        }
    }

    // 再移除指向失效结果的入边。集合副本避免在遍历时修改同一容器。
    for (const CacheKey &Key : Keys) {
        auto ReverseIt = ReverseAnalysisDependencies.find(Key);
        if (ReverseIt == ReverseAnalysisDependencies.end()) continue;

        std::set<CacheKey> Dependents = std::move(ReverseIt->second);
        ReverseAnalysisDependencies.erase(ReverseIt);
        for (const CacheKey &Dependent : Dependents) {
            auto ForwardIt = AnalysisDependencies.find(Dependent);
            if (ForwardIt == AnalysisDependencies.end()) continue;
            ForwardIt->second.erase(Key);
            if (ForwardIt->second.empty())
                AnalysisDependencies.erase(ForwardIt);
        }
    }
}

template <typename IRUnitT>
void AnalysisPassManager<IRUnitT>::clear() {
    assert(ActiveAnalysisStack.empty() &&
           "cannot clear analyses while an analysis is running");
    ResultCache.clear();
    AnalysisDependencies.clear();
    ReverseAnalysisDependencies.clear();
}

template <typename IRUnitT>
void AnalysisPassManager<IRUnitT>::invalidate(
    IRUnitT &IR, const PassResult &Result) {
    if (!Result.changedIR()) return;

    const PreservationStatus &PA = Result.preservedAnalyses();
    std::set<CacheKey> InvalidKeys;

    // 初始集合包含当前单元的结果，以及所有声明为跨单元的结果。
    for (auto &Entry : ResultCache) {
        PassID *AnalysisID = Entry.first.first;
        const bool IsCrossUnit =
            CrossUnitAnalysisIDs.count(AnalysisID) != 0;
        if (!IsCrossUnit && Entry.first.second != &IR) continue;
        if (!PA.isKept(AnalysisID) ||
            Entry.second->invalidate(*Entry.first.second, PA)) {
            InvalidKeys.insert(Entry.first);
        }
    }

    // 沿缓存键反向依赖图求闭包；跨单元分析的任一结果失效时，同类型的
    // 全部缓存结果都要进入闭包，再继续传播到各自的普通依赖者。
    std::vector<CacheKey> Worklist(InvalidKeys.begin(), InvalidKeys.end());
    std::set<PassID *> ExpandedCrossUnitIDs;
    for (std::size_t Index = 0; Index < Worklist.size(); ++Index) {
        const CacheKey Key = Worklist[Index];
        PassID *AnalysisID = Key.first;
        if (CrossUnitAnalysisIDs.count(AnalysisID) &&
            ExpandedCrossUnitIDs.insert(AnalysisID).second) {
            for (const auto &Entry : ResultCache) {
                if (Entry.first.first != AnalysisID) continue;
                if (InvalidKeys.insert(Entry.first).second)
                    Worklist.push_back(Entry.first);
            }
        }

        auto ReverseIt = ReverseAnalysisDependencies.find(Key);
        if (ReverseIt == ReverseAnalysisDependencies.end()) continue;
        for (const CacheKey &Dependent : ReverseIt->second) {
            if (!ResultCache.count(Dependent)) continue;
            if (InvalidKeys.insert(Dependent).second)
                Worklist.push_back(Dependent);
        }
    }

    eraseCachedResults(InvalidKeys);

    // 模块管理器利用回调同步清理函数分析；桥接器已处理时不重复执行。
    if (!PA.wasCrossUnitInvalidationHandled() && IRMutationCallback)
        IRMutationCallback();
}

template <typename IRUnitT>
void AnalysisPassManager<IRUnitT>::invalidate(
    IRUnitT &IR, PreservationStatus PA) {
    invalidate(IR, PassResult::fromLegacy(std::move(PA)));
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
