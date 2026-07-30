#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "lib/CFG.hpp"
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <vector>

//Loop前向声明用于循环级副作用查询
class Loop;

//ModRefInfo表示一段内存可能被读取或写入的组合关系
enum class ModRefInfo : unsigned {
    //既不读取也不写入目标内存
    NoModRef = 0,
    //可能读取目标内存
    Ref      = 1,
    //可能写入目标内存
    Mod      = 2,
    //可能同时读取和写入目标内存
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

//FuncAttr按照已知运行时契约描述内置函数的内存行为
enum class FuncAttr : unsigned {
    //尚未指定任何内存行为
    None         = 0,
    //SysY输入输出函数，调用本身具有外部可观察效果
    Sylib        = 1u << 0,
    //不访问SysY程序内存
    NoModRef     = 1u << 1,
    //只读取实参所指向的程序内存
    MemReadOnly  = 1u << 2,
    //只写入实参所指向的程序内存
    MemWriteOnly = 1u << 3,
    //同时读取和写入实参所指向的程序内存
    MemReadWrite = (1u << 2) | (1u << 3),
    //契约未知，必须按照保守读写屏障处理
    Untracked    = 1u << 4,
};

//IntrinsicID标识SideEffect能够精确建模的LLVM内存Intrinsic
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

//BaseAddressInfo记录一个指针值所有可追踪的底层基址
struct BaseAddressInfo {
    //指针可能来源的已知底层对象集合
    std::unordered_set<Value*> bases;
    //指针是否还可能来源于已知集合之外的对象
    bool untracked = false;

    //合并另一条指针来源信息并返回当前摘要是否发生变化
    bool merge(const BaseAddressInfo& Other);
    //判断当前摘要是否既没有已知基址也没有未知来源
    bool isEmpty() const;
    //将所有函数外可见的已知基址加入目标集合
    void collectExternalBases(std::unordered_set<Value*> &Dest) const;
};

//BaseAddressInfoMap将每个指针值映射到它的来源摘要
using BaseAddressInfoMap = std::unordered_map<Value*, BaseAddressInfo>;

//SideEffectInfo保存函数及其可达调用闭包的副作用分析结果
struct SideEffectInfo {
    //函数可能写入的外部可见底层对象集合
    std::unordered_set<Value*> ModSet;
    //函数可能读取的外部可见底层对象集合
    std::unordered_set<Value*> RefSet;
    //摘要是否包含内置调用、未知指针或其他不能按普通用户函数处理的行为
    bool hasBuiltin = false;
    //函数是否可能访问无法映射到具体对象的程序内存
    bool hasUnknownMemory = false;
    //函数是否具有输入输出等程序内存之外的可观察效果
    bool hasObservableEffect = false;
    //函数调用是否形成不可跨越的严格顺序屏障
    bool hasOrderingBarrier = false;
    //函数是否可能越过实参所属对象的边界继续访问内存
    bool mayCrossObjectBounds = false;
    //越过对象边界时仍然确定的读写角色
    ModRefInfo crossObjectModRef = ModRefInfo::NoModRef;
    //函数内每个指针值对应的底层基址来源摘要
    BaseAddressInfoMap ptrMap;

    //根据Pass保留状态判断结果是否失效并在失效时清空摘要
    bool invalidate(Function &F, const PreservationStatus &PA);
};

//CallMemoryEffect描述一次调用对一个实际内存区间的读写效果
struct CallMemoryEffect {
    //调用可能访问的实际指针和字节长度
    MemoryLocation Location;
    //调用对该内存区间可能产生的读写效果
    ModRefInfo ModRef = ModRefInfo::NoModRef;
};

//CallEffectSummary分别发布一次调用的程序内存效果和外部可观察效果
struct CallEffectSummary {
    //能够映射到具体实际指针的程序内存效果列表
    std::vector<CallMemoryEffect> ProgramMemory;
    //调用是否可能访问无法可靠映射的程序内存
    bool HasUnknownMemory = false;
    //调用是否具有输入输出等程序内存之外的可观察效果
    bool HasObservableEffect = false;
    //调用是否形成不可跨越的严格顺序屏障
    bool HasOrderingBarrier = false;
    //调用是否可能越过目标指针所属对象的边界
    bool MayCrossObjectBounds = false;
    //越过对象边界时仍然确定的读写角色
    ModRefInfo CrossObjectModRef = ModRefInfo::NoModRef;
};

//SideEffectPendingCall保存固定点传播阶段尚待映射的直接调用边
struct SideEffectPendingCall {
    //直接调用的用户函数
    Function *Callee = nullptr;
    //调用点传递给被调函数的实际参数
    std::vector<Value *> Args;
};

//SideEffectFunctionState保存一个函数的当前摘要和待传播调用边
struct SideEffectFunctionState {
    //函数当前已经求得的副作用摘要
    SideEffectInfo Info;
    //需要继续把被调函数摘要映射回来的直接调用列表
    std::vector<SideEffectPendingCall> Calls;
};

//SideEffectAnalysis构建可达调用图并求解函数级副作用摘要
class SideEffectAnalysis : public AnalysisDescriptor<SideEffectAnalysis> {
public:
    //Pass管理器缓存的函数级分析结果类型
    using Result = SideEffectInfo;
    //跨函数IR修改也需要使本分析结果失效
    static constexpr bool InvalidateAcrossIRUnits = true;
    //构建函数及其可达调用闭包的副作用分析结果
    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;
    //为指针构造访问长度未知的内存位置
    static MemoryLocation getUnknownSizeMemoryLocation(Value *Pointer);
    //从LLVM内存Intrinsic实参中提取可表示的常量访问长度
    static std::optional<std::size_t> getConstantMemoryIntrinsicLength(
        const std::vector<Value *> &Args);

private:
    //允许分析描述器访问本Pass的唯一标识
    friend AnalysisDescriptor<SideEffectAnalysis>;
    //SideEffectAnalysis在Pass管理器中的唯一标识
    inline static PassID Key;
    //为全局变量或函数形参初始化已知基址来源
    static void ensureInitialized(Value *V, BaseAddressInfoMap &Map);
    //构建函数内所有指针值到底层基址的映射
    static BaseAddressInfoMap buildBaseAddressInfoMap(Function &F);
    //按照内置函数契约把调用效果加入当前函数摘要
    static void processBuiltinCall(
        BuildInFunction *BIF, const std::vector<Value*> &Args,
        const BaseAddressInfoMap &CallerPtrMap, SideEffectInfo &Info);
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
//判断基本块中是否存在阻止循环变换的副作用
bool BasicBlockSideEffect(BasicBlock* BB,
                          const Loop& L,
                          BasicAAResult& AA,
                          const LoopInfo& LI,
                          FunctionAnalysisPassManager& FAM);
//判断循环及其所有子循环基本块中是否存在副作用
bool LoopSideEffect(const Loop& L,
                    BasicAAResult& AA,
                    const LoopInfo& LI,
                    FunctionAnalysisPassManager& FAM);
