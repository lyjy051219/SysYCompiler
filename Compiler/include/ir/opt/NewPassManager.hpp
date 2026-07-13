#pragma once
#include <set>
#include <memory>
#include <vector>
#include <map>
#include <utility>
#include <type_traits>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <cstdlib>
#include <iostream>
#include <typeinfo>
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
    // Module pipelines use this bit to distinguish an arbitrary module
    // mutation (which must conservatively clear FAM) from a
    // ModuleFunctionBridge mutation whose per-function invalidation has
    // already been performed.
    bool CrossUnitInvalidationHandled = false;
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

    bool wasCrossUnitInvalidationHandled() const {
        return CrossUnitInvalidationHandled;
    }

    bool isKept(PassID *ID) const {
        if (AbandonedIDs.count(ID)) return false;
        return PreservedAll || PreservedIDs.count(ID);
    }

    void markCrossUnitInvalidationHandled() {
        CrossUnitInvalidationHandled = true;
    }

    void merge(const PreservationStatus &Arg) {
        if (Arg.areAllKept()) return;

        // A genuinely unrestricted `all()` is the identity of intersection.
        // `all() - {A}`, however, is not: copying Arg in that case would lose
        // the fact that A was invalidated by an earlier component.
        if (areAllKept()) {
            *this = Arg;
            return;
        }

        // If either changed component did not perform its dependent-analysis
        // invalidation, the combined status still requires the conservative
        // callback.  Only an all-bridge change set remains handled.
        CrossUnitInvalidationHandled =
            CrossUnitInvalidationHandled &&
            Arg.CrossUnitInvalidationHandled;

        const bool ThisPreservedAll = PreservedAll;
        const bool ArgPreservedAll = Arg.PreservedAll;

        std::set<PassID*> MergedAbandoned = AbandonedIDs;
        MergedAbandoned.insert(Arg.AbandonedIDs.begin(),
                               Arg.AbandonedIDs.end());

        if (ThisPreservedAll && ArgPreservedAll) {
            // (Universe - A) intersect (Universe - B)
            PreservedAll = true;
            PreservedIDs.clear();
        } else if (ThisPreservedAll) {
            // (Universe - A) intersect ExplicitB
            PreservedAll = false;
            PreservedIDs = Arg.PreservedIDs;
        } else if (!ArgPreservedAll) {
            // ExplicitA intersect ExplicitB
            for (auto It = PreservedIDs.begin(); It != PreservedIDs.end();) {
                if (!Arg.PreservedIDs.count(*It))
                    It = PreservedIDs.erase(It);
                else
                    ++It;
            }
        }
        // The remaining case is ExplicitA intersect (Universe - B), for
        // which the explicit candidate set is already PreservedIDs.

        for (PassID *ID : MergedAbandoned)
            PreservedIDs.erase(ID);
        AbandonedIDs = std::move(MergedAbandoned);
    }
};

// A transform has two independent answers:
//   1. whether it changed the IR;
//   2. which cached analyses are still valid afterwards.
//
// Keeping the two facts separate is important.  A pass can mutate metadata or
// an IR property that none of the registered analyses observe, and therefore
// legitimately report Changed=true while preserving every analysis.  The old
// API encoded "changed" as "not all analyses were preserved", which made
// fixed-point convergence and invalidation accidentally depend on one another.
class PassResult {
private:
    bool IRChanged = false;
    PreservationStatus Preserved = PreservationStatus::all();

public:
    PassResult() = default;
    PassResult(bool Changed, PreservationStatus PA)
        : IRChanged(Changed), Preserved(std::move(PA)) {}

    static PassResult unchanged(
        PreservationStatus PA = PreservationStatus::all()) {
        return PassResult(false, std::move(PA));
    }

    static PassResult changed(
        PreservationStatus PA = PreservationStatus::none()) {
        return PassResult(true, std::move(PA));
    }

    // Compatibility adapter for transforms that have not migrated yet.
    // The legacy convention was: all() means unchanged, every other status
    // means changed.  New and corrected transforms should return PassResult
    // directly so they are not constrained by that convention.
    static PassResult fromLegacy(PreservationStatus PA) {
        const bool Changed = !PA.areAllKept();
        return PassResult(Changed, std::move(PA));
    }

    bool changedIR() const { return IRChanged; }
    const PreservationStatus &preservedAnalyses() const { return Preserved; }
    PreservationStatus &preservedAnalyses() { return Preserved; }

    void merge(const PassResult &Arg) {
        IRChanged = IRChanged || Arg.IRChanged;
        Preserved.merge(Arg.Preserved);
    }
};

namespace pass_manager_detail {

inline std::size_t mixFingerprint(std::size_t Seed, std::size_t Value) {
    // A small reversible-style mixer is enough for a debug contract check;
    // this is not used as a persistent IR identity or a security primitive.
    Seed ^= Value + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) +
            (Seed << 6) + (Seed >> 2);
    return Seed;
}

inline std::size_t valueFingerprint(const Value *V) {
    if (!V) return 0;
    std::size_t H = reinterpret_cast<std::uintptr_t>(V);
    auto Ty = V->getIRType();
    if (Ty) {
        H = mixFingerprint(H, static_cast<std::size_t>(Ty->getEnum()));
        H = mixFingerprint(H, Ty->getSize());
    }
    H = mixFingerprint(H, std::hash<std::string>{}(V->getIdent()));
    if (auto *U = dynamic_cast<const User *>(V)) {
        H = mixFingerprint(H, U->getNumOperands());
        for (std::size_t I = 0; I < U->getNumOperands(); ++I)
            H = mixFingerprint(
                H, reinterpret_cast<std::uintptr_t>(U->getOperand(I)));
    }
    return H;
}

inline std::size_t functionFingerprint(Function &F) {
    std::size_t H = valueFingerprint(&F);
    H = mixFingerprint(H, F.getParamCount());
    for (const auto &P : F.getParams())
        H = mixFingerprint(H, valueFingerprint(P.get()));
    for (BasicBlock *BB : F) {
        H = mixFingerprint(H, valueFingerprint(BB));
        for (Instruction *I : *BB) {
            H = mixFingerprint(H, valueFingerprint(I));
            H = mixFingerprint(H, static_cast<std::size_t>(I->instType));
        }
    }
    return H;
}

inline std::size_t irFingerprint(Function &F) {
    return functionFingerprint(F);
}

inline std::size_t irFingerprint(Module &M) {
    std::size_t H = 0;
    for (const auto &G : M.getGlobalVar())
        H = mixFingerprint(H, valueFingerprint(G.get()));
    for (const auto &F : M.getFunctions())
        H = mixFingerprint(H, functionFingerprint(*F));
    return H;
}

inline bool verifyPassContracts() {
    static const bool Enabled =
        std::getenv("LYJY_VERIFY_PASS_CONTRACT") != nullptr;
    return Enabled;
}

inline bool useStrictPassContracts() {
    static const bool Enabled =
        std::getenv("LYJY_STRICT_PASS_CONTRACT") != nullptr;
    return Enabled;
}

template <typename ResultT, typename IRUnitT, typename = void>
struct HasResultInvalidator : std::false_type {};

template <typename ResultT, typename IRUnitT>
struct HasResultInvalidator<
    ResultT, IRUnitT,
    std::void_t<decltype(std::declval<ResultT &>().invalidate(
        std::declval<IRUnitT &>(),
        std::declval<const PreservationStatus &>()))>> : std::true_type {};

} // namespace pass_manager_detail

// 通用描述符基类
template <typename DerivedT>
struct PassDescriptor {};

template <typename DerivedT>
struct AnalysisDescriptor : PassDescriptor<DerivedT> {
    static PassID *ID() { return &DerivedT::Key; }
};

template <typename IRUnitT> class AnalysisPassManager;
template <typename IRUnitT> using AnalysisManager = AnalysisPassManager<IRUnitT>;

// Most analyses only depend on the IR unit used as their cache key.  A small
// number of analyses (for example a call-graph based function analysis) also
// depend on other IR units managed by the same analysis manager.  Such an
// analysis must invalidate all of its cached results when any one unit stops
// preserving it, otherwise a cached caller summary can outlive a changed
// callee.
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
        if constexpr (pass_manager_detail::HasResultInvalidator<
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
        const bool Verify = pass_manager_detail::verifyPassContracts();
        const std::size_t Before = Verify
            ? pass_manager_detail::irFingerprint(IR) : 0;

        auto RawResult = Pass.run(IR, AM);
        PassResult Result = normalizeResult(std::move(RawResult));

        if (Verify) {
            const bool ObservedChange =
                Before != pass_manager_detail::irFingerprint(IR);
            // Only a false negative violates the safety contract.  A
            // composite pipeline may legitimately mutate IR in an early
            // stage and reconstruct the same final fingerprint later; its
            // accumulated Changed=true answer must not be diagnosed as an
            // error merely because the net structure matches.
            if (ObservedChange && !Result.changedIR()) {
                std::cerr << "[PassContract] " << typeid(PassT).name()
                          << " reported unchanged but mutated the IR\n";
                if (pass_manager_detail::useStrictPassContracts())
                    std::abort();

                // A false "unchanged" answer is dangerous because it can
                // terminate a fixed point and retain stale analysis results.
                // Debug verification therefore fails safe after reporting it.
                Result = PassResult::changed(PreservationStatus::none());
            }
        }
        return Result;
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
    // Dependencies are discovered from nested getResult() calls while an
    // analysis is being constructed.  This keeps invalidation aligned with
    // the code that actually consumes an analysis instead of a duplicated,
    // manually maintained dependency table.
    std::map<PassID*, std::set<PassID*>> AnalysisDependencies;
    std::vector<PassID*> ActiveAnalysisStack;
    std::function<void()> IRMutationCallback;

public:
    AnalysisPassManager() = default;
    AnalysisPassManager(AnalysisPassManager &&) = default;
    AnalysisPassManager& operator=(AnalysisPassManager &&) = default;

    void clear() { ResultCache.clear(); }

    void setIRMutationCallback(std::function<void()> Callback) {
        IRMutationCallback = std::move(Callback);
    }

    template <typename PassT>
    typename PassT::Result &getResult(IRUnitT &IR) {
        assert(AnalysisPasses.count(PassT::ID()) && "Pass not registered");
        PassID *RequestedID = PassT::ID();
        if (!ActiveAnalysisStack.empty() &&
            ActiveAnalysisStack.back() != RequestedID) {
            AnalysisDependencies[ActiveAnalysisStack.back()].insert(
                RequestedID);
        }

        auto Key = CacheKey{RequestedID, &IR};
        auto it = ResultCache.find(Key);
        if (it != ResultCache.end()) {
            using WrapperT = AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
            return static_cast<WrapperT &>(*it->second).Result;
        }
        auto &Pass = *AnalysisPasses.at(RequestedID);

        struct ActiveAnalysisGuard {
            std::vector<PassID*> &Stack;
            explicit ActiveAnalysisGuard(std::vector<PassID*> &Stack,
                                         PassID *ID)
                : Stack(Stack) { Stack.push_back(ID); }
            ~ActiveAnalysisGuard() { Stack.pop_back(); }
        } Guard(ActiveAnalysisStack, RequestedID);

        ResultCache[Key] = Pass.run(IR, *this);
        using WrapperT = AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
        return static_cast<WrapperT &>(*ResultCache[Key]).Result;
    }

    // Batch/interprocedural analyses may compute results for several IR units
    // in one fixed-point solve.  Let them publish those companion results so a
    // later query does not repeat the entire call-graph traversal.
    template <typename PassT>
    void setCachedResult(IRUnitT &IR, typename PassT::Result Result) {
        assert(AnalysisPasses.count(PassT::ID()) && "Pass not registered");
        using WrapperT =
            AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
        auto Key = CacheKey{PassT::ID(), &IR};
        // Never replace a live cache entry: clients are allowed to retain a
        // reference returned by getResult() while querying another IR unit.
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

    void invalidate(IRUnitT &IR, const PassResult &Result) {
        if (!Result.changedIR()) return;

        const PreservationStatus &PA = Result.preservedAnalyses();

        // First ask each result whether it remains valid.  Then propagate
        // invalidity through the dependencies observed while analyses were
        // constructed.  The closure is per cache key, so invalidating an
        // ordinary function analysis does not evict the same analysis for an
        // unrelated function; analyses marked cross-unit are relevant for all
        // keys by construction.
        std::set<CacheKey> RelevantKeys;
        std::set<CacheKey> InvalidKeys;
        for (auto &Entry : ResultCache) {
            PassID *AnalysisID = Entry.first.first;
            const bool IsCrossUnit =
                CrossUnitAnalysisIDs.count(AnalysisID) != 0;
            if (!IsCrossUnit && Entry.first.second != &IR) continue;
            RelevantKeys.insert(Entry.first);
            if (!PA.isKept(AnalysisID) ||
                Entry.second->invalidate(*Entry.first.second, PA)) {
                InvalidKeys.insert(Entry.first);
            }
        }

        bool AddedInvalid = true;
        while (AddedInvalid) {
            AddedInvalid = false;
            for (const CacheKey &Key : RelevantKeys) {
                if (InvalidKeys.count(Key)) continue;
                auto DepIt = AnalysisDependencies.find(Key.first);
                if (DepIt == AnalysisDependencies.end()) continue;
                for (PassID *Dependency : DepIt->second) {
                    if (Dependency == Key.first) continue;
                    const CacheKey DependencyKey{Dependency, Key.second};
                    if (!PA.isKept(Dependency) ||
                        InvalidKeys.count(DependencyKey)) {
                        InvalidKeys.insert(Key);
                        AddedInvalid = true;
                        break;
                    }
                }
            }
        }

        for (const CacheKey &Key : InvalidKeys)
            ResultCache.erase(Key);

        // This hook is primarily used by the module analysis manager to
        // invalidate function analyses.  It is driven by Changed rather than
        // by the preservation set: those facts are intentionally independent.
        if (!PA.wasCrossUnitInvalidationHandled() && IRMutationCallback)
            IRMutationCallback();
    }

    // Compatibility for the few transforms that explicitly invalidate in
    // the middle of their run.  Their historical convention is retained at
    // this boundary; new code should pass an explicit PassResult.
    void invalidate(IRUnitT &IR, PreservationStatus PA) {
        invalidate(IR, PassResult::fromLegacy(std::move(PA)));
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
        bool ReachedFixedPoint = false;
        const bool TraceFP = (std::getenv("LYJY_FP_TRACE") != nullptr);

        while (Iter < MaxIterations) {
            ++Iter;

            // 执行内部的 Pass 或 Pipeline
            PassResult IterResult = InnerPass->run(IR, AM);

            // 传播并合并分析失效状态
            AM.invalidate(IR, IterResult);
            Overall.merge(IterResult);

            if (TraceFP) {
                std::cerr << "[FixedPoint] iter=" << Iter
                          << " changed="
                          << (IterResult.changedIR() ? 1 : 0)
                          << "\n";
            }

            // 分析保留状态不参与收敛判定；只有显式 IRChanged=false
            // 才表示这一轮达到了结构不动点。
            if (!IterResult.changedIR()) {
                ReachedFixedPoint = true;
                break;
            }
        }

        if (TraceFP && Iter == MaxIterations && !ReachedFixedPoint) {
            std::cerr << "[FixedPoint] Warning: Reached MaxIterations (" << MaxIterations
                      << ") without reaching fixed point.\n";
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

    explicit ModuleFunctionBridge(std::unique_ptr<FunctionPassInterfaceT> Pass,
                                         FunctionAnalysisPassManager &FAM)
        : Pass(std::move(Pass)), FAM(FAM) {}

    PassResult run(Module &M, ModuleAnalysisPassManager &) {
        PreservationStatus PA = PreservationStatus::all();
        bool Changed = false;
        for (auto& F : M.getFunctions()) {
            PassResult FuncResult = Pass->run(*F, FAM);
            // A bridge invokes the wrapped pass interface directly, so the
            // normal FunctionPassManager invalidation hook is bypassed here.
            FAM.invalidate(*F, FuncResult);
            PA.merge(FuncResult.preservedAnalyses());
            if (FuncResult.changedIR()) {
                Changed = true;
            }
        }
        if (!Changed) return PassResult::unchanged(std::move(PA));
        PA.markCrossUnitInvalidationHandled();
        return PassResult::changed(std::move(PA));
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
