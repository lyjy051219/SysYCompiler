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

//PassID通过唯一对象地址标识一种分析或Pass类型
struct alignas(8) PassID {};

//PreservationStatus记录一次变换后仍然有效的分析集合
class PreservationStatus {
private:
    //PreservedAll表示默认保留所有未被显式丢弃的分析
    bool PreservedAll = false;
    //CrossUnitInvalidationHandled表示跨IR单元失效已经由桥接层处理
    bool CrossUnitInvalidationHandled = false;
    //PreservedIDs保存显式声明继续有效的分析标识
    std::set<PassID*> PreservedIDs;
    //AbandonedIDs保存显式声明失效并覆盖保留状态的分析标识
    std::set<PassID*> AbandonedIDs;

public:
    //构造不保留任何分析的状态
    static PreservationStatus none();
    //构造默认保留所有分析的状态
    static PreservationStatus all();

    //按分析类型显式保留对应分析结果
    template <typename AnalysisT>
    void keep() {
        keep(AnalysisT::ID());
    }

    //按分析类型显式丢弃对应分析结果
    template <typename AnalysisT>
    void discard() {
        discard(AnalysisT::ID());
    }

    //按PassID显式保留对应分析结果
    void keep(PassID *ID);
    //按PassID显式丢弃对应分析结果
    void discard(PassID *ID);
    //判断当前状态是否无条件保留全部分析
    bool areAllKept() const;
    //判断跨IR单元分析失效是否已经处理
    bool wasCrossUnitInvalidationHandled() const;
    //判断指定分析是否仍然有效
    bool isKept(PassID *ID) const;
    //标记跨IR单元分析失效已经处理
    void markCrossUnitInvalidationHandled();
    //按顺序组合语义合并另一份分析保留状态
    void merge(const PreservationStatus &Arg);
};

//PassResult同时记录IR变化状态和仍然有效的分析集合
class PassResult {
private:
    //IRChanged表示Pass是否实际修改了IR
    bool IRChanged = false;
    //Preserved记录Pass执行后可继续复用的分析
    PreservationStatus Preserved = PreservationStatus::all();

public:
    //构造默认的未修改且全部保留结果
    PassResult();
    //使用变化标志和分析保留状态构造结果
    PassResult(bool Changed, PreservationStatus PA);

    //构造未修改IR的Pass结果
    static PassResult unchanged(
        PreservationStatus PA = PreservationStatus::all());

    //构造已经修改IR的Pass结果
    static PassResult changed(
        PreservationStatus PA = PreservationStatus::none());
    //把旧式仅返回PreservationStatus的结果转换为PassResult
    static PassResult fromLegacy(PreservationStatus PA);

    //判断Pass是否实际修改了IR
    bool changedIR() const;
    //只读访问Pass保留的分析状态
    const PreservationStatus &preservedAnalyses() const;
    //可写访问Pass保留的分析状态
    PreservationStatus &preservedAnalyses();
    //合并顺序执行的另一个Pass结果
    void merge(const PassResult &Arg);
};

//PassManagerHasResultInvalidator默认表示分析结果没有自定义失效接口
template <typename ResultT, typename IRUnitT, typename = void>
struct PassManagerHasResultInvalidator : std::false_type {};

//PassManagerHasResultInvalidator特化探测匹配的invalidate接口
template <typename ResultT, typename IRUnitT>
struct PassManagerHasResultInvalidator<
    ResultT, IRUnitT,
    std::void_t<decltype(std::declval<ResultT &>().invalidate(
        std::declval<IRUnitT &>(),
        std::declval<const PreservationStatus &>()))>> : std::true_type {};

//PassDescriptor为CRTP Pass类型提供统一的类型描述基类
template <typename DerivedT>
struct PassDescriptor {};

//AnalysisDescriptor为分析类型提供稳定的静态PassID访问接口
template <typename DerivedT>
struct AnalysisDescriptor : PassDescriptor<DerivedT> {
    //返回派生分析类型唯一Key对象的地址
    static PassID *ID() { return &DerivedT::Key; }
};

//AnalysisPassManager前置声明函数级或模块级分析管理器模板
template <typename IRUnitT> class AnalysisPassManager;
//AnalysisManager提供分析管理器模板的简写
template <typename IRUnitT> using AnalysisManager = AnalysisPassManager<IRUnitT>;
//AnalysisInvalidationScope默认把分析结果限制在单个IR单元内
template <typename AnalysisT, typename = void>
struct AnalysisInvalidationScope {
    //AcrossIRUnits表示该分析失效无需传播到其他IR单元
    static constexpr bool AcrossIRUnits = false;
};

//AnalysisInvalidationScope特化读取分析类型声明的跨IR单元失效属性
template <typename AnalysisT>
struct AnalysisInvalidationScope<
    AnalysisT,
    std::void_t<decltype(AnalysisT::InvalidateAcrossIRUnits)>> {
    //AcrossIRUnits保存分析类型声明的跨IR单元失效属性
    static constexpr bool AcrossIRUnits =
        AnalysisT::InvalidateAcrossIRUnits;
};

//AnalysisResultInterface为类型擦除后的分析结果提供统一失效接口
template <typename IRUnitT>
struct AnalysisResultInterface {
    //通过虚析构函数正确释放具体分析结果包装器
    virtual ~AnalysisResultInterface() = default;
    //根据IR和分析保留状态判断当前结果是否必须失效
    virtual bool invalidate(IRUnitT &IR,
                            const PreservationStatus &PA) = 0;
};

//AnalysisResultWrapper保存具体分析结果并适配统一失效接口
template<typename IRUnitT, typename PassT, typename ResultT>
struct AnalysisResultWrapper : AnalysisResultInterface<IRUnitT> {
    //接收并持有一次具体分析运行得到的结果
    explicit AnalysisResultWrapper(ResultT Result) : Result(std::move(Result)) {}
    //优先调用结果自定义失效逻辑，否则依据分析PassID判断
    bool invalidate(IRUnitT &IR,
                    const PreservationStatus &PA) override {
        if constexpr (PassManagerHasResultInvalidator<
                          ResultT, IRUnitT>::value) {
            return Result.invalidate(IR, PA);
        }
        return !PA.isKept(PassT::ID());
    }
    //Result保存类型擦除包装器内部的具体分析结果
    ResultT Result;
};

//AnalysisPassInterface为类型擦除后的分析Pass提供统一运行接口
template <typename IRUnitT>
struct AnalysisPassInterface {
    //通过虚析构函数正确释放具体分析Pass包装器
    virtual ~AnalysisPassInterface() = default;
    //在给定IR单元上运行分析并返回类型擦除结果
    virtual std::unique_ptr<AnalysisResultInterface<IRUnitT>> run(IRUnitT &IR, AnalysisManager<IRUnitT> &AM) const = 0;
};

//AnalysisPassWrapper保存具体分析Pass并包装其运行结果
template <typename IRUnitT, typename PassT>
struct AnalysisPassWrapper : AnalysisPassInterface<IRUnitT> {
    //接收并持有具体分析Pass对象
    explicit AnalysisPassWrapper(PassT Pass) : Pass(std::move(Pass)) {}
    //运行具体分析并把返回值封装为类型擦除结果
    std::unique_ptr<AnalysisResultInterface<IRUnitT>> run(IRUnitT &IR, AnalysisManager<IRUnitT> &AM) const override {
        using ResultWrapperT = AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
        return std::make_unique<ResultWrapperT>(Pass.run(IR, AM));
    }
    //Pass保存类型擦除包装器内部的具体分析Pass
    PassT Pass;
};

//PassInterface为类型擦除后的变换Pass提供统一运行接口
template <typename IRUnitT, typename AnalysisManagerT>
struct PassInterface {
    //通过虚析构函数正确释放具体变换Pass包装器
    virtual ~PassInterface() = default;
    //在给定IR单元上运行变换并返回统一Pass结果
    virtual PassResult run(IRUnitT &IR, AnalysisManagerT &AM) = 0;
};

//PassWrapper保存具体变换Pass并统一其返回结果形式
template <typename IRUnitT, typename PassT, typename AnalysisManagerT>
struct PassWrapper : PassInterface<IRUnitT, AnalysisManagerT> {
    //接收并持有具体变换Pass对象
    explicit PassWrapper(PassT Pass) : Pass(std::move(Pass)) {}
    //运行具体Pass并把新旧两种返回形式规范为PassResult
    PassResult run(IRUnitT &IR, AnalysisManagerT &AM) override {
        auto RawResult = Pass.run(IR, AM);
        return normalizeResult(std::move(RawResult));
    }
    //Pass保存类型擦除包装器内部的具体变换Pass
    PassT Pass;

private:
    //直接接收已经采用新接口返回的PassResult
    static PassResult normalizeResult(PassResult Result) {
        return Result;
    }

    //把旧式PreservationStatus返回值转换为统一PassResult
    static PassResult normalizeResult(PreservationStatus PA) {
        return PassResult::fromLegacy(std::move(PA));
    }
};

//AnalysisPassManager注册、缓存并按依赖关系失效指定IR单元的分析结果
template <typename IRUnitT>
class AnalysisPassManager {
private:
    //ResultPtr表示类型擦除分析结果的独占所有权
    using ResultPtr = std::unique_ptr<AnalysisResultInterface<IRUnitT>>;
    //PassPtr表示类型擦除分析Pass的独占所有权
    using PassPtr = std::unique_ptr<AnalysisPassInterface<IRUnitT>>;
    //CacheKey使用分析PassID和IR单元地址唯一标识一个缓存结果
    using CacheKey = std::pair<PassID*, IRUnitT*>;

    //AnalysisPasses把每种分析ID映射到已注册的分析Pass
    std::map<PassID*, PassPtr> AnalysisPasses;
    //ResultCache保存每种分析在每个IR单元上的计算结果
    std::map<CacheKey, ResultPtr> ResultCache;
    //CrossUnitAnalysisIDs记录失效必须传播到其他IR单元的分析
    std::set<PassID*> CrossUnitAnalysisIDs;

    //AnalysisDependencies保存分析结果到其直接依赖结果的正向边
    std::map<CacheKey, std::set<CacheKey>> AnalysisDependencies;
    //ReverseAnalysisDependencies保存依赖结果到所有使用者的反向边
    std::map<CacheKey, std::set<CacheKey>> ReverseAnalysisDependencies;
    //ActiveAnalysisStack记录当前嵌套构造中的分析以捕获查询依赖
    std::vector<CacheKey> ActiveAnalysisStack;
    //IRMutationCallback在函数变化需要使外层IR分析失效时回调
    std::function<void()> IRMutationCallback;

    //记录一个分析结果对另一个分析结果的直接依赖
    void recordDependency(const CacheKey &Dependent,
                          const CacheKey &Dependency);
    //统一删除指定缓存结果及其正反向依赖图边
    void eraseCachedResults(const std::set<CacheKey> &Keys);

public:
    //构造空的分析Pass管理器
    AnalysisPassManager() = default;
    //移动构造分析Pass注册表、结果缓存和依赖状态
    AnalysisPassManager(AnalysisPassManager &&) = default;
    //移动赋值分析Pass注册表、结果缓存和依赖状态
    AnalysisPassManager& operator=(AnalysisPassManager &&) = default;

    //清空全部分析结果缓存及其依赖图
    void clear();

    //安装跨IR层级变化时调用的分析失效回调
    void setIRMutationCallback(std::function<void()> Callback) {
        IRMutationCallback = std::move(Callback);
    }

    //查询缓存或运行指定分析并记录嵌套分析依赖
    template <typename PassT>
    typename PassT::Result &getResult(IRUnitT &IR) {
        //1.生成精确到IR实例的缓存键；若另一分析正在构造，记录活动分析依赖本次查询
        assert(AnalysisPasses.count(PassT::ID()) && "Pass not registered");
        PassID *RequestedID = PassT::ID();
        auto Key = CacheKey{RequestedID, &IR};
        if (!ActiveAnalysisStack.empty())
            recordDependency(ActiveAnalysisStack.back(), Key);

        auto it = ResultCache.find(Key);
        if (it != ResultCache.end()) {
            //2.缓存命中只进行类型包装器下转，不重复运行分析
            using WrapperT = AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
            return static_cast<WrapperT &>(*it->second).Result;
        }
        //3.缓存未命中时以RAII维护活动分析栈并运行类型擦除分析
        auto &Pass = *AnalysisPasses.at(RequestedID);

        //ActiveAnalysisGuard保证所有退出路径都能恢复活动分析栈
        struct ActiveAnalysisGuard {
            //Stack引用分析管理器维护的活动分析栈
            std::vector<CacheKey> &Stack;
            //压入当前正在构造的分析缓存键
            explicit ActiveAnalysisGuard(std::vector<CacheKey> &Stack,
                                         CacheKey Key)
                : Stack(Stack) { Stack.push_back(Key); }
            //分析结束时弹出当前缓存键
            ~ActiveAnalysisGuard() { Stack.pop_back(); }
        } Guard(ActiveAnalysisStack, Key);

        ResultCache[Key] = Pass.run(IR, *this);
        using WrapperT = AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
        return static_cast<WrapperT &>(*ResultCache[Key]).Result;
    }

    //把外部预计算的分析结果写入尚未填充的缓存位置
    template <typename PassT>
    void setCachedResult(IRUnitT &IR, typename PassT::Result Result) {
        //外部预计算结果只填充空缓存，避免覆盖已经建立依赖关系的实例
        assert(AnalysisPasses.count(PassT::ID()) && "Pass not registered");
        using WrapperT =
            AnalysisResultWrapper<IRUnitT, PassT, typename PassT::Result>;
        auto Key = CacheKey{PassT::ID(), &IR};
        if (ResultCache.count(Key)) return;
        ResultCache.emplace(
            Key, std::make_unique<WrapperT>(std::move(Result)));
    }

    //注册一种分析Pass并记录其跨IR单元失效属性
    template <typename PassBuilderT>
    bool registerPass(PassBuilderT &&PassBuilder) {
        //同一分析ID只注册一次，带AcrossIRUnits标记的类型进入跨单元集合
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

    //依据新式PassResult失效当前及相关IR单元的分析缓存
    void invalidate(IRUnitT &IR, const PassResult &Result);

    //依据旧式PreservationStatus失效当前及相关IR单元的分析缓存
    void invalidate(IRUnitT &IR, PreservationStatus PA);
};

//显式实例化模块分析管理器模板
extern template class AnalysisPassManager<Module>;
//ModuleAnalysisPassManager表示模块级分析管理器
using ModuleAnalysisPassManager = AnalysisPassManager<Module>;
//显式实例化函数分析管理器模板
extern template class AnalysisPassManager<Function>;
//FunctionAnalysisPassManager表示函数级分析管理器
using FunctionAnalysisPassManager = AnalysisPassManager<Function>;

//PassManager按声明顺序运行变换Pass并同步维护分析缓存
template <typename IRUnitT, typename AnalysisManagerT = AnalysisManager<IRUnitT>>
class PassManager : public PassDescriptor<PassManager<IRUnitT, AnalysisManagerT>> {
protected:
    //PassInterfaceT表示当前IR层级使用的类型擦除Pass接口
    using PassInterfaceT = PassInterface<IRUnitT, AnalysisManagerT>;
    //Passes按添加顺序持有待运行的类型擦除Pass
    std::vector<std::unique_ptr<PassInterfaceT>> Passes;
    //IRMutationCallback把当前层IR变化通知给外层分析管理器
    std::function<void()> IRMutationCallback;

public:
    //构造空的Pass流水线
    explicit PassManager() = default;
    //移动构造Pass流水线及IR变化回调
    PassManager(PassManager &&Arg)
        : Passes(std::move(Arg.Passes)),
          IRMutationCallback(std::move(Arg.IRMutationCallback)) {}
    //移动赋值Pass流水线及IR变化回调
    PassManager &operator=(PassManager &&RHS) {
        Passes = std::move(RHS.Passes);
        IRMutationCallback = std::move(RHS.IRMutationCallback);
        return *this;
    }

    //顺序运行全部Pass并合并IR变化和分析保留结果
    PassResult run(IRUnitT &IR, AnalysisManagerT &AM) {
        //1.把上层变化回调传给分析管理器，使跨IR单元变化能够向外传播
        if (IRMutationCallback) {
            AM.setIRMutationCallback(IRMutationCallback);
        }
        //2.严格按添加顺序运行，每个Pass返回后立即失效缓存并合入流水线总结果
        PassResult Overall = PassResult::unchanged();
        for (auto &Pass : Passes) {
            PassResult Current = Pass->run(IR, AM);
            AM.invalidate(IR, Current);
            Overall.merge(Current);
        }
        return Overall;
    }

    //把具体变换Pass类型擦除后追加到流水线末尾
    template <typename PassT>
    std::enable_if_t<!std::is_same_v<PassT, PassManager>, void> addPass(PassT &&Pass) {
        //构建期完成类型擦除，Passes中的顺序即为最终运行顺序
        using PassWrapperT = PassWrapper<IRUnitT, PassT, AnalysisManagerT>;
        Passes.push_back(std::unique_ptr<PassInterfaceT>(
            new PassWrapperT(std::forward<PassT>(Pass))));
    }

    //安装当前流水线修改IR时调用的外层失效回调
    void setIRMutationCallback(std::function<void()> Callback) {
        IRMutationCallback = std::move(Callback);
    }
};

//FixedPointPass有界重复运行内部Pass直到IR不再变化
template <typename IRUnitT, typename AnalysisManagerT = AnalysisManager<IRUnitT>>
class FixedPointPass : public PassDescriptor<FixedPointPass<IRUnitT, AnalysisManagerT>> {
private:
    //PassInterfaceT表示当前IR层级使用的类型擦除Pass接口
    using PassInterfaceT = PassInterface<IRUnitT, AnalysisManagerT>;
    //InnerPass持有每轮需要重复运行的Pass或子流水线
    std::unique_ptr<PassInterfaceT> InnerPass;
    //MaxIterations限制固定点迭代次数以保证编译终止
    int MaxIterations;

public:
    //接收内部Pass并设置最大固定点迭代次数
    explicit FixedPointPass(std::unique_ptr<PassInterfaceT> Inner, int MaxIters = 8)
        : InnerPass(std::move(Inner)), MaxIterations(MaxIters) {}

    //有界重复内部Pass并在每轮后失效分析及合并结果
    PassResult run(IRUnitT &IR, AnalysisManagerT &AM){
        PassResult Overall = PassResult::unchanged();
        int Iter = 0;

        while (Iter < MaxIterations) {
            ++Iter;

            //每轮运行后立即失效缓存并合并结果，只有IR仍变化时才继续迭代
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

//显式实例化模块Pass管理器模板
extern template class PassManager<Module>;
//ModulePassManager表示模块级变换流水线
using ModulePassManager = PassManager<Module>;
//显式实例化函数Pass管理器模板
extern template class PassManager<Function>;
//FunctionPassManager表示函数级变换流水线
using FunctionPassManager = PassManager<Function>;

//ModuleFunctionBridge把一个函数Pass或流水线适配为模块Pass
class ModuleFunctionBridge : public PassDescriptor<ModuleFunctionBridge> {
public:
    //FunctionPassInterfaceT表示桥接层持有的类型擦除函数Pass接口
    using FunctionPassInterfaceT = PassInterface<Function, FunctionAnalysisPassManager>;

    //接收函数Pass并绑定其使用的函数分析管理器
    explicit ModuleFunctionBridge(
        std::unique_ptr<FunctionPassInterfaceT> Pass,
        FunctionAnalysisPassManager &FAM);

    //依次在模块全部函数上运行内部Pass并合并结果
    PassResult run(Module &M, ModuleAnalysisPassManager &MAM);

private:
    //Pass持有需要在每个函数上运行的类型擦除Pass
    std::unique_ptr<FunctionPassInterfaceT> Pass;
    //FAM引用桥接执行过程中使用的函数分析管理器
    FunctionAnalysisPassManager &FAM;
};

//把具体函数Pass包装为可加入模块流水线的ModuleFunctionBridge
template <typename FunctionPassT>
ModuleFunctionBridge createModuleFunctionBridge(FunctionPassT &&Pass,
                                                              FunctionAnalysisPassManager &FAM) {
    using PassWrapperT = PassWrapper<Function, FunctionPassT, FunctionAnalysisPassManager>;
    return ModuleFunctionBridge(
        std::unique_ptr<ModuleFunctionBridge::FunctionPassInterfaceT>(
            new PassWrapperT(std::forward<FunctionPassT>(Pass))),
        FAM);
}

//把具体Pass包装为具有迭代上限的FixedPointPass
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
