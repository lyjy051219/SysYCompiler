#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "lib/CFG.hpp"
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <vector>

/*
BaseAddressInfo:描述基址信息
SideEffectInfo:描述callee的副作用信息
CallEffectSummary：把callee副作用映射到实参
生成Caller的SideEffectInfo
*/

//ModRefInfo
enum class ModRefInfo : unsigned {
    //既不读取也不写入
    NoModRef = 0,
    //可能读取
    Ref      = 1,
    //可能写入
    Mod      = 2,
    //可能同时读取和写入
    ModRef   = 3
};
//判断ModRef结果是否包含写效果
bool isModSet(ModRefInfo MRI);
//判断ModRef结果是否包含读效果
bool isRefSet(ModRefInfo MRI);
//判断ModRef结果是否不含任何读写效果
bool isNoModRef(ModRefInfo MRI);
//合并两个ModRef结果中的读写效果
ModRefInfo unionModRef(ModRefInfo A, ModRefInfo B);

//FuncAttr：BuiltinFunc的分类
enum class FuncAttr : unsigned {
    //SysY I/O
    Sylib        = 1u << 0,
    //不读也不写
    NoModRef     = 1u << 1,
    //只读
    MemReadOnly  = 1u << 2,
    //只写
    MemWriteOnly = 1u << 3,
    //读取和写入
    MemReadWrite = (1u << 2) | (1u << 3),
    //未知
    Untracked    = 1u << 4,
};

//IntrinsicID：精确标识LLVM Intrinsic
enum class IntrinsicID {
    //不是已知的内存Intrinsic
    None   = 0,
    //内存复制Intrinsic
    Memcpy = 1,
    //内存填充Intrinsic
    Memset = 2,
    //允许源和目标重叠的内存移动Intrinsic
    Memmove = 3,
};

//BaseAddressInfo：记录指针的基址信息
struct BaseAddressInfo {
    //基址集合
    std::unordered_set<Value*> bases;
    //是否存在未知来源
    bool untracked = false;

    //将另一份指针来源信息合并到当前对象
    /*
    bases     = bases ∪ Other.bases
    untracked = untracked || Other.untracked
    */
    bool merge(const BaseAddressInfo& Other);
    //判断是否既没有已知基址也没有未知来源
    bool isEmpty() const;
};

//SideEffectInfo：最终返回的结果
struct SideEffectInfo {
    //可能写入的外部对象：全局变量/函数形参
    std::unordered_set<Value*> ModSet;
    //可能读取的外部对象
    std::unordered_set<Value*> RefSet;

    //历史保守门禁：是否存在内置行为或无法完全追踪的内存行为
    bool hasBuiltin = false;
    //Call是否访问未知内存
    bool hasUnknownMemory = false;
    //是否存在I/O、计时等Runtime操作
    bool hasObservableEffect = false;
    //Call是否形成严格顺序约束
    bool hasOrderingBarrier = false;
    //Call是否可能超出内存
    bool mayCrossObjectBounds = false;

    //越界的ModRefInfo：仅在MayCrossObjectBounds为true时有效
    ModRefInfo crossObjectModRef = ModRefInfo::NoModRef;
    //指针来源
    std::unordered_map<Value *, BaseAddressInfo> ptrMap;

    bool invalidate(Function &F, const PreservationStatus &PA);
};

//CallMemoryEffect：一次Call对一个实际内存区间的读写效果
struct CallMemoryEffect {
    //一个具体的内存区域
    MemoryLocation Location;
    //ModRefInfo
    ModRefInfo ModRef = ModRefInfo::NoModRef;
};

//CallEffectSummary：一次Call的完整效果
struct CallEffectSummary {
    //该Call对实际内存区间的读写效果
    std::vector<CallMemoryEffect> ProgramMemory;
    //Call是否访问未知内存
    bool HasUnknownMemory = false;
    //Call是否存在I/O、计时等Runtime操作
    bool HasObservableEffect = false;
    //Call是否形成严格顺序约束
    bool HasOrderingBarrier = false;
    //Call是否可能超出内存
    bool MayCrossObjectBounds = false;
    //越界的ModRefInfo：仅在MayCrossObjectBounds为true时有效
    ModRefInfo CrossObjectModRef = ModRefInfo::NoModRef;
};

//SideEffectAnalysis包装类
class SideEffectAnalysis : public AnalysisDescriptor<SideEffectAnalysis> {
public:
    using Result = SideEffectInfo;
    //跨函数IR修改也需要使本分析结果失效
    static constexpr bool InvalidateAcrossIRUnits = true;

    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

private:
    friend AnalysisDescriptor<SideEffectAnalysis>;
    friend CallEffectSummary getCallEffectSummary(CallInst *CI, FunctionAnalysisPassManager &FAM);
    inline static PassID Key;

    //从一个长度值中提取LLVM内存Intrinsic的无符号字节数
    static std::optional<std::size_t> getConstantMemoryIntrinsicLength(
        Value *LengthValue);
    //判断内存Intrinsic是否必须按volatile调用处理
    static bool isVolatileMemoryIntrinsic(CallInst *CI);
    //向调用点摘要中添加或合并一条内存效果
    static void appendCallMemoryEffect(
        CallEffectSummary &Summary, Value *Pointer,
        std::optional<std::size_t> Size, ModRefInfo Effect);
    //构造一个可能执行任意行为的不透明调用摘要
    static CallEffectSummary getOpaqueCallSummary();
    //按CFG发布的内置函数契约构造调用点摘要
    static CallEffectSummary summarizeBuiltinCall(CallInst *CI);
    //将被调函数的全局量或形参映射到当前调用点
    static Value *mapCalleeValueToActual(
        CallInst *CI, Value *CalleeValue);
    //将被调函数的全局量和形参摘要实例化为调用点实参效果
    static CallEffectSummary instantiateCalleeSummary(
        CallInst *CI, const SideEffectInfo &CalleeSummary,
        bool *HadUnmappedEffect = nullptr);

    //为全局变量或函数形参初始化已知基址来源
    static void ensureInitialized(
        Value *V,
        std::unordered_map<Value *, BaseAddressInfo> &PointerOrigins);
    //构建函数内所有指针值到底层基址的映射
    static std::unordered_map<Value *, BaseAddressInfo>
    buildBaseAddressInfoMap(Function &F);
    //将一个实际指针的读写效果映射为函数外可见底层对象
    static bool addPointerEffect(
        Value *Pointer, ModRefInfo Effect,
        const std::unordered_map<Value *, BaseAddressInfo> &PointerOrigins,
        SideEffectInfo &Summary);
    //将调用点摘要投影到调用者的函数级摘要
    static bool mergeCallEffects(
        const CallEffectSummary &CallSummary, bool LegacyHasBuiltin,
        const std::unordered_map<Value *, BaseAddressInfo> &PointerOrigins,
        SideEffectInfo &FunctionSummary);
    //按现有保守契约标记无法完全建模的内存访问
    static bool markUnmodelledMemory(SideEffectInfo &Summary);
    //按现有保守契约标记无法解析目标的调用
    static bool markOpaqueCall(SideEffectInfo &Summary);
    //单调地合并一个布尔副作用属性
    static bool mergeFlag(bool &Destination, bool Source);
    //将内置调用效果加入当前函数摘要
    static void processBuiltinCall(
        CallInst *CI,
        const std::unordered_map<Value *, BaseAddressInfo> &PointerOrigins,
        SideEffectInfo &Summary);
    //从请求函数开始发现可达调用图并收集每个函数的局部效果
    static std::unordered_map<Function *, SideEffectInfo>
    collectReachableFunctionSummaries(Function &Root);
    //将被调函数摘要映射并合并到一个直接调用者
    static bool mergeCalleeEffects(
        CallInst *CI, const SideEffectInfo &CalleeSummary,
        SideEffectInfo &CallerSummary);
    //在可达调用图上单调传播副作用直到固定点
    static void solveToFixedPoint(
        std::unordered_map<Function *, SideEffectInfo> &Summaries);
    //将伴随函数结果写入Pass缓存并返回请求函数摘要
    static SideEffectInfo publishResults(
        Function &Root,
        std::unordered_map<Function *, SideEffectInfo> &Summaries,
        FunctionAnalysisPassManager &FAM);
};

//按照已知运行时契约分类内置函数
FuncAttr classifyBuiltin(BuildInFunction* BIF);
//识别能够精确建模的LLVM内存Intrinsic
IntrinsicID getIntrinsicID(BuildInFunction* BIF);

//查询一条指令对指定内存区间的读写效果
ModRefInfo getInstModRefInfo(const MemoryLocation &Location, Instruction *I,
                             BasicAAResult &AA, const LoopInfo &LI,
                             FunctionAnalysisPassManager &FAM);
//查询一条指令对长度未知的目标指针的读写效果
ModRefInfo getInstModRefInfo(Value* V, Instruction* I,
                             BasicAAResult& AA, const LoopInfo& LI,
                             FunctionAnalysisPassManager& FAM);

//将函数副作用摘要聚合成单个保守ModRef结果
ModRefInfo getFunctionModRefInfo(const SideEffectInfo& SEI);
//返回一次调用对SysY程序内存的整体读写效果
ModRefInfo getCallProgramModRefInfo(
    CallInst *CI, FunctionAnalysisPassManager &FAM);
//返回一次调用的程序内存、可观察效果和顺序屏障摘要
CallEffectSummary getCallEffectSummary(
    CallInst *CI, FunctionAnalysisPassManager &FAM);
