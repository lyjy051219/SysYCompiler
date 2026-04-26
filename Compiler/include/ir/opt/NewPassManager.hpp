#pragma once
#include <set>
#include <memory>
#include <vector>
#include <map>
#include <utility>
#include <type_traits>
#include <cassert>
#include <cstddef>
#include <unordered_set>
#include <cstdlib>
#include <iostream>
#include "lib/CFG.hpp"

// 中端: NewPassManager
//参考llvm实现，但做了极大精简，仅保留CRTP，类型擦除机制，失效机制改为手动失效
class Module;
class Function;


struct alignas(8) PassID {};

class PreservationStatus {
private:
    bool PreservedAll = false;
    std::set<PassID*> PreservedIDs;
    std::set<PassID*> AbandonedIDs;

public:
    static PreservationStatus none() { return PreservationStatus(); }

    static PreservationStatus all() {
        PreservationStatus PA;
        PA.PreservedAll = true;
        return PA;
    }

    template <typename AnalysisT>
    void keep() {
        keep(AnalysisT::ID());
    }

    template <typename AnalysisT>
    void discard() {
        discard(AnalysisT::ID());
    }

    void keep(PassID *ID) {
        AbandonedIDs.erase(ID);
        if (!PreservedAll) {
            PreservedIDs.insert(ID);
        }
    }

    void discard(PassID *ID) {
        PreservedIDs.erase(ID);
        AbandonedIDs.insert(ID);
    }

    bool areAllKept() const {
        return PreservedAll && AbandonedIDs.empty();
    }

    bool isKept(PassID *ID) const {
        if (AbandonedIDs.count(ID)) return false;
        return PreservedAll || PreservedIDs.count(ID);
    }

    void merge(const PreservationStatus &Arg) {
        if (Arg.areAllKept()) return;

        if (PreservedAll) {
            *this = Arg;
            return;
        }

        for (auto id : Arg.AbandonedIDs) {
            PreservedIDs.erase(id);
            AbandonedIDs.insert(id);
        }

        for (auto it = PreservedIDs.begin(); it != PreservedIDs.end(); ) {
            if (!Arg.PreservedIDs.count(*it))
                it = PreservedIDs.erase(it);
            else
                ++it;
        }
    }
};

// 通用描述符基类
template <typename DerivedT>
struct PassDescriptor {};

template <typename DerivedT>
struct AnalysisDescriptor : PassDescriptor<DerivedT> {
    static PassID *ID() { return &DerivedT::Key; }
};

template <typename IRUnitT> class AnalysisPassManager;
template <typename IRUnitT> using AnalysisManager = AnalysisPassManager<IRUnitT>;

template <typename IRUnitT>
struct AnalysisResultInterface {
    virtual ~AnalysisResultInterface() = default;
};

template<typename IRUnitT, typename PassT, typename ResultT>
struct AnalysisResultWrapper : AnalysisResultInterface<IRUnitT> {
    explicit AnalysisResultWrapper(ResultT Result) : Result(std::move(Result)) {}
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
    virtual PreservationStatus run(IRUnitT &IR, AnalysisManagerT &AM) = 0;
};

template <typename IRUnitT, typename PassT, typename AnalysisManagerT>
struct PassWrapper : PassInterface<IRUnitT, AnalysisManagerT> {
    explicit PassWrapper(PassT Pass) : Pass(std::move(Pass)) {}
    PreservationStatus run(IRUnitT &IR, AnalysisManagerT &AM) override {
        return Pass.run(IR, AM);
    }
    PassT Pass;
};

template <typename IRUnitT>
class AnalysisPassManager {
private:
    using ResultPtr = std::unique_ptr<AnalysisResultInterface<IRUnitT>>;
    using PassPtr = std::unique_ptr<AnalysisPassInterface<IRUnitT>>;
    using CacheKey = std::pair<PassID*, IRUnitT*>;

    std::map<PassID*, PassPtr> AnalysisPasses;
    std::map<CacheKey, ResultPtr> ResultCache;

public:
    AnalysisPassManager() = default;
    AnalysisPassManager(AnalysisPassManager &&) = default;
    AnalysisPassManager& operator=(AnalysisPassManager &&) = default;

    void clear() { ResultCache.clear(); }

    template <typename PassT>
    typename PassT::Result &getResult(IRUnitT &IR) {
        assert(AnalysisPasses.count(PassT::ID()) && "Pass not registered");
        auto Key = CacheKey{PassT::ID(), &IR};
        auto it = ResultCache.find(Key);
        if (it != ResultCache.end()) {
            using WrapperT = AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
            return static_cast<WrapperT &>(*it->second).Result;
        }
        auto &Pass = *AnalysisPasses.at(PassT::ID());
        ResultCache[Key] = Pass.run(IR, *this);
        using WrapperT = AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
        return static_cast<WrapperT &>(*ResultCache[Key]).Result;
    }

    template <typename PassBuilderT>
    bool registerPass(PassBuilderT &&PassBuilder) {
        using PassT = std::remove_reference_t<decltype(PassBuilder())>;
        using PassWrapperT = AnalysisPassWrapper<IRUnitT, PassT>;
        auto &PassPtr = AnalysisPasses[PassT::ID()];
        if (PassPtr) return false;
        PassPtr.reset(new PassWrapperT(std::forward<PassBuilderT>(PassBuilder)()));
        return true;
    }

    void invalidate(IRUnitT &IR, const PreservationStatus &PA) {
        if (PA.areAllKept()) return;

        for (auto it = ResultCache.begin(); it != ResultCache.end(); ) {
            if (it->first.second == &IR && !PA.isKept(it->first.first)) {
                it = ResultCache.erase(it);
            } else {
                ++it;
            }
        }
    }
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

public:
    explicit PassManager() = default;
    PassManager(PassManager &&Arg) : Passes(std::move(Arg.Passes)) {}
    PassManager &operator=(PassManager &&RHS) {
        Passes = std::move(RHS.Passes);
        return *this;
    }

    PreservationStatus run(IRUnitT &IR, AnalysisManagerT &AM) {
        PreservationStatus PA = PreservationStatus::all();
        for (auto &Pass : Passes) {
            PreservationStatus PassPA = Pass->run(IR, AM);
            AM.invalidate(IR, PassPA);
            PA.merge(PassPA);
        }
        return PA;
    }

    template <typename PassT>
    std::enable_if_t<!std::is_same_v<PassT, PassManager>, void> addPass(PassT &&Pass) { // 修复了之前的语法警告
        using PassWrapperT = PassWrapper<IRUnitT, PassT, AnalysisManagerT>;
        Passes.push_back(std::unique_ptr<PassInterfaceT>(
            new PassWrapperT(std::forward<PassT>(Pass))));
    }
};

// 固定点迭代包装器：重复执行一个 pass/pipeline，直到本轮不再产生改动
// （通过 IterPA.areAllKept() 判断），或达到最大迭代次数防止无限死循环。
template <typename IRUnitT, typename AnalysisManagerT = AnalysisManager<IRUnitT>>
class FixedPointPass : public PassDescriptor<FixedPointPass<IRUnitT, AnalysisManagerT>> {
private:
    using PassInterfaceT = PassInterface<IRUnitT, AnalysisManagerT>;
    std::unique_ptr<PassInterfaceT> InnerPass;
    int MaxIterations; // 最大迭代次数安全锁

public:
    explicit FixedPointPass(std::unique_ptr<PassInterfaceT> Inner, int MaxIters = 8)
        : InnerPass(std::move(Inner)), MaxIterations(MaxIters) {}

    PreservationStatus run(IRUnitT &IR, AnalysisManagerT &AM){
        PreservationStatus OverallPA = PreservationStatus::all();
        int Iter = 0;
        const bool TraceFP = (std::getenv("LYJY_FP_TRACE") != nullptr);

        while (Iter < MaxIterations) {
            ++Iter;

            // 执行内部的 Pass 或 Pipeline
            PreservationStatus IterPA = InnerPass->run(IR, AM);

            // 传播并合并分析失效状态
            AM.invalidate(IR, IterPA);
            OverallPA.merge(IterPA);

            if (TraceFP) {
                std::cerr << "[FixedPoint] iter=" << Iter
                          << " allKept=" << (IterPA.areAllKept() ? 1 : 0)
                          << "\n";
            }

            // 【核心退出条件】：如果这一整轮 Pipeline 下来，所有的 Pass 都返回了 "allKept"
            // 说明没有任何一个 Pass 对 IR 进行了实质性修改，我们完美达到了不动点！
            if (IterPA.areAllKept()) {
                break;
            }
        }

        if (TraceFP && Iter == MaxIterations && !OverallPA.areAllKept()) {
            std::cerr << "[FixedPoint] Warning: Reached MaxIterations (" << MaxIterations
                      << ") without reaching fixed point.\n";
        }

        return OverallPA;
    }
};;

extern template class PassManager<Module>;
using ModulePassManager = PassManager<Module>;
extern template class PassManager<Function>;
using FunctionPassManager = PassManager<Function>;

class ModuleFunctionBridge : public PassDescriptor<ModuleFunctionBridge> {
public:
    using FunctionPassInterfaceT = PassInterface<Function, FunctionAnalysisPassManager>;

    explicit ModuleFunctionBridge(std::unique_ptr<FunctionPassInterfaceT> Pass,
                                         FunctionAnalysisPassManager &FAM)
        : Pass(std::move(Pass)), FAM(FAM) {}

    PreservationStatus run(Module &M, ModuleAnalysisPassManager &) {
        PreservationStatus PA = PreservationStatus::all();
        for (auto& F : M.getFunctions()) {
            PreservationStatus FuncPA = Pass->run(*F, FAM);
            if (!FuncPA.areAllKept())
                PA = PreservationStatus::none();
        }
        return PA;
    }

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
template class AnalysisPassManager<Module>;
template class AnalysisPassManager<Function>;
template class PassManager<Module>;
template class PassManager<Function>;