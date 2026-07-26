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

// 中端: NewPassManager
// 项目原生的 CRTP + 类型擦除 pass 框架；修改契约、依赖跟踪和跨层级
// 失效都由本项目 IR 的所有权模型驱动。
class Module;
class Function;


struct alignas(8) PassID {};

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

template <typename ResultT, typename IRUnitT, typename = void>
struct PassManagerHasResultInvalidator : std::false_type {};

template <typename ResultT, typename IRUnitT>
struct PassManagerHasResultInvalidator<
    ResultT, IRUnitT,
    std::void_t<decltype(std::declval<ResultT &>().invalidate(
        std::declval<IRUnitT &>(),
        std::declval<const PreservationStatus &>()))>> : std::true_type {};

// 通用描述符基类
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

template <typename IRUnitT>
class AnalysisPassManager {
private:
    using ResultPtr = std::unique_ptr<AnalysisResultInterface<IRUnitT>>;
    using PassPtr = std::unique_ptr<AnalysisPassInterface<IRUnitT>>;
    using CacheKey = std::pair<PassID*, IRUnitT*>;

    std::map<PassID*, PassPtr> AnalysisPasses;
    std::map<CacheKey, ResultPtr> ResultCache;
    std::set<PassID*> CrossUnitAnalysisIDs;
    // 分析构造期间的嵌套 getResult() 会形成精确到缓存键的依赖边。
    // 正向图用于删除旧边，反向图用于在依赖失效后驱逐所有传递依赖者。
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
        assert(AnalysisPasses.count(PassT::ID()) && "Pass not registered");
        PassID *RequestedID = PassT::ID();
        auto Key = CacheKey{RequestedID, &IR};
        if (!ActiveAnalysisStack.empty())
            recordDependency(ActiveAnalysisStack.back(), Key);

        auto it = ResultCache.find(Key);
        if (it != ResultCache.end()) {
            using WrapperT = AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
            return static_cast<WrapperT &>(*it->second).Result;
        }
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

    // 兼容少数在执行过程中主动失效分析的旧变换；新代码应显式传递 PassResult。
    void invalidate(IRUnitT &IR, PreservationStatus PA);
};

extern template class AnalysisPassManager<Module>;
using ModuleAnalysisPassManager = AnalysisPassManager<Module>;
extern template class AnalysisPassManager<Function>;
using FunctionAnalysisPassManager = AnalysisPassManager<Function>;

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
        if (IRMutationCallback) {
            AM.setIRMutationCallback(IRMutationCallback);
        }
        PassResult Overall = PassResult::unchanged();
        for (auto &Pass : Passes) {
            PassResult Current = Pass->run(IR, AM);
            AM.invalidate(IR, Current);
            Overall.merge(Current);
        }
        return Overall;
    }

    template <typename PassT>
    std::enable_if_t<!std::is_same_v<PassT, PassManager>, void> addPass(PassT &&Pass) { // 修复了之前的语法警告
        using PassWrapperT = PassWrapper<IRUnitT, PassT, AnalysisManagerT>;
        Passes.push_back(std::unique_ptr<PassInterfaceT>(
            new PassWrapperT(std::forward<PassT>(Pass))));
    }

    void setIRMutationCallback(std::function<void()> Callback) {
        IRMutationCallback = std::move(Callback);
    }
};

// 固定点迭代包装器：重复执行一个 pass/pipeline，直到本轮不再产生改动
// （通过显式 Changed 判断），或达到最大迭代次数防止无限死循环。
template <typename IRUnitT, typename AnalysisManagerT = AnalysisManager<IRUnitT>>
class FixedPointPass : public PassDescriptor<FixedPointPass<IRUnitT, AnalysisManagerT>> {
private:
    using PassInterfaceT = PassInterface<IRUnitT, AnalysisManagerT>;
    std::unique_ptr<PassInterfaceT> InnerPass;
    int MaxIterations; // 最大迭代次数安全锁

public:
    explicit FixedPointPass(std::unique_ptr<PassInterfaceT> Inner, int MaxIters = 8)
        : InnerPass(std::move(Inner)), MaxIterations(MaxIters) {}

    PassResult run(IRUnitT &IR, AnalysisManagerT &AM){
        PassResult Overall = PassResult::unchanged();
        int Iter = 0;

        while (Iter < MaxIterations) {
            ++Iter;

            // 执行内部的 Pass 或 Pipeline
            PassResult IterResult = InnerPass->run(IR, AM);

            // 传播并合并分析失效状态
            AM.invalidate(IR, IterResult);
            Overall.merge(IterResult);

            // 分析保留状态不参与收敛判定；只有显式 IRChanged=false
            // 才表示这一轮达到了结构不动点。
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
createFixedPointPass(PassT &&Pass, int MaxIters = 8) { // 默认最大迭代8次
    using PassWrapperT = PassWrapper<IRUnitT, PassT, AnalysisManagerT>;
    return FixedPointPass<IRUnitT, AnalysisManagerT>(
        std::unique_ptr<PassInterface<IRUnitT, AnalysisManagerT>>(
            new PassWrapperT(std::forward<PassT>(Pass))), MaxIters);
}
