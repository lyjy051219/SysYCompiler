#pragma once
#include "ir/opt/NewPassManager.hpp"
#include "ir/analysis/BasicAliasAnalysis.hpp"
#include "lib/CFG.hpp"
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <vector>

class Loop;

/*跨函数级副作用分析：
如果实现了Select就去补全Select的相关分析
*/

//ModRefInfo:内存读写行为
enum class ModRefInfo : unsigned {
    NoModRef = 0, //既不修改也不读取
    Ref      = 1, //只读取
    Mod      = 2, //只修改
    ModRef   = 3  //既修改也读取
};
bool isModSet(ModRefInfo MRI);
bool isRefSet(ModRefInfo MRI);
bool isNoModRef(ModRefInfo MRI);
ModRefInfo unionModRef(ModRefInfo A, ModRefInfo B);
//FuncAttr:内置函数分类
enum class FuncAttr : unsigned {
    None         = 0,                     //无特殊属性
    Sylib        = 1u << 0,               //SysY运行库函数
    NoModRef     = 1u << 1,               //不修改不读取
    MemReadOnly  = 1u << 2,               //只读内存
    MemWriteOnly = 1u << 3,               //只写内存
    MemReadWrite = (1u << 2) | (1u << 3), //读写内存
    Untracked    = 1u << 4,               //未跟踪
};


enum class IntrinsicID {
    None   = 0,
    Memcpy = 1,
    Memset = 2,
    Memmove = 3,
};

// BaseAddressInfo:指针的基地址信息
struct BaseAddressInfo {
    std::unordered_set<Value*> bases; //基地址集合
    bool untracked = false;           //未追踪信息

    bool merge(const BaseAddressInfo& Other);
    bool isEmpty() const;
    void collectExternalBases(std::unordered_set<Value*> &Dest) const;
};

using BaseAddressInfoMap = std::unordered_map<Value*, BaseAddressInfo>;


struct SideEffectInfo {
    std::unordered_set<Value*> ModSet;
    std::unordered_set<Value*> RefSet;
    bool hasBuiltin = false;

    BaseAddressInfoMap ptrMap;

    bool invalidate(Function &F, const PreservationStatus &PA);
};

// 跨函数固定点求解时记录尚待合并的调用边。
struct SideEffectPendingCall {
    Function *Callee = nullptr;
    std::vector<Value *> Args;
};

// 跨函数固定点求解时保存每个函数的局部摘要和调用边。
struct SideEffectFunctionState {
    SideEffectInfo Info;
    std::vector<SideEffectPendingCall> Calls;
};

//SideEffectAnalysis
class SideEffectAnalysis : public AnalysisDescriptor<SideEffectAnalysis> {
public:
    using Result = SideEffectInfo;

    // A function summary contains the transitive effects of its callees.  A
    // changed callee therefore invalidates cached summaries for other
    // functions in the same FunctionAnalysisPassManager as well.
    static constexpr bool InvalidateAcrossIRUnits = true;

    Result run(Function &F, FunctionAnalysisPassManager &FAM) const;

    // 指令级副作用查询使用的内存区间辅助函数。
    static MemoryLocation getUnknownSizeMemoryLocation(Value *Pointer);
    static std::optional<std::size_t> getConstantMemoryIntrinsicLength(
        const std::vector<Value *> &Args);

private:
    friend AnalysisDescriptor<SideEffectAnalysis>;
    static PassID Key;
    static void ensureInitialized(Value *V, BaseAddressInfoMap &Map);
    static BaseAddressInfoMap buildBaseAddressInfoMap(Function &F);
    static void processBuiltinCall(
        BuildInFunction *BIF, const std::vector<Value*> &Args,
        const BaseAddressInfoMap &CallerPtrMap, SideEffectInfo &Info);
};


FuncAttr    classifyBuiltin(BuildInFunction* BIF);
IntrinsicID getIntrinsicID(BuildInFunction* BIF);

// 指令级副作用查询接口
// 查询 Instruction I 对内存地址 V 的读写行为
ModRefInfo getInstModRefInfo(const MemoryLocation &Location, Instruction *I,
                             BasicAAResult &AA, const LoopInfo &LI,
                             FunctionAnalysisPassManager &FAM);
ModRefInfo getInstModRefInfo(Value* V, Instruction* I,
                             BasicAAResult& AA, const LoopInfo& LI,
                             FunctionAnalysisPassManager& FAM);

// 函数级副作用查询接口
// 查询函数整体对外部内存的读写行为
ModRefInfo getFunctionModRefInfo(const SideEffectInfo& SEI);

// 基本块级副作用查询接口
bool BasicBlockSideEffect(BasicBlock* BB,
                               const Loop& L,
                               BasicAAResult& AA,
                               const LoopInfo& LI,
                               FunctionAnalysisPassManager& FAM);

// 循环级副作用查询接口
bool LoopSideEffect(const Loop& L,
                         BasicAAResult& AA,
                         const LoopInfo& LI,
                         FunctionAnalysisPassManager& FAM);
