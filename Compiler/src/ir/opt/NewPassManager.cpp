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
    // 结果合并 1. 顺序组合多个 pass 的保存集合时取交集：任一 pass 显式 discard 的分析
    // 始终失效；“全部保留”是交集单位元，进入非单位元合并后，跨单元失效已
    // 处理标志也按两侧取交。
    if (Arg.areAllKept()) return;

    if (areAllKept()) {
        *this = Arg;
        return;
    }

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
    // 结果合并 2. pipeline 的 changed 取并集，preservation 取交集，得到组合摘要。
    IRChanged = IRChanged || Arg.IRChanged;
    Preserved.merge(Arg.Preserved);
}

template <typename IRUnitT>
void AnalysisPassManager<IRUnitT>::recordDependency(
    const typename AnalysisPassManager<IRUnitT>::CacheKey &Dependent,
    const typename AnalysisPassManager<IRUnitT>::CacheKey &Dependency) {
    // 2.1 分析构造期间的嵌套 getResult 形成 dependent→dependency 边，并同步维护
    // 反向图，供某个依赖失效时传递驱逐所有使用者。
    if (Dependent == Dependency) return;

    auto &Dependencies = AnalysisDependencies[Dependent];
    if (!Dependencies.insert(Dependency).second) return;
    ReverseAnalysisDependencies[Dependency].insert(Dependent);
}

template <typename IRUnitT>
void AnalysisPassManager<IRUnitT>::eraseCachedResults(
    const std::set<typename AnalysisPassManager<IRUnitT>::CacheKey> &Keys) {
    // 3.3 先删除结果对象，再分别移除每个键的正向依赖边和反向被依赖边；两侧集合
    // 变空时同时删除图节点，避免缓存重建后继承陈旧依赖。
    for (const CacheKey &Key : Keys) ResultCache.erase(Key);

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
    // 4. 显式整体清理只有在没有分析正在构造时才合法；结果缓存与依赖图同步归零。
    assert(ActiveAnalysisStack.empty() &&
           "cannot clear analyses while an analysis is running");
    ResultCache.clear();
    AnalysisDependencies.clear();
    ReverseAnalysisDependencies.clear();
}

template <typename IRUnitT>
void AnalysisPassManager<IRUnitT>::invalidate(
    IRUnitT &IR, const PassResult &Result) {
    // 3.1 unchanged pass 不触发任何失效；changed pass 先按 preservation 与分析结果
    // 自身 invalidate 钩子筛出当前 IR 单元和跨单元分析的初始失效键。
    if (!Result.changedIR()) return;

    const PreservationStatus &PA = Result.preservedAnalyses();
    std::set<CacheKey> InvalidKeys;

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

    // 3.2 沿反向依赖图求传递闭包；跨 IR 单元分析一处失效时，同一分析 ID 的全部
    // 缓存实例一起驱逐，保证模块级/跨函数摘要一致。
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

    // 3.3 闭包稳定后统一删缓存与图边，避免遍历依赖关系时修改容器。
    eraseCachedResults(InvalidKeys);

    // 3.4 若当前层没有负责跨单元失效，则通知上层桥接/模块管理器处理外层缓存。
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
    // 2. 依次在每个函数上运行同一个类型擦除 pass，并立即用单函数结果失效 FAM；
    // 所有函数结果的 changed/preservation 再合并为模块桥接结果。
    PreservationStatus PA = PreservationStatus::all();
    bool Changed = false;
    for (auto &F : M.getFunctions()) {
        PassResult FuncResult = Pass->run(*F, FAM);

        FAM.invalidate(*F, FuncResult);
        PA.merge(FuncResult.preservedAnalyses());
        if (FuncResult.changedIR()) Changed = true;
    }
    // 3. changed 时标记函数级跨单元失效已经逐函数处理，避免模块回调重复清空；
    // 全部未改则保留精确的合并 preservation。
    if (!Changed) return PassResult::unchanged(std::move(PA));
    PA.markCrossUnitInvalidationHandled();
    return PassResult::changed(std::move(PA));
}

template class AnalysisPassManager<Module>;
template class AnalysisPassManager<Function>;
template class PassManager<Module>;
template class PassManager<Function>;
