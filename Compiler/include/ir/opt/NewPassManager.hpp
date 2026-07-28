#pragma once
#include <set>
#include <memory>
#include <vector>
#include <map>
#include <utility>
#include <type_traits>
#include <cassert>
#include <functional>
#include <unordered_set>
#include "lib/CFG.hpp"

class Module;
class Function;

// 每个分析类型持有唯一静态 PassID 地址，作为注册表、缓存和 preservation 的稳定键。
struct alignas(8) PassID {};

// PreservationStatus 表示一次变换后仍有效的分析集合：PreservedAll 提供补集表示，
// AbandonedIDs 覆盖显式丢弃，组合 pipeline 时按“所有阶段均保留”取交集。
class PreservationStatus {
private:
    bool PreservedAll = false;
    bool CrossUnitInvalidationHandled = false;
    std::set<PassID*> PreservedIDs;
    std::set<PassID*> AbandonedIDs;

public:
    static PreservationStatus none();
    static PreservationStatus all();

    template <typename AnalysisT>
    void keep() {
        keep(AnalysisT::ID());
    }

    template <typename AnalysisT>
    void discard() {
        discard(AnalysisT::ID());
    }

    void keep(PassID *ID);
    void discard(PassID *ID);
    bool areAllKept() const;
    bool wasCrossUnitInvalidationHandled() const;
    bool isKept(PassID *ID) const;
    void markCrossUnitInvalidationHandled();
    void merge(const PreservationStatus &Arg);
};

// PassResult 将“IR 是否真的变化”与“哪些分析仍有效”分离；旧式只返回 preservation
// 的 pass 会通过 fromLegacy 规范化，供统一的 manager 执行流程消费。
class PassResult {
private:
    bool IRChanged = false;
    PreservationStatus Preserved = PreservationStatus::all();

public:
    PassResult();
    PassResult(bool Changed, PreservationStatus PA);

    static PassResult unchanged(
        PreservationStatus PA = PreservationStatus::all());

    static PassResult changed(
        PreservationStatus PA = PreservationStatus::none());
    static PassResult fromLegacy(PreservationStatus PA);

    bool changedIR() const;
    const PreservationStatus &preservedAnalyses() const;
    PreservationStatus &preservedAnalyses();
    void merge(const PassResult &Arg);
};

// 编译期探测分析结果是否实现自定义 invalidate(IR, PA)，没有时按 PassID 通用失效。
template <typename ResultT, typename IRUnitT, typename = void>
struct PassManagerHasResultInvalidator : std::false_type {};

template <typename ResultT, typename IRUnitT>
struct PassManagerHasResultInvalidator<
    ResultT, IRUnitT,
    std::void_t<decltype(std::declval<ResultT &>().invalidate(
        std::declval<IRUnitT &>(),
        std::declval<const PreservationStatus &>()))>> : std::true_type {};

// CRTP 描述符只提供类型身份；AnalysisDescriptor 进一步暴露该分析的静态 ID。
template <typename DerivedT>
struct PassDescriptor {};

template <typename DerivedT>
struct AnalysisDescriptor : PassDescriptor<DerivedT> {
    static PassID *ID() { return &DerivedT::Key; }
};

template <typename IRUnitT> class AnalysisPassManager;
template <typename IRUnitT> using AnalysisManager = AnalysisPassManager<IRUnitT>;
template <typename AnalysisT, typename = void>
struct AnalysisInvalidationScope {
    static constexpr bool AcrossIRUnits = false;
};

template <typename AnalysisT>
struct AnalysisInvalidationScope<
    AnalysisT,
    std::void_t<decltype(AnalysisT::InvalidateAcrossIRUnits)>> {
    static constexpr bool AcrossIRUnits =
        AnalysisT::InvalidateAcrossIRUnits;
};

// 分析 pass 与结果通过两层类型擦除存入 manager：wrapper 保留具体类型，interface
// 提供统一 run/invalidate 边界。
template <typename IRUnitT>
struct AnalysisResultInterface {
    virtual ~AnalysisResultInterface() = default;
    virtual bool invalidate(IRUnitT &IR,
                            const PreservationStatus &PA) = 0;
};

template<typename IRUnitT, typename PassT, typename ResultT>
struct AnalysisResultWrapper : AnalysisResultInterface<IRUnitT> {
    explicit AnalysisResultWrapper(ResultT Result) : Result(std::move(Result)) {}
    bool invalidate(IRUnitT &IR,
                    const PreservationStatus &PA) override {
        if constexpr (PassManagerHasResultInvalidator<
                          ResultT, IRUnitT>::value) {
            return Result.invalidate(IR, PA);
        }
        return !PA.isKept(PassT::ID());
    }
    ResultT Result;
};

template <typename IRUnitT>
struct AnalysisPassInterface {
    virtual ~AnalysisPassInterface() = default;
    virtual std::unique_ptr<AnalysisResultInterface<IRUnitT>> run(IRUnitT &IR, AnalysisManager<IRUnitT> &AM) const = 0;
};

template <typename IRUnitT, typename PassT>
struct AnalysisPassWrapper : AnalysisPassInterface<IRUnitT> {
    explicit AnalysisPassWrapper(PassT Pass) : Pass(std::move(Pass)) {}
    std::unique_ptr<AnalysisResultInterface<IRUnitT>> run(IRUnitT &IR, AnalysisManager<IRUnitT> &AM) const override {
        using ResultWrapperT = AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
        return std::make_unique<ResultWrapperT>(Pass.run(IR, AM));
    }
    PassT Pass;
};

// 变换 pass wrapper 将 PassResult 或旧式 PreservationStatus 统一规范为 PassResult。
template <typename IRUnitT, typename AnalysisManagerT>
struct PassInterface {
    virtual ~PassInterface() = default;
    virtual PassResult run(IRUnitT &IR, AnalysisManagerT &AM) = 0;
};

template <typename IRUnitT, typename PassT, typename AnalysisManagerT>
struct PassWrapper : PassInterface<IRUnitT, AnalysisManagerT> {
    explicit PassWrapper(PassT Pass) : Pass(std::move(Pass)) {}
    PassResult run(IRUnitT &IR, AnalysisManagerT &AM) override {
        auto RawResult = Pass.run(IR, AM);
        return normalizeResult(std::move(RawResult));
    }
    PassT Pass;

private:
    static PassResult normalizeResult(PassResult Result) {
        return Result;
    }

    static PassResult normalizeResult(PreservationStatus PA) {
        return PassResult::fromLegacy(std::move(PA));
    }
};

// AnalysisPassManager 流程：1. 注册分析工厂并标记跨 IR 单元分析；2. getResult
// 按 (PassID, IR*) 查询或构造缓存，同时记录嵌套查询依赖；3. 变换后从直接失效键
// 出发求反向依赖闭包；4. 统一驱逐结果和图边，并在需要时通知外层 IR 单元。
template <typename IRUnitT>
class AnalysisPassManager {
private:
    using ResultPtr = std::unique_ptr<AnalysisResultInterface<IRUnitT>>;
    using PassPtr = std::unique_ptr<AnalysisPassInterface<IRUnitT>>;
    using CacheKey = std::pair<PassID*, IRUnitT*>;

    std::map<PassID*, PassPtr> AnalysisPasses;
    std::map<CacheKey, ResultPtr> ResultCache;
    std::set<PassID*> CrossUnitAnalysisIDs;

    // 正向图用于删除旧依赖边，反向图用于从失效分析传播到所有传递使用者。
    std::map<CacheKey, std::set<CacheKey>> AnalysisDependencies;
    std::map<CacheKey, std::set<CacheKey>> ReverseAnalysisDependencies;
    std::vector<CacheKey> ActiveAnalysisStack;
    std::function<void()> IRMutationCallback;

    void recordDependency(const CacheKey &Dependent,
                          const CacheKey &Dependency);
    void eraseCachedResults(const std::set<CacheKey> &Keys);

public:
    AnalysisPassManager() = default;
    AnalysisPassManager(AnalysisPassManager &&) = default;
    AnalysisPassManager& operator=(AnalysisPassManager &&) = default;

    void clear();

    void setIRMutationCallback(std::function<void()> Callback) {
        IRMutationCallback = std::move(Callback);
    }

    template <typename PassT>
    typename PassT::Result &getResult(IRUnitT &IR) {
        // 2.1 生成精确到 IR 实例的缓存键；若另一分析正在构造，先记录
        // “活动分析依赖本次查询”的边。
        assert(AnalysisPasses.count(PassT::ID()) && "Pass not registered");
        PassID *RequestedID = PassT::ID();
        auto Key = CacheKey{RequestedID, &IR};
        if (!ActiveAnalysisStack.empty())
            recordDependency(ActiveAnalysisStack.back(), Key);

        auto it = ResultCache.find(Key);
        if (it != ResultCache.end()) {
            // 2.2 缓存命中只做安全的 wrapper 下转，不重复运行分析。
            using WrapperT = AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
            return static_cast<WrapperT &>(*it->second).Result;
        }
        // 2.3 缓存未命中时以 RAII 维护活动分析栈，运行类型擦除分析并原位缓存结果；
        // 析构 guard 保证异常路径也不会遗留伪活动状态。
        auto &Pass = *AnalysisPasses.at(RequestedID);

        struct ActiveAnalysisGuard {
            std::vector<CacheKey> &Stack;
            explicit ActiveAnalysisGuard(std::vector<CacheKey> &Stack,
                                         CacheKey Key)
                : Stack(Stack) { Stack.push_back(Key); }
            ~ActiveAnalysisGuard() { Stack.pop_back(); }
        } Guard(ActiveAnalysisStack, Key);

        ResultCache[Key] = Pass.run(IR, *this);
        using WrapperT = AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
        return static_cast<WrapperT &>(*ResultCache[Key]).Result;
    }

    template <typename PassT>
    void setCachedResult(IRUnitT &IR, typename PassT::Result Result) {
        // 2.4 外部预计算结果只填充空缓存，不覆盖已建立依赖关系的现有实例。
        assert(AnalysisPasses.count(PassT::ID()) && "Pass not registered");
        using WrapperT =
            AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
        auto Key = CacheKey{PassT::ID(), &IR};
        if (ResultCache.count(Key)) return;
        ResultCache.emplace(
            Key, std::make_unique<WrapperT>(std::move(Result)));
    }

    template <typename PassBuilderT>
    bool registerPass(PassBuilderT &&PassBuilder) {
        // 1. 同一分析 ID 只注册一次；带 AcrossIRUnits 标记的类型进入跨单元集合。
        using PassT = std::remove_reference_t<decltype(PassBuilder())>;
        using PassWrapperT = AnalysisPassWrapper<IRUnitT, PassT>;
        auto &PassPtr = AnalysisPasses[PassT::ID()];
        if (PassPtr) return false;
        PassPtr.reset(new PassWrapperT(std::forward<PassBuilderT>(PassBuilder)()));
        if constexpr (AnalysisInvalidationScope<PassT>::AcrossIRUnits) {
            CrossUnitAnalysisIDs.insert(PassT::ID());
        }
        return true;
    }

    void invalidate(IRUnitT &IR, const PassResult &Result);

    void invalidate(IRUnitT &IR, PreservationStatus PA);
};

extern template class AnalysisPassManager<Module>;
using ModuleAnalysisPassManager = AnalysisPassManager<Module>;
extern template class AnalysisPassManager<Function>;
using FunctionAnalysisPassManager = AnalysisPassManager<Function>;

// PassManager 流程：1. addPass 完成类型擦除并保持声明顺序；2. run 安装跨层级
// mutation callback；3. 逐 pass 执行并立即失效 analysis cache；4. 合并 changed
// 与 preservation，作为整个 pipeline 的结果。
template <typename IRUnitT, typename AnalysisManagerT = AnalysisManager<IRUnitT>>
class PassManager : public PassDescriptor<PassManager<IRUnitT, AnalysisManagerT>> {
protected:
    using PassInterfaceT = PassInterface<IRUnitT, AnalysisManagerT>;
    std::vector<std::unique_ptr<PassInterfaceT>> Passes;
    std::function<void()> IRMutationCallback;

public:
    explicit PassManager() = default;
    PassManager(PassManager &&Arg)
        : Passes(std::move(Arg.Passes)),
          IRMutationCallback(std::move(Arg.IRMutationCallback)) {}
    PassManager &operator=(PassManager &&RHS) {
        Passes = std::move(RHS.Passes);
        IRMutationCallback = std::move(RHS.IRMutationCallback);
        return *this;
    }

    PassResult run(IRUnitT &IR, AnalysisManagerT &AM) {
        // 2. 上层 mutation callback 先传给分析管理器，使跨 IR 单元变化能向外冒泡。
        if (IRMutationCallback) {
            AM.setIRMutationCallback(IRMutationCallback);
        }
        // 3—4. 严格按 addPass 顺序运行；每个变换返回后立即失效缓存，再把 changed
        // 与 preservation 合入 pipeline 总结果，后续 pass 因而只能看到有效分析。
        PassResult Overall = PassResult::unchanged();
        for (auto &Pass : Passes) {
            PassResult Current = Pass->run(IR, AM);
            AM.invalidate(IR, Current);
            Overall.merge(Current);
        }
        return Overall;
    }

    template <typename PassT>
    std::enable_if_t<!std::is_same_v<PassT, PassManager>, void> addPass(PassT &&Pass) {
        // 1. 在构建期完成类型擦除，vector 顺序就是运行顺序。
        using PassWrapperT = PassWrapper<IRUnitT, PassT, AnalysisManagerT>;
        Passes.push_back(std::unique_ptr<PassInterfaceT>(
            new PassWrapperT(std::forward<PassT>(Pass))));
    }

    void setIRMutationCallback(std::function<void()> Callback) {
        IRMutationCallback = std::move(Callback);
    }
};

// FixedPointPass 流程：1. 持有一个已类型擦除的 pass/pipeline；2. 有界重复执行并
// 在每轮后立即失效缓存、合并结果；3. 首次报告 unchanged 即达到固定点，达到
// MaxIterations 则以当前累计结果安全退出。
template <typename IRUnitT, typename AnalysisManagerT = AnalysisManager<IRUnitT>>
class FixedPointPass : public PassDescriptor<FixedPointPass<IRUnitT, AnalysisManagerT>> {
private:
    using PassInterfaceT = PassInterface<IRUnitT, AnalysisManagerT>;
    std::unique_ptr<PassInterfaceT> InnerPass;
    int MaxIterations;

public:
    explicit FixedPointPass(std::unique_ptr<PassInterfaceT> Inner, int MaxIters = 8)
        : InnerPass(std::move(Inner)), MaxIterations(MaxIters) {}

    PassResult run(IRUnitT &IR, AnalysisManagerT &AM){
        PassResult Overall = PassResult::unchanged();
        int Iter = 0;

        while (Iter < MaxIterations) {
            ++Iter;

            // 2. 运行一轮，随后立即失效该轮缓存并合入总结果；changedIR 是唯一
            // 继续条件，preservation 只参与累计结果合并。
            PassResult IterResult = InnerPass->run(IR, AM);

            AM.invalidate(IR, IterResult);
            Overall.merge(IterResult);

            if (!IterResult.changedIR()) {
                break;
            }
        }

        return Overall;
    }
};

extern template class PassManager<Module>;
using ModulePassManager = PassManager<Module>;
extern template class PassManager<Function>;
using FunctionPassManager = PassManager<Function>;

// ModuleFunctionBridge 流程：1. 把函数 pass 类型擦除为模块 pass；2. 按模块函数顺序
// 逐个执行并立即失效 FAM；3. 合并所有函数结果，标记跨单元失效已由桥接层处理。
class ModuleFunctionBridge : public PassDescriptor<ModuleFunctionBridge> {
public:
    using FunctionPassInterfaceT = PassInterface<Function, FunctionAnalysisPassManager>;

    explicit ModuleFunctionBridge(
        std::unique_ptr<FunctionPassInterfaceT> Pass,
        FunctionAnalysisPassManager &FAM);

    PassResult run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    std::unique_ptr<FunctionPassInterfaceT> Pass;
    FunctionAnalysisPassManager &FAM;
};

template <typename FunctionPassT>
ModuleFunctionBridge createModuleFunctionBridge(FunctionPassT &&Pass,
                                                              FunctionAnalysisPassManager &FAM) {
    using PassWrapperT = PassWrapper<Function, FunctionPassT, FunctionAnalysisPassManager>;
    return ModuleFunctionBridge(
        std::unique_ptr<ModuleFunctionBridge::FunctionPassInterfaceT>(
            new PassWrapperT(std::forward<FunctionPassT>(Pass))),
        FAM);
}

template <typename IRUnitT,
          typename AnalysisManagerT = AnalysisManager<IRUnitT>,
          typename PassT>
FixedPointPass<IRUnitT, AnalysisManagerT>
createFixedPointPass(PassT &&Pass, int MaxIters = 8) {
    using PassWrapperT = PassWrapper<IRUnitT, PassT, AnalysisManagerT>;
    return FixedPointPass<IRUnitT, AnalysisManagerT>(
        std::unique_ptr<PassInterface<IRUnitT, AnalysisManagerT>>(
            new PassWrapperT(std::forward<PassT>(Pass))), MaxIters);
}
